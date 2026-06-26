// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the TI CDCE6214 clock generator
 *
 * datasheet available at https://www.ti.com/lit/gpn/cdce6214
 *
 * Copyright (c) 2023 Alvin Å ipraga <alsi@bang-olufsen.dk>
 * Copyright (c) 2025 Sascha Hauer <s.hauer@pengutronix.de>
 */

#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/bitfield.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <dt-bindings/clock/ti,cdce6214.h>

#define R0	0
#define RO_I2C_A0			BIT(15)
#define RO_PDN_INPUT_SEL		BIT(14)
#define RO_GPIO4_DIR_SEL		BIT(13)
#define RO_GPIO1_DIR_SEL		BIT(12)
#define RO_ZDM_CLOCKSEL			BIT(10)
#define RO_ZDM_EN			BIT(8)
#define RO_SYNC				BIT(5)
#define RO_RECAL			BIT(4)
#define RO_RESETN_SOFT			BIT(3)
#define RO_SWRST			BIT(2)
#define RO_POWERDOWN			BIT(1)
#define RO_MODE				BIT(0)

#define R1	1
#define R1_GPIO4_INPUT_SEL		GENMASK(15, 12)
#define R1_GPIO3_INPUT_SEL		GENMASK(11, 8)
#define R1_GPIO2_INPUT_SEL		GENMASK(7, 4)
#define R1_GPIO1_INPUT_SEL		GENMASK(3, 0)

#define R2	2
#define R2_GPIO4_OUTPUT_SEL		GENMASK(9, 6)
#define R2_GPIO1_OUTPUT_SEL		GENMASK(5, 2)
#define R2_REFSEL_SW			GENMASK(1, 0)

#define R3	3
#define R3_DISABLE_CRC			BIT(13)
#define R3_UPDATE_CRC			BIT(12)
#define R3_NVMCOMMIT			BIT(11)
#define R3_REGCOMMIT			BIT(10)
#define R3_REGCOMMIT_PAGE		BIT(9)
#define R3_FREQ_DEC_REG			BIT(6)
#define R3_FREQ_INC_REG			BIT(5)
#define R3_FREQ_INC_DEC_REG_MODE	BIT(4)
#define R3_FREQ_INC_DEC_EN		BIT(3)

#define R4	4
#define R4_CH4_PD			BIT(7)
#define R4_CH3_PD			BIT(6)
#define R4_CH2_PD			BIT(5)
#define R4_CH1_PD			BIT(4)
#define R4_POST_EE_DLY			GENMASK(3, 0)

#define R5	5
#define R5_PLL_VCOBUFF_LDO_PD		BIT(8)
#define R5_PLL_VCO_LDO_PD		BIT(7)
#define R5_PLL_VCO_BUFF_PD		BIT(6)
#define R5_PLL_CP_LDO_PD		BIT(5)
#define R5_PLL_LOCKDET_PD		BIT(4)
#define R5_PLL_PSB_PD			BIT(3)
#define R5_PLL_PSA_PD			BIT(2)
#define R5_PLL_PFD_PD			BIT(1)

#define R7	7
#define R7_NVMCRCERR			BIT(5)
#define R7_LOCK_DET_S			BIT(1)
#define R7_LOCK_DET			BIT(0)

#define R9	9
#define R9_NVMLCRC			GENMASK(15, 0)

#define R10	10
#define R10_NVMSCRC			GENMASK(15, 0)

#define R11	11
#define R11_NVM_RD_ADDR			GENMASK(5, 0)

#define R12	12
#define R12_NVM_RD_DATA			GENMASK(15, 0)

#define R13	13
#define R13_NVM_WR_ADDR			GENMASK(5, 0)

#define R14	14
#define R14_NVM_WR_DATA			GENMASK(15, 0)

#define R15	15
#define R15_EE_LOCK			GENMASK(15, 12)
#define R15_CAL_MUTE			BIT(5)

#define R24	24
#define R24_IP_PRIREF_BUF_SEL		BIT(15)
#define R24_IP_XO_CLOAD			GENMASK(12, 8)
#define R24_IP_BIAS_SEL_XO		GENMASK(5, 2)
#define R24_IP_SECREF_BUF_SEL		GENMASK(1, 0)
#define R24_IP_SECREF_BUF_SEL_XTAL	0
#define R24_IP_SECREF_BUF_SEL_LVCMOS	1
#define R24_IP_SECREF_BUF_SEL_DIFF	2

#define R25	25
#define R25_IP_REF_TO_OUT4_EN		BIT(14)
#define R25_IP_REF_TO_OUT3_EN		BIT(13)
#define R25_IP_REF_TO_OUT2_EN		BIT(12)
#define R25_IP_REF_TO_OUT1_EN		BIT(11)
#define R25_IP_BYP_OUT0_EN		BIT(10)
#define R25_REF_CH_MUX			BIT(9)
#define R25_IP_RDIV			GENMASK(7, 0)

#define R27	27
#define R27_MASH_ORDER			GENMASK(1, 0)

#define R30	30
#define R30_PLL_NDIV			GENMASK(14, 0)

#define R31	31
#define R31_PLL_NUM_15_0		GENMASK(15, 0)

#define R32	32
#define R32_PLL_NUM_23_16		GENMASK(7, 0)

#define R33	33
#define R33_PLL_DEN_15_0		GENMASK(15, 0)

#define R34	34
#define R34_PLL_DEN_23_16		GENMASK(7, 0)

#define R41	41
#define R41_SSC_EN			BIT(15)

#define R42	42
#define R42_SSC_TYPE			BIT(5)
#define R42_SSC_SEL			GENMASK(3, 1)

#define R43	43
#define R43_FREQ_INC_DEC_DELTA		GENMASK(15, 0)

#define R47	47
#define R47_PLL_CP_DN			GENMASK(12, 7)
#define R47_PLL_PSB			GENMASK(6, 5)
#define R47_PLL_PSA			GENMASK(4, 3)

#define R48	48
#define R48_PLL_LF_RES			GENMASK(14, 11)
#define R48_PLL_CP_UP			GENMASK(5, 0)

#define R49	49
#define R49_PLL_LF_ZCAP			GENMASK(4, 0)

#define R50	50
#define R50_PLL_LOCKDET_WINDOW		GENMASK(10, 8)

#define R51	51
#define R51_PLL_PFD_DLY_EN		BIT(10)
#define R51_PLL_PFD_CTRL		BIT(6)

#define R52	52
#define R52_PLL_NCTRL_EN		BIT(6)
#define R52_PLL_CP_EN			BIT(3)

#define R55	55
#define R55_PLL_LF_3_PCTRIM		GENMASK(9, 8)
#define R55_PLL_LF_3_PRTRIM		GENMASK(7, 6)

