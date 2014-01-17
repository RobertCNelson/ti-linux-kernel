/*
 * OMAP L3 Interconnect  error handling driver header
 *
 * Copyright (C) 2011 Texas Corporation
 *	Santosh Shilimkar <santosh.shilimkar@ti.com>
 *	sricharan <r.sricharan@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */
#ifndef OMAP_L3_NOC_H
#define OMAP_L3_NOC_H

#define AM4372_L3_MODULES		2
#define OMAP_L3_MODULES			3
#define MAX_L3_MODULES			OMAP_L3_MODULES
#define CLEAR_STDERR_LOG		(1 << 31)
#define CUSTOM_ERROR			0x2
#define STANDARD_ERROR			0x0
#define INBAND_ERROR			0x0
#define L3_APPLICATION_ERROR		0x0
#define L3_DEBUG_ERROR			0x1

/* L3 TARG register offsets */
#define L3_TARG_STDERRLOG_MAIN		0x48
#define L3_TARG_STDERRLOG_SLVOFSLSB	0x5c
#define L3_TARG_STDERRLOG_MSTADDR	0x68
#define L3_FLAGMUX_REGERR0		0xc
#define L3_FLAGMUX_MASK0		0x8

#define L3_FLAGMUX_TARGET_OFS_INVALID	0xdeadbeef
#define L3_FLAGMUX_TARGET_OFS_TIMEOUT	L3_FLAGMUX_TARGET_OFS_INVALID

#define OMAP_NUM_OF_L3_MASTERS	(sizeof(omap_l3_masters)/sizeof(l3_masters[0]))
#define MAX_TARGETS_IN_CLKDM		21

static u32 omap_l3_flagmux[] = {
	0x500,
	0x1000,
	0X0200
};

static u32 am4372_l3_flagmux[] = {
	0x1000,
	0x600,
};

/* L3 Target standard Error register offsets */
static u32 omap_l3_targ_inst_clk1[] = {
	0x100, /* DMM1 */
	0x200, /* DMM2 */
	0x300, /* ABE */
	0x400, /* L4CFG */
	0x600,  /* CLK2 PWR DISC */
	0x0,	/* Host CLK1 */
	0x900	/* L4 Wakeup */
};

static u32 omap_l3_targ_inst_clk2[] = {
	0x500, /* CORTEX M3 */
	0x300, /* DSS */
	0x100, /* GPMC */
	0x400, /* ISS */
	0x700, /* IVAHD */
	0xD00, /* missing in TRM  corresponds to AES1*/
	0x900, /* L4 PER0*/
	0x200, /* OCMRAM */
	0x100, /* missing in TRM corresponds to GPMC sERROR*/
	0x600, /* SGX */
	0x800, /* SL2 */
	0x1600, /* C2C */
	0x1100,	/* missing in TRM corresponds PWR DISC CLK1*/
	0xF00, /* missing in TRM corrsponds to SHA1*/
	0xE00, /* missing in TRM corresponds to AES2*/
	0xC00, /* L4 PER3 */
	0xA00, /* L4 PER1*/
	0xB00, /* L4 PER2*/
	0x0, /* HOST CLK2 */
	0x1800, /* CAL */
	0x1700 /* LLI */
};

static u32 omap_l3_targ_inst_clk3[] = {
	0x0100	/* EMUSS */,
	0x0300, /* DEBUGSS_CT_TBR */
	0x0 /* HOST CLK3 */
};

static u32 am4372_l3_targ_inst_200f[] = {
	0xF00, /* EMIF */
	0x1200, /* DES */
	0x400, /* OCMCRAM */
	0x700, /* TPTC0 */
	0x800, /* TPTC1 */
	0x900, /* TPTC2 */
	0xB00, /* TPCC */
	0xD00, /* DEBUGSS */
	L3_FLAGMUX_TARGET_OFS_TIMEOUT, /* TIMEOUT */
	0x200, /* SHA */
	0xC00, /* SGX530 */
	0x500, /* AES0 */
	0xA00, /* L4_FAST */
	0x300, /* MPUSS L2 RAM */
	0x100, /* ICSS */
};

static u32 am4372_l3_targ_inst_100s[] = {
	0x100, /* L4_PER 0 */
	0x200, /* L4_PER 1 */
	0x300, /* L4_PER 2 */
	0x400, /* L4_PER 3 */
	0x800, /* McASP 0 */
	0x900, /* McASP 1 */
	0xC00, /* MMCHS2 */
	0x700, /* GPMC */
	0xD00, /* L4_FW */
	L3_FLAGMUX_TARGET_OFS_TIMEOUT, /* TIMEOUT */
	0x500, /* ADCTSC */
	0xE00, /* L4_WKUP */
	0xA00, /* MAG_CARD */
};

struct l3_masters_data {
	u32 id;
	char name[20];
};

static struct l3_masters_data omap_l3_masters[] = {
	{ 0x0 , "MPU"},
	{ 0x10, "CS_ADP"},
	{ 0x14, "xxx"},
	{ 0x20, "DSP"},
	{ 0x30, "IVAHD"},
	{ 0x40, "ISS"},
	{ 0x44, "DucatiM3"},
	{ 0x48, "FaceDetect"},
	{ 0x50, "SDMA_Rd"},
	{ 0x54, "SDMA_Wr"},
	{ 0x58, "xxx"},
	{ 0x5C, "xxx"},
	{ 0x60, "SGX"},
	{ 0x70, "DSS"},
	{ 0x80, "C2C"},
	{ 0x88, "xxx"},
	{ 0x8C, "xxx"},
	{ 0x90, "HSI"},
	{ 0xA0, "MMC1"},
	{ 0xA4, "MMC2"},
	{ 0xA8, "MMC6"},
	{ 0xB0, "UNIPRO1"},
	{ 0xC0, "USBHOSTHS"},
	{ 0xC4, "USBOTGHS"},
	{ 0xC8, "USBHOSTFS"}
};

