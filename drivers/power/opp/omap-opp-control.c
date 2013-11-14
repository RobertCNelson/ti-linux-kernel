/*
 * TI OPP Modifier Driver
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 * Dave Gerlach <d-gerlach@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/list.h>

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/opp.h>
#include <linux/opp-modifier.h>

#include <dt-bindings/opp/am33xx.h>

#define AM33XX_CTRL_DEVICE_ID_DEVREV_SHIFT		28
#define AM33XX_CTRL_DEVICE_ID_DEVREV_MASK		0xF0000000

#define AM33XX_EFUSE_SMA_MAX_FREQ_MASK			0x1fff

static struct of_device_id opp_omap_of_match[];

struct opp_efuse_context {
	struct device   *dev;
	void __iomem    *efuse;
	void __iomem	*id;
};

static struct opp_efuse_context *opp_efuse;

static unsigned long rev_id;

static unsigned long opp_omap_efuse_read(int offset)
{
	return readl(opp_efuse->efuse + offset);
}

static unsigned long am33xx_devrev_to_opp_rev(int rev)
{
	if (rev_id == 0)
		return OPP_REV(1, 0);
	else if (rev_id == 1)
		return OPP_REV(2, 0);
	else if (rev_id == 2)
		return OPP_REV(2, 1);
	else
		return 0;
}

static int of_opp_check_availability(struct device *dev, struct device_node *np)
{
	const struct property *prop;
	const __be32 *val;
	unsigned long efuse_val, freq, rev, offset, bit;
	int nr;

	if (!dev || !np)
		return -EINVAL;

	prop = of_find_property(np, "opp-modifier", NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -EINVAL;

	nr = prop->length / sizeof(u32);
	if (nr % 4) {
		pr_err("%s: Invalid OMAP OPP Available list\n", __func__);
		return -EINVAL;
	}

	val = prop->value;
	while (nr) {
		freq = be32_to_cpup(val++) * 1000;
		rev = be32_to_cpup(val++);
		offset = be32_to_cpup(val++);
		bit = be32_to_cpup(val++);

		efuse_val = opp_omap_efuse_read(offset);

		if (OPP_REV_CMP(rev, am33xx_devrev_to_opp_rev(rev_id))) {
			if (((~efuse_val & bit) || !bit))
				opp_enable(dev, freq);
			else if (bit && !(~efuse_val & bit))
				opp_disable(dev, freq);
		} else if (!OPP_REV_CMP(rev,
					am33xx_devrev_to_opp_rev(rev_id))) {
			opp_disable(dev, freq);
		} else {
			opp_disable(dev, freq);
		}

		nr -= 4;
	}

	return 0;
}

static int omap_opp_device_modify(struct device *dev)
{
	struct device_node *np;
	int ret;

	if (!dev)
		return -EINVAL;

	np = of_parse_phandle(dev->of_node, "platform-opp-modifier", 0);

	if (!np)
		return -EINVAL;

	ret = of_opp_check_availability(dev, np);

	if (ret)
		pr_err("Error modifying available OPPs\n");

	of_node_put(np);

	return ret;
}

static struct opp_modifier_ops omap_opp_ops = {
	.modify = omap_opp_device_modify,
};

static struct opp_modifier_dev omap_opp_modifier_dev = {
	.ops = &omap_opp_ops,
};

static int opp_omap_probe(struct platform_device *pdev)
{
	struct resource *res;
	int ret = 0;

	opp_efuse = devm_kzalloc(&pdev->dev, sizeof(*opp_efuse), GFP_KERNEL);
	if (!opp_efuse) {
		dev_err(opp_efuse->dev, "efuse context memory allocation failed\n");
		ret = -ENOMEM;
		goto err;
	}

	opp_efuse->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(&pdev->dev, "no memory resource for id\n");
		ret = -ENXIO;
		goto err;
	}

	opp_efuse->id = devm_ioremap(opp_efuse->dev, res->start,
					resource_size(res));
	if (!opp_efuse->id) {
		dev_err(opp_efuse->dev, "could not ioremap id\n");
		ret = -EADDRNOTAVAIL;
		goto err;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no memory resource for efuse\n");
		ret = -ENXIO;
		goto err;
	}

	opp_efuse->efuse = devm_request_and_ioremap(opp_efuse->dev, res);
	if (!opp_efuse->efuse) {
		dev_err(opp_efuse->dev, "could not ioremap efuse\n");
		ret = -EADDRNOTAVAIL;
		goto err;
	}

	rev_id = (readl(opp_efuse->id) & AM33XX_CTRL_DEVICE_ID_DEVREV_MASK)
		  >> AM33XX_CTRL_DEVICE_ID_DEVREV_SHIFT;

	omap_opp_modifier_dev.ops = &omap_opp_ops;
	omap_opp_modifier_dev.of_node = pdev->dev.of_node;

	opp_modifier_register(&omap_opp_modifier_dev);

err:
	return ret;
}

static int opp_omap_remove(struct platform_device *pdev)
{
	return 0;
}

static struct of_device_id opp_omap_of_match[] = {
	{ .compatible = "ti,opp-omap",},
	{ },
};
MODULE_DEVICE_TABLE(of, opp_omap_of_match);

static struct platform_driver opp_omap_driver = {
	.probe		= opp_omap_probe,
	.remove		= opp_omap_remove,
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "ti-opp",
		.of_match_table	= opp_omap_of_match,
	},
};

module_platform_driver(opp_omap_driver);

MODULE_AUTHOR("Dave Gerlach <d-gerlach@ti.com>");
MODULE_DESCRIPTION("OPP Modifier driver for TI SoCs");
MODULE_LICENSE("GPL v2");