#define R56	56
#define R56_CH1_MUX			GENMASK(15, 14)
#define R56_CH1_DIV			GENMASK(13, 0)

#define R57	57
#define R57_CH1_LPHCSL_EN		BIT(14)
#define R57_CH1_1P8VDET			BIT(12)
#define R57_CH1_GLITCHLESS_EN		BIT(9)
#define R57_CH1_SYNC_DELAY		GENMASK(8, 4)
#define R57_CH1_SYNC_EN			BIT(3)
#define R57_CH1_MUTE_SEL		BIT(1)
#define R57_CH1_MUTE			BIT(0)

#define R59	59
#define R59_CH1_LVDS_EN			BIT(15)
#define R59_CH1_CMOSN_EN		BIT(14)
#define R59_CH1_CMOSP_EN		BIT(13)
#define R59_CH1_CMOSN_POL		BIT(12)
#define R59_CH1_CMOSP_POL		BIT(11)

#define R60	60
#define R60_CH1_DIFFBUF_IBIAS_TRIM	GENMASK(15, 12)
#define R60_CH1_LVDS_CMTRIM_INC		GENMASK(11, 10)
#define R60_CH1_LVDS_CMTRIM_DEC		GENMASK(5, 4)
#define R60_CH1_CMOS_SLEW_RATE_CTRL	GENMASK(3, 0)

#define R62	62
#define R62_CH2_MUX			GENMASK(15, 14)
#define R62_CH2_DIV			GENMASK(13, 0)

#define R63	63
#define R63_CH2_LPHCSL_EN		BIT(13)
#define R63_CH2_1P8VDET			BIT(12)
#define R63_CH2_GLITCHLESS_EN		BIT(9)
#define R63_CH2_SYNC_DELAY		GENMASK(8, 4)
#define R63_CH2_SYNC_EN			BIT(3)
#define R63_CH2_MUTE_SEL		BIT(1)
#define R63_CH2_MUTE			BIT(0)

#define R65	65
#define R65_CH2_LVDS_CMTRIM_DEC		GENMASK(14, 13)
#define R65_CH2_LVDS_EN			BIT(11)

#define R66	66
#define R66_CH2_LVDS_CMTRIM_IN		GENMASK(5, 4)
#define R66_CH2_DIFFBUF_IBIAS_TRIM	GENMASK(3, 0)

#define R67	67
#define R67_CH3_MUX			GENMASK(15, 14)
#define R67_CH3_DIV			GENMASK(13, 0)

#define R68	68
#define R68_CH3_LPHCSL_EN		BIT(13)
#define R68_CH3_1P8VDET			BIT(12)
#define R68_CH3_GLITCHLESS_EN		BIT(9)
#define R68_CH3_SYNC_DELAY		GENMASK(8, 4)
#define R68_CH3_SYNC_EN			BIT(3)
#define R68_CH3_MUTE_SEL		BIT(1)
#define R68_CH3_MUTE			BIT(0)

#define R70	70
#define R70_CH3_LVDS_EN			BIT(11)

#define R71	71
#define R71_CH3_LVDS_CMTRIM_DEC		GENMASK(10, 9)
#define R71_CH3_LVDS_CMTRIM_INC		GENMASK(5, 4)
#define R71_CH3_DIFFBUF_IBIAS_TR	GENMASK(3, 0)

#define R72	72
#define R72_CH4_MUX			GENMASK(15, 14)
#define R72_CH4_DIV			GENMASK(13, 0)

#define R73	73
#define R73_CH4_LPHCSL_EN		BIT(13)
#define R73_CH4_1P8VDET			BIT(12)
#define R73_CH4_GLITCHLESS_EN		BIT(9)
#define R73_CH4_SYNC_DELAY		GENMASK(8, 4)
#define R73_CH4_SYNC_EN			BIT(3)
#define R73_CH4_MUTE_SEL		BIT(1)
#define R73_CH4_MUTE			BIT(0)

#define R75	75
#define R75_CH4_LVDS_EN			BIT(15)
#define R75_CH4_CMOSP_EN		BIT(14)
#define R75_CH4_CMOSN_EN		BIT(13)
#define R75_CH4_CMOSP_POL		BIT(12)
#define R75_CH4_CMOSN_POL		BIT(11)

#define R76	76
#define R76_CH4_DIFFBUF_IBIAS_TRIM	GENMASK(9, 6)
#define R76_CH4_LVDS_CMTRIM_IN		GENMASK(5, 4)
#define R76_CH4_CMOS_SLEW_RATE_CTRL	GENMASK(3, 0)

#define R77	77
#define R77_CH4_LVDS_CMTRIM_DEC		GENMASK(1, 0)

#define R78	78
#define R78_CH0_EN			BIT(12)

#define R79	79
#define R79_SAFETY_1P8V_MODE		BIT(9)
#define R79_CH0_CMOS_SLEW_RATE_CTRL	GENMASK(3, 0)

#define R81	81
#define R81_PLL_LOCK_MASK		BIT(3)

#define CDCE6214_VCO_MIN 2335000000
#define CDCE6214_VCO_MAX 2625000000
#define CDCE6214_PLL_NDIV_MIN 24
#define CDCE6214_DENOM_DEFAULT 0x1000000

#define CDCE6214_CLK_PRIREF	0
#define CDCE6214_CLK_SECREF	1

static const char * const clk_names[] = {
	[CDCE6214_CLK_PRIREF] = "priref",
	[CDCE6214_CLK_SECREF] = "secref",
	[CDCE6214_CLK_OUT0] = "out0",
	[CDCE6214_CLK_OUT1] = "out1",
	[CDCE6214_CLK_OUT2] = "out2",
	[CDCE6214_CLK_OUT3] = "out3",
	[CDCE6214_CLK_OUT4] = "out4",
	[CDCE6214_CLK_PLL] = "pll",
	[CDCE6214_CLK_PSA] = "psa",
	[CDCE6214_CLK_PSB] = "psb",
};

#define CDCE6214_NUM_CLOCKS	ARRAY_SIZE(clk_names)

struct cdce6214;

struct cdce6214_clock {
	struct clk_hw hw;
	struct cdce6214 *priv;
	unsigned int index;
};

struct cdce6214 {
	struct i2c_client *client;
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct cdce6214_clock clk[CDCE6214_NUM_CLOCKS];
};

static inline struct cdce6214_clock *hw_to_cdce6214_clk(struct clk_hw *hw)
{
	return container_of(hw, struct cdce6214_clock, hw);
}

static struct clk_hw *cdce6214_of_clk_get(struct of_phandle_args *clkspec,
					  void *data)
{
	struct cdce6214 *priv = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= CDCE6214_NUM_CLOCKS)
		return ERR_PTR(-EINVAL);
	if (idx <= CDCE6214_CLK_SECREF)
		return ERR_PTR(-EINVAL);

	return &priv->clk[idx].hw;
}

