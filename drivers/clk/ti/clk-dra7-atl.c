/*
 * DRA7 ATL (Audio Tracking Logic) clock driver
 *
 * Copyright (C) 2013 Texas Instruments, Inc.
 *
 * Peter Ujfalusi <peter.ujfalusi@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/clk-provider.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define DRA7_ATL_INSTANCES	4

#define DRA7_ATL_PPMR_REG(id)		(0x200 + (id * 0x80))
#define DRA7_ATL_BBSR_REG(id)		(0x204 + (id * 0x80))
#define DRA7_ATL_ATLCR_REG(id)		(0x208 + (id * 0x80))
#define DRA7_ATL_SWEN_REG(id)		(0x210 + (id * 0x80))
#define DRA7_ATL_BWSMUX_REG(id)		(0x214 + (id * 0x80))
#define DRA7_ATL_AWSMUX_REG(id)		(0x218 + (id * 0x80))
#define DRA7_ATL_PCLKMUX_REG(id)	(0x21c + (id * 0x80))

#define DRA7_ATL_SWEN			BIT(0)
#define DRA7_ATL_DIVIDER_MASK		(0x1f)
#define DRA7_ATL_PCLKMUX		BIT(0)
struct dra7_atl_clock_info;

struct dra7_atl_desc {
	struct clk *clk;
	struct clk_hw hw;
	struct dra7_atl_clock_info *cinfo;
	int id;

	bool valid;		/* configured */
	bool enabled;
	u32 bws;		/* Baseband Word Select Mux */
	u32 aws;		/* Audio Word Select Mux */
	u32 divider;		/* Cached divider value */
};

struct dra7_atl_clock_info {
	struct device *dev;
	void __iomem *iobase;
	struct clk_onecell_data clk_data;

	struct dra7_atl_desc cdesc[DRA7_ATL_INSTANCES];
};

#define to_atl_desc(hw)		container_of(hw, struct dra7_atl_desc, hw)

static inline void atl_write(struct dra7_atl_clock_info *cinfo, u32 reg,
			     u32 val)
{
	__raw_writel(val, cinfo->iobase + reg);
}

static inline int atl_read(struct dra7_atl_clock_info *cinfo, u32 reg)
{
	return __raw_readl(cinfo->iobase + reg);
}

static int atl_clk_enable(struct clk_hw *hw)
{
	struct dra7_atl_desc *cdesc = to_atl_desc(hw);

	if (unlikely(!cdesc->valid))
		dev_warn(cdesc->cinfo->dev, "atl%d has not been configured\n",
			 cdesc->id);
	pm_runtime_get_sync(cdesc->cinfo->dev);

	atl_write(cdesc->cinfo, DRA7_ATL_ATLCR_REG(cdesc->id),
		  cdesc->divider - 1);
	atl_write(cdesc->cinfo, DRA7_ATL_SWEN_REG(cdesc->id), DRA7_ATL_SWEN);
	cdesc->enabled = true;

	return 0;
}

static void atl_clk_disable(struct clk_hw *hw)
{
	struct dra7_atl_desc *cdesc = to_atl_desc(hw);

	atl_write(cdesc->cinfo, DRA7_ATL_SWEN_REG(cdesc->id), 0);
	cdesc->enabled = false;

	pm_runtime_put_sync(cdesc->cinfo->dev);
}

static int atl_clk_is_enabled(struct clk_hw *hw)
{
	struct dra7_atl_desc *cdesc = to_atl_desc(hw);

	return cdesc->enabled;
}

static unsigned long atl_clk_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct dra7_atl_desc *cdesc = to_atl_desc(hw);

	return parent_rate / cdesc->divider;
}

static long atl_clk_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *parent_rate)
{
	unsigned divider;

	divider = (*parent_rate + rate / 2) / rate;
	if (divider > DRA7_ATL_DIVIDER_MASK + 1)
		divider = DRA7_ATL_DIVIDER_MASK + 1;

	return *parent_rate / divider;
}

static int atl_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct dra7_atl_desc *cdesc = to_atl_desc(hw);
	u32 divider;

	divider = ((parent_rate + rate / 2) / rate) - 1;
	if (divider > DRA7_ATL_DIVIDER_MASK)
		divider = DRA7_ATL_DIVIDER_MASK;

	cdesc->divider = divider + 1;

	return 0;
}

const struct clk_ops atl_clk_ops = {
	.enable		= atl_clk_enable,
	.disable	= atl_clk_disable,
	.is_enabled	= atl_clk_is_enabled,
	.recalc_rate	= atl_clk_recalc_rate,
	.round_rate	= atl_clk_round_rate,
	.set_rate	= atl_clk_set_rate,
};

const char *parent_name = "atl_gfclk_mux";