static struct l3_masters_data am4372_l3_masters[] = {
	{ 0x0, "M1 (128-bit)"},
	{ 0x0, "M2 (64-bit)"},
	{ 0x4, "DAP"},
	{ 0x5, "P1500"},
	{ 0xC, "ICSS0"},
	{ 0xD, "ICSS1"},
	{ 0x18, "TPTC0 Read"},
	{ 0x19, "TPTC0 Write"},
	{ 0x1A, "TPTC1 Read"},
	{ 0x1B, "TPTC1 Write"},
	{ 0x1C, "TPTC2 Read"},
	{ 0x1D, "TPTC2 Write"},
	{ 0x20, "SGX530"},
	{ 0x25, "DSS"},
	{ 0x28, "Crypto DMA RD"},
	{ 0x29, "Crypto DMA WR"},
	{ 0x2C, "VPFE0"},
	{ 0x2D, "VPFE1"},
	{ 0x30, "GEMAC"},
	{ 0x34, "USB0 RD"},
	{ 0x35, "USB0 WR"},
	{ 0x36, "USB1 RD"},
	{ 0x37, "USB1 WR"},
};

static char *omap_l3_targ_inst_name[][MAX_TARGETS_IN_CLKDM] = {
	{
		"DMM1",
		"DMM2",
		"ABE",
		"L4CFG",
		"CLK2 PWR DISC",
		"HOST CLK1",
		"L4 WAKEUP"
	},
	{
		"CORTEX M3" ,
		"DSS ",
		"GPMC ",
		"ISS ",
		"IVAHD ",
		"AES1",
		"L4 PER0",
		"OCMRAM ",
		"GPMC sERROR",
		"SGX ",
		"SL2 ",
		"C2C ",
		"PWR DISC CLK1",
		"SHA1",
		"AES2",
		"L4 PER3",
		"L4 PER1",
		"L4 PER2",
		"HOST CLK2",
		"CAL",
		"LLI"
	},
	{
		"EMUSS",
		"DEBUG SOURCE",
		"HOST CLK3"
	},
};

static char *am4372_l3_targ_inst_name[][MAX_TARGETS_IN_CLKDM] = {
	{
		"EMIF",
		"DES",
		"OCMCRAM",
		"TPTC0",
		"TPTC1",
		"TPTC2",
		"TPCC",
		"DEBUGSS",
		"TIMEOUT",
		"SHA",
		"SGX530",
		"AES0",
		"L4_FAST",
		"MPUSS L2 RAM",
		"ICSS",
	},
	{
		"L4_PER 0",
		"L4_PER 1",
		"L4_PER 2",
		"L4_PER 3",
		"McASP 0",
		"McASP 1",
		"MMCHS2",
		"GPMC",
		"L4_FW",
		"TIMEOUT",
		"ADCTSC",
		"L4_WKUP",
		"MAG_CARD",
	},
};

static u32 *omap_l3_targ[] = {
	omap_l3_targ_inst_clk1,
	omap_l3_targ_inst_clk2,
	omap_l3_targ_inst_clk3,
};

static u32 *am4372_l3_targ[] = {
	am4372_l3_targ_inst_200f,
	am4372_l3_targ_inst_100s,
};

struct omap_l3 {
	struct device *dev;
	struct clk *ick;

	/* memory base */
	void __iomem *l3_base[3];

	u32 **l3_targets;
	struct l3_masters_data *masters_names;
	char *(*target_names)[MAX_TARGETS_IN_CLKDM];
	u32 **l3_timeout_targets;
	u32 *l3_flag_mux;
	int debug_irq;
	int app_irq;
	int num_modules;
	int num_masters;
	unsigned num_targets[MAX_L3_MODULES];
};

struct omap_l3 omap_l3_data = {
	.l3_targets = omap_l3_targ,
	.masters_names = omap_l3_masters,
	.target_names = omap_l3_targ_inst_name,
	.l3_timeout_targets = NULL,
	.num_modules = OMAP_L3_MODULES,
	.num_masters = sizeof(omap_l3_masters)/sizeof(struct l3_masters_data),
	.l3_flag_mux = omap_l3_flagmux,
	.num_targets = {
		ARRAY_SIZE(omap_l3_targ_inst_clk1),
		ARRAY_SIZE(omap_l3_targ_inst_clk2),
		ARRAY_SIZE(omap_l3_targ_inst_clk3),
	},
};

struct omap_l3 am4372_l3_data = {
	.l3_targets = am4372_l3_targ,
	.masters_names = am4372_l3_masters,
	.target_names = am4372_l3_targ_inst_name,
	.l3_timeout_targets = NULL,
	.num_modules = AM4372_L3_MODULES,
	.num_masters = sizeof(am4372_l3_masters)/sizeof(struct l3_masters_data),
	.l3_flag_mux = am4372_l3_flagmux,
	.num_targets = {
		ARRAY_SIZE(am4372_l3_targ_inst_200f),
		ARRAY_SIZE(am4372_l3_targ_inst_100s),
	},
};

#endif