static const struct regmap_config cdce6214_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 1,
	.max_register = 0x0055,
};

static int cdce6214_configure(struct cdce6214 *priv)
{
	regmap_update_bits(priv->regmap, R2, R2_REFSEL_SW,
			   FIELD_PREP(R2_REFSEL_SW, 2));

	return 0;
}

static unsigned long cdce6214_clk_out0_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int val, div;

	regmap_read(priv->regmap, R25, &val);

	div = FIELD_GET(R25_IP_RDIV, val);

	if (!div)
		return parent_rate * 2;

	return DIV_ROUND_UP_ULL((u64)parent_rate, div);
}

static int cdce6214_clk_out0_determine_rate(struct clk_hw *hw,
					    struct clk_rate_request *req)
{
	unsigned int div;

	if (req->rate >= req->best_parent_rate)
		req->rate = req->best_parent_rate * 2;

	div = DIV_ROUND_CLOSEST(req->best_parent_rate, req->rate);

	req->rate = DIV_ROUND_UP_ULL((u64)req->best_parent_rate, div);

	return 0;
}

static int cdce6214_clk_out0_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int div;

	if (rate >= parent_rate) {
		regmap_update_bits(priv->regmap, R25, R25_IP_RDIV, FIELD_PREP(R25_IP_RDIV, 0));
		return 0;
	}

	div = DIV_ROUND_CLOSEST(parent_rate, rate);
	if (div > R25_IP_RDIV)
		div = R25_IP_RDIV;

	regmap_update_bits(priv->regmap, R25, R25_IP_RDIV, FIELD_PREP(R25_IP_RDIV, div));

	return 0;
}

static u8 cdce6214_clk_out0_get_parent(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int val;

	regmap_read(priv->regmap, R2, &val);

	if (FIELD_GET(R2_REFSEL_SW, val) == 2)
		return 1;

	return 0;
}

static int cdce6214_clk_out0_set_parent(struct clk_hw *hw, u8 index)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	regmap_update_bits(priv->regmap, R25, R25_REF_CH_MUX, FIELD_PREP(R25_REF_CH_MUX, index));

	return 0;
}

static const struct clk_ops cdce6214_clk_out0_ops = {
	.recalc_rate = cdce6214_clk_out0_recalc_rate,
	.determine_rate = cdce6214_clk_out0_determine_rate,
	.set_rate = cdce6214_clk_out0_set_rate,
	.get_parent = cdce6214_clk_out0_get_parent,
	.set_parent = cdce6214_clk_out0_set_parent,
};

static unsigned int cdce6214_clk_out_mask(unsigned int index)
{
	switch (index) {
	case CDCE6214_CLK_OUT1:
		return R4_CH1_PD;
	case CDCE6214_CLK_OUT2:
		return R4_CH2_PD;
	case CDCE6214_CLK_OUT3:
		return R4_CH3_PD;
	case CDCE6214_CLK_OUT4:
		return R4_CH4_PD;
	default:
		return 0;
	};
}

static int cdce6214_clk_out_prepare(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int mask = cdce6214_clk_out_mask(clock->index);

	if (!mask)
		return -EINVAL;

	return regmap_clear_bits(priv->regmap, R4, mask);
}

static void cdce6214_clk_out_unprepare(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int mask = cdce6214_clk_out_mask(clock->index);

	if (!mask)
		return;

	regmap_set_bits(priv->regmap, R4, mask);
}

static int cdce6214_clk_out_is_prepared(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int mask = cdce6214_clk_out_mask(clock->index);
	unsigned int val;

	if (!mask)
		return -EINVAL;

	regmap_read(priv->regmap, R4, &val);

	return !(val & mask);
}

static unsigned long cdce6214_clk_out_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int val, div;
	unsigned long r;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		regmap_read(priv->regmap, R56, &val);
		div = FIELD_GET(R56_CH1_DIV, val);
		break;
	case CDCE6214_CLK_OUT2:
		regmap_read(priv->regmap, R62, &val);
		div = FIELD_GET(R62_CH2_DIV, val);
		break;
	case CDCE6214_CLK_OUT3:
		regmap_read(priv->regmap, R67, &val);
		div = FIELD_GET(R67_CH3_DIV, val);
		break;
	case CDCE6214_CLK_OUT4:
		regmap_read(priv->regmap, R72, &val);
		div = FIELD_GET(R72_CH4_DIV, val);
		break;
	};

	if (!div)
		div = 1;

	r = DIV_ROUND_UP_ULL((u64)parent_rate, div);

	return r;
}

static unsigned int cdce6214_get_out_div(unsigned long rate, unsigned long parent_rate)
{
	unsigned int div;

	div = divider_get_val(rate, parent_rate, NULL, 14, CLK_DIVIDER_ONE_BASED);

	if (div < 1)
		div = 1;

	return div;
}

static int cdce6214_clk_out_determine_rate(struct clk_hw *hw,
					   struct clk_rate_request *req)
{
	unsigned int div = cdce6214_get_out_div(req->rate, req->best_parent_rate);

	req->rate = DIV_ROUND_UP_ULL((u64)req->best_parent_rate, div);

	return 0;
}

static int cdce6214_clk_out_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	unsigned int div = cdce6214_get_out_div(rate, parent_rate);
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		regmap_update_bits(priv->regmap, R56, R56_CH1_DIV,
				   FIELD_PREP(R56_CH1_DIV, div));
		break;
	case CDCE6214_CLK_OUT2:
		regmap_update_bits(priv->regmap, R62, R62_CH2_DIV,
				   FIELD_PREP(R62_CH2_DIV, div));
		break;
	case CDCE6214_CLK_OUT3:
		regmap_update_bits(priv->regmap, R67, R67_CH3_DIV,
				   FIELD_PREP(R67_CH3_DIV, div));
		break;
	case CDCE6214_CLK_OUT4:
		regmap_update_bits(priv->regmap, R72, R72_CH4_DIV,
				   FIELD_PREP(R72_CH4_DIV, div));
		break;
	};

	return 0;
}

static u8 cdce6214_clk_out_get_parent(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int val, idx;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		regmap_read(priv->regmap, R56, &val);
		idx = FIELD_GET(R56_CH1_MUX, val);
		break;
	case CDCE6214_CLK_OUT2:
		regmap_read(priv->regmap, R62, &val);
		idx = FIELD_GET(R62_CH2_MUX, val);
		break;
	case CDCE6214_CLK_OUT3:
		regmap_read(priv->regmap, R67, &val);
		idx = FIELD_GET(R67_CH3_MUX, val);
		break;
	case CDCE6214_CLK_OUT4:
		regmap_read(priv->regmap, R72, &val);
		idx = FIELD_GET(R72_CH4_MUX, val);
		break;
	};

	return idx;
}

