/*
 * Copyright (C) 2012 Texas Instruments
 * Author: Rob Clark <robdclark@gmail.com>
 * Author: Darren Etheridge <detheridge@ti.com>
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


#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt


#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <video/da8xx-fb.h>
#include <video/da8xx-tda998x-hdmi.h>

#define TDA998X_DEBUG

#ifdef TDA998X_DEBUG
#define DBG(fmt, ...) printk(KERN_DEBUG "%s: " fmt, __func__, ##__VA_ARGS__)
#else
#define DBG(fmt, ...)
#endif


#define tda998x_encoder da8xx_encoder

struct tda998x_priv {
	struct i2c_client *cec;
	uint16_t rev;
	uint8_t current_page;
	int dpms;
	bool is_hdmi_sink;
	u8 vip_cntrl_0;
	u8 vip_cntrl_1;
	u8 vip_cntrl_2;
	struct tda998x_encoder_params params;
};

#define to_tda998x_priv(x)  ((struct tda998x_priv *)x->priv)
#define tda998x_i2c_encoder_get_client(x) ((struct i2c_client *)x->client)

/* The TDA9988 series of devices use a paged register scheme.. to simplify
 * things we encode the page # in upper bits of the register #.  To read/
 * write a given register, we need to make sure CURPAGE register is set
 * appropriately.  Which implies reads/writes are not atomic.  Fun!
 */

#define REG(page, addr) (((page) << 8) | (addr))
#define REG2ADDR(reg)   ((reg) & 0xff)
#define REG2PAGE(reg)   (((reg) >> 8) & 0xff)

#define REG_CURPAGE               0xff                /* write */


