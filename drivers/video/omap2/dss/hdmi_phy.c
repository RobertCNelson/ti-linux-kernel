/*
 * HDMI PHY
 *
 * Copyright (C) 2013 Texas Instruments Incorporated
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <video/omapdss.h>

#include "dss.h"
#include "hdmi.h"

struct hdmi_phy_features {
	bool bist_ctrl;
	bool calc_freqout;
	bool ldo_voltage;
	unsigned long dcofreq_min;	/* in KHz */
	unsigned long max_phy;		/* in KHz */
};

static const struct hdmi_phy_features *phy_feat;

void hdmi_phy_dump(struct hdmi_phy_data *phy, struct seq_file *s)
{
#define DUMPPHY(r) seq_printf(s, "%-35s %08x\n", #r,\
		hdmi_read_reg(phy->base, r))

	DUMPPHY(HDMI_TXPHY_TX_CTRL);
	DUMPPHY(HDMI_TXPHY_DIGITAL_CTRL);
	DUMPPHY(HDMI_TXPHY_POWER_CTRL);
	DUMPPHY(HDMI_TXPHY_PAD_CFG_CTRL);
	if (phy_feat->bist_ctrl)
		DUMPPHY(HDMI_TXPHY_BIST_CONTROL);
}

int hdmi_phy_configure(struct hdmi_phy_data *phy, struct hdmi_config *cfg)
{
	u8 freqout;

	/*
	 * Read address 0 in order to get the SCP reset done completed
	 * Dummy access performed to make sure reset is done
	 */
	hdmi_read_reg(phy->base, HDMI_TXPHY_TX_CTRL);

	/*
	 * In OMAP5+, the HFBITCLK must be divided by 2 before issuing the
	 * HDMI_PHYPWRCMD_LDOON command.
	*/
	if (phy_feat->bist_ctrl)
		REG_FLD_MOD(phy->base, HDMI_TXPHY_BIST_CONTROL, 1, 11, 11);

	if (phy_feat->calc_freqout) {
		/* DCOCLK/10 is pixel clock, compare pclk with DCOCLK_MIN/10 */
		u32 dco_min = phy_feat->dcofreq_min / 10;
		u32 pclk = cfg->timings.pixel_clock;

		if (pclk < dco_min) {
			freqout = 0;
		} else if ((pclk >= dco_min) && (pclk < phy_feat->max_phy)) {
			freqout = 1;
		} else {
			freqout = 2;
		}
	} else {
		freqout = 1;
	}

	/*
	 * Write to phy address 0 to configure the clock
	 * use HFBITCLK write HDMI_TXPHY_TX_CONTROL_FREQOUT field
	 */
	REG_FLD_MOD(phy->base, HDMI_TXPHY_TX_CTRL, freqout, 31, 30);

	/* Write to phy address 1 to start HDMI line (TXVALID and TMDSCLKEN) */
	hdmi_write_reg(phy->base, HDMI_TXPHY_DIGITAL_CTRL, 0xF0000000);

	/* Setup max LDO voltage */
	if (phy_feat->ldo_voltage)
		REG_FLD_MOD(phy->base, HDMI_TXPHY_POWER_CTRL, 0xB, 3, 0);

	/* Write to phy address 3 to change the polarity control */
	REG_FLD_MOD(phy->base, HDMI_TXPHY_PAD_CFG_CTRL, 0x1, 27, 27);

	return 0;
}

static const struct hdmi_phy_features omap44xx_phy_feats = {
	.bist_ctrl	=	false,
	.calc_freqout	=	false,
	.ldo_voltage	=	true,
	.dcofreq_min	=	500000,
	.max_phy	=	185675,
};

static const struct hdmi_phy_features omap54xx_phy_feats = {
	.bist_ctrl	=	true,
	.calc_freqout	=	true,
	.ldo_voltage	=	false,
	.dcofreq_min	=	750000,
	.max_phy	=	186000,
};

static int hdmi_phy_init_features(struct platform_device *pdev)
{
	struct hdmi_phy_features *dst;
	const struct hdmi_phy_features *src;

	dst = devm_kzalloc(&pdev->dev, sizeof(*dst), GFP_KERNEL);
	if (!dst) {
		dev_err(&pdev->dev, "Failed to allocate HDMI PHY Features\n");
		return -ENOMEM;
	}

	switch (omapdss_get_version()) {
	case OMAPDSS_VER_OMAP4430_ES1:
	case OMAPDSS_VER_OMAP4430_ES2:
	case OMAPDSS_VER_OMAP4:
		src = &omap44xx_phy_feats;
		break;

	case OMAPDSS_VER_OMAP5:
	case OMAPDSS_VER_DRA7xx:
		src = &omap54xx_phy_feats;
		break;

	default:
		return -ENODEV;
	}

	memcpy(dst, src, sizeof(*dst));
	phy_feat = dst;

	return 0;
}

int hdmi_phy_init(struct platform_device *pdev, struct hdmi_phy_data *phy)
{
	int r;
	struct resource *res;

	r = hdmi_phy_init_features(pdev);
	if (r)
		return r;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "hdmi_txphy");
	if (!res) {
		DSSERR("can't get PLL CTRL IORESOURCE_MEM HDMI\n");
		return -EINVAL;
	}

	phy->base = devm_request_and_ioremap(&pdev->dev, res);
	if (!phy->base) {
		DSSERR("can't ioremap PLL ctrl\n");
		return -ENOMEM;
	}

	return 0;
}