static int cdce6214_clk_out_set_parent(struct clk_hw *hw, u8 index)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		regmap_update_bits(priv->regmap, R56, R56_CH1_MUX, FIELD_PREP(R56_CH1_MUX, index));
		break;
	case CDCE6214_CLK_OUT2:
		regmap_update_bits(priv->regmap, R62, R62_CH2_MUX, FIELD_PREP(R62_CH2_MUX, index));
		break;
	case CDCE6214_CLK_OUT3:
		regmap_update_bits(priv->regmap, R67, R67_CH3_MUX, FIELD_PREP(R67_CH3_MUX, index));
		break;
	case CDCE6214_CLK_OUT4:
		regmap_update_bits(priv->regmap, R72, R72_CH4_MUX, FIELD_PREP(R72_CH4_MUX, index));
		break;
	};

	return 0;
}

static const struct clk_ops cdce6214_clk_out_ops = {
	.prepare = cdce6214_clk_out_prepare,
	.unprepare = cdce6214_clk_out_unprepare,
	.is_prepared = cdce6214_clk_out_is_prepared,
	.recalc_rate = cdce6214_clk_out_recalc_rate,
	.determine_rate = cdce6214_clk_out_determine_rate,
	.set_rate = cdce6214_clk_out_set_rate,
	.get_parent = cdce6214_clk_out_get_parent,
	.set_parent = cdce6214_clk_out_set_parent,
};

static int pll_calc_values(unsigned long parent_rate, unsigned long out,
			   unsigned long *ndiv, unsigned long *num, unsigned long *den)
{
	u64 a;

	if (out < CDCE6214_VCO_MIN || out > CDCE6214_VCO_MAX)
		return -EINVAL;

	*den = 10000000;
	*ndiv = out / parent_rate;
	a = out % parent_rate;
	a *= *den;
	do_div(a, parent_rate);
	*num = a;

	return 0;
}

static unsigned long cdce6214_clk_pll_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned long ndiv, num, den;
	unsigned int val;

	regmap_read(priv->regmap, R30, &val);
	ndiv = FIELD_GET(R30_PLL_NDIV, val);

	regmap_read(priv->regmap, R31, &val);
	num = FIELD_GET(R31_PLL_NUM_15_0, val);

	regmap_read(priv->regmap, R32, &val);
	num |= FIELD_GET(R32_PLL_NUM_23_16, val) << 16;

	regmap_read(priv->regmap, R33, &val);
	den = FIELD_GET(R33_PLL_DEN_15_0, val);

	regmap_read(priv->regmap, R34, &val);
	den |= FIELD_GET(R34_PLL_DEN_23_16, val) << 16;

	if (!den)
		den = CDCE6214_DENOM_DEFAULT;

	return parent_rate * ndiv + DIV_ROUND_CLOSEST(parent_rate * num, den);
}

static int cdce6214_clk_pll_determine_rate(struct clk_hw *hw,
					   struct clk_rate_request *req)
{
	req->rate = clamp(req->rate, CDCE6214_VCO_MIN, CDCE6214_VCO_MAX);

	if (req->rate < req->best_parent_rate * CDCE6214_PLL_NDIV_MIN)
		return -EINVAL;

	req->min_rate = CDCE6214_VCO_MIN;
	req->max_rate = CDCE6214_VCO_MAX;

	return 0;
}

static bool cdce6214_pll_locked(struct cdce6214 *priv)
{
	unsigned int val;

	regmap_read(priv->regmap, R7, &val);

	return val & R7_LOCK_DET;
}

static int cdce6214_wait_pll_lock(struct cdce6214 *priv)
{
	unsigned int val;
	int ret;

	ret = regmap_read_poll_timeout(priv->regmap, R7, val,
				       val & R7_LOCK_DET, 0, 1000);
	if (ret)
		dev_err(priv->dev, "Timeout waiting for PLL lock\n");

	return ret;
}

#define R5_PLL_POWER_BITS (R5_PLL_VCOBUFF_LDO_PD | \
			   R5_PLL_VCO_LDO_PD | \
			   R5_PLL_VCO_BUFF_PD)

static int cdce6214_clk_pll_prepare(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	regmap_clear_bits(priv->regmap, R5, R5_PLL_POWER_BITS);

	regmap_set_bits(priv->regmap, R0, RO_RECAL);

	return cdce6214_wait_pll_lock(priv);
}

static void cdce6214_clk_pll_unprepare(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	regmap_set_bits(priv->regmap, R5, R5_PLL_POWER_BITS);
}

static bool cdce6214_clk_pll_powered(struct cdce6214 *priv)
{
	unsigned int val;

	regmap_read(priv->regmap, R5, &val);

	return (val & R5_PLL_POWER_BITS) == 0;
}

static int cdce6214_clk_pll_is_prepared(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	return cdce6214_pll_locked(priv);
}

static int cdce6214_clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned long ndiv, num, den;
	int ret;

	ret = pll_calc_values(parent_rate, rate, &ndiv, &num, &den);
	if (ret < 0)
		return ret;

	if (den == CDCE6214_DENOM_DEFAULT)
		den = 0;

	regmap_update_bits(priv->regmap, R34, R34_PLL_DEN_23_16,
			   FIELD_PREP(R34_PLL_DEN_23_16, den >> 16));
	regmap_update_bits(priv->regmap, R33, R33_PLL_DEN_15_0,
			   FIELD_PREP(R33_PLL_DEN_15_0, den & 0xffff));
	regmap_update_bits(priv->regmap, R32, R32_PLL_NUM_23_16,
			   FIELD_PREP(R32_PLL_NUM_23_16, num >> 16));
	regmap_update_bits(priv->regmap, R31, R31_PLL_NUM_15_0,
			   FIELD_PREP(R31_PLL_NUM_15_0, num & 0xffff));
	regmap_update_bits(priv->regmap, R30, R30_PLL_NDIV,
			   FIELD_PREP(R30_PLL_NDIV, ndiv));

	regmap_update_bits(priv->regmap, R3, R3_FREQ_INC_DEC_REG_MODE | R3_FREQ_INC_DEC_EN,
			   R3_FREQ_INC_DEC_REG_MODE | R3_FREQ_INC_DEC_EN);

	if (cdce6214_clk_pll_powered(priv)) {
		regmap_set_bits(priv->regmap, R0, RO_RECAL);
		ret = cdce6214_wait_pll_lock(priv);
	}

	return ret;
}