/* Page 00h: General Control */
#define REG_VERSION_LSB           REG(0x00, 0x00)     /* read */
#define REG_MAIN_CNTRL0           REG(0x00, 0x01)     /* read/write */
# define MAIN_CNTRL0_SR           (1 << 0)
# define MAIN_CNTRL0_DECS         (1 << 1)
# define MAIN_CNTRL0_DEHS         (1 << 2)
# define MAIN_CNTRL0_CECS         (1 << 3)
# define MAIN_CNTRL0_CEHS         (1 << 4)
# define MAIN_CNTRL0_SCALER       (1 << 7)
#define REG_VERSION_MSB           REG(0x00, 0x02)     /* read */
#define REG_SOFTRESET             REG(0x00, 0x0a)     /* write */
# define SOFTRESET_AUDIO          (1 << 0)
# define SOFTRESET_I2C_MASTER     (1 << 1)
#define REG_DDC_DISABLE           REG(0x00, 0x0b)     /* read/write */
#define REG_CCLK_ON               REG(0x00, 0x0c)     /* read/write */
#define REG_I2C_MASTER            REG(0x00, 0x0d)     /* read/write */
# define I2C_MASTER_DIS_MM        (1 << 0)
# define I2C_MASTER_DIS_FILT      (1 << 1)
# define I2C_MASTER_APP_STRT_LAT  (1 << 2)
#define REG_FEAT_POWERDOWN        REG(0x00, 0x0e)     /* read/write */
# define FEAT_POWERDOWN_SPDIF     (1 << 3)
#define REG_INT_FLAGS_0           REG(0x00, 0x0f)     /* read/write */
#define REG_INT_FLAGS_1           REG(0x00, 0x10)     /* read/write */
#define REG_INT_FLAGS_2           REG(0x00, 0x11)     /* read/write */
# define INT_FLAGS_2_EDID_BLK_RD  (1 << 1)
#define REG_ENA_ACLK              REG(0x00, 0x16)     /* read/write */
#define REG_ENA_VP_0              REG(0x00, 0x18)     /* read/write */
#define REG_ENA_VP_1              REG(0x00, 0x19)     /* read/write */
#define REG_ENA_VP_2              REG(0x00, 0x1a)     /* read/write */
#define REG_ENA_AP                REG(0x00, 0x1e)     /* read/write */
#define REG_VIP_CNTRL_0           REG(0x00, 0x20)     /* write */
# define VIP_CNTRL_0_MIRR_A       (1 << 7)
# define VIP_CNTRL_0_SWAP_A(x)    (((x) & 7) << 4)
# define VIP_CNTRL_0_MIRR_B       (1 << 3)
# define VIP_CNTRL_0_SWAP_B(x)    (((x) & 7) << 0)
#define REG_VIP_CNTRL_1           REG(0x00, 0x21)     /* write */
# define VIP_CNTRL_1_MIRR_C       (1 << 7)
# define VIP_CNTRL_1_SWAP_C(x)    (((x) & 7) << 4)
# define VIP_CNTRL_1_MIRR_D       (1 << 3)
# define VIP_CNTRL_1_SWAP_D(x)    (((x) & 7) << 0)
#define REG_VIP_CNTRL_2           REG(0x00, 0x22)     /* write */
# define VIP_CNTRL_2_MIRR_E       (1 << 7)
# define VIP_CNTRL_2_SWAP_E(x)    (((x) & 7) << 4)
# define VIP_CNTRL_2_MIRR_F       (1 << 3)
# define VIP_CNTRL_2_SWAP_F(x)    (((x) & 7) << 0)
#define REG_VIP_CNTRL_3           REG(0x00, 0x23)     /* write */
# define VIP_CNTRL_3_X_TGL        (1 << 0)
# define VIP_CNTRL_3_H_TGL        (1 << 1)
# define VIP_CNTRL_3_V_TGL        (1 << 2)
# define VIP_CNTRL_3_EMB          (1 << 3)
# define VIP_CNTRL_3_SYNC_DE      (1 << 4)
# define VIP_CNTRL_3_SYNC_HS      (1 << 5)
# define VIP_CNTRL_3_DE_INT       (1 << 6)
# define VIP_CNTRL_3_EDGE         (1 << 7)
#define REG_VIP_CNTRL_4           REG(0x00, 0x24)     /* write */
# define VIP_CNTRL_4_BLC(x)       (((x) & 3) << 0)
# define VIP_CNTRL_4_BLANKIT(x)   (((x) & 3) << 2)
# define VIP_CNTRL_4_CCIR656      (1 << 4)
# define VIP_CNTRL_4_656_ALT      (1 << 5)
# define VIP_CNTRL_4_TST_656      (1 << 6)
# define VIP_CNTRL_4_TST_PAT      (1 << 7)
#define REG_VIP_CNTRL_5           REG(0x00, 0x25)     /* write */
# define VIP_CNTRL_5_CKCASE       (1 << 0)
# define VIP_CNTRL_5_SP_CNT(x)    (((x) & 3) << 1)
#define REG_MUX_AP                REG(0x00, 0x26)     /* read/write */
#define REG_MUX_VP_VIP_OUT        REG(0x00, 0x27)     /* read/write */
#define REG_MAT_CONTRL            REG(0x00, 0x80)     /* write */
# define MAT_CONTRL_MAT_SC(x)     (((x) & 3) << 0)
# define MAT_CONTRL_MAT_BP        (1 << 2)
#define REG_VIDFORMAT             REG(0x00, 0xa0)     /* write */
#define REG_REFPIX_MSB            REG(0x00, 0xa1)     /* write */
#define REG_REFPIX_LSB            REG(0x00, 0xa2)     /* write */
#define REG_REFLINE_MSB           REG(0x00, 0xa3)     /* write */
#define REG_REFLINE_LSB           REG(0x00, 0xa4)     /* write */
#define REG_NPIX_MSB              REG(0x00, 0xa5)     /* write */
#define REG_NPIX_LSB              REG(0x00, 0xa6)     /* write */
#define REG_NLINE_MSB             REG(0x00, 0xa7)     /* write */
#define REG_NLINE_LSB             REG(0x00, 0xa8)     /* write */
#define REG_VS_LINE_STRT_1_MSB    REG(0x00, 0xa9)     /* write */
#define REG_VS_LINE_STRT_1_LSB    REG(0x00, 0xaa)     /* write */
#define REG_VS_PIX_STRT_1_MSB     REG(0x00, 0xab)     /* write */
#define REG_VS_PIX_STRT_1_LSB     REG(0x00, 0xac)     /* write */
#define REG_VS_LINE_END_1_MSB     REG(0x00, 0xad)     /* write */
#define REG_VS_LINE_END_1_LSB     REG(0x00, 0xae)     /* write */
#define REG_VS_PIX_END_1_MSB      REG(0x00, 0xaf)     /* write */
#define REG_VS_PIX_END_1_LSB      REG(0x00, 0xb0)     /* write */
#define REG_VS_LINE_STRT_2_MSB    REG(0x00, 0xb1)     /* write */
#define REG_VS_LINE_STRT_2_LSB    REG(0x00, 0xb2)     /* write */
#define REG_VS_PIX_STRT_2_MSB     REG(0x00, 0xb3)     /* write */
#define REG_VS_PIX_STRT_2_LSB     REG(0x00, 0xb4)     /* write */
#define REG_VS_LINE_END_2_MSB     REG(0x00, 0xb5)     /* write */
#define REG_VS_LINE_END_2_LSB     REG(0x00, 0xb6)     /* write */
#define REG_VS_PIX_END_2_MSB      REG(0x00, 0xb7)     /* write */
#define REG_VS_PIX_END_2_LSB      REG(0x00, 0xb8)     /* write */
#define REG_HS_PIX_START_MSB      REG(0x00, 0xb9)     /* write */
#define REG_HS_PIX_START_LSB      REG(0x00, 0xba)     /* write */
#define REG_HS_PIX_STOP_MSB       REG(0x00, 0xbb)     /* write */
#define REG_HS_PIX_STOP_LSB       REG(0x00, 0xbc)     /* write */
#define REG_VWIN_START_1_MSB      REG(0x00, 0xbd)     /* write */
#define REG_VWIN_START_1_LSB      REG(0x00, 0xbe)     /* write */
#define REG_VWIN_END_1_MSB        REG(0x00, 0xbf)     /* write */
#define REG_VWIN_END_1_LSB        REG(0x00, 0xc0)     /* write */
#define REG_VWIN_START_2_MSB      REG(0x00, 0xc1)     /* write */
#define REG_VWIN_START_2_LSB      REG(0x00, 0xc2)     /* write */
#define REG_VWIN_END_2_MSB        REG(0x00, 0xc3)     /* write */
#define REG_VWIN_END_2_LSB        REG(0x00, 0xc4)     /* write */
#define REG_DE_START_MSB          REG(0x00, 0xc5)     /* write */
#define REG_DE_START_LSB          REG(0x00, 0xc6)     /* write */
#define REG_DE_STOP_MSB           REG(0x00, 0xc7)     /* write */
#define REG_DE_STOP_LSB           REG(0x00, 0xc8)     /* write */
#define REG_TBG_CNTRL_0           REG(0x00, 0xca)     /* write */
# define TBG_CNTRL_0_TOP_TGL      (1 << 0)
# define TBG_CNTRL_0_TOP_SEL      (1 << 1)
# define TBG_CNTRL_0_DE_EXT       (1 << 2)
# define TBG_CNTRL_0_TOP_EXT      (1 << 3)
# define TBG_CNTRL_0_FRAME_DIS    (1 << 5)
# define TBG_CNTRL_0_SYNC_MTHD    (1 << 6)
# define TBG_CNTRL_0_SYNC_ONCE    (1 << 7)
#define REG_TBG_CNTRL_1           REG(0x00, 0xcb)     /* write */
# define TBG_CNTRL_1_H_TGL        (1 << 0)
# define TBG_CNTRL_1_V_TGL        (1 << 1)
# define TBG_CNTRL_1_TGL_EN       (1 << 2)
# define TBG_CNTRL_1_X_EXT        (1 << 3)
# define TBG_CNTRL_1_H_EXT        (1 << 4)
# define TBG_CNTRL_1_V_EXT        (1 << 5)
# define TBG_CNTRL_1_DWIN_DIS     (1 << 6)
#define REG_ENABLE_SPACE          REG(0x00, 0xd6)     /* write */
#define REG_HVF_CNTRL_0           REG(0x00, 0xe4)     /* write */
# define HVF_CNTRL_0_SM           (1 << 7)
# define HVF_CNTRL_0_RWB          (1 << 6)
# define HVF_CNTRL_0_PREFIL(x)    (((x) & 3) << 2)
# define HVF_CNTRL_0_INTPOL(x)    (((x) & 3) << 0)
#define REG_HVF_CNTRL_1           REG(0x00, 0xe5)     /* write */
# define HVF_CNTRL_1_FOR          (1 << 0)
# define HVF_CNTRL_1_YUVBLK       (1 << 1)
# define HVF_CNTRL_1_VQR(x)       (((x) & 3) << 2)
# define HVF_CNTRL_1_PAD(x)       (((x) & 3) << 4)
# define HVF_CNTRL_1_SEMI_PLANAR  (1 << 6)
#define REG_RPT_CNTRL             REG(0x00, 0xf0)     /* write */
#define REG_I2S_FORMAT            REG(0x00, 0xfc)     /* read/write */
# define I2S_FORMAT(x)            (((x) & 3) << 0)
#define REG_AIP_CLKSEL            REG(0x00, 0xfd)     /* write */
# define AIP_CLKSEL_FS(x)         (((x) & 3) << 0)
# define AIP_CLKSEL_CLK_POL(x)    (((x) & 1) << 2)
# define AIP_CLKSEL_AIP(x)        (((x) & 7) << 3)


