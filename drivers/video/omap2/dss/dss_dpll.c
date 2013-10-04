/*
 * Copyright (C) 2013 Texas Instruments Ltd
 *
 * Copy of the DSI PLL code
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include <video/omapdss.h>

#include "dss.h"
#include "dss_features.h"

#define CLK_CTRL		0x054
#define PLL_CONTROL		0x300
#define PLL_STATUS		0x304
#define PLL_GO			0x308
#define PLL_CONFIGURATION1	0x30C
#define PLL_CONFIGURATION2	0x310
#define PLL_CONFIGURATION3	0x314
#define PLL_SSC_CONFIGURATION1	0x318
#define PLL_SSC_CONFIGURATION2	0x31C

#define CTRL_BASE		0x4a002500
#define DSS_PLL_CONTROL		0x38

#define REG_GET(dpll, idx, start, end) \
	FLD_GET(dpll_read_reg(dpll, idx), start, end)

#define REG_FLD_MOD(dpll, idx, val, start, end) \
	dpll_write_reg(dpll, idx, FLD_MOD(dpll_read_reg(dpll, idx), val, start, end))

static struct {
	struct platform_device *pdev;
	struct regulator *vdda_video_reg;

	void __iomem *base[2], *control_base;
	unsigned scp_refcount[2];
	struct clk *sys_clk[2];
	bool enabled[3];
} dss_dpll;

static inline u32 dpll_read_reg(enum dss_dpll dpll, u16 offset)
{
	return __raw_readl(dss_dpll.base[dpll] + offset);
}

static inline void dpll_write_reg(enum dss_dpll dpll, u16 offset, u32 val)
{
	__raw_writel(val, dss_dpll.base[dpll] + offset);
}

#define CTRL_REG_GET(start, end) \
	FLD_GET(ctrl_read_reg(), start, end)

#define CTRL_REG_FLD_MOD(val, start, end) \
	ctrl_write_reg(FLD_MOD(ctrl_read_reg(), val, start, end))

static inline u32 ctrl_read_reg(void)
{
	return __raw_readl(dss_dpll.control_base + DSS_PLL_CONTROL);
}

static inline void ctrl_write_reg(u32 val)
{
	__raw_writel(val, dss_dpll.control_base + DSS_PLL_CONTROL);
}

static inline int wait_for_bit_change(enum dss_dpll dpll,
		const u16 offset, int bitnum, int value)
{
	unsigned long timeout;
	ktime_t wait;
	int t;

	/* first busyloop to see if the bit changes right away */
	t = 100;
	while (t-- > 0) {
		if (REG_GET(dpll, offset, bitnum, bitnum) == value)
			return value;
	}

	/* then loop for 500ms, sleeping for 1ms in between */
	timeout = jiffies + msecs_to_jiffies(500);
	while (time_before(jiffies, timeout)) {
		if (REG_GET(dpll, offset, bitnum, bitnum) == value)
			return value;

		wait = ns_to_ktime(1000 * 1000);
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_hrtimeout(&wait, HRTIMER_MODE_REL);
	}

	return !value;
}

bool dss_dpll_disabled(enum dss_dpll dpll)
{
	return !dss_dpll.enabled[dpll];
}

unsigned long dpll_get_clkin(enum dss_dpll dpll)
{
	return clk_get_rate(dss_dpll.sys_clk[dpll]);
}

bool dss_dpll_calc(enum dss_dpll dpll, unsigned long clkin,
		unsigned long pll_min, unsigned long pll_max,
		dss_dpll_calc_func func, void *data)
{
	int regn, regn_start, regn_stop;
	int regm, regm_start, regm_stop;
	unsigned long fint, pll;
	const unsigned long pll_hw_max = 1800000000;
	unsigned long fint_hw_min, fint_hw_max, regm_max, regn_max;

	fint_hw_min = dss_feat_get_param_min(FEAT_PARAM_DSIPLL_FINT);
	fint_hw_max = dss_feat_get_param_max(FEAT_PARAM_DSIPLL_FINT);
	regm_max = dss_feat_get_param_max(FEAT_PARAM_DSIPLL_REGM);
	regn_max = dss_feat_get_param_max(FEAT_PARAM_DSIPLL_REGN);

	regn_start = max(DIV_ROUND_UP(clkin, fint_hw_max), 1ul);
	regn_stop = min(clkin / fint_hw_min, regn_max);

	pll_max = pll_max ? pll_max : ULONG_MAX;

	for (regn = regn_start; regn <= regn_stop; ++regn) {
		fint = clkin / regn;

		regm_start = max(DIV_ROUND_UP(DIV_ROUND_UP(pll_min, fint), 2),
				1ul);
		regm_stop = min3(pll_max / fint / 2,
				pll_hw_max / fint / 2,
				regm_max);

		for (regm = regm_start; regm <= regm_stop; ++regm) {
			pll = 2 * regm * fint;

			if (func(regn, regm, fint, pll, data))
				return true;
		}
	}

	return false;
}