static const struct clk_ops cdce6214_clk_pll_ops = {
	.prepare = cdce6214_clk_pll_prepare,
	.unprepare = cdce6214_clk_pll_unprepare,
	.is_prepared = cdce6214_clk_pll_is_prepared,
	.recalc_rate = cdce6214_clk_pll_recalc_rate,
	.determine_rate = cdce6214_clk_pll_determine_rate,
	.set_rate = cdce6214_clk_pll_set_rate,
};

static unsigned int cdce6214_clk_psx_mask(int index)
{
	switch (index) {
	case CDCE6214_CLK_PSA:
		return R5_PLL_PSA_PD;
	case CDCE6214_CLK_PSB:
		return R5_PLL_PSB_PD;
	default:
		return 0;
	};
}

static int cdce6214_clk_psx_prepare(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int mask = cdce6214_clk_psx_mask(clock->index);

	if (!mask)
		return -EINVAL;

	return regmap_clear_bits(priv->regmap, R5, mask);
}

static void cdce6214_clk_psx_unprepare(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int mask = cdce6214_clk_psx_mask(clock->index);

	if (!mask)
		return;

	regmap_set_bits(priv->regmap, R5, mask);
}

static int cdce6214_clk_psx_is_prepared(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int mask = cdce6214_clk_psx_mask(clock->index);
	unsigned int val;

	if (!mask)
		return -EINVAL;

	regmap_read(priv->regmap, R5, &val);

	return !(val & mask);
}

static unsigned long cdce6214_clk_psx_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	const unsigned int psx[] = { 4, 5, 6, 6 };
	unsigned int val, div;

	regmap_read(priv->regmap, R47, &val);

	switch (clock->index) {
	case CDCE6214_CLK_PSA:
		div = psx[FIELD_GET(R47_PLL_PSA, val)];
		break;
	case CDCE6214_CLK_PSB:
		div = psx[FIELD_GET(R47_PLL_PSB, val)];
		break;
	};

	return DIV_ROUND_UP_ULL((u64)parent_rate, div);
}

static int cdce6214_get_psx_div(unsigned long rate, unsigned long parent_rate)
{
	unsigned int div = DIV_ROUND_CLOSEST(parent_rate, rate);

	return clamp(div, 4, 6);
}

static int cdce6214_clk_psx_determine_rate(struct clk_hw *hw,
					   struct clk_rate_request *req)
{
	unsigned int div = cdce6214_get_psx_div(req->rate, req->best_parent_rate);

	req->rate = DIV_ROUND_UP_ULL((u64)req->best_parent_rate, div);

	return 0;
}

static int cdce6214_clk_psx_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	unsigned int div = cdce6214_get_psx_div(rate, parent_rate);
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	switch (clock->index) {
	case CDCE6214_CLK_PSA:
		regmap_update_bits(priv->regmap, R47, R47_PLL_PSA,
				   FIELD_PREP(R47_PLL_PSA, div));
		break;
	case CDCE6214_CLK_PSB:
		regmap_update_bits(priv->regmap, R47, R47_PLL_PSB,
				   FIELD_PREP(R47_PLL_PSB, div));
		break;
	};

	return 0;
}

static const struct clk_ops cdce6214_clk_psx_ops = {
	.prepare = cdce6214_clk_psx_prepare,
	.unprepare = cdce6214_clk_psx_unprepare,
	.is_prepared = cdce6214_clk_psx_is_prepared,
	.recalc_rate = cdce6214_clk_psx_recalc_rate,
	.determine_rate = cdce6214_clk_psx_determine_rate,
	.set_rate = cdce6214_clk_psx_set_rate,
};

static int cdce6214_clk_register(struct cdce6214 *priv)
{
	struct clk_init_data init[CDCE6214_NUM_CLOCKS] = { 0 };
	struct clk_parent_data pdata_out0[2] = {};
	struct clk_parent_data pdata_out[4] = {};
	struct clk_parent_data pdata_pll = {};
	struct clk_parent_data pdata_psx = {};
	int i, ret;

	pdata_out0[0].fw_name = "priref";
	pdata_out0[1].fw_name = "secref";

	init[CDCE6214_CLK_OUT0].ops = &cdce6214_clk_out0_ops;
	init[CDCE6214_CLK_OUT0].num_parents = ARRAY_SIZE(pdata_out);
	init[CDCE6214_CLK_OUT0].parent_data = pdata_out0;
	init[CDCE6214_CLK_OUT0].flags = CLK_SET_RATE_NO_REPARENT;

	pdata_out[0].hw = &priv->clk[CDCE6214_CLK_PSA].hw;
	pdata_out[1].hw = &priv->clk[CDCE6214_CLK_PSB].hw;
	pdata_out[3].hw = &priv->clk[CDCE6214_CLK_OUT0].hw;

	for (i = CDCE6214_CLK_OUT1; i <= CDCE6214_CLK_OUT4; i++) {
		init[i].ops = &cdce6214_clk_out_ops;
		init[i].num_parents = ARRAY_SIZE(pdata_out);
		init[i].parent_data = pdata_out;
		init[i].flags = CLK_SET_RATE_NO_REPARENT;
	}

	init[CDCE6214_CLK_PLL].ops = &cdce6214_clk_pll_ops;
	init[CDCE6214_CLK_PLL].num_parents = 1;
	pdata_pll.hw = &priv->clk[CDCE6214_CLK_OUT0].hw;
	init[CDCE6214_CLK_PLL].parent_data = &pdata_pll;

	pdata_psx.hw = &priv->clk[CDCE6214_CLK_PLL].hw;
	for (i = CDCE6214_CLK_PSA; i <= CDCE6214_CLK_PSB; i++) {
		init[i].ops = &cdce6214_clk_psx_ops;
		init[i].num_parents = 1;
		init[i].parent_data = &pdata_psx;
	}

	for (i = 0; i < CDCE6214_NUM_CLOCKS; i++) {
		struct cdce6214_clock *clk = &priv->clk[i];
		char name[128];

		if (!init[i].ops)
			continue;

		snprintf(name, sizeof(name), "%s_%s", dev_name(priv->dev), clk_names[i]);
		init[i].name = name;
		clk->hw.init = &init[i];
		clk->priv = priv;
		clk->index = i;
		ret = devm_clk_hw_register(priv->dev, &clk->hw);
		if (ret)
			return ret;
	}

	return 0;
}

enum cdce6214_pin_name {
	NONE,
	PRIREF,
	SECREF,
	OUT0,
	OUT1,
	OUT2,
	OUT3,
	OUT4,
};

static const struct pinctrl_pin_desc cdce6214_pinctrl_pins[] = {
	PINCTRL_PIN(PRIREF, "priref"),
	PINCTRL_PIN(SECREF, "secref"),
	PINCTRL_PIN(OUT0, "out0"),
	PINCTRL_PIN(OUT1, "out1"),
	PINCTRL_PIN(OUT2, "out2"),
	PINCTRL_PIN(OUT3, "out3"),
	PINCTRL_PIN(OUT4, "out4"),
};

