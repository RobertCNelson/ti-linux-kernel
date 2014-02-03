/*
 * drivers/video/omap2/displays-new/encoder-sil9022.c
 *
 * Copyright (C) 2013 Texas Instruments
 * Author : Sathya Prakash M R <sathyap@ti.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 *
 */

#ifndef _SI9022_H_
#define _SI9022_H_

#define SIL9022_DRV_NAME	"sii9022"

#define SIL9022_REG_CHIPID0		0x1B
#define SIL9022_REG_TPI_RQB		0xC7
#define SIL9022_CHIPID_902x		0xB0

#define HDMI_I2C_MONITOR_ADDRESS            0x50

/* HDMI EDID Length  */
#define HDMI_EDID_MAX_LENGTH	256

#define VERTICAL_FREQ			0x3C

/*  HDMI TPI Registers  */
#define HDMI_TPI_VIDEO_DATA_BASE_REG	0x00
#define HDMI_TPI_PIXEL_CLK_LSB_REG	 (HDMI_TPI_VIDEO_DATA_BASE_REG + 0x00)
#define HDMI_TPI_PIXEL_CLK_MSB_REG (HDMI_TPI_VIDEO_DATA_BASE_REG + 0x01)
#define HDMI_TPI_VFREQ_LSB_REG	(HDMI_TPI_VIDEO_DATA_BASE_REG + 0x02)
#define HDMI_TPI_VFREQ_MSB_REG	(HDMI_TPI_VIDEO_DATA_BASE_REG + 0x03)
#define HDMI_TPI_PIXELS_LSB_REG	(HDMI_TPI_VIDEO_DATA_BASE_REG + 0x04)
#define HDMI_TPI_PIXELS_MSB_REG	(HDMI_TPI_VIDEO_DATA_BASE_REG + 0x05)
#define HDMI_TPI_LINES_LSB_REG		(HDMI_TPI_VIDEO_DATA_BASE_REG + 0x06)
#define HDMI_TPI_LINES_MSB_REG		(HDMI_TPI_VIDEO_DATA_BASE_REG + 0x07)

#define HDMI_TPI_PIXEL_REPETITION_REG	0x08

#define HDMI_TPI_AVI_INOUT_BASE_REG	0x09
#define HDMI_TPI_AVI_IN_FORMAT_REG	(HDMI_TPI_AVI_INOUT_BASE_REG + 0x00)
#define HDMI_TPI_AVI_OUT_FORMAT_REG	(HDMI_TPI_AVI_INOUT_BASE_REG + 0x01)

#define HDMI_SYS_CTRL_DATA_REG		0x1A
#define HDMI_TPI_POWER_STATE_CTRL_REG		0x1E
#define HDMI_TPI_DEVICE_POWER_STATE_DATA		0x1E


/* HDCP */
#define HDMI_TPI_HDCP_QUERYDATA_REG         0x29
#define HDMI_TPI_HDCP_CONTROLDATA_REG       0x2A

/* HDMI_TPI_DEVICE_ID_REG  */
#define TPI_DEVICE_ID                       0xB0

/* HDMI_TPI_REVISION_REG  */
#define TPI_REVISION                        0x00

/* HDMI_TPI_ID_BYTE2_REG  */
#define TPI_ID_BYTE2_VALUE                  0x00

/* HDMI_SYS_CTRL_DATA_REG  */
#define TPI_SYS_CTRL_POWER_DOWN             (1 << 4)
#define TPI_SYS_CTRL_POWER_ACTIVE           (0 << 4)
#define TPI_SYS_CTRL_AV_MUTE                (1 << 3)
#define TPI_SYS_CTRL_DDC_BUS_REQUEST        (1 << 2)
#define TPI_SYS_CTRL_DDC_BUS_GRANTED        (1 << 1)
#define TPI_SYS_CTRL_OUTPUT_MODE_HDMI       (1 << 0)
#define TPI_SYS_CTRL_OUTPUT_MODE_DVI        (0 << 0)

/* HDMI_TPI_PIXEL_REPETITION  */
#define TPI_AVI_PIXEL_REP_BUS_24BIT         (1 << 5)
#define TPI_AVI_PIXEL_REP_BUS_12BIT         (0 << 5)
#define TPI_AVI_PIXEL_REP_RISING_EDGE       (1 << 4)
#define TPI_AVI_PIXEL_REP_FALLING_EDGE      (0 << 4)
#define TPI_AVI_PIXEL_REP_4X                (3 << 0)
#define TPI_AVI_PIXEL_REP_2X                (1 << 0)
#define TPI_AVI_PIXEL_REP_NONE              (0 << 0)

/*Ratio of TDMS Clock to input Video Clock*/
#define TPI_CLK_RATIO_HALF		(0 << 6)
#define TPI_CLK_RATIO_1X		(1 << 6)
#define TPI_CLK_RATIO_2X              (2 << 6)
#define TPI_CLK_RATIO_4X             (3 << 6)


/* HDMI_TPI_AVI_INPUT_FORMAT  */
#define TPI_AVI_INPUT_BITMODE_12BIT         (1 << 7)
#define TPI_AVI_INPUT_BITMODE_8BIT          (0 << 7)
#define TPI_AVI_INPUT_DITHER                (1 << 6)
#define TPI_AVI_INPUT_RANGE_LIMITED         (2 << 2)
#define TPI_AVI_INPUT_RANGE_FULL            (1 << 2)
#define TPI_AVI_INPUT_RANGE_AUTO            (0 << 2)
#define TPI_AVI_INPUT_COLORSPACE_BLACK      (3 << 0)
#define TPI_AVI_INPUT_COLORSPACE_YUV422     (2 << 0)
#define TPI_AVI_INPUT_COLORSPACE_YUV444     (1 << 0)
#define TPI_AVI_INPUT_COLORSPACE_RGB        (0 << 0)


/* HDMI_TPI_AVI_OUTPUT_FORMAT  */
#define TPI_AVI_OUTPUT_CONV_BT709           (1 << 4)
#define TPI_AVI_OUTPUT_CONV_BT601           (0 << 4)
#define TPI_AVI_OUTPUT_RANGE_LIMITED        (2 << 2)
#define TPI_AVI_OUTPUT_RANGE_FULL           (1 << 2)
#define TPI_AVI_OUTPUT_RANGE_AUTO           (0 << 2)
#define TPI_AVI_OUTPUT_COLORSPACE_RGBDVI    (3 << 0)
#define TPI_AVI_OUTPUT_COLORSPACE_YUV422    (2 << 0)
#define TPI_AVI_OUTPUT_COLORSPACE_YUV444    (1 << 0)
#define TPI_AVI_OUTPUT_COLORSPACE_RGBHDMI   (0 << 0)

/* HDMI_TPI_DEVICE_POWER_STATE  */
#define TPI_AVI_POWER_STATE_D3              (3 << 0)
#define TPI_AVI_POWER_STATE_D2              (2 << 0)
#define TPI_AVI_POWER_STATE_D0              (0 << 0)

#endif