bool dss_dpll_hsdiv_calc(enum dss_dpll dpll, unsigned long pll,
		unsigned long out_min, dss_dpll_hsdiv_calc_func func, void *data)
{
	int regm, regm_start, regm_stop;
	unsigned long out_max;
	unsigned long out;
	unsigned long regm_dispc_max;

	regm_dispc_max = dss_feat_get_param_max(FEAT_PARAM_DSIPLL_REGM_DISPC);

	out_min = out_min ? out_min : 1;
	out_max = dss_feat_get_param_max(FEAT_PARAM_DSS_FCK);

	regm_start = max(DIV_ROUND_UP(pll, out_max), 1ul);
	regm_stop = min(pll / out_min, regm_dispc_max);

	for (regm = regm_start; regm <= regm_stop; ++regm) {
		out = pll / regm;

		if (func(regm, out, data))
			return true;
	}

	return false;
}

int dss_dpll_set_clock_div(enum dss_dpll dpll, struct dss_dpll_cinfo *cinfo)
{
	int r = 0;
	u32 l;
	u8 regn_start, regn_end, regm_start, regm_end;
	u8 regm_hsdiv_start, regm_hsdiv_end;

	DSSDBG("DPLL_VIDEO%d clock config starts\n", dpll + 1);

	DSSDBG("DPLL Fint %ld\n", cinfo->fint);

	DSSDBG("clkin rate %ld\n", cinfo->clkin);

	DSSDBG("CLKOUT = 2 * %d / %d * %lu = %lu\n",
			cinfo->regm,
			cinfo->regn,
			cinfo->clkin,
			cinfo->clkout);

	DSSDBG("regm_hsdiv = %d\n", cinfo->regm_hsdiv);

	dss_feat_get_reg_field(FEAT_REG_DSIPLL_REGN, &regn_start, &regn_end);
	dss_feat_get_reg_field(FEAT_REG_DSIPLL_REGM, &regm_start, &regm_end);
	dss_feat_get_reg_field(FEAT_REG_DSIPLL_REGM_DISPC, &regm_hsdiv_start,
			&regm_hsdiv_end);

	/* PLL_AUTOMODE = manual */
	REG_FLD_MOD(dpll, PLL_CONTROL, 0, 0, 0);

	/* CONFIGURATION1 */
	l = dpll_read_reg(dpll, PLL_CONFIGURATION1);
	/* PLL_REGN */
	l = FLD_MOD(l, cinfo->regn - 1, regn_start, regn_end);
	/* PLL_REGM */
	l = FLD_MOD(l, cinfo->regm, regm_start, regm_end);
	/* M4_CLOCK_DIV */
	l = FLD_MOD(l, cinfo->regm_hsdiv > 0 ? cinfo->regm_hsdiv - 1 : 0,
			regm_hsdiv_start, regm_hsdiv_end);
	dpll_write_reg(dpll, PLL_CONFIGURATION1, l);

	/* CONFIGURATION2 */
	l = dpll_read_reg(dpll, PLL_CONFIGURATION2);
	l = FLD_MOD(l, 1, 13, 13);	/* PLL_REFEN */
	l = FLD_MOD(l, 0, 14, 14);	/* DSIPHY_CLKINEN */
	l = FLD_MOD(l, 1, 20, 20);	/* HSDIVBYPASS */
	l = FLD_MOD(l, 3, 22, 21);	/* REF_SYSCLK = sysclk */
	dpll_write_reg(dpll, PLL_CONFIGURATION2, l);

	REG_FLD_MOD(dpll, PLL_GO, 1, 0, 0);	/* PLL_GO */

	if (wait_for_bit_change(dpll, PLL_GO, 0, 0) != 0) {
		DSSERR("dsi pll go bit not going down.\n");
		r = -EIO;
		goto err;
	}

	if (wait_for_bit_change(dpll, PLL_STATUS, 1, 1) != 1) {
		DSSERR("cannot lock PLL\n");
		r = -EIO;
		goto err;
	}

	l = dpll_read_reg(dpll, PLL_CONFIGURATION2);
	l = FLD_MOD(l, 0, 0, 0);	/* PLL_IDLE */
	l = FLD_MOD(l, 0, 5, 5);	/* PLL_PLLLPMODE */
	l = FLD_MOD(l, 0, 6, 6);	/* PLL_LOWCURRSTBY */
	l = FLD_MOD(l, 0, 7, 7);	/* PLL_TIGHTPHASELOCK */
	l = FLD_MOD(l, 0, 8, 8);	/* PLL_DRIFTGUARDEN */
	l = FLD_MOD(l, 0, 10, 9);	/* PLL_LOCKSEL */
	l = FLD_MOD(l, 1, 13, 13);	/* PLL_REFEN */
	l = FLD_MOD(l, 1, 14, 14);	/* PHY_CLKINEN */
	l = FLD_MOD(l, 0, 15, 15);	/* BYPASSEN */
	l = FLD_MOD(l, 1, 16, 16);	/* CLOCK_EN */
	l = FLD_MOD(l, 0, 17, 17);	/* CLOCK_PWDN */
	l = FLD_MOD(l, 1, 18, 18);	/* PROTO_CLOCK_EN */
	l = FLD_MOD(l, 0, 19, 19);	/* PROTO_CLOCK_PWDN */
	l = FLD_MOD(l, 0, 20, 20);	/* HSDIVBYPASS */
	dpll_write_reg(dpll, PLL_CONFIGURATION2, l);

	DSSDBG("PLL config done\n");

err:
	return r;
}