enum cdce6214_io_standards {
	cdce6214_iostd_min,
	cdce6214_iostd_cmos,
	cdce6214_iostd_lvds,
	cdce6214_iostd_lp_hcsl,
	cdce6214_iostd_xtal,
	cdce6214_iostd_diff,
	cdce6214_iostd_max
};

#define PIN_CONFIG_IOSTANDARD		(PIN_CONFIG_END + 1)
#define PIN_CONFIG_CMOSN_MODE		(PIN_CONFIG_END + 2)
#define PIN_CONFIG_CMOSP_MODE		(PIN_CONFIG_END + 3)
#define PIN_CONFIG_XO_CLOAD		(PIN_CONFIG_END + 4)
#define PIN_CONFIG_XO_BIAS		(PIN_CONFIG_END + 5)

static const struct pinconf_generic_params cdce6214_dt_params[] = {
	{"ti,io-standard", PIN_CONFIG_IOSTANDARD, cdce6214_iostd_min},
	{"ti,cmosn-mode", PIN_CONFIG_CMOSN_MODE, CDCE6214_CMOS_MODE_LOW},
	{"ti,cmosp-mode", PIN_CONFIG_CMOSP_MODE, CDCE6214_CMOS_MODE_HIGH},
	{"ti,xo-cload-femtofarad", PIN_CONFIG_XO_CLOAD, CDCE6214_CMOS_MODE_HIGH},
	{"ti,xo-bias-microampere", PIN_CONFIG_XO_BIAS, CDCE6214_CMOS_MODE_HIGH},
};

static const struct pin_config_item cdce6214_conf_items[] = {
	PCONFDUMP(PIN_CONFIG_IOSTANDARD, "IO-standard", NULL, true),
	PCONFDUMP(PIN_CONFIG_CMOSN_MODE, "CMOS-N mode", NULL, true),
	PCONFDUMP(PIN_CONFIG_CMOSP_MODE, "CMOS-P mode", NULL, true),
	PCONFDUMP(PIN_CONFIG_XO_CLOAD, "XO cload", "fF", true),
	PCONFDUMP(PIN_CONFIG_XO_BIAS, "XO bias", "uA", true),
};

static int cdce6214_pinconf_get_iostd(struct cdce6214 *priv, unsigned int pin)
{
	struct regmap *reg = priv->regmap;
	unsigned int r24, r57, r59, r63, r65, r68, r70, r73, r75;

	switch (pin) {
	case OUT0:
		return cdce6214_iostd_cmos;
	case OUT1:
		regmap_read(reg, R57, &r57);
		regmap_read(reg, R59, &r59);
		if (r59 & R59_CH1_LVDS_EN)
			return cdce6214_iostd_lvds;
		if (r57 & R57_CH1_LPHCSL_EN)
			return cdce6214_iostd_lp_hcsl;
		return cdce6214_iostd_cmos;
	case OUT2:
		regmap_read(reg, R63, &r63);
		regmap_read(reg, R65, &r65);
		if (r65 & R65_CH2_LVDS_EN)
			return cdce6214_iostd_lvds;
		if (r63 & R63_CH2_LPHCSL_EN)
			return cdce6214_iostd_lp_hcsl;
		return cdce6214_iostd_min;
	case OUT3:
		regmap_read(reg, R68, &r68);
		regmap_read(reg, R70, &r70);
		if (r70 & R70_CH3_LVDS_EN)
			return cdce6214_iostd_lvds;
		if (r68 & R68_CH3_LPHCSL_EN)
			return cdce6214_iostd_lp_hcsl;
		return cdce6214_iostd_min;
	case OUT4:
		regmap_read(reg, R73, &r73);
		regmap_read(reg, R75, &r75);
		if (r75 & R75_CH4_LVDS_EN)
			return cdce6214_iostd_lvds;
		if (r73 & R73_CH4_LPHCSL_EN)
			return cdce6214_iostd_lp_hcsl;
		return cdce6214_iostd_cmos;
	case PRIREF:
		regmap_read(reg, R24, &r24);
		if (r24 & R24_IP_PRIREF_BUF_SEL)
			return cdce6214_iostd_lvds;
		else
			return cdce6214_iostd_cmos;
	case SECREF:
		regmap_read(reg, R24, &r24);
		if (r24 & R24_IP_SECREF_BUF_SEL)
			return cdce6214_iostd_cmos;
		else
			return cdce6214_iostd_xtal;
	default:
		return -EINVAL;
	}
}

static int cdce6214_pinconf_get_cmos_mode(struct cdce6214 *priv, unsigned int pin)
{
	unsigned int reg, val;

	switch (pin) {
	case OUT0:
	case OUT2:
	case OUT3:
	case PRIREF:
	case SECREF:
		return -ENOTSUPP;
	case OUT1:
		reg = R59;
		break;
	case OUT4:
		reg = R75;
		break;
	default:
		return -EINVAL;
	}

	regmap_read(priv->regmap, reg, &val);

	return val;
}

static int cdce6214_pinconf_get_cmosn_mode(struct cdce6214 *priv, unsigned int pin)
{
	int val;

	val = cdce6214_pinconf_get_cmos_mode(priv, pin);
	if (val < 0)
		return val;

	if (!(val & R59_CH1_CMOSN_EN))
		return CDCE6214_CMOS_MODE_DISABLED;
	if (val & R59_CH1_CMOSN_POL)
		return CDCE6214_CMOS_MODE_HIGH;
	else
		return CDCE6214_CMOS_MODE_LOW;
}

static int cdce6214_pinconf_get_cmosp_mode(struct cdce6214 *priv, unsigned int pin)
{
	int val;

	val = cdce6214_pinconf_get_cmos_mode(priv, pin);
	if (val < 0)
		return val;

	if (!(val & R59_CH1_CMOSP_EN))
		return CDCE6214_CMOS_MODE_DISABLED;
	if (val & R59_CH1_CMOSP_POL)
		return CDCE6214_CMOS_MODE_HIGH;
	else
		return CDCE6214_CMOS_MODE_LOW;
}

static const unsigned short ip_xo_cload[] = {
	/* index is the register value */
	3000, 3200, 3400, 3600, 3800, 4000, 4200, 4400,
	4600, 4800, 5000, 5200, 5400, 5600, 5800, 6000,
	6200, 6400, 6500, 6700, 6900, 7100, 7300, 7500,
	7700, 7900, 8100, 8300, 8500, 8700, 8900, 9000
};

static int cdce6214_pinconf_get_xo_cload(struct cdce6214 *priv, unsigned int pin)
{
	unsigned int val;

	if (pin != SECREF)
		return -ENOTSUPP;

	regmap_read(priv->regmap, R24, &val);

	val = FIELD_GET(R24_IP_XO_CLOAD, val);

	if (val >= ARRAY_SIZE(ip_xo_cload))
		return -EINVAL;

	return ip_xo_cload[val];
}