static struct clk_init_data atl_clks_hw_init[DRA7_ATL_INSTANCES] = {
	{
		.name = "atl_clk0",
		.ops = &atl_clk_ops,
		.parent_names = &parent_name,
		.num_parents = 1,
		.flags = CLK_IGNORE_UNUSED,
	}, {
		.name = "atl_clk1",
		.ops = &atl_clk_ops,
		.parent_names = &parent_name,
		.num_parents = 1,
		.flags = CLK_IGNORE_UNUSED,
	}, {
		.name = "atl_clk2",
		.ops = &atl_clk_ops,
		.parent_names = &parent_name,
		.num_parents = 1,
		.flags = CLK_IGNORE_UNUSED,
	}, {
		.name = "atl_clk3",
		.ops = &atl_clk_ops,
		.parent_names = &parent_name,
		.num_parents = 1,
		.flags = CLK_IGNORE_UNUSED,
	},
};

static int dra7_atl_reparent_clock(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk *fck, *parent_clk;
	const char *parent_name;
	int ret;

	parent_name = of_get_property(node, "fck_parent", NULL);
	if (!parent_name)
		return 0;

	fck = clk_get(&pdev->dev, "fck");
	if (IS_ERR(fck)) {
		dev_err(&pdev->dev, "failed to get fck\n");
		return PTR_ERR(fck);
	}

	parent_clk = clk_get(NULL, parent_name);
	if (IS_ERR(parent_clk)) {
		dev_err(&pdev->dev, "failed to get new parent clock parent\n");
		ret = PTR_ERR(parent_clk);
		goto err1;
	}

	ret = clk_set_parent(fck, parent_clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to reparent fck\n");
		goto err2;
	}

err2:
	clk_put(parent_clk);
err1:
	clk_put(fck);
	return ret;
}

static int of_dra7_atl_clk_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct dra7_atl_clock_info *cinfo;
	int i, ret;

	if (!node)
		return -ENODEV;

	ret = dra7_atl_reparent_clock(pdev);
	if (ret)
		return ret;

	cinfo = devm_kzalloc(&pdev->dev, sizeof(*cinfo), GFP_KERNEL);
	if (!cinfo)
		return -ENOMEM;

	cinfo->clk_data.clks = devm_kzalloc(&pdev->dev,
			sizeof(*cinfo->clk_data.clks) * DRA7_ATL_INSTANCES,
			GFP_KERNEL);
	if (!cinfo->clk_data.clks)
		return -ENOMEM;

	cinfo->iobase = of_iomap(node, 0);
	cinfo->dev = &pdev->dev;
	pm_runtime_enable(cinfo->dev);

	pm_runtime_get_sync(cinfo->dev);
	atl_write(cinfo, DRA7_ATL_PCLKMUX_REG(0), DRA7_ATL_PCLKMUX);

	for (i = 0; i < DRA7_ATL_INSTANCES; i++) {
		struct device_node *cfg_node;
		char prop[5];
		struct dra7_atl_desc *cdesc = &cinfo->cdesc[i];

		cdesc->cinfo = cinfo;
		cdesc->id = i;
		cdesc->divider = 1;
		cdesc->hw.init = &atl_clks_hw_init[i];

		cdesc->clk = devm_clk_register(&pdev->dev, &cdesc->hw);
		cinfo->clk_data.clks[i] = cdesc->clk;
		cinfo->clk_data.clk_num++;

		/* Get configuration for the ATL instances */
		snprintf(prop, sizeof(prop), "atl%u", i);
		cfg_node = of_find_node_by_name(node, prop);
		if (cfg_node) {
			ret = of_property_read_u32(cfg_node, "bws",
						   &cdesc->bws);
			ret |= of_property_read_u32(cfg_node, "aws",
						    &cdesc->aws);
			if (!ret) {
				cdesc->valid = true;
				atl_write(cinfo, DRA7_ATL_BWSMUX_REG(i),
					  cdesc->bws);
				atl_write(cinfo, DRA7_ATL_AWSMUX_REG(i),
					  cdesc->aws);
			}
		}
	}
	pm_runtime_put_sync(cinfo->dev);

	ret = of_clk_add_provider(node, of_clk_src_onecell_get,
				  &cinfo->clk_data);
	if (ret < 0) {
		dev_err(&pdev->dev, "Fail to add clock driver, %d\n", ret);
		pm_runtime_disable(cinfo->dev);
	}
	return ret;
}

static int of_dra7_atl_clk_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static struct of_device_id of_dra7_atl_clk_match_tbl[] = {
	{ .compatible = "ti,dra7-atl-clock", },
	{},
};
MODULE_DEVICE_TABLE(of, of_dra7_atl_clk_match_tbl);

static struct platform_driver dra7_atl_clk_driver = {
	.driver = {
		.name = "dra7-atl-clock",
		.owner = THIS_MODULE,
		.of_match_table = of_dra7_atl_clk_match_tbl,
	},
	.probe = of_dra7_atl_clk_probe,
	.remove = of_dra7_atl_clk_remove,
};

module_platform_driver(dra7_atl_clk_driver);

MODULE_DESCRIPTION("Clock driver for DRA7 Audio Tracking Logic");
MODULE_ALIAS("platform:dra7-atl-clock");
MODULE_AUTHOR("Peter Ujfalusi <peter.ujfalusi@ti.com>");
MODULE_LICENSE("GPL v2");

