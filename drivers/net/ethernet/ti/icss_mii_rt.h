/* SPDX-License-Identifier: GPL-2.0 */

/* PRU-ICSS MII_RT register definitions
 *
 * Copyright (C) 2015-2018 Texas Instruments Incorporated - http://www.ti.com
 */

#ifndef __NET_PRUSS_MII_RT_H__
#define __NET_PRUSS_MII_RT_H__

/* PRUSS_MII_RT Registers */
#define PRUSS_MII_RT_RXCFG0		0x0
#define PRUSS_MII_RT_RXCFG1		0x4
#define PRUSS_MII_RT_TXCFG0		0x10
#define PRUSS_MII_RT_TXCFG1		0x14
#define PRUSS_MII_RT_TX_CRC0		0x20
#define PRUSS_MII_RT_TX_CRC1		0x24
#define PRUSS_MII_RT_TX_IPG0		0x30
#define PRUSS_MII_RT_TX_IPG1		0x34
#define PRUSS_MII_RT_PRS0		0x38
#define PRUSS_MII_RT_PRS1		0x3c
#define PRUSS_MII_RT_RX_FRMS0		0x40
#define PRUSS_MII_RT_RX_FRMS1		0x44
#define PRUSS_MII_RT_RX_PCNT0		0x48
#define PRUSS_MII_RT_RX_PCNT1		0x4c
#define PRUSS_MII_RT_RX_ERR0		0x50
#define PRUSS_MII_RT_RX_ERR1		0x54

/* PRUSS_MII_RT_RXCFG0/1 bits */
#define PRUSS_MII_RT_RXCFG_RX_ENABLE		BIT(0)
#define PRUSS_MII_RT_RXCFG_RX_DATA_RDY_MODE_DIS	BIT(1)
#define PRUSS_MII_RT_RXCFG_RX_CUT_PREAMBLE	BIT(2)
#define PRUSS_MII_RT_RXCFG_RX_MUX_SEL		BIT(3)
#define PRUSS_MII_RT_RXCFG_RX_L2_EN		BIT(4)
#define PRUSS_MII_RT_RXCFG_RX_BYTE_SWAP		BIT(5)
#define PRUSS_MII_RT_RXCFG_RX_AUTO_FWD_PRE	BIT(6)
#define PRUSS_MII_RT_RXCFG_RX_L2_EOF_SCLR_DIS	BIT(9)

/* PRUSS_MII_RT_TXCFG0/1 bits */
#define PRUSS_MII_RT_TXCFG_TX_ENABLE		BIT(0)
#define PRUSS_MII_RT_TXCFG_TX_AUTO_PREAMBLE	BIT(1)
#define PRUSS_MII_RT_TXCFG_TX_EN_MODE		BIT(2)
#define PRUSS_MII_RT_TXCFG_TX_BYTE_SWAP		BIT(3)
#define PRUSS_MII_RT_TXCFG_TX_MUX_SEL		BIT(8)
#define PRUSS_MII_RT_TXCFG_PRE_TX_AUTO_SEQUENCE	BIT(9)
#define PRUSS_MII_RT_TXCFG_PRE_TX_AUTO_ESC_ERR	BIT(10)
#define PRUSS_MII_RT_TXCFG_TX_32_MODE_EN	BIT(11)

#define PRUSS_MII_RT_TXCFG_TX_START_DELAY_SHIFT	16
#define PRUSS_MII_RT_TXCFG_TX_START_DELAY_MASK	GENMASK(25, 16)

#define PRUSS_MII_RT_TXCFG_TX_CLK_DELAY_SHIFT	28
#define PRUSS_MII_RT_TXCFG_TX_CLK_DELAY_MASK	GENMASK(30, 28)

/* PRUSS_MII_RT_TX_IPG0/1 bits */
#define PRUSS_MII_RT_TX_IPG_IPG_SHIFT	0
#define PRUSS_MII_RT_TX_IPG_IPG_MASK	GENMASK(9, 0)

/* PRUSS_MII_RT_PRS0/1 bits */
#define PRUSS_MII_RT_PRS_COL	BIT(0)
#define PRUSS_MII_RT_PRS_CRS	BIT(1)

/* PRUSS_MII_RT_RX_FRMS0/1 bits */
#define PRUSS_MII_RT_RX_FRMS_MIN_FRM_SHIFT	0
#define PRUSS_MII_RT_RX_FRMS_MIN_FRM_MASK	GENMASK(15, 0)

#define PRUSS_MII_RT_RX_FRMS_MAX_FRM_SHIFT	16
#define PRUSS_MII_RT_RX_FRMS_MAX_FRM_MASK	GENMASK(31, 16)

/* PRUSS_MII_RT_RX_PCNT0/1 bits */
#define PRUSS_MII_RT_RX_PCNT_MIN_PCNT_SHIFT	0
#define PRUSS_MII_RT_RX_PCNT_MIN_PCNT_MASK	GENMASK(3, 0)

#define PRUSS_MII_RT_RX_PCNT_MAX_PCNT_SHIFT	4
#define PRUSS_MII_RT_RX_PCNT_MAX_PCNT_MASK	GENMASK(7, 4)