static const unsigned short ip_bias_sel_xo[] = {
	/* index is the register value */
	0, 14, 29, 44,
	59, 148, 295, 443,
	591, 884, 1177, 1468, 1758
};

static int cdce6214_pinconf_get_xo_bias(struct cdce6214 *priv, unsigned int pin)
{
	unsigned int val;

	if (pin != SECREF)
		return -ENOTSUPP;

	regmap_read(priv->regmap, R24, &val);

	val = FIELD_GET(R24_IP_BIAS_SEL_XO, val);

	if (val >= ARRAY_SIZE(ip_bias_sel_xo))
		return -EINVAL;

	return ip_bias_sel_xo[val];
}

static int cdce6214_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
				unsigned long *config)
{
	struct cdce6214 *priv = pinctrl_dev_get_drvdata(pctldev);
	unsigned int param = pinconf_to_config_param(*config);
	int arg = 0;

	switch (param) {
	case PIN_CONFIG_IOSTANDARD:
		arg = cdce6214_pinconf_get_iostd(priv, pin);
		break;
	case PIN_CONFIG_CMOSN_MODE:
		arg = cdce6214_pinconf_get_cmosn_mode(priv, pin);
		break;
	case PIN_CONFIG_CMOSP_MODE:
		arg = cdce6214_pinconf_get_cmosp_mode(priv, pin);
		break;
	case PIN_CONFIG_XO_CLOAD:
		arg = cdce6214_pinconf_get_xo_cload(priv, pin);
		break;
	case PIN_CONFIG_XO_BIAS:
		arg = cdce6214_pinconf_get_xo_bias(priv, pin);
		break;
	default:
		return -ENOTSUPP;
	}

	if (arg < 0)
		return arg;

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int cdce6214_pinconf_set_iostd(struct cdce6214 *priv, unsigned int pin,
				      unsigned int param)
{
	struct regmap *reg = priv->regmap;

	switch (pin) {
	case OUT0:
		if (param == CDCE6214_IOSTD_CMOS)
			break;
		goto err_illegal_fmt;
	case OUT1:
		switch (param) {
		case CDCE6214_IOSTD_CMOS:
			regmap_clear_bits(reg, R59, R59_CH1_LVDS_EN);
			regmap_clear_bits(reg, R57, R57_CH1_LPHCSL_EN);
			break;
		case CDCE6214_IOSTD_LVDS:
			regmap_clear_bits(reg, R57, R57_CH1_LPHCSL_EN);
			regmap_set_bits(reg, R59, R59_CH1_LVDS_EN);
			break;
		case CDCE6214_IOSTD_LP_HCSL:
			regmap_clear_bits(reg, R59, R59_CH1_LVDS_EN);
			regmap_set_bits(reg, R57, R57_CH1_LPHCSL_EN);
			break;
		default:
			goto err_illegal_fmt;
		}
		break;
	case OUT2:
		switch (param) {
		case CDCE6214_IOSTD_LVDS:
			regmap_set_bits(reg, R65, R65_CH2_LVDS_EN);
			regmap_clear_bits(reg, R63, R63_CH2_LPHCSL_EN);
			break;
		case CDCE6214_IOSTD_LP_HCSL:
			regmap_set_bits(reg, R63, R63_CH2_LPHCSL_EN);
			regmap_clear_bits(reg, R65, R65_CH2_LVDS_EN);
			break;
		default:
			goto err_illegal_fmt;
		}
		break;
	case OUT3:
		switch (param) {
		case CDCE6214_IOSTD_LVDS:
			regmap_set_bits(reg, R70, R70_CH3_LVDS_EN);
			regmap_clear_bits(reg, R68, R68_CH3_LPHCSL_EN);
			break;
		case CDCE6214_IOSTD_LP_HCSL:
			regmap_set_bits(reg, R70, R70_CH3_LVDS_EN);
			regmap_clear_bits(reg, R68, R65_CH2_LVDS_EN);
			break;
		}
		break;
	case OUT4:
		switch (param) {
		case CDCE6214_IOSTD_CMOS:
			regmap_clear_bits(reg, R75, R75_CH4_LVDS_EN);
			regmap_clear_bits(reg, R73, R73_CH4_LPHCSL_EN);
			break;
		case CDCE6214_IOSTD_LVDS:
			regmap_clear_bits(reg, R73, R73_CH4_LPHCSL_EN);
			regmap_set_bits(reg, R75, R75_CH4_LVDS_EN);
			break;
		case CDCE6214_IOSTD_LP_HCSL:
			regmap_clear_bits(reg, R75, R75_CH4_LVDS_EN);
			regmap_set_bits(reg, R72, R73_CH4_LPHCSL_EN);
			break;
		default:
			goto err_illegal_fmt;
		}
		break;
	case PRIREF:
		switch (param) {
		case CDCE6214_IOSTD_CMOS:
			regmap_clear_bits(reg, R24, R24_IP_PRIREF_BUF_SEL);
			break;
		case CDCE6214_IOSTD_DIFF:
			regmap_set_bits(reg, R24, R24_IP_PRIREF_BUF_SEL);
			break;
		default:
			goto err_illegal_fmt;
		}
		break;
	case SECREF:
		switch (param) {
		case CDCE6214_IOSTD_CMOS:
			regmap_update_bits(reg, R24, R24_IP_SECREF_BUF_SEL,
					   R24_IP_SECREF_BUF_SEL_LVCMOS);
			break;
		case CDCE6214_IOSTD_XTAL:
			regmap_update_bits(reg, R24, R24_IP_SECREF_BUF_SEL,
					   R24_IP_SECREF_BUF_SEL_XTAL);
			break;
		case CDCE6214_IOSTD_DIFF:
			regmap_update_bits(reg, R24, R24_IP_SECREF_BUF_SEL,
					   R24_IP_SECREF_BUF_SEL_DIFF);
			break;
		default:
			goto err_illegal_fmt;
		}

		break;
	}

	return 0;

err_illegal_fmt:

	return -EINVAL;
}

static int cdce6214_pinconf_set_cmosn_mode(struct cdce6214 *priv, unsigned int pin,
					   unsigned int param)
{
	unsigned int reg, val;

	switch (pin) {
	case OUT1:
		reg = R59;
		break;
	case OUT4:
		reg = R75;
		break;
	default:
		return -EINVAL;
	}

	switch (param) {
	case CDCE6214_CMOS_MODE_LOW:
		val = R59_CH1_CMOSN_EN;
		break;
	case CDCE6214_CMOS_MODE_HIGH:
		val = R59_CH1_CMOSN_POL | R59_CH1_CMOSN_EN;
		break;
	case CDCE6214_CMOS_MODE_DISABLED:
		val = 0;
		break;
	}

	/* Relevant fields are identical for register 59 and 75 */
	regmap_update_bits(priv->regmap, reg, R59_CH1_CMOSN_POL | R59_CH1_CMOSN_EN, val);

	return 0;
}

static int cdce6214_pinconf_set_cmosp_mode(struct cdce6214 *priv, unsigned int pin,
					   unsigned int param)
{
	unsigned int reg, val;

	switch (pin) {
	case OUT1:
		reg = R59;
		break;
	case OUT4:
		reg = R75;
		break;
	default:
		return -EINVAL;
	}

	switch (param) {
	case CDCE6214_CMOS_MODE_LOW:
		val = R59_CH1_CMOSP_EN;
		break;
	case CDCE6214_CMOS_MODE_HIGH:
		val = R59_CH1_CMOSP_POL | R59_CH1_CMOSP_EN;
		break;
	case CDCE6214_CMOS_MODE_DISABLED:
		val = 0;
		break;
	}

	/* Relevant fields are identical for register 59 and 75 */
	regmap_update_bits(priv->regmap, reg, R59_CH1_CMOSP_POL | R59_CH1_CMOSP_EN, val);

	return 0;
}

static int cdce6214_pinconf_set_xo_cload(struct cdce6214 *priv, unsigned int pin,
					 unsigned int param)
{
	int i;

	if (pin != SECREF)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ip_xo_cload); i++)
		if (param <= ip_xo_cload[i])
			break;

	if (i >= ARRAY_SIZE(ip_xo_cload))
		i = ARRAY_SIZE(ip_xo_cload) - 1;

	regmap_update_bits(priv->regmap, R24, R24_IP_XO_CLOAD,
			   FIELD_PREP(R24_IP_XO_CLOAD, i));

	return 0;
}

