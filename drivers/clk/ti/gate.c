/*
 * OMAP gate clock support
 *
 * Copyright (C) 2013 Texas Instruments, Inc.
 *
 * Tero Kristo <t-kristo@ti.com>
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

#include <linux/clk-provider.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk/ti.h>

#define to_clk_divider(_hw) container_of(_hw, struct clk_divider, hw)

static int omap36xx_gate_clk_enable_with_hsdiv_restore(struct clk_hw *clk);

static const struct clk_ops omap_gate_clkdm_clk_ops = {
	.init		= &omap2_init_clk_clkdm,
	.enable		= &omap2_clkops_enable_clkdm,
	.disable	= &omap2_clkops_disable_clkdm,
};

static const struct clk_ops omap_gate_clk_ops = {
	.init		= &omap2_init_clk_clkdm,
	.enable		= &omap2_dflt_clk_enable,
	.disable	= &omap2_dflt_clk_disable,
	.is_enabled	= &omap2_dflt_clk_is_enabled,
};

static const struct clk_ops omap_gate_clk_hsdiv_restore_ops = {
	.init		= &omap2_init_clk_clkdm,
	.enable		= &omap36xx_gate_clk_enable_with_hsdiv_restore,
	.disable	= &omap2_dflt_clk_disable,
	.is_enabled	= &omap2_dflt_clk_is_enabled,
};

/**
 * omap36xx_gate_clk_enable_with_hsdiv_restore - enable clocks suffering
 *         from HSDivider PWRDN problem Implements Errata ID: i556.
 * @clk: DPLL output struct clk
 *
 * 3630 only: dpll3_m3_ck, dpll4_m2_ck, dpll4_m3_ck, dpll4_m4_ck,
 * dpll4_m5_ck & dpll4_m6_ck dividers gets loaded with reset
 * valueafter their respective PWRDN bits are set.  Any dummy write
 * (Any other value different from the Read value) to the
 * corresponding CM_CLKSEL register will refresh the dividers.
 */
static int omap36xx_gate_clk_enable_with_hsdiv_restore(struct clk_hw *clk)
{
	struct clk_divider *parent;
	struct clk_hw *parent_hw;
	u32 dummy_v, orig_v;
	int ret;

	/* Clear PWRDN bit of HSDIVIDER */
	ret = omap2_dflt_clk_enable(clk);

	/* Parent is the x2 node, get parent of parent for the m2 div */
	parent_hw = __clk_get_hw(__clk_get_parent(__clk_get_parent(clk->clk)));
	parent = to_clk_divider(parent_hw);

	/* Restore the dividers */
	if (!ret) {
		orig_v = __raw_readl(parent->reg);
		dummy_v = orig_v;

		/* Write any other value different from the Read value */
		dummy_v ^= (1 << parent->shift);
		__raw_writel(dummy_v, parent->reg);

		/* Write the original divider */
		__raw_writel(orig_v, parent->reg);
	}

	return ret;
}

static void __init _of_omap_gate_clk_setup(struct device_node *node,
					  void __iomem *reg,
					  const struct clk_ops *ops,
					  const struct clk_hw_omap_ops *hw_ops)
{
	struct clk *clk;
	struct clk_init_data init = { 0 };
	struct clk_hw_omap *clk_hw;
	const char *clk_name = node->name;
	const char *parent_name;
	u32 val;

	clk_hw = kzalloc(sizeof(*clk_hw), GFP_KERNEL);
	if (!clk_hw) {
		pr_err("%s: could not allocate clk_hw_omap\n", __func__);
		return;
	}

	clk_hw->hw.init = &init;

	of_property_read_string(node, "clock-output-names", &clk_name);

	init.name = clk_name;
	init.ops = ops;
	clk_hw->enable_reg = reg;
	if (!of_property_read_u32(node, "ti,enable-bit", &val))
		clk_hw->enable_bit = val;
	clk_hw->ops = hw_ops;

	parent_name = of_clk_get_parent_name(node, 0);
	init.parent_names = &parent_name;
	init.num_parents = 1;

	if (of_property_read_bool(node, "ti,set-rate-parent"))
		init.flags |= CLK_SET_RATE_PARENT;

	if (of_property_read_bool(node, "ti,set-bit-to-disable"))
		clk_hw->flags |= INVERT_ENABLE;

	clk = clk_register(NULL, &clk_hw->hw);

	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
}

static void __init of_omap_clkdm_gate_clk_setup(struct device_node *node)
{
	_of_omap_gate_clk_setup(node, NULL, &omap_gate_clkdm_clk_ops, NULL);
}
CLK_OF_DECLARE(omap_clkdm_gate_clk, "ti,clkdm-gate-clock",
	       of_omap_clkdm_gate_clk_setup);

static void __init of_omap_hsdiv_gate_clk_setup(struct device_node *node)
{
	void __iomem *reg = of_iomap(node, 0);
	_of_omap_gate_clk_setup(node, reg, &omap_gate_clk_hsdiv_restore_ops,
				&clkhwops_wait);
}
CLK_OF_DECLARE(omap_hsdiv_gate_clk, "ti,hsdiv-gate-clock",
	       of_omap_hsdiv_gate_clk_setup);

static void __init of_omap_gate_clk_setup(struct device_node *node)
{
	void __iomem *reg = of_iomap(node, 0);
	_of_omap_gate_clk_setup(node, reg, &omap_gate_clk_ops, &clkhwops_wait);
}
CLK_OF_DECLARE(omap_gate_clk, "ti,gate-clock", of_omap_gate_clk_setup);

static void __init of_omap_am35xx_gate_clk_setup(struct device_node *node)
{
	void __iomem *reg = of_iomap(node, 0);
	_of_omap_gate_clk_setup(node, reg, &omap_gate_clk_ops,
				&clkhwops_am35xx_ipss_module_wait);
}
CLK_OF_DECLARE(omap_am35xx_gate_clk, "ti,am35xx-gate-clock",
	       of_omap_am35xx_gate_clk_setup);

static void __init of_omap_dss_gate_clk_setup(struct device_node *node)
{
	void __iomem *reg = of_iomap(node, 0);
	_of_omap_gate_clk_setup(node, reg, &omap_gate_clk_ops,
				&clkhwops_omap3430es2_dss_usbhost_wait);
}
CLK_OF_DECLARE(omap_dss_gate_clk, "ti,dss-gate-clock",
	       of_omap_dss_gate_clk_setup);
