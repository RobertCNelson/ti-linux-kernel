/*
 * OMAP clockdomain support
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
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk/ti.h>

void __init of_omap_clockdomain_setup(struct device_node *node)
{
	struct clk *clk;
	struct clk_hw *clk_hw;
	const char *clkdm_name = node->name;
	int i;
	int num_clks;

	num_clks = of_count_phandle_with_args(node, "clocks", "#clock-cells");

	for (i = 0; i < num_clks; i++) {
		clk = of_clk_get(node, i);
		if (__clk_get_flags(clk) & CLK_IS_BASIC) {
			pr_warn("%s: can't setup clkdm for basic clk %s\n",
				__func__, __clk_get_name(clk));
			continue;
		}
		clk_hw = __clk_get_hw(clk);
		to_clk_hw_omap(clk_hw)->clkdm_name = clkdm_name;
		omap2_init_clk_clkdm(clk_hw);
	}
}
CLK_OF_DECLARE(omap_clockdomain, "ti,clockdomain", of_omap_clockdomain_setup);
