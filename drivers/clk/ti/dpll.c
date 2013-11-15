/*
 * OMAP DPLL clock support
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
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk/ti.h>

static const struct clk_ops dpll_m4xen_ck_ops = {
	.enable		= &omap3_noncore_dpll_enable,
	.disable	= &omap3_noncore_dpll_disable,
	.recalc_rate	= &omap4_dpll_regm4xen_recalc,
	.round_rate	= &omap4_dpll_regm4xen_round_rate,
	.set_rate	= &omap3_noncore_dpll_set_rate,
	.get_parent	= &omap2_init_dpll_parent,
};

static const struct clk_ops dpll_core_ck_ops = {
	.recalc_rate	= &omap3_dpll_recalc,
	.get_parent	= &omap2_init_dpll_parent,
};

static const struct clk_ops omap3_dpll_core_ck_ops = {
	.init		= &omap2_init_clk_clkdm,
	.get_parent	= &omap2_init_dpll_parent,
	.recalc_rate	= &omap3_dpll_recalc,
	.round_rate	= &omap2_dpll_round_rate,
};

static const struct clk_ops dpll_ck_ops = {
	.enable		= &omap3_noncore_dpll_enable,
	.disable	= &omap3_noncore_dpll_disable,
	.recalc_rate	= &omap3_dpll_recalc,
	.round_rate	= &omap2_dpll_round_rate,
	.set_rate	= &omap3_noncore_dpll_set_rate,
	.get_parent	= &omap2_init_dpll_parent,
	.init		= &omap2_init_clk_clkdm,
};

static const struct clk_ops dpll_no_gate_ck_ops = {
	.recalc_rate	= &omap3_dpll_recalc,
	.get_parent	= &omap2_init_dpll_parent,
	.round_rate	= &omap2_dpll_round_rate,
	.set_rate	= &omap3_noncore_dpll_set_rate,
};

static const struct clk_ops omap3_dpll_ck_ops = {
	.init		= &omap2_init_clk_clkdm,
	.enable		= &omap3_noncore_dpll_enable,
	.disable	= &omap3_noncore_dpll_disable,
	.get_parent	= &omap2_init_dpll_parent,
	.recalc_rate	= &omap3_dpll_recalc,
	.set_rate	= &omap3_noncore_dpll_set_rate,
	.round_rate	= &omap2_dpll_round_rate,
};

static const struct clk_ops omap3_dpll_per_ck_ops = {
	.init		= &omap2_init_clk_clkdm,
	.enable		= &omap3_noncore_dpll_enable,
	.disable	= &omap3_noncore_dpll_disable,
	.get_parent	= &omap2_init_dpll_parent,
	.recalc_rate	= &omap3_dpll_recalc,
	.set_rate	= &omap3_dpll4_set_rate,
	.round_rate	= &omap2_dpll_round_rate,
};

static const struct clk_ops dpll_x2_ck_ops = {
	.recalc_rate	= &omap3_clkoutx2_recalc,
};

static struct clk *omap_clk_register_dpll(struct device *dev, const char *name,
		const char **parent_names, int num_parents, unsigned long flags,
		struct dpll_data *dpll_data, const char *clkdm_name,
		const struct clk_ops *ops)
{
	struct clk *clk;
	struct clk_init_data init = { 0 };
	struct clk_hw_omap *clk_hw;

	/* allocate the divider */
	clk_hw = kzalloc(sizeof(*clk_hw), GFP_KERNEL);
	if (!clk_hw) {
		pr_err("%s: could not allocate clk_hw_omap\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	clk_hw->dpll_data = dpll_data;
	clk_hw->ops = &clkhwops_omap3_dpll;
	clk_hw->clkdm_name = clkdm_name;
	clk_hw->hw.init = &init;

	init.name = name;
	init.ops = ops;
	init.flags = flags;
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	/* register the clock */
	clk = clk_register(dev, &clk_hw->hw);

	if (IS_ERR(clk))
		kfree(clk_hw);
	else
		omap2_init_clk_hw_omap_clocks(clk);

	return clk;
}

static struct clk *omap_clk_register_dpll_x2(struct device *dev,
		const char *name, const char *parent_name, void __iomem *reg,
		const struct clk_ops *ops)
{
	struct clk *clk;
	struct clk_init_data init = { 0 };
	struct clk_hw_omap *clk_hw;

	if (!parent_name) {
		pr_err("%s: dpll_x2 must have parent\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	clk_hw = kzalloc(sizeof(*clk_hw), GFP_KERNEL);
	if (!clk_hw) {
		pr_err("%s: could not allocate clk_hw_omap\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	clk_hw->ops = &clkhwops_omap4_dpllmx;
	clk_hw->clksel_reg = reg;
	clk_hw->hw.init = &init;

	init.name = name;
	init.ops = ops;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	/* register the clock */
	clk = clk_register(dev, &clk_hw->hw);

	if (IS_ERR(clk))
		kfree(clk_hw);
	else
		omap2_init_clk_hw_omap_clocks(clk);

	return clk;
}

/**
 * of_omap_dpll_setup() - Setup function for OMAP DPLL clocks
 *
 * @node: device node containing the DPLL info
 * @ops: ops for the DPLL
 * @ddt: DPLL data template to use
 */
static void __init of_omap_dpll_setup(struct device_node *node,
					const struct clk_ops *ops,
					const struct dpll_data *ddt)
{
	struct clk *clk;
	const char *clk_name = node->name;
	int num_parents;
	const char **parent_names = NULL;
	const char *clkdm_name = NULL;
	u8 dpll_flags = 0;
	struct dpll_data *dd;
	int i;
	u32 val;

	dd = kzalloc(sizeof(*dd), GFP_KERNEL);
	if (!dd) {
		pr_err("%s: could not allocate dpll_data\n", __func__);
		return;
	}

	memcpy(dd, ddt, sizeof(*dd));

	of_property_read_string(node, "clock-output-names", &clk_name);

	num_parents = of_clk_get_parent_count(node);
	if (num_parents < 1) {
		pr_err("%s: omap dpll %s must have parent(s)\n",
		       __func__, node->name);
		goto cleanup;
	}

	parent_names = kzalloc(sizeof(char *) * num_parents, GFP_KERNEL);

	for (i = 0; i < num_parents; i++)
		parent_names[i] = of_clk_get_parent_name(node, i);

	dd->clk_ref = of_clk_get(node, 0);
	dd->clk_bypass = of_clk_get(node, 1);

	if (IS_ERR(dd->clk_ref)) {
		pr_err("%s: ti,clk-ref for %s not found\n", __func__,
		       clk_name);
		goto cleanup;
	}

	if (IS_ERR(dd->clk_bypass)) {
		pr_err("%s: ti,clk-bypass for %s not found\n", __func__,
		       clk_name);
		goto cleanup;
	}

	of_property_read_string(node, "ti,clkdm-name", &clkdm_name);

	i = of_property_match_string(node, "reg-names", "control");
	if (i >= 0)
		dd->control_reg = of_iomap(node, i);

	i = of_property_match_string(node, "reg-names", "idlest");
	if (i >= 0)
		dd->idlest_reg = of_iomap(node, i);

	i = of_property_match_string(node, "reg-names", "autoidle");
	if (i >= 0)
		dd->autoidle_reg = of_iomap(node, i);

	i = of_property_match_string(node, "reg-names", "mult-div1");
	if (i >= 0)
		dd->mult_div1_reg = of_iomap(node, i);

	if (!of_property_read_u32(node, "ti,modes", &val))
		dd->modes = val;

	clk = omap_clk_register_dpll(NULL, clk_name, parent_names,
				num_parents, dpll_flags, dd,
				clkdm_name, ops);

	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
	return;

cleanup:
	kfree(dd);
	kfree(parent_names);
	return;
}

static void __init of_omap_dpll_x2_setup(struct device_node *node)
{
	struct clk *clk;
	const char *clk_name = node->name;
	void __iomem *reg;
	const char *parent_name;

	of_property_read_string(node, "clock-output-names", &clk_name);

	parent_name = of_clk_get_parent_name(node, 0);

	reg = of_iomap(node, 0);

	clk = omap_clk_register_dpll_x2(NULL, clk_name, parent_name,
				reg, &dpll_x2_ck_ops);

	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
}
CLK_OF_DECLARE(omap_dpll_x2_clock, "ti,omap4-dpll-x2-clock",
	       of_omap_dpll_x2_setup);

static void __init of_omap3_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1,
		.enable_mask = 0x7,
		.autoidle_mask = 0x7,
		.mult_mask = 0x7ff << 8,
		.div1_mask = 0x7f,
		.max_multiplier = 2047,
		.max_divider = 128,
		.min_divider = 1,
		.freqsel_mask = 0xf0,
		.modes = (1 << DPLL_LOW_POWER_BYPASS) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &omap3_dpll_ck_ops, &dd);
}
CLK_OF_DECLARE(omap3_dpll_clock, "ti,omap3-dpll-clock", of_omap3_dpll_setup);

static void __init of_omap3_core_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1,
		.enable_mask = 0x7,
		.autoidle_mask = 0x7,
		.mult_mask = 0x7ff << 16,
		.div1_mask = 0x7f << 8,
		.max_multiplier = 2047,
		.max_divider = 128,
		.min_divider = 1,
		.freqsel_mask = 0xf0,
	};

	of_omap_dpll_setup(node, &omap3_dpll_core_ck_ops, &dd);
}
CLK_OF_DECLARE(omap3_core_dpll_clock, "ti,omap3-dpll-core-clock",
	       of_omap3_core_dpll_setup);

static void __init of_omap3_per_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1 << 1,
		.enable_mask = 0x7 << 16,
		.autoidle_mask = 0x7 << 3,
		.mult_mask = 0x7ff << 8,
		.div1_mask = 0x7f,
		.max_multiplier = 2047,
		.max_divider = 128,
		.min_divider = 1,
		.freqsel_mask = 0xf00000,
		.modes = (1 << DPLL_LOW_POWER_STOP) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &omap3_dpll_per_ck_ops, &dd);
}
CLK_OF_DECLARE(omap3_per_dpll_clock, "ti,omap3-dpll-per-clock",
	       of_omap3_per_dpll_setup);

