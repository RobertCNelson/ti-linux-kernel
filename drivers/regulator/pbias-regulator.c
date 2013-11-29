/*
 * pbias-regulator.c
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Balaji T K <balajitk@ti.com>
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

#include <linux/err.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>

struct pbias_bit_map {
	u32 enable;
	u32 enable_mask;
	u32 vmode;
};

struct pbias_regulator_data {
	struct regulator_desc desc;
	void __iomem *pbias_addr;
	unsigned int pbias_reg;
	struct regulator_dev *dev;
	struct regmap *syscon;


	const struct pbias_bit_map *bmap;
	int voltage;
};

static int pbias_regulator_set_voltage(struct regulator_dev *dev,
			int min_uV, int max_uV, unsigned *selector)
{
	struct pbias_regulator_data *data = rdev_get_drvdata(dev);
	const struct pbias_bit_map *bmap = data->bmap;
	int ret, vmode;

	if (min_uV <= 1800000)
		vmode = 0;
	else if (min_uV > 1800000)
		vmode = bmap->vmode;

	ret = regmap_update_bits(data->syscon, data->pbias_reg,
						bmap->vmode, vmode);
	data->voltage = min_uV;

	return ret;
}

static int pbias_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct pbias_regulator_data *data = rdev_get_drvdata(rdev);

	return data->voltage;
}

static int pbias_regulator_enable(struct regulator_dev *rdev)
{
	struct pbias_regulator_data *data = rdev_get_drvdata(rdev);
	const struct pbias_bit_map *bmap = data->bmap;
	int ret;

	ret = regmap_update_bits(data->syscon, data->pbias_reg,
					bmap->enable_mask, bmap->enable);

	return ret;
}

static int pbias_regulator_disable(struct regulator_dev *rdev)
{
	struct pbias_regulator_data *data = rdev_get_drvdata(rdev);
	const struct pbias_bit_map *bmap = data->bmap;
	int ret;

	ret = regmap_update_bits(data->syscon, data->pbias_reg,
						bmap->enable_mask, 0);
	return ret;
}

static int pbias_regulator_is_enable(struct regulator_dev *rdev)
{
	struct pbias_regulator_data *data = rdev_get_drvdata(rdev);
	const struct pbias_bit_map *bmap = data->bmap;
	int value;

	regmap_read(data->syscon, data->pbias_reg, &value);

	return value & bmap->enable_mask;
}

static struct regulator_ops pbias_regulator_voltage_ops = {
	.set_voltage	= pbias_regulator_set_voltage,
	.get_voltage	= pbias_regulator_get_voltage,
	.enable		= pbias_regulator_enable,
	.disable	= pbias_regulator_disable,
	.is_enabled	= pbias_regulator_is_enable,
};

#if CONFIG_OF
static const struct pbias_bit_map pbias_omap3 = {
	.enable = BIT(1),
	.enable_mask = BIT(1),
	.vmode = BIT(0),
};

static const struct pbias_bit_map pbias_omap4 = {
	.enable = BIT(26) | BIT(22),
	.enable_mask = BIT(26) | BIT(25) | BIT(22),
	.vmode = BIT(21),
};

static const struct pbias_bit_map pbias_omap5 = {
	.enable = BIT(27) | BIT(26),
	.enable_mask = BIT(27) | BIT(25) | BIT(26),
	.vmode = BIT(21),
};

static const struct of_device_id pbias_of_match[] = {
	{ .compatible = "regulator-pbias-omap3", .data = &pbias_omap3},
	{ .compatible = "regulator-pbias-omap4", .data = &pbias_omap4},
	{ .compatible = "regulator-pbias-omap5", .data = &pbias_omap5},
	{},
};
MODULE_DEVICE_TABLE(of, pbias_of_match);
#endif

static int pbias_regulator_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *syscon_np;
	struct pbias_regulator_data *drvdata;
	const struct of_device_id *id;
	const char *supply_name;
	struct regulator_init_data *initdata;
	struct regulator_config cfg = { };
	unsigned startup_delay;
	int ret;

	drvdata = devm_kzalloc(&pdev->dev, sizeof(struct pbias_regulator_data),
			       GFP_KERNEL);
	if (drvdata == NULL) {
		dev_err(&pdev->dev, "Failed to allocate device data\n");
		return -ENOMEM;
	}

	id = of_match_device(of_match_ptr(pbias_of_match), &pdev->dev);
	if (!id)
		return -ENODEV;

	drvdata->bmap = id->data;
	if (!drvdata->bmap)
		return -ENODEV;

	initdata = of_get_regulator_init_data(&pdev->dev, np);
	if (!initdata)
		return -EINVAL;

	supply_name = initdata->constraints.name;

	of_property_read_u32(np, "startup-delay-us", &startup_delay);
	ret = of_property_read_u32(np, "pbias-reg-offset",
				   &drvdata->pbias_reg);
	if (ret) {
		dev_err(&pdev->dev, "no pbias-reg-offset property set\n");
		return ret;
	}

	syscon_np = of_get_parent(np);
	if (!syscon_np)
		return -ENODEV;

	drvdata->syscon = syscon_node_to_regmap(syscon_np);
	of_node_put(syscon_np);
	if (IS_ERR(drvdata->syscon))
		return PTR_ERR(drvdata->syscon);

	drvdata->desc.name = kstrdup(supply_name, GFP_KERNEL);
	if (drvdata->desc.name == NULL) {
		dev_err(&pdev->dev, "Failed to allocate supply name\n");
		return -ENOMEM;
	}

	drvdata->desc.owner = THIS_MODULE;
	drvdata->desc.enable_time = startup_delay;
	drvdata->desc.type = REGULATOR_VOLTAGE;
	drvdata->desc.ops = &pbias_regulator_voltage_ops;
	drvdata->desc.n_voltages = 3;

	cfg.dev = &pdev->dev;
	cfg.init_data = initdata;
	cfg.driver_data = drvdata;
	cfg.of_node = np;

	drvdata->dev = regulator_register(&drvdata->desc, &cfg);
	if (IS_ERR(drvdata->dev)) {
		ret = PTR_ERR(drvdata->dev);
		dev_err(&pdev->dev, "Failed to register regulator: %d\n", ret);
		goto err_regulator;
	}

	platform_set_drvdata(pdev, drvdata);

	return 0;

err_regulator:
	kfree(drvdata->desc.name);
	return ret;
}

static int pbias_regulator_remove(struct platform_device *pdev)
{
	struct pbias_regulator_data *drvdata = platform_get_drvdata(pdev);

	regulator_unregister(drvdata->dev);
	kfree(drvdata->desc.name);

	return 0;
}

static struct platform_driver pbias_regulator_driver = {
	.probe		= pbias_regulator_probe,
	.remove		= pbias_regulator_remove,
	.driver		= {
		.name		= "pbias-regulator",
		.owner		= THIS_MODULE,
		.of_match_table = of_match_ptr(pbias_of_match),
	},
};

static int __init pbias_regulator_init(void)
{
	return platform_driver_register(&pbias_regulator_driver);
}
subsys_initcall(pbias_regulator_init);

static void __exit pbias_regulator_exit(void)
{
	platform_driver_unregister(&pbias_regulator_driver);
}
module_exit(pbias_regulator_exit);

MODULE_AUTHOR("Balaji T K <balajitk@ti.com>");
MODULE_DESCRIPTION("pbias voltage regulator");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pbias-regulator");
