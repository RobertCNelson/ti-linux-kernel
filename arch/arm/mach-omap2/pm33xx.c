/*
 * AM33XX Power Management Routines
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
 * Vaibhav Bedia <vaibhav.bedia@ti.com>
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/ti_emif.h>
#include <linux/omap-mailbox.h>

#include <asm/suspend.h>
#include <asm/proc-fns.h>
#include <asm/sizes.h>
#include <asm/fncpy.h>
#include <asm/system_misc.h>

#include "pm.h"
#include "cm33xx.h"
#include "pm33xx.h"
#include "common.h"
#include "clockdomain.h"
#include "powerdomain.h"
#include "soc.h"
#include "sram.h"

static void __iomem *am33xx_emif_base;
static struct powerdomain *cefuse_pwrdm, *gfx_pwrdm, *per_pwrdm, *mpu_pwrdm;
static struct clockdomain *gfx_l4ls_clkdm;
static struct clockdomain *l3s_clkdm, *l4fw_clkdm, *clk_24mhz_clkdm;

static struct am33xx_pm_context *am33xx_pm;

static DECLARE_COMPLETION(am33xx_pm_sync);

static void (*am33xx_do_wfi_sram)(struct am33xx_suspend_params *);

static struct am33xx_suspend_params susp_params;

#ifdef CONFIG_SUSPEND

static int am33xx_do_sram_idle(long unsigned int unused)
{
	am33xx_do_wfi_sram(&susp_params);
	return 0;
}

static int am33xx_pm_suspend(unsigned int state)
{
	int i, ret = 0;
	int status = 0;
	struct wkup_m3_wakeup_src wakeup_src;

	if (state == PM_SUSPEND_STANDBY) {
		clkdm_wakeup(l3s_clkdm);
		clkdm_wakeup(l4fw_clkdm);
		clkdm_wakeup(clk_24mhz_clkdm);
	}

	/* Try to put GFX to sleep */
	omap_set_pwrdm_state(gfx_pwrdm, PWRDM_POWER_OFF);

	ret = cpu_suspend(0, am33xx_do_sram_idle);

	status = pwrdm_read_pwrst(gfx_pwrdm);
	if (status != PWRDM_POWER_OFF)
		pr_err("PM: GFX domain did not transition\n");

	/*
	 * BUG: GFX_L4LS clock domain needs to be woken up to
	 * ensure thet L4LS clock domain does not get stuck in transition
	 * If that happens L3 module does not get disabled, thereby leading
	 * to PER power domain transition failing
	 */
	clkdm_wakeup(gfx_l4ls_clkdm);
	clkdm_sleep(gfx_l4ls_clkdm);

	if (ret) {
		pr_err("PM: Kernel suspend failure\n");
	} else {
		i = wkup_m3_pm_status();
		switch (i) {
		case 0:
			pr_info("PM: Successfully put all powerdomains to target state\n");

			/*
			 * The PRCM registers on AM335x do not contain
			 * previous state information like those present on
			 * OMAP4 so we must manually indicate transition so
			 * state counters are properly incremented
			 */
			pwrdm_post_transition(mpu_pwrdm);
			pwrdm_post_transition(per_pwrdm);
			break;
		case 1:
			pr_err("PM: Could not transition all powerdomains to target state\n");
			ret = -1;
			break;
		default:
			pr_err("PM: CM3 returned unknown result = %d\n", i);
			ret = -1;
		}
		/* print the wakeup reason */
		wakeup_src = wkup_m3_wake_src();

		pr_info("PM: Wakeup source %s\n", wakeup_src.src);
	}

	return ret;
}