static void __init of_omap3_per_jtype_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1 << 1,
		.enable_mask = 0x7 << 16,
		.autoidle_mask = 0x7 << 3,
		.mult_mask = 0xfff << 8,
		.div1_mask = 0x7f,
		.max_multiplier = 4095,
		.max_divider = 128,
		.min_divider = 1,
		.sddiv_mask = 0xff << 24,
		.dco_mask = 0xe << 20,
		.flags = DPLL_J_TYPE,
		.modes = (1 << DPLL_LOW_POWER_STOP) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &omap3_dpll_per_ck_ops, &dd);
}
CLK_OF_DECLARE(omap3_per_jtype_dpll_clock, "ti,omap3-dpll-per-j-type-clock",
	       of_omap3_per_jtype_dpll_setup);

static void __init of_omap4_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1,
		.enable_mask = 0x7,
		.autoidle_mask = 0x7,
		.mult_mask = 0x7ff << 8,
		.div1_mask = 0x7f,
		.max_multiplier = 2047,
		.max_divider = 128,
		.min_divider = 1,
		.modes = (1 << DPLL_LOW_POWER_BYPASS) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &dpll_ck_ops, &dd);
}
CLK_OF_DECLARE(omap4_dpll_clock, "ti,omap4-dpll-clock", of_omap4_dpll_setup);

