/*
 * TI clock drivers support
 *
 * Copyright (C) 2013 Texas Instruments, Inc.
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
#ifndef __LINUX_CLK_TI_H__
#define __LINUX_CLK_TI_H__

#include <linux/clkdev.h>

/**
 * struct dpll_data - DPLL registers and integration data
 * @mult_div1_reg: register containing the DPLL M and N bitfields
 * @mult_mask: mask of the DPLL M bitfield in @mult_div1_reg
 * @div1_mask: mask of the DPLL N bitfield in @mult_div1_reg
 * @clk_bypass: struct clk pointer to the clock's bypass clock input
 * @clk_ref: struct clk pointer to the clock's reference clock input
 * @control_reg: register containing the DPLL mode bitfield
 * @enable_mask: mask of the DPLL mode bitfield in @control_reg
 * @last_rounded_rate: cache of the last rate result of omap2_dpll_round_rate()
 * @last_rounded_m: cache of the last M result of omap2_dpll_round_rate()
 * @last_rounded_m4xen: cache of the last M4X result of
 *                     omap4_dpll_regm4xen_round_rate()
 * @last_rounded_lpmode: cache of the last lpmode result of
 *                      omap4_dpll_lpmode_recalc()
 * @max_multiplier: maximum valid non-bypass multiplier value (actual)
 * @last_rounded_n: cache of the last N result of omap2_dpll_round_rate()
 * @min_divider: minimum valid non-bypass divider value (actual)
 * @max_divider: maximum valid non-bypass divider value (actual)
 * @modes: possible values of @enable_mask
 * @autoidle_reg: register containing the DPLL autoidle mode bitfield
 * @idlest_reg: register containing the DPLL idle status bitfield
 * @autoidle_mask: mask of the DPLL autoidle mode bitfield in @autoidle_reg
 * @freqsel_mask: mask of the DPLL jitter correction bitfield in @control_reg
 * @idlest_mask: mask of the DPLL idle status bitfield in @idlest_reg
 * @lpmode_mask: mask of the DPLL low-power mode bitfield in @control_reg
 * @m4xen_mask: mask of the DPLL M4X multiplier bitfield in @control_reg
 * @auto_recal_bit: bitshift of the driftguard enable bit in @control_reg
 * @recal_en_bit: bitshift of the PRM_IRQENABLE_* bit for recalibration IRQs
 * @recal_st_bit: bitshift of the PRM_IRQSTATUS_* bit for recalibration IRQs
 * @flags: DPLL type/features (see below)
 *
 * Possible values for @flags:
 * DPLL_J_TYPE: "J-type DPLL" (only some 36xx, 4xxx DPLLs)
 *
 * @freqsel_mask is only used on the OMAP34xx family and AM35xx.
 *
 * XXX Some DPLLs have multiple bypass inputs, so it's not technically
 * correct to only have one @clk_bypass pointer.
 *
 * XXX The runtime-variable fields (@last_rounded_rate, @last_rounded_m,
 * @last_rounded_n) should be separated from the runtime-fixed fields
 * and placed into a different structure, so that the runtime-fixed data
 * can be placed into read-only space.
 */
struct dpll_data {
	void __iomem		*mult_div1_reg;
	u32			mult_mask;
	u32			div1_mask;
	struct clk		*clk_bypass;
	struct clk		*clk_ref;
	void __iomem		*control_reg;
	u32			enable_mask;
	unsigned long		last_rounded_rate;
	u16			last_rounded_m;
	u8			last_rounded_m4xen;
	u8			last_rounded_lpmode;
	u16			max_multiplier;
	u8			last_rounded_n;
	u8			min_divider;
	u16			max_divider;
	u8			modes;
	void __iomem		*autoidle_reg;
	void __iomem		*idlest_reg;
	u32			autoidle_mask;
	u32			freqsel_mask;
	u32			idlest_mask;
	u32			dco_mask;
	u32			sddiv_mask;
	u32			lpmode_mask;
	u32			m4xen_mask;
	u8			auto_recal_bit;
	u8			recal_en_bit;
	u8			recal_st_bit;
	u8			flags;
};