/* PRUSS_MII_RT_RX_ERR0/1 bits */
#define PRUSS_MII_RT_RX_ERR_MIN_PCNT_ERR	BIT(0)
#define PRUSS_MII_RT_RX_ERR_MAX_PCNT_ERR	BIT(1)
#define PRUSS_MII_RT_RX_ERR_MIN_FRM_ERR		BIT(2)
#define PRUSS_MII_RT_RX_ERR_MAX_FRM_ERR		BIT(3)

/* TX IPG Values to be set for 100M and 1G link speeds.  These values are
 * in ocp_clk cycles. So need change if ocp_clk is changed for a specific
 * h/w design.
 */
#define MII_RT_TX_IPG_100M	0x166
#define MII_RT_TX_IPG_1G	0x18

#define RGMII_CFG_OFFSET	4

/* Constant to choose between MII0 and MII1 */
#define ICSS_MII0	0
#define ICSS_MII1	1

/* RGMII CFG Register bits */
#define RGMII_CFG_GIG_EN_MII0	BIT(17)
#define RGMII_CFG_GIG_EN_MII1	BIT(21)
#define RGMII_CFG_FULL_DUPLEX_MII0	BIT(18)
#define RGMII_CFG_FULL_DUPLEX_MII1	BIT(22)
#define RGMII_CFG_SPEED_MII0	GENMASK(2, 1)
#define RGMII_CFG_SPEED_MII1	GENMASK(6, 5)
#define RGMII_CFG_SPEED_MII0_SHIFT	1
#define RGMII_CFG_SPEED_MII1_SHIFT	5
#define RGMII_CFG_FULLDUPLEX_MII0	BIT(3)
#define RGMII_CFG_FULLDUPLEX_MII1	BIT(7)
#define RGMII_CFG_FULLDUPLEX_MII0_SHIFT	3
#define RGMII_CFG_FULLDUPLEX_MII1_SHIFT	7
#define RGMII_CFG_SPEED_10M	0
#define RGMII_CFG_SPEED_100M	1
#define RGMII_CFG_SPEED_1G	2

static inline void icssg_update_rgmii_cfg(struct regmap *miig_rt, bool gig_en,
					  bool full_duplex, int mii)
{
	u32 gig_en_mask, gig_val = 0, full_duplex_mask, full_duplex_val = 0;

	gig_en_mask = (mii == ICSS_MII0) ? RGMII_CFG_GIG_EN_MII0 :
					RGMII_CFG_GIG_EN_MII1;
	if (gig_en)
		gig_val = gig_en_mask;
	regmap_update_bits(miig_rt, RGMII_CFG_OFFSET, gig_en_mask, gig_val);

	full_duplex_mask = (mii == ICSS_MII0) ? RGMII_CFG_FULL_DUPLEX_MII0 :
					   RGMII_CFG_FULL_DUPLEX_MII1;
	if (full_duplex)
		full_duplex_val = full_duplex_mask;
	regmap_update_bits(miig_rt, RGMII_CFG_OFFSET, full_duplex_mask,
			   full_duplex_val);
}

static inline u32 icssg_rgmii_cfg_get_bitfield(struct regmap *miig_rt,
					       u32 mask, u32 shift)
{
	u32 val;

	regmap_read(miig_rt, RGMII_CFG_OFFSET, &val);
	val &= mask;
	val >>= shift;

	return val;
}

static inline u32 icssg_rgmii_get_speed(struct regmap *miig_rt, int mii)
{
	u32 shift = RGMII_CFG_SPEED_MII0_SHIFT, mask = RGMII_CFG_SPEED_MII0;

	if (mii == ICSS_MII1) {
		shift = RGMII_CFG_SPEED_MII1_SHIFT;
		mask = RGMII_CFG_SPEED_MII1;
	}

	return icssg_rgmii_cfg_get_bitfield(miig_rt, mask, shift);
}

static inline u32 icssg_rgmii_get_fullduplex(struct regmap *miig_rt, int mii)
{
	u32 shift = RGMII_CFG_FULLDUPLEX_MII0_SHIFT;
	u32 mask = RGMII_CFG_FULLDUPLEX_MII0;

	if (mii == ICSS_MII1) {
		shift = RGMII_CFG_FULLDUPLEX_MII1_SHIFT;
		mask = RGMII_CFG_FULLDUPLEX_MII1;
	}

	return icssg_rgmii_cfg_get_bitfield(miig_rt, mask, shift);
}

static inline void icssg_update_mii_rt_cfg(struct regmap *mii_rt, int speed,
					   int mii)
{
	u32 ipg_reg, val;

	ipg_reg = (mii == ICSS_MII0) ? PRUSS_MII_RT_TX_IPG0 :
				       PRUSS_MII_RT_TX_IPG1;
	switch (speed) {
	case SPEED_1000:
		val = MII_RT_TX_IPG_1G;
		break;
	case SPEED_100:
		val = MII_RT_TX_IPG_100M;
		break;
	default:
		/* Other links speeds not supported */
		pr_err("Unsupported link speed\n");
		return;
	}
	regmap_write(mii_rt, ipg_reg, val);
}
#endif /* __NET_PRUSS_MII_RT_H__ */