static void dss_dpll_disable_scp_clk(enum dss_dpll dpll)
{
	unsigned *refcount;

	refcount = &dss_dpll.scp_refcount[dpll];

	WARN_ON(*refcount == 0);
	if (--(*refcount) == 0)
		REG_FLD_MOD(dpll, CLK_CTRL, 0, 14, 14); /* CIO_CLK_ICG */
}

static void dss_dpll_enable_scp_clk(enum dss_dpll dpll)
{
	unsigned *refcount;

	refcount = &dss_dpll.scp_refcount[dpll];

	if ((*refcount)++ == 0)
		REG_FLD_MOD(dpll, CLK_CTRL, 1, 14, 14); /* CIO_CLK_ICG */
}

static int dpll_power(enum dss_dpll dpll, int state)
{
	int t = 0;
	/* PLL_PWR_CMD = enable both hsdiv and clkout*/
	REG_FLD_MOD(dpll, CLK_CTRL, state, 31, 30);

	/* PLL_PWR_STATUS: (NOTE: apparently buggy) */
	while (FLD_GET(dpll_read_reg(dpll, CLK_CTRL), 29, 28) != state) {
		if (++t > 1000) {
			DSSERR("Failed to set DPLL power mode to %d\n", state);
			/* return -ENODEV; */
			return 0;
		}
		udelay(1);
	}

	return 0;
}

void dss_dpll_enable_ctrl(enum dss_dpll dpll, bool enable)
{
	u8 bit;

	switch (dpll) {
	case DSS_DPLL_VIDEO1:
		bit = 0;
		break;
	case DSS_DPLL_VIDEO2:
		bit = 1;
		break;
	case DSS_DPLL_HDMI:
		bit = 2;
		break;
	default:
		DSSERR("invalid dpll\n");
		return;
	}

	CTRL_REG_FLD_MOD(!enable, bit, bit);

	dss_dpll.enabled[dpll] = enable;
}

static int dpll_init(enum dss_dpll dpll)
{
	int r;

	clk_prepare_enable(dss_dpll.sys_clk[dpll]);
	dss_dpll_enable_scp_clk(dpll);

	r = regulator_enable(dss_dpll.vdda_video_reg);
	if (r)
		goto err_reg;

	if (wait_for_bit_change(dpll, PLL_STATUS, 0, 1) != 1) {
		DSSERR("PLL not coming out of reset.\n");
		r = -ENODEV;
		goto err_reset;
	}

	r = dpll_power(dpll, 0x2);
	if (r)
		goto err_reset;

	return 0;

err_reset:
	regulator_disable(dss_dpll.vdda_video_reg);
err_reg:
	dss_dpll_disable_scp_clk(dpll);
	clk_disable_unprepare(dss_dpll.sys_clk[dpll]);

	return r;
}

