/*
 * AM43XX Clock init
 *
 * Copyright (C) 2013 Texas Instruments, Inc
 *     Tero Kristo (t-kristo@ti.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/clk-provider.h>
#include <linux/clk/ti.h>


static struct omap_dt_clk am43xx_clks[] = {
	DT_CLK(NULL, "clk_32768_ck", "clk_32768_ck"),
	DT_CLK(NULL, "clk_rc32k_ck", "clk_rc32k_ck"),
	DT_CLK(NULL, "virt_19200000_ck", "virt_19200000_ck"),
	DT_CLK(NULL, "virt_24000000_ck", "virt_24000000_ck"),
	DT_CLK(NULL, "virt_25000000_ck", "virt_25000000_ck"),
	DT_CLK(NULL, "virt_26000000_ck", "virt_26000000_ck"),
	DT_CLK(NULL, "sys_clkin_ck", "sys_clkin_ck"),
	DT_CLK(NULL, "tclkin_ck", "tclkin_ck"),
	DT_CLK(NULL, "dpll_core_ck", "dpll_core_ck"),
	DT_CLK(NULL, "dpll_core_x2_ck", "dpll_core_x2_ck"),
	DT_CLK(NULL, "dpll_core_m4_ck", "dpll_core_m4_ck"),
	DT_CLK(NULL, "dpll_core_m5_ck", "dpll_core_m5_ck"),
	DT_CLK(NULL, "dpll_core_m6_ck", "dpll_core_m6_ck"),
	DT_CLK(NULL, "dpll_mpu_ck", "dpll_mpu_ck"),
	DT_CLK(NULL, "dpll_mpu_m2_ck", "dpll_mpu_m2_ck"),
	DT_CLK(NULL, "dpll_ddr_ck", "dpll_ddr_ck"),
	DT_CLK(NULL, "dpll_ddr_m2_ck", "dpll_ddr_m2_ck"),
	DT_CLK(NULL, "dpll_disp_ck", "dpll_disp_ck"),
	DT_CLK(NULL, "dpll_disp_m2_ck", "dpll_disp_m2_ck"),
	DT_CLK(NULL, "dpll_per_ck", "dpll_per_ck"),
	DT_CLK(NULL, "dpll_per_m2_ck", "dpll_per_m2_ck"),
	DT_CLK(NULL, "dpll_per_m2_div4_wkupdm_ck", "dpll_per_m2_div4_wkupdm_ck"),
	DT_CLK(NULL, "dpll_per_m2_div4_ck", "dpll_per_m2_div4_ck"),
	DT_CLK(NULL, "adc_tsc_fck", "adc_tsc_fck"),
	DT_CLK(NULL, "clkdiv32k_ck", "clkdiv32k_ck"),
	DT_CLK(NULL, "clkdiv32k_ick", "clkdiv32k_ick"),
	DT_CLK(NULL, "dcan0_fck", "dcan0_fck"),
	DT_CLK(NULL, "dcan1_fck", "dcan1_fck"),
	DT_CLK(NULL, "pruss_ocp_gclk", "pruss_ocp_gclk"),
	DT_CLK(NULL, "mcasp0_fck", "mcasp0_fck"),
	DT_CLK(NULL, "mcasp1_fck", "mcasp1_fck"),
	DT_CLK(NULL, "smartreflex0_fck", "smartreflex0_fck"),
	DT_CLK(NULL, "smartreflex1_fck", "smartreflex1_fck"),
	DT_CLK(NULL, "sha0_fck", "sha0_fck"),
	DT_CLK(NULL, "rng_fck", "rng_fck"),
	DT_CLK(NULL, "aes0_fck", "aes0_fck"),
	DT_CLK(NULL, "timer1_fck", "timer1_fck"),
	DT_CLK(NULL, "timer2_fck", "timer2_fck"),
	DT_CLK(NULL, "timer3_fck", "timer3_fck"),
	DT_CLK(NULL, "timer4_fck", "timer4_fck"),
	DT_CLK(NULL, "timer5_fck", "timer5_fck"),
	DT_CLK(NULL, "timer6_fck", "timer6_fck"),
	DT_CLK(NULL, "timer7_fck", "timer7_fck"),
	DT_CLK(NULL, "wdt1_fck", "wdt1_fck"),
	DT_CLK(NULL, "l3_gclk", "l3_gclk"),
	DT_CLK(NULL, "dpll_core_m4_div2_ck", "dpll_core_m4_div2_ck"),
	DT_CLK(NULL, "l4hs_gclk", "l4hs_gclk"),
	DT_CLK(NULL, "l3s_gclk", "l3s_gclk"),
	DT_CLK(NULL, "l4ls_gclk", "l4ls_gclk"),
	DT_CLK(NULL, "clk_24mhz", "clk_24mhz"),
	DT_CLK(NULL, "cpsw_125mhz_gclk", "cpsw_125mhz_gclk"),
	DT_CLK(NULL, "cpsw_cpts_rft_clk", "cpsw_cpts_rft_clk"),
	DT_CLK(NULL, "gpio0_dbclk_mux_ck", "gpio0_dbclk_mux_ck"),
	DT_CLK(NULL, "gpio0_dbclk", "gpio0_dbclk"),
	DT_CLK(NULL, "gpio1_dbclk", "gpio1_dbclk"),
	DT_CLK(NULL, "gpio2_dbclk", "gpio2_dbclk"),
	DT_CLK(NULL, "gpio3_dbclk", "gpio3_dbclk"),
	DT_CLK(NULL, "gpio4_dbclk", "gpio4_dbclk"),
	DT_CLK(NULL, "gpio5_dbclk", "gpio5_dbclk"),
	DT_CLK(NULL, "mmc_clk", "mmc_clk"),
	DT_CLK(NULL, "gfx_fclk_clksel_ck", "gfx_fclk_clksel_ck"),
	DT_CLK(NULL, "gfx_fck_div_ck", "gfx_fck_div_ck"),
	DT_CLK(NULL, "timer_32k_ck", "clkdiv32k_ick"),
	DT_CLK(NULL, "timer_sys_ck", "sys_clkin_ck"),
	DT_CLK(NULL, "sysclk_div", "sysclk_div"),
	DT_CLK(NULL, "disp_clk", "disp_clk"),
	DT_CLK(NULL, "clk_32k_mosc_ck", "clk_32k_mosc_ck"),
	DT_CLK(NULL, "clk_32k_tpm_ck", "clk_32k_tpm_ck"),
	DT_CLK(NULL, "dpll_extdev_ck", "dpll_extdev_ck"),
	DT_CLK(NULL, "dpll_extdev_m2_ck", "dpll_extdev_m2_ck"),
	DT_CLK(NULL, "mux_synctimer32k_ck", "mux_synctimer32k_ck"),
	DT_CLK(NULL, "synctimer_32kclk", "synctimer_32kclk"),
	DT_CLK(NULL, "timer8_fck", "timer8_fck"),
	DT_CLK(NULL, "timer9_fck", "timer9_fck"),
	DT_CLK(NULL, "timer10_fck", "timer10_fck"),
	DT_CLK(NULL, "timer11_fck", "timer11_fck"),
	DT_CLK(NULL, "cpsw_50m_clkdiv", "cpsw_50m_clkdiv"),
	DT_CLK(NULL, "cpsw_5m_clkdiv", "cpsw_5m_clkdiv"),
	DT_CLK(NULL, "dpll_ddr_x2_ck", "dpll_ddr_x2_ck"),
	DT_CLK(NULL, "dpll_ddr_m4_ck", "dpll_ddr_m4_ck"),
	DT_CLK(NULL, "dpll_per_clkdcoldo", "dpll_per_clkdcoldo"),
	DT_CLK(NULL, "dll_aging_clk_div", "dll_aging_clk_div"),
	DT_CLK(NULL, "div_core_25m_ck", "div_core_25m_ck"),
	DT_CLK(NULL, "func_12m_clk", "func_12m_clk"),
	DT_CLK(NULL, "vtp_clk_div", "vtp_clk_div"),
	DT_CLK(NULL, "usbphy_32khz_clkmux", "usbphy_32khz_clkmux"),
	DT_CLK(NULL, "vpfe0_fck", "vpfe0_fck"),
	DT_CLK(NULL, "vpfe1_fck", "vpfe1_fck"),
	DT_CLK(NULL, "clkout2_ck", "clkout2_ck"),
	{ .node_name = NULL },
};

static const char *enable_init_clks[] = {
	/* Required for external peripherals like WL8 etc */
	"clkout2_ck",
};