static int am33xx_pm_enter(suspend_state_t suspend_state)
{
	int ret = 0;

	switch (suspend_state) {
	case PM_SUSPEND_STANDBY:
	case PM_SUSPEND_MEM:
		ret = am33xx_pm_suspend(suspend_state);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}


static void am33xx_m3_state_machine_reset(void)
{
	int i;

	am33xx_pm->ipc.reg1 = IPC_CMD_RESET;

	wkup_m3_pm_set_cmd(&am33xx_pm->ipc);

	am33xx_pm->state = M3_STATE_MSG_FOR_RESET;

	if (!wkup_m3_ping()) {
		i = wait_for_completion_timeout(&am33xx_pm_sync,
					msecs_to_jiffies(500));
		if (!i) {
			WARN(1, "PM: MPU<->CM3 sync failure\n");
			am33xx_pm->state = M3_STATE_UNKNOWN;
		}
	} else {
		pr_warn("PM: Unable to ping CM3\n");
	}
}

static int am33xx_pm_begin(suspend_state_t state)
{
	int i;

	cpu_idle_poll_ctrl(true);

	switch (state) {
	case PM_SUSPEND_MEM:
		am33xx_pm->ipc.reg1	= IPC_CMD_DS0;
		break;
	case PM_SUSPEND_STANDBY:
		am33xx_pm->ipc.reg1	= IPC_CMD_STANDBY;
		break;
	}

	am33xx_pm->ipc.reg2		= DS_IPC_DEFAULT;
	am33xx_pm->ipc.reg3		= DS_IPC_DEFAULT;

	wkup_m3_pm_set_cmd(&am33xx_pm->ipc);

	am33xx_pm->state = M3_STATE_MSG_FOR_LP;

	if (!wkup_m3_ping()) {
		i = wait_for_completion_timeout(&am33xx_pm_sync,
					msecs_to_jiffies(500));
		if (!i) {
			WARN(1, "PM: MPU<->CM3 sync failure\n");
			return -1;
		}
	} else {
		pr_warn("PM: Unable to ping CM3\n");
		return -1;
	}

	return 0;
}

static void am33xx_pm_end(void)
{
	am33xx_m3_state_machine_reset();

	cpu_idle_poll_ctrl(false);

	return;
}

static int am33xx_pm_valid(suspend_state_t state)
{
	switch (state) {
	case PM_SUSPEND_STANDBY:
	case PM_SUSPEND_MEM:
		return 1;
	default:
		return 0;
	}
}

static const struct platform_suspend_ops am33xx_pm_ops = {
	.begin		= am33xx_pm_begin,
	.end		= am33xx_pm_end,
	.enter		= am33xx_pm_enter,
	.valid		= am33xx_pm_valid,
};
#endif /* CONFIG_SUSPEND */

static void am33xx_txev_handler(void)
{
	switch (am33xx_pm->state) {
	case M3_STATE_RESET:
		am33xx_pm->state = M3_STATE_INITED;
		complete(&am33xx_pm_sync);
		break;
	case M3_STATE_MSG_FOR_RESET:
		am33xx_pm->state = M3_STATE_INITED;
		complete(&am33xx_pm_sync);
		break;
	case M3_STATE_MSG_FOR_LP:
		complete(&am33xx_pm_sync);
		break;
	case M3_STATE_UNKNOWN:
		pr_warn("PM: Unknown CM3 State\n");
	}

	return;
}

static void am33xx_m3_fw_ready_cb(void)
{
	int ret = 0;

	ret = wkup_m3_prepare();
	if (ret) {
		pr_err("PM: Could not prepare WKUP_M3\n");
		return;
	}

	ret = wait_for_completion_timeout(&am33xx_pm_sync,
					msecs_to_jiffies(500));

	if (WARN(ret == 0, "PM: MPU<->CM3 sync failure\n"))
		return;

	am33xx_pm->ver = wkup_m3_fw_version_read();

	if (am33xx_pm->ver == M3_VERSION_UNKNOWN ||
		am33xx_pm->ver < M3_BASELINE_VERSION) {
		pr_warn("PM: CM3 Firmware Version %x not supported\n",
					am33xx_pm->ver);
		return;
	} else {
		pr_info("PM: CM3 Firmware Version = 0x%x\n",
					am33xx_pm->ver);
	}

#ifdef CONFIG_SUSPEND
	suspend_set_ops(&am33xx_pm_ops);
#endif /* CONFIG_SUSPEND */
}

static struct wkup_m3_ops am33xx_wkup_m3_ops = {
	.txev_handler = am33xx_txev_handler,
	.firmware_loaded = am33xx_m3_fw_ready_cb,
};

/*
 * Push the minimal suspend-resume code to SRAM
 */
void am33xx_push_sram_idle(void)
{
	am33xx_do_wfi_sram = (void *)omap_sram_push
					(am33xx_do_wfi, am33xx_do_wfi_sz);
}

static int __init am33xx_map_emif(void)
{
	am33xx_emif_base = ioremap(AM33XX_EMIF_BASE, SZ_32K);

	if (!am33xx_emif_base)
		return -ENOMEM;

	return 0;
}

int __init am33xx_pm_init(void)
{
	int ret;
	u32 temp;

	if (!soc_is_am33xx())
		return -ENODEV;

	gfx_pwrdm = pwrdm_lookup("gfx_pwrdm");
	per_pwrdm = pwrdm_lookup("per_pwrdm");
	mpu_pwrdm = pwrdm_lookup("mpu_pwrdm");

	gfx_l4ls_clkdm = clkdm_lookup("gfx_l4ls_gfx_clkdm");
	l3s_clkdm = clkdm_lookup("l3s_clkdm");
	l4fw_clkdm = clkdm_lookup("l4fw_clkdm");
	clk_24mhz_clkdm = clkdm_lookup("clk_24mhz_clkdm");

	if ((!gfx_pwrdm) || (!per_pwrdm) || (!mpu_pwrdm) || (!gfx_l4ls_clkdm) ||
	    (!l3s_clkdm) || (!l4fw_clkdm) || (!clk_24mhz_clkdm)) {
		ret = -ENODEV;
		goto err;
	}

	am33xx_pm = kzalloc(sizeof(*am33xx_pm), GFP_KERNEL);
	if (!am33xx_pm) {
		pr_err("Memory allocation failed\n");
		ret = -ENOMEM;
		return ret;
	}

	ret = am33xx_map_emif();
	if (ret) {
		pr_err("PM: Could not ioremap EMIF\n");
		goto err;
	}

	/* Determine Memory Type */
	temp = readl(am33xx_emif_base + EMIF_SDRAM_CONFIG);
	temp = (temp & SDRAM_TYPE_MASK) >> SDRAM_TYPE_SHIFT;
	/* Parameters to pass to aseembly code */
	susp_params.emif_addr_virt = am33xx_emif_base;
	susp_params.dram_sync = am33xx_dram_sync;
	susp_params.mem_type = temp;
	am33xx_pm->ipc.reg4 = temp;

	(void) clkdm_for_each(omap_pm_clkdms_setup, NULL);

	/* CEFUSE domain can be turned off post bootup */
	cefuse_pwrdm = pwrdm_lookup("cefuse_pwrdm");
	if (cefuse_pwrdm)
		omap_set_pwrdm_state(cefuse_pwrdm, PWRDM_POWER_OFF);
	else
		pr_err("PM: Failed to get cefuse_pwrdm\n");

	am33xx_pm->state = M3_STATE_RESET;

	wkup_m3_set_ops(&am33xx_wkup_m3_ops);

	/* m3 may have already loaded but ops were not set yet,
	 * manually invoke */

	if (wkup_m3_is_valid())
		am33xx_m3_fw_ready_cb();

	/* Physical resume address to be used by ROM code */
	am33xx_pm->ipc.reg0 = (AM33XX_OCMC_END -
		am33xx_do_wfi_sz + am33xx_resume_offset + 0x4);

	return 0;

err:
	kfree(am33xx_pm);
	return ret;
}