static void __init of_omap4_core_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1,
		.enable_mask = 0x7,
		.autoidle_mask = 0x7,
		.mult_mask = 0x7ff << 8,
		.div1_mask = 0x7f,
		.max_multiplier = 2047,
		.max_divider = 128,
		.min_divider = 1,
		.modes = (1 << DPLL_LOW_POWER_BYPASS) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &dpll_core_ck_ops, &dd);
}
CLK_OF_DECLARE(omap4_core_dpll_clock, "ti,omap4-dpll-core-clock",
	       of_omap4_core_dpll_setup);

static void __init of_omap4_m4xen_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1,
		.enable_mask = 0x7,
		.autoidle_mask = 0x7,
		.mult_mask = 0x7ff << 8,
		.div1_mask = 0x7f,
		.max_multiplier = 2047,
		.max_divider = 128,
		.min_divider = 1,
		.m4xen_mask = 0x800,
		.lpmode_mask = 1 << 10,
		.modes = (1 << DPLL_LOW_POWER_BYPASS) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &dpll_m4xen_ck_ops, &dd);
}
CLK_OF_DECLARE(omap4_m4xen_dpll_clock, "ti,omap4-dpll-m4xen-clock",
	       of_omap4_m4xen_dpll_setup);

static void __init of_omap4_jtype_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1,
		.enable_mask = 0x7,
		.autoidle_mask = 0x7,
		.mult_mask = 0xfff << 8,
		.div1_mask = 0xff,
		.max_multiplier = 4095,
		.max_divider = 256,
		.min_divider = 1,
		.sddiv_mask = 0xff << 24,
		.flags = DPLL_J_TYPE,
		.modes = (1 << DPLL_LOW_POWER_BYPASS) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &dpll_m4xen_ck_ops, &dd);
}
CLK_OF_DECLARE(omap4_jtype_dpll_clock, "ti,omap4-dpll-j-type-clock",
	       of_omap4_jtype_dpll_setup);

static void __init of_omap4_no_gate_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1,
		.enable_mask = 0x7,
		.autoidle_mask = 0x7,
		.mult_mask = 0x7ff << 8,
		.div1_mask = 0x7f,
		.max_multiplier = 2047,
		.max_divider = 128,
		.min_divider = 1,
		.modes = (1 << DPLL_LOW_POWER_BYPASS) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &dpll_no_gate_ck_ops, &dd);
}
CLK_OF_DECLARE(omap4_no_gate_dpll_clock, "ti,omap4-dpll-no-gate-clock",
	       of_omap4_no_gate_dpll_setup);

static void __init of_omap4_no_gate_jtype_dpll_setup(struct device_node *node)
{
	const struct dpll_data dd = {
		.idlest_mask = 0x1,
		.enable_mask = 0x7,
		.autoidle_mask = 0x7,
		.mult_mask = 0x7ff << 8,
		.div1_mask = 0x7f,
		.max_multiplier = 2047,
		.max_divider = 128,
		.min_divider = 1,
		.flags = DPLL_J_TYPE,
		.modes = (1 << DPLL_LOW_POWER_BYPASS) | (1 << DPLL_LOCKED),
	};

	of_omap_dpll_setup(node, &dpll_no_gate_ck_ops, &dd);
}
CLK_OF_DECLARE(omap4_no_gate_jtype_dpll_clock,
	       "ti,omap4-dpll-no-gate-j-type-clock",
	       of_omap4_no_gate_jtype_dpll_setup);