int __init am43xx_clk_init(void)
{
	struct clk *clk1, *clk2;

	of_clk_init(NULL);

	omap_dt_clocks_register(am43xx_clks);

	omap2_clk_disable_autoidle_all();

	omap2_clk_enable_init_clocks(enable_init_clks,
				     ARRAY_SIZE(enable_init_clks));

	/*
	 * The external 32KHz RTC clock source may not always be available
	 * on board like in the case of ePOS EVM. By default sync timer, which
	 * is used as clock source, feeds of this clock. This is a problem.
	 * Change the parent of sync timer to PER PLL 32KHz clock instead
	 * which is always present. This has a side effect that in low power
	 * modes, sync timer will stop.
	 */
	clk1 = clk_get_sys(NULL, "mux_synctimer32k_ck");
	clk2 = clk_get_sys(NULL, "clkdiv32k_ick");
	clk_set_parent(clk1, clk2);

	/*
	 * The On-Chip 32K RC Osc clock is not an accurate clock-source as per
	 * the design/spec, so as a result, for example, timer which supposed
	 * to get expired @60Sec, but will expire somewhere ~@40Sec, which is
	 * not expected by any use-case, so change WDT1 clock source to PRCM
	 * 32KHz clock.
	 */
	clk1 = clk_get_sys(NULL, "wdt1_fck");
	clk2 = clk_get_sys(NULL, "clkdiv32k_ick");
	clk_set_parent(clk1, clk2);

	return 0;
}