int dss_dpll_activate(enum dss_dpll dpll)
{
	int r;

	/* enable from control module */
	dss_dpll_enable_ctrl(dpll, true);

	r = dpll_init(dpll);

	return r;
}

void dss_dpll_set_control_mux(enum omap_channel channel, enum dss_dpll dpll)
{
	u8 start, end;
	u8 val;

	if (channel == OMAP_DSS_CHANNEL_LCD) {
		start = 4;
		end = 3;

		switch (dpll) {
		case DSS_DPLL_VIDEO1:
			val = 0;
			break;
		default:
			printk("error in mux config\n");
			return;
		}
	} else if (channel == OMAP_DSS_CHANNEL_LCD2) {
		start = 6;
		end = 5;

		switch (dpll) {
		case DSS_DPLL_VIDEO1:
			val = 1;
			break;
		case DSS_DPLL_VIDEO2:
			val = 0;
			break;
		default:
			printk("error in mux config\n");
			return;
		}
	} else {
		start = 7;
		end = 8;

		switch (dpll) {
		case DSS_DPLL_VIDEO1:
			val = 0;
			break;
		case DSS_DPLL_VIDEO2:
			val = 1;
			break;
		default:
			printk("error in mux config\n");
			return;
		}
	}

	CTRL_REG_FLD_MOD(val, start, end);
}

void dss_dpll_disable(enum dss_dpll dpll)
{
	dpll_power(dpll, 0);

	regulator_disable(dss_dpll.vdda_video_reg);

	dss_dpll_disable_scp_clk(dpll);
	clk_disable_unprepare(dss_dpll.sys_clk[dpll]);

	dss_dpll_enable_ctrl(dpll, false);
}

static int dss_dpll_configure_one(struct platform_device *pdev,
		enum dss_dpll dpll)
{
	struct resource *dpll_mem;

	dpll_mem = platform_get_resource(pdev, IORESOURCE_MEM, dpll + 1);
	if (!dpll_mem) {
		DSSERR("can't get IORESOURCE_MEM for DPLL %d\n", dpll);
		return -EINVAL;
	}

	dss_dpll.base[dpll] = devm_ioremap(&pdev->dev, dpll_mem->start,
				resource_size(dpll_mem));
	if (!dss_dpll.base[dpll]) {
		DSSERR("can't ioremap DPLL %d\n", dpll);
		return -ENOMEM;
	}

	dss_dpll.sys_clk[dpll] = devm_clk_get(&pdev->dev,
		dpll == DSS_DPLL_VIDEO1 ? "video1_clk" : "video2_clk");
	if (IS_ERR(dss_dpll.sys_clk[dpll])) {
		DSSERR("can't get sys clock for DPLL_VIDEO%d\n", dpll + 1);
		return PTR_ERR(dss_dpll.sys_clk[dpll]);
	}

	return 0;
}

int dss_dpll_init_regulator(enum dss_dpll dpll)
{
	struct regulator *reg;
	struct platform_device *pdev = dss_dpll.pdev;

	if (dpll == DSS_DPLL_NONE)
		return 0;

	reg = devm_regulator_get(&pdev->dev, "vdda_video");
	if (IS_ERR(reg)) {
		DSSERR("can't get vdda_video regulator\n");
		return PTR_ERR(reg);
	}

	dss_dpll.vdda_video_reg = reg;

	return 0;
}

int dss_dpll_configure(struct platform_device *pdev)
{
	int r;

	r = dss_dpll_configure_one(pdev, DSS_DPLL_VIDEO1);
	if (r)
		return r;

	r = dss_dpll_configure_one(pdev, DSS_DPLL_VIDEO2);
	if (r)
		return r;

	dss_dpll.pdev = pdev;

	return 0;
}

int dss_dpll_configure_ctrl(void)
{
	dss_dpll.control_base = ioremap(CTRL_BASE, SZ_1K);

	if (!dss_dpll.control_base) {
		DSSERR("can't ioremap control base\n");
		return -ENOMEM;
	}

	return 0;
}

void dss_dpll_unconfigure_ctrl(void) {
	if (dss_dpll.control_base)
		iounmap(dss_dpll.control_base);
}