/* Page 02h: PLL settings */
#define REG_PLL_SERIAL_1          REG(0x02, 0x00)     /* read/write */
# define PLL_SERIAL_1_SRL_FDN     (1 << 0)
# define PLL_SERIAL_1_SRL_IZ(x)   (((x) & 3) << 1)
# define PLL_SERIAL_1_SRL_MAN_IZ  (1 << 6)
#define REG_PLL_SERIAL_2          REG(0x02, 0x01)     /* read/write */
# define PLL_SERIAL_2_SRL_NOSC(x) (((x) & 3) << 0)
# define PLL_SERIAL_2_SRL_PR(x)   (((x) & 0xf) << 4)
#define REG_PLL_SERIAL_3          REG(0x02, 0x02)     /* read/write */
# define PLL_SERIAL_3_SRL_CCIR    (1 << 0)
# define PLL_SERIAL_3_SRL_DE      (1 << 2)
# define PLL_SERIAL_3_SRL_PXIN_SEL (1 << 4)
#define REG_SERIALIZER            REG(0x02, 0x03)     /* read/write */
#define REG_BUFFER_OUT            REG(0x02, 0x04)     /* read/write */
#define REG_PLL_SCG1              REG(0x02, 0x05)     /* read/write */
#define REG_PLL_SCG2              REG(0x02, 0x06)     /* read/write */
#define REG_PLL_SCGN1             REG(0x02, 0x07)     /* read/write */
#define REG_PLL_SCGN2             REG(0x02, 0x08)     /* read/write */
#define REG_PLL_SCGR1             REG(0x02, 0x09)     /* read/write */
#define REG_PLL_SCGR2             REG(0x02, 0x0a)     /* read/write */
#define REG_AUDIO_DIV             REG(0x02, 0x0e)     /* read/write */
#define REG_SEL_CLK               REG(0x02, 0x11)     /* read/write */
# define SEL_CLK_SEL_CLK1         (1 << 0)
# define SEL_CLK_SEL_VRF_CLK(x)   (((x) & 3) << 1)
# define SEL_CLK_ENA_SC_CLK       (1 << 3)
#define REG_ANA_GENERAL           REG(0x02, 0x12)     /* read/write */