static int cdce6214_pinconf_set_xo_bias(struct cdce6214 *priv, unsigned int pin,
					unsigned int param)
{
	int i;

	if (pin != SECREF)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ip_bias_sel_xo); i++)
		if (param <= ip_bias_sel_xo[i])
			break;

	if (i >= ARRAY_SIZE(ip_bias_sel_xo))
		i = ARRAY_SIZE(ip_bias_sel_xo) - 1;

	regmap_update_bits(priv->regmap, R24, R24_IP_BIAS_SEL_XO,
			   FIELD_PREP(R24_IP_BIAS_SEL_XO, i));

	return 0;
}

static int cdce6214_pinconf_set_one(struct cdce6214 *priv,
			unsigned int pin, unsigned long config)
{
	unsigned int param;
	u32 param_val;
	int ret;

	param = pinconf_to_config_param(config);
	param_val = pinconf_to_config_argument(config);

	switch (param) {
	case PIN_CONFIG_IOSTANDARD:
		ret = cdce6214_pinconf_set_iostd(priv, pin, param_val);
		break;
	case PIN_CONFIG_CMOSN_MODE:
		ret = cdce6214_pinconf_set_cmosn_mode(priv, pin, param_val);
		break;
	case PIN_CONFIG_CMOSP_MODE:
		ret = cdce6214_pinconf_set_cmosp_mode(priv, pin, param_val);
		break;
	case PIN_CONFIG_XO_CLOAD:
		ret = cdce6214_pinconf_set_xo_cload(priv, pin, param_val);
		break;
	case PIN_CONFIG_XO_BIAS:
		ret = cdce6214_pinconf_set_xo_bias(priv, pin, param_val);
		break;
	default:
		dev_err(priv->dev, "Property %u not supported\n", param);
		ret = -ENOTSUPP;
	}

	return ret;
}

static int cdce6214_pinconf_set(struct pinctrl_dev *pctldev,
			unsigned int pin, unsigned long *configs,
			unsigned int num_configs)
{
	struct cdce6214 *priv = pinctrl_dev_get_drvdata(pctldev);
	int ret, i;

	for (i = 0; i < num_configs; i++) {
		ret = cdce6214_pinconf_set_one(priv, pin, configs[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int rtc_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	return 0;
}

static const char *rtc_pinctrl_get_group_name(struct pinctrl_dev *pctldev,
					      unsigned int group)
{
	return NULL;
}

static const struct pinctrl_ops rtc_pinctrl_ops = {
	.get_groups_count = rtc_pinctrl_get_groups_count,
	.get_group_name = rtc_pinctrl_get_group_name,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static const struct pinconf_ops cdce6214_pinconf_ops = {
	.is_generic = true,
	.pin_config_get = cdce6214_pinconf_get,
	.pin_config_set = cdce6214_pinconf_set,
};

static struct pinctrl_desc cdce6214_pdesc = {
	.name = "cdce6214-pinctrl",
	.pins = cdce6214_pinctrl_pins,
	.npins = ARRAY_SIZE(cdce6214_pinctrl_pins),
	.pctlops = &rtc_pinctrl_ops,
	.owner = THIS_MODULE,
	.confops = &cdce6214_pinconf_ops,
	.num_custom_params = ARRAY_SIZE(cdce6214_dt_params),
	.custom_params = cdce6214_dt_params,
	.custom_conf_items = cdce6214_conf_items,
};

static int cdce6214_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct cdce6214 *priv;
	struct pinctrl_dev *pctl;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->dev = dev;
	i2c_set_clientdata(client, priv);
	dev_set_drvdata(dev, priv);

	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio)) {
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				     "failed to get reset gpio\n");
	}

	priv->regmap = devm_regmap_init_i2c(client, &cdce6214_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to init regmap\n");

	ret = cdce6214_configure(priv);
	if (ret)
		return ret;

	ret = devm_pinctrl_register_and_init(dev, &cdce6214_pdesc, priv, &pctl);
	if (ret)
		return dev_err_probe(dev, ret, "pinctrl register failed");

	ret = pinctrl_enable(pctl);
	if (ret)
		return dev_err_probe(dev, ret, "pinctrl enable failed");

	ret = cdce6214_clk_register(priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register clocks\n");

	return devm_of_clk_add_hw_provider(dev, cdce6214_of_clk_get, priv);
}

static const struct of_device_id cdce6214_ids[] = {
	{ .compatible = "ti,cdce6214" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cdce6214_ids);

static struct i2c_driver cdce6214_driver = {
	.driver = {
		.name = "cdce6214",
		.of_match_table = cdce6214_ids,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = cdce6214_probe,
};
module_i2c_driver(cdce6214_driver);

MODULE_AUTHOR("Alvin Å ipraga <alsi@bang-olufsen.dk>");
MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_DESCRIPTION("TI CDCE6214 driver");
MODULE_LICENSE("GPL");
