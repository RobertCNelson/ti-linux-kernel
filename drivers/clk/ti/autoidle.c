/*
 * OMAP clock autoidle support
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

struct clk_omap_autoidle {
	void __iomem		*reg;
	u8			shift;
	u8			flags;
	const char		*name;
	struct list_head	node;
};

#define AUTOIDLE_LOW		0x1

static LIST_HEAD(autoidle_clks);

static void omap_allow_autoidle(struct clk_omap_autoidle *clk)
{
	u32 val;

	val = readl(clk->reg);

	if (clk->flags & AUTOIDLE_LOW)
		val &= ~(1 << clk->shift);
	else
		val |= (1 << clk->shift);

	writel(val, clk->reg);
}

static void omap_deny_autoidle(struct clk_omap_autoidle *clk)
{
	u32 val;

	val = readl(clk->reg);

	if (clk->flags & AUTOIDLE_LOW)
		val |= (1 << clk->shift);
	else
		val &= ~(1 << clk->shift);

	writel(val, clk->reg);
}

void of_omap_clk_allow_autoidle_all(void)
{
	struct clk_omap_autoidle *c;

	list_for_each_entry(c, &autoidle_clks, node)
		omap_allow_autoidle(c);
}

void of_omap_clk_deny_autoidle_all(void)
{
	struct clk_omap_autoidle *c;

	list_for_each_entry(c, &autoidle_clks, node)
		omap_deny_autoidle(c);
}

static void __init of_omap_autoidle_setup(struct device_node *node)
{
	u32 shift;
	void __iomem *reg;
	struct clk_omap_autoidle *clk;

	if (of_property_read_u32(node, "ti,autoidle-shift", &shift))
		return;

	reg = of_iomap(node, 0);

	clk = kzalloc(sizeof(*clk), GFP_KERNEL);

	if (!clk) {
		pr_err("%s: kzalloc failed\n", __func__);
		return;
	}

	clk->shift = shift;
	clk->name = node->name;
	clk->reg = reg;

	if (of_property_read_bool(node, "ti,autoidle-low"))
		clk->flags |= AUTOIDLE_LOW;

	list_add(&clk->node, &autoidle_clks);
}

static void __init of_omap_divider_setup(struct device_node *node)
{
	of_divider_clk_setup(node);
	of_omap_autoidle_setup(node);
}
CLK_OF_DECLARE(omap_divider_clock, "ti,divider-clock", of_omap_divider_setup);

static void __init of_omap_fixed_factor_setup(struct device_node *node)
{
	of_fixed_factor_clk_setup(node);
	of_omap_autoidle_setup(node);
}
CLK_OF_DECLARE(omap_fixed_factor_clock, "ti,fixed-factor-clock",
	       of_omap_fixed_factor_setup);