/* Page 09h: EDID Control */
#define REG_EDID_DATA_0           REG(0x09, 0x00)     /* read */
/* next 127 successive registers are the EDID block */
#define REG_EDID_CTRL             REG(0x09, 0xfa)     /* read/write */
#define REG_DDC_ADDR              REG(0x09, 0xfb)     /* read/write */
#define REG_DDC_OFFS              REG(0x09, 0xfc)     /* read/write */
#define REG_DDC_SEGM_ADDR         REG(0x09, 0xfd)     /* read/write */
#define REG_DDC_SEGM              REG(0x09, 0xfe)     /* read/write */


/* Page 10h: information frames and packets */
#define REG_IF1_HB0               REG(0x10, 0x20)     /* read/write */
#define REG_IF2_HB0               REG(0x10, 0x40)     /* read/write */
#define REG_IF3_HB0               REG(0x10, 0x60)     /* read/write */
#define REG_IF4_HB0               REG(0x10, 0x80)     /* read/write */
#define REG_IF5_HB0               REG(0x10, 0xa0)     /* read/write */


/* Page 11h: audio settings and content info packets */
#define REG_AIP_CNTRL_0           REG(0x11, 0x00)     /* read/write */
# define AIP_CNTRL_0_RST_FIFO     (1 << 0)
# define AIP_CNTRL_0_SWAP         (1 << 1)
# define AIP_CNTRL_0_LAYOUT       (1 << 2)
# define AIP_CNTRL_0_ACR_MAN      (1 << 5)
# define AIP_CNTRL_0_RST_CTS      (1 << 6)
#define REG_CA_I2S                REG(0x11, 0x01)     /* read/write */
# define CA_I2S_CA_I2S(x)         (((x) & 31) << 0)
# define CA_I2S_HBR_CHSTAT        (1 << 6)
#define REG_LATENCY_RD            REG(0x11, 0x04)     /* read/write */
#define REG_ACR_CTS_0             REG(0x11, 0x05)     /* read/write */
#define REG_ACR_CTS_1             REG(0x11, 0x06)     /* read/write */
#define REG_ACR_CTS_2             REG(0x11, 0x07)     /* read/write */
#define REG_ACR_N_0               REG(0x11, 0x08)     /* read/write */
#define REG_ACR_N_1               REG(0x11, 0x09)     /* read/write */
#define REG_ACR_N_2               REG(0x11, 0x0a)     /* read/write */
#define REG_CTS_N                 REG(0x11, 0x0c)     /* read/write */
# define CTS_N_K(x)               (((x) & 7) << 0)
# define CTS_N_M(x)               (((x) & 3) << 4)
#define REG_ENC_CNTRL             REG(0x11, 0x0d)     /* read/write */
# define ENC_CNTRL_RST_ENC        (1 << 0)
# define ENC_CNTRL_RST_SEL        (1 << 1)
# define ENC_CNTRL_CTL_CODE(x)    (((x) & 3) << 2)
#define REG_DIP_FLAGS             REG(0x11, 0x0e)     /* read/write */
# define DIP_FLAGS_ACR            (1 << 0)
# define DIP_FLAGS_GC             (1 << 1)
#define REG_DIP_IF_FLAGS          REG(0x11, 0x0f)     /* read/write */
# define DIP_IF_FLAGS_IF1         (1 << 1)
# define DIP_IF_FLAGS_IF2         (1 << 2)
# define DIP_IF_FLAGS_IF3         (1 << 3)
# define DIP_IF_FLAGS_IF4         (1 << 4)
# define DIP_IF_FLAGS_IF5         (1 << 5)
#define REG_CH_STAT_B(x)          REG(0x11, 0x14 + (x)) /* read/write */


/* Page 12h: HDCP and OTP */
#define REG_TX3                   REG(0x12, 0x9a)     /* read/write */
#define REG_TX4                   REG(0x12, 0x9b)     /* read/write */
# define TX4_PD_RAM               (1 << 1)
#define REG_TX33                  REG(0x12, 0xb8)     /* read/write */
# define TX33_HDMI                (1 << 1)


/* Page 13h: Gamut related metadata packets */



/* CEC registers: (not paged)
 */
#define REG_CEC_FRO_IM_CLK_CTRL   0xfb                /* read/write */
# define CEC_FRO_IM_CLK_CTRL_GHOST_DIS (1 << 7)
# define CEC_FRO_IM_CLK_CTRL_ENA_OTP   (1 << 6)
# define CEC_FRO_IM_CLK_CTRL_IMCLK_SEL (1 << 1)
# define CEC_FRO_IM_CLK_CTRL_FRO_DIV   (1 << 0)
#define REG_CEC_RXSHPDLEV         0xfe                /* read */
# define CEC_RXSHPDLEV_RXSENS     (1 << 0)
# define CEC_RXSHPDLEV_HPD        (1 << 1)

#define REG_CEC_ENAMODS           0xff                /* read/write */
# define CEC_ENAMODS_DIS_FRO      (1 << 6)
# define CEC_ENAMODS_DIS_CCLK     (1 << 5)
# define CEC_ENAMODS_EN_RXSENS    (1 << 2)
# define CEC_ENAMODS_EN_HDMI      (1 << 1)
# define CEC_ENAMODS_EN_CEC       (1 << 0)


