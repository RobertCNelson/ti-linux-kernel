/*
 * OMAP APLL clock support
 *
 * Copyright (C) 2013 Texas Instruments, Inc.
 *
 * J Keerthy <j-keerthy@ti.com>
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
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/log2.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk/ti.h>
#include <linux/delay.h>

#define APLL_FORCE_LOCK 0x1
#define APLL_AUTO_IDLE	0x2
#define MAX_APLL_WAIT_TRIES		1000000

static int dra7_apll_enable(struct clk_hw *hw)
{
	struct clk_hw_omap *clk = to_clk_hw_omap(hw);
	int r = 0, i = 0;
	struct dpll_data *ad;
	const char *clk_name;
	u8 state = 1;
	u32 v;

	ad = clk->dpll_data;
	if (!ad)
		return -EINVAL;

	clk_name = __clk_get_name(clk->hw.clk);

	state <<= __ffs(ad->idlest_mask);

	/* Check is already locked */
	if ((readl(ad->idlest_reg) & ad->idlest_mask) == state)
		return r;

	v = readl(ad->control_reg);
	v &= ~ad->enable_mask;
	v |= APLL_FORCE_LOCK << __ffs(ad->enable_mask);
	writel(v, ad->control_reg);

	state <<= __ffs(ad->idlest_mask);

	while (((readl(ad->idlest_reg) & ad->idlest_mask) != state) &&
	       i < MAX_APLL_WAIT_TRIES) {
		i++;
		udelay(1);
	}

	if (i == MAX_APLL_WAIT_TRIES) {
		pr_warn("clock: %s failed transition to '%s'\n",
			clk_name, (state) ? "locked" : "bypassed");
	} else {
		pr_debug("clock: %s transition to '%s' in %d loops\n",
			 clk_name, (state) ? "locked" : "bypassed", i);

		r = 0;
	}

	return r;
}

static void dra7_apll_disable(struct clk_hw *hw)
{
	struct clk_hw_omap *clk = to_clk_hw_omap(hw);
	struct dpll_data *ad;
	u8 state = 1;
	u32 v;

	ad = clk->dpll_data;

	state <<= __ffs(ad->idlest_mask);

	v = readl(ad->control_reg);
	v &= ~ad->enable_mask;
	v |= APLL_AUTO_IDLE << __ffs(ad->enable_mask);
	writel(v, ad->control_reg);
}

static u8 dra7_init_apll_parent(struct clk_hw *hw)
{
	return 0;
}

static const struct clk_ops apll_ck_ops = {
	.enable		= &dra7_apll_enable,
	.disable	= &dra7_apll_disable,
	.get_parent	= &dra7_init_apll_parent,
};

static struct clk *omap_clk_register_apll(struct device *dev, const char *name,
		const char **parent_names, int num_parents, unsigned long flags,
		struct dpll_data *dpll_data, const char *clkdm_name,
		const struct clk_ops *ops)
{
	struct clk *clk;
	struct clk_init_data init = { 0 };
	struct clk_hw_omap *clk_hw;

	clk_hw = kzalloc(sizeof(*clk_hw), GFP_KERNEL);
	if (!clk_hw) {
		pr_err("%s: could not allocate clk_hw_omap\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	clk_hw->dpll_data = dpll_data;
	clk_hw->hw.init = &init;

	init.name = name;
	init.ops = ops;
	init.flags = flags;
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	/* register the clock */
	clk = clk_register(dev, &clk_hw->hw);

	return clk;
}

void __init of_dra7_apll_setup(struct device_node *node)
{
	const struct clk_ops *ops;
	struct clk *clk;
	const char *clk_name = node->name;
	int num_parents;
	const char **parent_names = NULL;
	u8 apll_flags = 0;
	struct dpll_data *ad;
	u32 idlest_mask = 0x1;
	u32 autoidle_mask = 0x3;
	int i;

	ops = &apll_ck_ops;
	ad = kzalloc(sizeof(*ad), GFP_KERNEL);
	if (!ad) {
		pr_err("%s: could not allocate dpll_data\n", __func__);
		return;
	}

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

	ad->clk_ref = of_clk_get(node, 0);
	ad->clk_bypass = of_clk_get(node, 1);

	if (IS_ERR(ad->clk_ref)) {
		pr_err("%s: ti,clk-ref for %s not found\n", __func__,
		       clk_name);
		goto cleanup;
	}

	if (IS_ERR(ad->clk_bypass)) {
		pr_err("%s: ti,clk-bypass for %s not found\n", __func__,
		       clk_name);
		goto cleanup;
	}

	i = of_property_match_string(node, "reg-names", "control");
	if (i >= 0)
		ad->control_reg = of_iomap(node, i);

	i = of_property_match_string(node, "reg-names", "idlest");
	if (i >= 0)
		ad->idlest_reg = of_iomap(node, i);

	ad->idlest_mask = idlest_mask;
	ad->enable_mask = autoidle_mask;

	clk = omap_clk_register_apll(NULL, clk_name, parent_names,
				num_parents, apll_flags, ad,
				NULL, ops);

	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
	return;

cleanup:
	kfree(parent_names);
	kfree(ad);
	return;
}
CLK_OF_DECLARE(dra7_apll_clock, "ti,dra7-apll-clock", of_dra7_apll_setup);