struct clk_hw_omap_ops;

/**
 * struct clk_hw_omap - OMAP struct clk
 * @node: list_head connecting this clock into the full clock list
 * @enable_reg: register to write to enable the clock (see @enable_bit)
 * @enable_bit: bitshift to write to enable/disable the clock (see @enable_reg)
 * @flags: see "struct clk.flags possibilities" above
 * @clksel_reg: for clksel clks, register va containing src/divisor select
 * @clksel_mask: bitmask in @clksel_reg for the src/divisor selector
 * @clksel: for clksel clks, pointer to struct clksel for this clock
 * @dpll_data: for DPLLs, pointer to struct dpll_data for this clock
 * @clkdm_name: clockdomain name that this clock is contained in
 * @clkdm: pointer to struct clockdomain, resolved from @clkdm_name at runtime
 * @rate_offset: bitshift for rate selection bitfield (OMAP1 only)
 * @src_offset: bitshift for source selection bitfield (OMAP1 only)
 *
 * XXX @rate_offset, @src_offset should probably be removed and OMAP1
 * clock code converted to use clksel.
 *
 */
struct clk_hw_omap {
	struct clk_hw		hw;
	struct list_head	node;
	unsigned long		fixed_rate;
	u8			fixed_div;
	void __iomem		*enable_reg;
	u8			enable_bit;
	u8			flags;
	void __iomem		*clksel_reg;
	u32			clksel_mask;
	const struct clksel	*clksel;
	struct dpll_data	*dpll_data;
	const char		*clkdm_name;
	struct clockdomain	*clkdm;
	const struct clk_hw_omap_ops	*ops;
};

/* CM_CLKEN_PLL*.EN* bit values - not all are available for every DPLL */
#define DPLL_LOW_POWER_STOP	0x1
#define DPLL_LOW_POWER_BYPASS	0x5
#define DPLL_LOCKED		0x7

/* DPLL Type and DCO Selection Flags */
#define DPLL_J_TYPE		0x1

/**
 * struct omap_dt_clk - OMAP DT clock alias declarations
 * @lk: clock lookup definition
 * @node_name: clock DT node to map to
 */
struct omap_dt_clk {
	struct clk_lookup		lk;
	char				*node_name;
};

#define DT_CLK(dev, con, name)		\
	{				\
		.lk = {			\
			.dev_id = dev,	\
			.con_id = con,	\
		},			\
		.node_name = name,	\
	}

void omap2_init_clk_hw_omap_clocks(struct clk *clk);
int omap3_noncore_dpll_enable(struct clk_hw *hw);
void omap3_noncore_dpll_disable(struct clk_hw *hw);
int omap3_noncore_dpll_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate);
unsigned long omap4_dpll_regm4xen_recalc(struct clk_hw *hw,
					 unsigned long parent_rate);
long omap4_dpll_regm4xen_round_rate(struct clk_hw *hw,
				    unsigned long target_rate,
				    unsigned long *parent_rate);
u8 omap2_init_dpll_parent(struct clk_hw *hw);
unsigned long omap3_dpll_recalc(struct clk_hw *hw, unsigned long parent_rate);
long omap2_dpll_round_rate(struct clk_hw *hw, unsigned long target_rate,
			   unsigned long *parent_rate);
void omap2_init_clk_clkdm(struct clk_hw *clk);
unsigned long omap3_clkoutx2_recalc(struct clk_hw *hw,
				    unsigned long parent_rate);
int omap3_dpll4_set_rate(struct clk_hw *clk, unsigned long rate,
			 unsigned long parent_rate);

void omap_dt_clocks_register(struct omap_dt_clk *oclks);
#ifdef CONFIG_OF
void of_omap_clk_allow_autoidle_all(void);
void of_omap_clk_deny_autoidle_all(void);
#else
static inline void of_omap_clk_allow_autoidle_all(void) { }
static inline void of_omap_clk_deny_autoidle_all(void) { }
#endif

extern const struct clk_hw_omap_ops clkhwops_omap3_dpll;
extern const struct clk_hw_omap_ops clkhwops_omap4_dpllmx;

#endif