/* Device versions: */
#define TDA9989N2                 0x0101
#define TDA19989                  0x0201
#define TDA19989N2                0x0202
#define TDA19988                  0x0301

static void
cec_write(struct tda998x_encoder *encoder, uint16_t addr, uint8_t val)
{
	struct i2c_client *client = to_tda998x_priv(encoder)->cec;
	uint8_t buf[] = {addr, val};
	int ret;

	ret = i2c_master_send(client, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		dev_err(&client->dev, "Error %d writing to cec:0x%x\n",
			ret, addr);
}

static void
set_page(struct tda998x_encoder *encoder, uint16_t reg)
{
	struct tda998x_priv *priv = to_tda998x_priv(encoder);

	if (REG2PAGE(reg) != priv->current_page) {
		struct i2c_client *client =
			tda998x_i2c_encoder_get_client(encoder);
		uint8_t buf[] = {
				REG_CURPAGE, REG2PAGE(reg)
		};
		int ret = i2c_master_send(client, buf, sizeof(buf));
		if (ret < 0)
			dev_err(&client->dev,
				"Error %d writing to REG_CURPAGE\n", ret);

		priv->current_page = REG2PAGE(reg);
	}
}

static int
reg_read_range(struct tda998x_encoder *encoder,
	uint16_t reg, char *buf, int cnt)
{
	struct i2c_client *client = tda998x_i2c_encoder_get_client(encoder);
	uint8_t addr = REG2ADDR(reg);
	int ret;

	set_page(encoder, reg);

	ret = i2c_master_send(client, &addr, sizeof(addr));
	if (ret < 0)
		goto fail;

	ret = i2c_master_recv(client, buf, cnt);
	if (ret < 0)
		goto fail;

	return ret;

fail:
	dev_err(&client->dev, "Error %d reading from 0x%x\n", ret, reg);
	return ret;
}

static uint8_t
reg_read(struct tda998x_encoder *encoder, uint16_t reg)
{
	uint8_t val = 0;
	reg_read_range(encoder, reg, &val, sizeof(val));
	return val;
}

static void
reg_write(struct tda998x_encoder *encoder, uint16_t reg, uint8_t val)
{
	struct i2c_client *client = tda998x_i2c_encoder_get_client(encoder);
	uint8_t buf[] = {REG2ADDR(reg), val};
	int ret;

	set_page(encoder, reg);

	ret = i2c_master_send(client, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		dev_err(&client->dev, "Error %d writing to 0x%x\n", ret, reg);
}

static void
reg_write16(struct tda998x_encoder *encoder, uint16_t reg, uint16_t val)
{
	struct i2c_client *client = tda998x_i2c_encoder_get_client(encoder);
	uint8_t buf[] = {REG2ADDR(reg), val >> 8, val};
	int ret;

	set_page(encoder, reg);

	ret = i2c_master_send(client, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		dev_err(&client->dev, "Error %d writing to 0x%x\n", ret, reg);
}

static void
reg_set(struct tda998x_encoder *encoder, uint16_t reg, uint8_t val)
{
	reg_write(encoder, reg, reg_read(encoder, reg) | val);
}

static void
reg_clear(struct tda998x_encoder *encoder, uint16_t reg, uint8_t val)
{
	reg_write(encoder, reg, reg_read(encoder, reg) & ~val);
}

static void
tda998x_reset(struct tda998x_encoder *encoder)
{
	/* reset audio and i2c master: */
	reg_set(encoder, REG_SOFTRESET, SOFTRESET_AUDIO | SOFTRESET_I2C_MASTER);
	msleep(50);
	reg_clear(encoder, REG_SOFTRESET,
		SOFTRESET_AUDIO | SOFTRESET_I2C_MASTER);
	msleep(50);

	/* reset transmitter: */
	reg_set(encoder, REG_MAIN_CNTRL0, MAIN_CNTRL0_SR);
	reg_clear(encoder, REG_MAIN_CNTRL0, MAIN_CNTRL0_SR);

	/* PLL registers common configuration */
	reg_write(encoder, REG_PLL_SERIAL_1, 0x00);
	reg_write(encoder, REG_PLL_SERIAL_2, PLL_SERIAL_2_SRL_NOSC(1));
	reg_write(encoder, REG_PLL_SERIAL_3, 0x00);
	reg_write(encoder, REG_SERIALIZER,   0x00);
	reg_write(encoder, REG_BUFFER_OUT,   0x00);
	reg_write(encoder, REG_PLL_SCG1,     0x00);
	reg_write(encoder, REG_AUDIO_DIV,    0x03);
	reg_write(encoder, REG_SEL_CLK,
		SEL_CLK_SEL_CLK1 | SEL_CLK_ENA_SC_CLK);
	reg_write(encoder, REG_PLL_SCGN1,    0xfa);
	reg_write(encoder, REG_PLL_SCGN2,    0x00);
	reg_write(encoder, REG_PLL_SCGR1,    0x5b);
	reg_write(encoder, REG_PLL_SCGR2,    0x00);
	reg_write(encoder, REG_PLL_SCG2,     0x10);
}

struct tda_mode {
	uint32_t clock;
	uint32_t vrefresh;
	uint32_t hdisplay;
	uint32_t hsync_start;
	uint32_t hsync_end;
	uint32_t htotal;
	uint32_t vdisplay;
	uint32_t vsync_start;
	uint32_t vsync_end;
	uint32_t vtotal;
	uint32_t flags;
	uint32_t hskew;
};

static void convert_to_display_mode(struct tda_mode *mode,
			struct fb_videomode *timing)
{
	mode->clock = (PICOS2KHZ(timing->pixclock)/10)*10;
	mode->vrefresh = timing->refresh;

	mode->hdisplay = timing->xres;
	mode->hsync_start = mode->hdisplay + timing->right_margin;
	mode->hsync_end = mode->hsync_start + timing->hsync_len;
	mode->htotal = mode->hsync_end + timing->left_margin;

	mode->vdisplay = timing->yres;
	mode->vsync_start = mode->vdisplay + timing->lower_margin;
	mode->vsync_end = mode->vsync_start + timing->vsync_len;
	mode->vtotal = mode->vsync_end + timing->upper_margin;

	mode->flags = timing->sync;



	pr_debug("mode->htotal %d, mode->vtotal %d, mode->flags %x\n",
		mode->htotal, mode->vtotal, mode->flags);
	pr_debug("mode->clock %d\n", mode->clock);
	pr_debug("mode->vrefresh %d\n", mode->vrefresh);
	pr_debug("mode->hdisplay %d\n", mode->hdisplay);
	pr_debug("mode->hsync_start %d\n", mode->hsync_start);
	pr_debug("mode->hsync_end %d\n", mode->hsync_end);
	pr_debug("mode->vdisplay %d\n", mode->vdisplay);
	pr_debug("mode->vsync_start %d\n", mode->vsync_start);
	pr_debug("mode->vsync_end %d\n", mode->vsync_end);

	/*
	 * this is a workaround to fix up the mode so that the non-vesa
	 * compliant LCD controller can work with the NXP HDMI encoder
	 * we invert the horizontal sync pulse, and then add some hskew
	 * to move the picture to the right on the screen by a sync pulse
	 * worth of pixels
	 */
	mode->hskew = mode->hsync_end - mode->hsync_start;
	mode->flags ^= FB_SYNC_HOR_HIGH_ACT;

	pr_debug("mode->hskew %d\n", mode->hskew);

}


void da8xx_tda998x_setmode(struct tda998x_encoder *encoder,
			struct fb_videomode *vid_mode)
{
	struct tda998x_priv *priv = to_tda998x_priv(encoder);
	uint16_t ref_pix, ref_line, n_pix, n_line;
	uint16_t hs_pix_s, hs_pix_e;
	uint16_t vs1_pix_s, vs1_pix_e, vs1_line_s, vs1_line_e;
	uint16_t vs2_pix_s, vs2_pix_e, vs2_line_s, vs2_line_e;
	uint16_t vwin1_line_s, vwin1_line_e;
	uint16_t vwin2_line_s, vwin2_line_e;
	uint16_t de_pix_s, de_pix_e;
	uint8_t reg, div, rep;
	struct tda_mode tda_mode;
	struct tda_mode *mode = &tda_mode;

	convert_to_display_mode(mode, vid_mode);

	/*
	 * Internally TDA998x is using ITU-R BT.656 style sync but
	 * we get VESA style sync. TDA998x is using a reference pixel
	 * relative to ITU to sync to the input frame and for output
	 * sync generation.
	 *
	 * Now there is some issues to take care of:
	 * - HDMI data islands require sync-before-active
	 * - TDA998x register values must be > 0 to be enabled
	 * - REFLINE needs an additional offset of +1
	 * - REFPIX needs an addtional offset of +1 for UYUV and +3 for RGB
	 *
	 * So we add +1 to all horizontal and vertical register values,
	 * plus an additional +3 for REFPIX as we are using RGB input only.
	 */
	n_pix        = mode->htotal;
	n_line       = mode->vtotal;

	ref_pix      = 3 + mode->hsync_start - mode->hdisplay;

	/*
	 * handle issue on TILCDC where it is outputing
	 * non-VESA compliant sync signals the workaround
	 * forces us to invert the HSYNC, so need to adjust display to
	 * the left by hskew pixels, provided by the tilcdc driver
	 */
	ref_pix += mode->hskew;

	de_pix_s     = mode->htotal - mode->hdisplay;
	de_pix_e     = de_pix_s + mode->hdisplay;
	hs_pix_s     = mode->hsync_start - mode->hdisplay;
	hs_pix_e     = hs_pix_s + mode->hsync_end - mode->hsync_start;

	ref_line     = 1 + mode->vsync_start - mode->vdisplay;
	vwin1_line_s = mode->vtotal - mode->vdisplay - 1;
	vwin1_line_e = vwin1_line_s + mode->vdisplay;
	vs1_pix_s    = vs1_pix_e = hs_pix_s;
	vs1_line_s   = mode->vsync_start - mode->vdisplay;

	vs1_line_e   = vs1_line_s +
		mode->vsync_end - mode->vsync_start;

	vwin2_line_s = vwin2_line_e = 0;
	vs2_pix_s    = vs2_pix_e  = 0;
	vs2_line_s   = vs2_line_e = 0;

	div = 148500 / mode->clock;

	/* Setup the VIP mappings, enable audio and video ports */
	reg_write(encoder, REG_ENA_AP, 0xff);
	reg_write(encoder, REG_ENA_VP_0, 0xff);
	reg_write(encoder, REG_ENA_VP_1, 0xff);
	reg_write(encoder, REG_ENA_VP_2, 0xff);
	/* set muxing after enabling ports: */
	reg_write(encoder, REG_VIP_CNTRL_0,
		VIP_CNTRL_0_SWAP_A(2) | VIP_CNTRL_0_SWAP_B(3));
	reg_write(encoder, REG_VIP_CNTRL_1,
		VIP_CNTRL_1_SWAP_C(4) | VIP_CNTRL_1_SWAP_D(5));
	reg_write(encoder, REG_VIP_CNTRL_2,
		VIP_CNTRL_2_SWAP_E(0) | VIP_CNTRL_2_SWAP_F(1));


	/* mute the audio FIFO: */
	reg_set(encoder, REG_AIP_CNTRL_0, AIP_CNTRL_0_RST_FIFO);

	/* set HDMI HDCP mode off: */
	reg_set(encoder, REG_TBG_CNTRL_1, TBG_CNTRL_1_DWIN_DIS);
	reg_clear(encoder, REG_TX33, TX33_HDMI);

	reg_write(encoder, REG_ENC_CNTRL, ENC_CNTRL_CTL_CODE(0));
	/* no pre-filter or interpolator: */
	reg_write(encoder, REG_HVF_CNTRL_0, HVF_CNTRL_0_PREFIL(0) |
			HVF_CNTRL_0_INTPOL(0));
	reg_write(encoder, REG_VIP_CNTRL_5, VIP_CNTRL_5_SP_CNT(0));
	reg_write(encoder, REG_VIP_CNTRL_4, VIP_CNTRL_4_BLANKIT(0) |
			VIP_CNTRL_4_BLC(0));
	reg_clear(encoder, REG_PLL_SERIAL_3, PLL_SERIAL_3_SRL_CCIR);

	reg_clear(encoder, REG_PLL_SERIAL_1, PLL_SERIAL_1_SRL_MAN_IZ);
	reg_clear(encoder, REG_PLL_SERIAL_3, PLL_SERIAL_3_SRL_DE);
	reg_write(encoder, REG_SERIALIZER, 0);
	reg_write(encoder, REG_HVF_CNTRL_1, HVF_CNTRL_1_VQR(0));

	/* TODO enable pixel repeat for pixel rates less than 25Msamp/s */
	rep = 0;
	reg_write(encoder, REG_RPT_CNTRL, 0);
	reg_write(encoder, REG_SEL_CLK, SEL_CLK_SEL_VRF_CLK(0) |
			SEL_CLK_SEL_CLK1 | SEL_CLK_ENA_SC_CLK);

	reg_write(encoder, REG_PLL_SERIAL_2, PLL_SERIAL_2_SRL_NOSC(div) |
			PLL_SERIAL_2_SRL_PR(rep));

	/* set color matrix bypass flag: */
	reg_set(encoder, REG_MAT_CONTRL, MAT_CONTRL_MAT_BP);

	/* set BIAS tmds value: */
	reg_write(encoder, REG_ANA_GENERAL, 0x09);

	reg_clear(encoder, REG_TBG_CNTRL_0, TBG_CNTRL_0_SYNC_MTHD);

	/*
	 * Sync on rising HSYNC/VSYNC
	 */
	reg_write(encoder, REG_VIP_CNTRL_3, 0);
	reg_set(encoder, REG_VIP_CNTRL_3, VIP_CNTRL_3_SYNC_HS);

	/*
	 * TDA19988 requires high-active sync at input stage,
	 * so invert low-active sync provided by master encoder here
	 */
	if ((mode->flags & FB_SYNC_HOR_HIGH_ACT) == 0)
		reg_set(encoder, REG_VIP_CNTRL_3, VIP_CNTRL_3_H_TGL);
	if ((mode->flags & FB_SYNC_VERT_HIGH_ACT) == 0)
		reg_set(encoder, REG_VIP_CNTRL_3, VIP_CNTRL_3_V_TGL);

	/*
	 * Always generate sync polarity relative to input sync and
	 * revert input stage toggled sync at output stage
	 */
	reg = TBG_CNTRL_1_TGL_EN;
	if ((mode->flags & FB_SYNC_HOR_HIGH_ACT) == 0)
		reg |= TBG_CNTRL_1_H_TGL;
	if ((mode->flags & FB_SYNC_VERT_HIGH_ACT) == 0)
		reg |= TBG_CNTRL_1_V_TGL;
	reg_write(encoder, REG_TBG_CNTRL_1, reg);

	reg_write(encoder, REG_VIDFORMAT, 0x00);
	reg_write16(encoder, REG_REFPIX_MSB, ref_pix);
	reg_write16(encoder, REG_REFLINE_MSB, ref_line);
	reg_write16(encoder, REG_NPIX_MSB, n_pix);
	reg_write16(encoder, REG_NLINE_MSB, n_line);
	reg_write16(encoder, REG_VS_LINE_STRT_1_MSB, vs1_line_s);
	reg_write16(encoder, REG_VS_PIX_STRT_1_MSB, vs1_pix_s);
	reg_write16(encoder, REG_VS_LINE_END_1_MSB, vs1_line_e);
	reg_write16(encoder, REG_VS_PIX_END_1_MSB, vs1_pix_e);
	reg_write16(encoder, REG_VS_LINE_STRT_2_MSB, vs2_line_s);
	reg_write16(encoder, REG_VS_PIX_STRT_2_MSB, vs2_pix_s);
	reg_write16(encoder, REG_VS_LINE_END_2_MSB, vs2_line_e);
	reg_write16(encoder, REG_VS_PIX_END_2_MSB, vs2_pix_e);
	reg_write16(encoder, REG_HS_PIX_START_MSB, hs_pix_s);
	reg_write16(encoder, REG_HS_PIX_STOP_MSB, hs_pix_e);
	reg_write16(encoder, REG_VWIN_START_1_MSB, vwin1_line_s);
	reg_write16(encoder, REG_VWIN_END_1_MSB, vwin1_line_e);
	reg_write16(encoder, REG_VWIN_START_2_MSB, vwin2_line_s);
	reg_write16(encoder, REG_VWIN_END_2_MSB, vwin2_line_e);
	reg_write16(encoder, REG_DE_START_MSB, de_pix_s);
	reg_write16(encoder, REG_DE_STOP_MSB, de_pix_e);

	if (priv->rev == TDA19988) {
		/* let incoming pixels fill the active space (if any) */
		reg_write(encoder, REG_ENABLE_SPACE, 0x01);
	}

	/* must be last register set: */
	reg_clear(encoder, REG_TBG_CNTRL_0, TBG_CNTRL_0_SYNC_ONCE);

}

/* I2C driver functions */
static int
da8xx_tda998x_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct tda998x_priv *priv;
	struct da8xx_encoder *encoder;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->current_page = 0;

	encoder = kzalloc(sizeof(*encoder), GFP_KERNEL);
	if (!encoder) {
		kfree(priv);
		return -ENOMEM;
	}

	priv->cec = i2c_new_dummy(client->adapter, 0x34);

	encoder->client = client;
	encoder->priv = priv;
	encoder->node = client->dev.of_node;
	encoder->set_mode = da8xx_tda998x_setmode;


	/* wake up the device: */
	cec_write(encoder, REG_CEC_ENAMODS,
			CEC_ENAMODS_EN_RXSENS | CEC_ENAMODS_EN_HDMI);

	tda998x_reset(encoder);

	/* read version: */
	priv->rev = reg_read(encoder, REG_VERSION_LSB) |
			reg_read(encoder, REG_VERSION_MSB) << 8;

	/* mask off feature bits: */
	priv->rev &= ~0x30; /* not-hdcp and not-scalar bit */

	switch (priv->rev) {
	case TDA9989N2:
		dev_info(&client->dev, "found TDA9989 n2");
		break;
	case TDA19989:
		dev_info(&client->dev, "found TDA19989");
		break;
	case TDA19989N2:
		dev_info(&client->dev, "found TDA19989 n2");
		break;
	case TDA19988:
		dev_info(&client->dev, "found TDA19988");
		break;
	default:
		DBG("found unsupported device: %04x", priv->rev);
		goto fail;
	}

	da8xx_register_encoder(encoder);

	/* after reset, enable DDC: */
	reg_write(encoder, REG_DDC_DISABLE, 0x00);

	/* set clock on DDC channel: */
	reg_write(encoder, REG_TX3, 39);

	/* if necessary, disable multi-master: */
	if (priv->rev == TDA19989)
		reg_set(encoder, REG_I2C_MASTER, I2C_MASTER_DIS_MM);

	cec_write(encoder, REG_CEC_FRO_IM_CLK_CTRL,
			CEC_FRO_IM_CLK_CTRL_GHOST_DIS |
				CEC_FRO_IM_CLK_CTRL_IMCLK_SEL);

	i2c_set_clientdata(client, encoder);

	return 0;

fail:
	/* if encoder_init fails, the encoder slave is never registered,
	 * so cleanup here:
	 */
	if (priv->cec)
		i2c_unregister_device(priv->cec);

	kfree(priv);
	kfree(encoder);
	return -ENXIO;
}

static int
da8xx_tda998x_remove(struct i2c_client *client)
{
	struct da8xx_encoder *da8xx_encoder;
	struct tda998x_priv *priv;

	da8xx_encoder = i2c_get_clientdata(client);
	if (da8xx_encoder) {
		da8xx_unregister_encoder(da8xx_encoder);
		priv = to_tda998x_priv(da8xx_encoder);
		if (priv->cec) {
			/* disable the device: */
			cec_write(da8xx_encoder, REG_CEC_ENAMODS, 0);
			i2c_unregister_device(priv->cec);
		}
		kfree(da8xx_encoder->priv);
		kfree(da8xx_encoder);
	}
	return 0;
}


static struct i2c_device_id da8xx_tda998x_ids[] = {
	{ "tda998x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, da8xx_tda998x_ids);

static struct i2c_driver da8xx_tda998x_driver = {
	.probe = da8xx_tda998x_probe,
	.remove = da8xx_tda998x_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "tda998x",
	},
	.id_table = da8xx_tda998x_ids,
};

module_i2c_driver(da8xx_tda998x_driver);

MODULE_DESCRIPTION("NXP TDA998x HDMI encoder driver for TI AM335x/DA8xx");
MODULE_AUTHOR("Texas Instruments");
MODULE_LICENSE("GPL");
