/*
 * ALSA SoC TLV320AIC31XX codec driver
 *
 * Copyright (C) 2013 Texas Instruments, Inc.
 *
 * Author: Jyri Sarha <jsarha@ti.com>
 *
 * Based on ground work by: Ajit Kulkarni <x0175765@ti.com>
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED AS IS AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * The TLV320AIC31xx series of audio codec is a low-power, highly integrated
 * high performance codec which provides a stereo DAC, a mono ADC,
 * and mono/stereo Class-D speaker driver.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "tlv320aic31xx.h"

static const struct regmap_range_cfg aic31xx_ranges[] = {
	{
		.name = "codec-regmap",
		.range_min = 128,
		.range_max = 13 * 128,
		.selector_reg = 0,
		.selector_mask = 0xff,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 128,
	},
};

struct regmap_config aicxxx_i2c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
	.ranges = aic31xx_ranges,
	.num_ranges = ARRAY_SIZE(aic31xx_ranges),
	.max_register =	 13 * 128,
};

#define AIC31XX_NUM_SUPPLIES	6
static const char * const aic31xx_supply_names[] = {
	"HPVDD",
	"SPRVDD",
	"SPLVDD",
	"AVDD",
	"IOVDD",
	"DVDD",
};

struct aic31xx_disable_nb {
	struct notifier_block nb;
	struct aic31xx_priv *aic31xx;
};

struct aic31xx_priv {
	struct snd_soc_codec *codec;
	u8 i2c_regs_status;
	struct device *dev;
	struct regmap *regmap;
	struct aic31xx_pdata pdata;
	struct regulator_bulk_data supplies[AIC31XX_NUM_SUPPLIES];
	struct aic31xx_disable_nb disable_nb[AIC31XX_NUM_SUPPLIES];
	int power;
	unsigned int sysclk;
};

struct aic31xx_rate_divs {
	u32 mclk;
	u32 rate;
	u8 p_val;
	u8 pll_j;
	u16 pll_d;
	u16 dosr;
	u8 ndac;
	u8 mdac;
	u8 aosr;
	u8 nadc;
	u8 madc;
	u8 bclk_n;
};

static const struct aic31xx_rate_divs aic31xx_divs[] = {
	/* mclk	   rate	  p  j	   d dosr  nd md aors  na  ma blck_n */
	/* 8k rate */
	{12000000, 8000,  1, 7, 6800, 768,  5, 3, 128,	5, 18, 24},
	{24000000, 8000,  2, 7, 6800, 768, 15, 1,  64, 45,  4, 24},
	{25000000, 8000,  2, 7, 3728, 768, 15, 1,  64, 45,  4, 24},
	/* 11.025k rate */
	{12000000, 11025, 1, 7, 5264, 512,  8, 2, 128,	8,  8, 16},
	{24000000, 11025, 2, 7, 5264, 512, 16, 1,  64, 32,  4, 16},
	/* 16k rate */
	{12000000, 16000, 1, 7, 6800, 384,  5, 3, 128,	5,  9, 12},
	{24000000, 16000, 2, 7, 6800, 384, 15, 1,  64, 18,  5, 12},
	{25000000, 16000, 2, 7, 3728, 384, 15, 1,  64, 18,  5, 12},
	/* 22.05k rate */
	{12000000, 22050, 1, 7, 5264, 256,  4, 4, 128,	4,  8,	8},
	{24000000, 22050, 2, 7, 5264, 256, 16, 1,  64, 16,  4,	8},
	{25000000, 22050, 2, 7, 2253, 256, 16, 1,  64, 16,  4,	8},
	/* 32k rate */
	{12000000, 32000, 1, 7, 1680, 192,  2, 7,  64,	2, 21,	6},
	{24000000, 32000, 2, 7, 1680, 192,  7, 2,  64,	7,  6,	6},
	/* 44.1k rate */
	{12000000, 44100, 1, 7, 5264, 128,  2, 8, 128,	2,  8,	4},
	{24000000, 44100, 2, 7, 5264, 128,  8, 2,  64,	8,  4,	4},
	{25000000, 44100, 2, 7, 2253, 128,  8, 2,  64,	8,  4,	4},
	/* 48k rate */
	{12000000, 48000, 1, 8, 1920, 128,  2, 8, 128,	2,  8,	4},
	{24000000, 48000, 2, 8, 1920, 128,  8, 2,  64,	8,  4,	4},
	{25000000, 48000, 2, 7, 8643, 128,  8, 2,  64,	8,  4,	4},
};

static const char * const ldac_in_text[] = {
	"off", "Left Data", "Right Data", "Mono"
};

static const char * const rdac_in_text[] = {
	"off", "Right Data", "Left Data", "Mono"
};

static SOC_ENUM_SINGLE_DECL(ldac_in_enum, AIC31XX_DACSETUP, 4, ldac_in_text);

static SOC_ENUM_SINGLE_DECL(rdac_in_enum, AIC31XX_DACSETUP, 2, rdac_in_text);

static const char * const mic_select_text[] = {
	"off", "FFR 10 Ohm", "FFR 20 Ohm", "FFR 40 Ohm"
};

static const
SOC_ENUM_SINGLE_DECL(mic1lp_p_enum, AIC31XX_MICPGAPI, 6, mic_select_text);
static const
SOC_ENUM_SINGLE_DECL(mic1rp_p_enum, AIC31XX_MICPGAPI, 4, mic_select_text);
static const
SOC_ENUM_SINGLE_DECL(mic1lm_p_enum, AIC31XX_MICPGAPI, 2, mic_select_text);

static const
SOC_ENUM_SINGLE_DECL(cm_m_enum, AIC31XX_MICPGAMI, 6, mic_select_text);
static const
SOC_ENUM_SINGLE_DECL(mic1lm_m_enum, AIC31XX_MICPGAMI, 4, mic_select_text);

static const DECLARE_TLV_DB_SCALE(dac_vol_tlv, -6350, 50, 0);
static const DECLARE_TLV_DB_SCALE(adc_fgain_tlv, 0, 10, 0);
static const DECLARE_TLV_DB_SCALE(adc_cgain_tlv, -2000, 50, 0);
static const DECLARE_TLV_DB_SCALE(mic_pga_tlv, 0, 50, 0);
static const DECLARE_TLV_DB_SCALE(hp_drv_tlv, 0, 100, 0);
static const DECLARE_TLV_DB_SCALE(class_D_drv_tlv, 600, 600, 0);
static const DECLARE_TLV_DB_SCALE(hp_vol_tlv, -6350, 50, 0);
static const DECLARE_TLV_DB_SCALE(sp_vol_tlv, -6350, 50, 0);

/*
 * controls to be exported to the user space
 */
static const struct snd_kcontrol_new aic31xx_snd_controls[] = {
	/* DAC Volume Control*/
	SOC_DOUBLE_R_SX_TLV("DAC Playback Volume", AIC31XX_LDACVOL,
			    AIC31XX_RDACVOL, 0, 0x81, 0xaf, dac_vol_tlv),
	/* HP driver mute control */
	SOC_DOUBLE_R("HP Driver Playback Switch", AIC31XX_HPLGAIN,
		     AIC31XX_HPRGAIN, 2, 1, 0),

	/* ADC FINE GAIN */
	SOC_SINGLE_TLV("ADC Fine Capture Volume", AIC31XX_ADCFGA, 4, 4, 1,
		       adc_fgain_tlv),

	/* ADC mute control */
	SOC_SINGLE("ADC Capture Switch", AIC31XX_ADCFGA, 7, 1, 1),

	/* ADC COARSE GAIN */
	SOC_DOUBLE_R_SX_TLV("ADC Capture Volume", AIC31XX_ADCVOL,
			    AIC31XX_ADCVOL,	0, 0x28, 0x40,
			    adc_cgain_tlv),
	/* ADC MIC PGA GAIN */
	SOC_SINGLE_TLV("Mic PGA Capture Volume", AIC31XX_MICPGA, 0,
		       119, 0, mic_pga_tlv),

	/* HP driver Volume Control */
	SOC_DOUBLE_R_TLV("HP Driver Playback Volume", AIC31XX_HPLGAIN,
			 AIC31XX_HPRGAIN, 3, 0x09, 0, hp_drv_tlv),
	/* Left DAC input selection control */

	/* Throughput of 7-bit vol ADC for pin control */
	/* HP Analog Gain Volume Control */
	SOC_DOUBLE_R_TLV("HP Analog Playback Volume", AIC31XX_LANALOGHPL,
			 AIC31XX_RANALOGHPR, 0, 0x7F, 1, hp_vol_tlv),
};

static const struct snd_kcontrol_new aic311x_snd_controls[] = {
	/* SP Class-D driver output stage gain Control */
	SOC_DOUBLE_R_TLV("SP Driver Playback Volume", AIC31XX_SPLGAIN,
			 AIC31XX_SPRGAIN, 3, 0x04, 0, class_D_drv_tlv),
	/* SP Analog Gain Volume Control */
	SOC_DOUBLE_R_TLV("SP Analog Playback Volume", AIC31XX_LANALOGSPL,
			 AIC31XX_RANALOGSPR, 0, 0x7F, 1, sp_vol_tlv),
	/* SP driver mute control */
	SOC_DOUBLE_R("SP Driver Playback Switch", AIC31XX_SPLGAIN,
		     AIC31XX_SPRGAIN, 2, 1, 0),
};

static const struct snd_kcontrol_new aic310x_snd_controls[] = {
	/* SP Class-D driver output stage gain Control */
	SOC_SINGLE_TLV("SP Driver Playback Volume", AIC31XX_SPLGAIN,
		       3, 0x04, 0, class_D_drv_tlv),
	/* SP Analog Gain Volume Control */
	SOC_SINGLE_TLV("SP Analog Playback Volume", AIC31XX_LANALOGSPL,
		       0, 0x7F, 1, sp_vol_tlv),
	SOC_SINGLE("SP Driver Playback Switch", AIC31XX_SPLGAIN,
		   2, 1, 0),
};

static const struct snd_kcontrol_new ldac_in_control =
	SOC_DAPM_ENUM("DAC Left Input", ldac_in_enum);

static const struct snd_kcontrol_new rdac_in_control =
	SOC_DAPM_ENUM("DAC Right Input", rdac_in_enum);

int aic31xx_wait_bits(struct aic31xx_priv *aic31xx, unsigned int reg,
		      unsigned int mask, unsigned int wbits, int sleep,
		      int count)
{
	unsigned int bits;
	int counter = count;
	int ret = regmap_read(aic31xx->regmap, reg, &bits);
	while ((bits & mask) != wbits && counter && !ret) {
		usleep_range(sleep, sleep * 2);
		ret = regmap_read(aic31xx->regmap, reg, &bits);
		counter--;
	}
	if ((bits & mask) != wbits) {
		dev_err(aic31xx->dev,
			"%s: Failed! 0x%x was 0x%x expected 0x%x (%d, 0x%x, %d us)\n",
			__func__, reg, bits, wbits, ret, mask,
			(count - counter) * sleep);
		ret = -1;
	}
	return ret;
}

static int aic31xx_power_up_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(w->codec);
	unsigned int reg = AIC31XX_DACFLAG1;
	unsigned int mask;

	if (!strcmp(w->name, "DAC Left")) {
		mask = AIC31XX_LDACPWRSTATUS_MASK;
	} else if (!strcmp(w->name, "DAC Right")) {
		mask = AIC31XX_RDACPWRSTATUS_MASK;
	} else if (!strcmp(w->name, "HPL Driver")) {
		mask = AIC31XX_HPLDRVPWRSTATUS_MASK;
	} else if (!strcmp(w->name, "HPR Driver")) {
		mask = AIC31XX_HPRDRVPWRSTATUS_MASK;
	} else if (!strcmp(w->name, "SPL ClassD") ||
		   !strcmp(w->name, "SPK ClassD")) {
		mask = AIC31XX_SPLDRVPWRSTATUS_MASK;
	} else if (!strcmp(w->name, "SPR ClassD")) {
		mask = AIC31XX_SPRDRVPWRSTATUS_MASK;
	} else if (!strcmp(w->name, "ADC")) {
		mask = AIC31XX_ADCPWRSTATUS_MASK;
		reg = AIC31XX_ADCFLAG;
	} else {
		dev_err(w->codec->dev, "Unknown widget '%s' calling %s/n",
			w->name, __func__);
		return -1;
	}

	if (event == SND_SOC_DAPM_POST_PMU)
		return aic31xx_wait_bits(aic31xx, reg, mask, mask, 5000, 100);
	else if (event == SND_SOC_DAPM_POST_PMD)
		return aic31xx_wait_bits(aic31xx, reg, mask, 0, 5000, 100);

	dev_dbg(w->codec->dev, "Unhandled dapm widget event %d from %s\n",
		event, w->name);
	return 0;
}

static const struct snd_kcontrol_new left_output_switches[] = {
	SOC_DAPM_SINGLE("From Left DAC", AIC31XX_DACMIXERROUTE, 6, 1, 0),
	SOC_DAPM_SINGLE("From MIC1LP", AIC31XX_DACMIXERROUTE, 5, 1, 0),
	SOC_DAPM_SINGLE("From MIC1RP", AIC31XX_DACMIXERROUTE, 4, 1, 0),
};

static const struct snd_kcontrol_new right_output_switches[] = {
	SOC_DAPM_SINGLE("From Right DAC", AIC31XX_DACMIXERROUTE, 2, 1, 0),
	SOC_DAPM_SINGLE("From MIC1RP", AIC31XX_DACMIXERROUTE, 1, 1, 0),
};

static const struct snd_kcontrol_new p_term_mic1lp =
	SOC_DAPM_ENUM("MIC1LP P-Terminal", mic1lp_p_enum);

static const struct snd_kcontrol_new p_term_mic1rp =
	SOC_DAPM_ENUM("MIC1RP P-Terminal", mic1rp_p_enum);

static const struct snd_kcontrol_new p_term_mic1lm =
	SOC_DAPM_ENUM("MIC1LM P-Terminal", mic1lm_p_enum);

static const struct snd_kcontrol_new m_term_cm =
	SOC_DAPM_ENUM("CM M-Terminal", cm_m_enum);

static const struct snd_kcontrol_new m_term_mic1lm =
	SOC_DAPM_ENUM("MIC1LM M-Terminal", mic1lm_m_enum);

static const struct snd_kcontrol_new aic31xx_dapm_hpl_switch =
	SOC_DAPM_SINGLE("Switch", AIC31XX_LANALOGHPL, 7, 1, 0);

static const struct snd_kcontrol_new aic31xx_dapm_hpr_switch =
	SOC_DAPM_SINGLE("Switch", AIC31XX_RANALOGHPR, 7, 1, 0);

static const struct snd_kcontrol_new aic31xx_dapm_spl_switch =
	SOC_DAPM_SINGLE("Switch", AIC31XX_LANALOGSPL, 7, 1, 0);

static const struct snd_kcontrol_new aic31xx_dapm_spr_switch =
	SOC_DAPM_SINGLE("Switch", AIC31XX_RANALOGSPR, 7, 1, 0);

static int pll_power_on_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	if (SND_SOC_DAPM_EVENT_ON(event))
		dev_dbg(codec->dev, "pll->on pre_pmu");
	else if (SND_SOC_DAPM_EVENT_OFF(event))
		dev_dbg(codec->dev, "pll->off\n");

	mdelay(10);
	return 0;
}

static int mic_bias_event(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(codec);
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* change mic bias voltage to user defined */
		if (aic31xx->pdata.micbias_vg != AIC31XX_MICBIAS_OFF) {
			snd_soc_update_bits(codec, AIC31XX_MICBIAS,
					    AIC31XX_MICBIAS_MASK,
					    aic31xx->pdata.micbias_vg <<
					    AIC31XX_MICBIAS_SHIFT);
			dev_dbg(codec->dev, "%s: turned on\n", __func__);
		}
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (aic31xx->pdata.micbias_vg != AIC31XX_MICBIAS_OFF) {
			snd_soc_update_bits(codec, AIC31XX_MICBIAS,
					    AIC31XX_MICBIAS_MASK, 0);
			dev_dbg(codec->dev, "%s: turned off\n", __func__);
		}
		break;
	}
	return 0;
}

static const struct snd_soc_dapm_widget aic31xx_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("DAC IN", "DAC Playback", 0, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_MUX("DAC Left Input",
			 SND_SOC_NOPM, 0, 0, &ldac_in_control),
	SND_SOC_DAPM_MUX("DAC Right Input",
			 SND_SOC_NOPM, 0, 0, &rdac_in_control),
	/* DACs */
	SND_SOC_DAPM_DAC_E("DAC Left", "DAC Left Input",
			   AIC31XX_DACSETUP, 7, 0, aic31xx_power_up_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_DAC_E("DAC Right", "DAC Right Input",
			   AIC31XX_DACSETUP, 6, 0, aic31xx_power_up_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	/* Output Mixers */
	SND_SOC_DAPM_MIXER("Output Left", SND_SOC_NOPM, 0, 0,
			   left_output_switches,
			   ARRAY_SIZE(left_output_switches)),
	SND_SOC_DAPM_MIXER("Output Right", SND_SOC_NOPM, 0, 0,
			   right_output_switches,
			   ARRAY_SIZE(right_output_switches)),

	SND_SOC_DAPM_SWITCH("HP Left", SND_SOC_NOPM, 0, 0,
			    &aic31xx_dapm_hpl_switch),
	SND_SOC_DAPM_SWITCH("HP Right", SND_SOC_NOPM, 0, 0,
			    &aic31xx_dapm_hpr_switch),

	/* Output drivers */
	SND_SOC_DAPM_OUT_DRV_E("HPL Driver", AIC31XX_HPDRIVER, 7, 0,
			       NULL, 0, aic31xx_power_up_event,
			       SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_OUT_DRV_E("HPR Driver", AIC31XX_HPDRIVER, 6, 0,
			       NULL, 0, aic31xx_power_up_event,
			       SND_SOC_DAPM_POST_PMU),

	/* ADC */
	SND_SOC_DAPM_ADC_E("ADC", "Capture", AIC31XX_ADCSETUP, 7, 0,
			   aic31xx_power_up_event, SND_SOC_DAPM_POST_PMU |
			   SND_SOC_DAPM_POST_PMD),

	/* Input Selection to MIC_PGA */
	SND_SOC_DAPM_MUX("MIC1LP P-Terminal", SND_SOC_NOPM, 0, 0,
			 &p_term_mic1lp),
	SND_SOC_DAPM_MUX("MIC1RP P-Terminal", SND_SOC_NOPM, 0, 0,
			 &p_term_mic1rp),
	SND_SOC_DAPM_MUX("MIC1LM P-Terminal", SND_SOC_NOPM, 0, 0,
			 &p_term_mic1lm),

	SND_SOC_DAPM_MUX("CM M-Terminal", SND_SOC_NOPM, 0, 0, &m_term_cm),
	SND_SOC_DAPM_MUX("MIC1LM M-Terminal", SND_SOC_NOPM, 0, 0,
			 &m_term_mic1lm),
	/* Enabling & Disabling MIC Gain Ctl */
	SND_SOC_DAPM_PGA("MIC_GAIN_CTL", AIC31XX_MICPGA,
			 7, 1, NULL, 0),

	SND_SOC_DAPM_SUPPLY("PLLCLK", AIC31XX_PLLPR, 7, 0, pll_power_on_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("BCLKN_DIV", AIC31XX_BCLKN, 7, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("CODEC_CLK_IN", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("NDAC_DIV", AIC31XX_NDAC, 7, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MDAC_DIV", AIC31XX_MDAC, 7, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("NADC_DIV", AIC31XX_NADC, 7, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MADC_DIV", AIC31XX_MADC, 7, 0, NULL, 0),

	/* Mic Bias */
	SND_SOC_DAPM_SUPPLY("Mic Bias", SND_SOC_NOPM, 0, 0, mic_bias_event,
			    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	/* Outputs */
	SND_SOC_DAPM_OUTPUT("HPL"),
	SND_SOC_DAPM_OUTPUT("HPR"),

	/* Inputs */
	SND_SOC_DAPM_INPUT("MIC1LP"),
	SND_SOC_DAPM_INPUT("MIC1RP"),
	SND_SOC_DAPM_INPUT("MIC1LM"),
};

static const struct snd_soc_dapm_widget aic311x_dapm_widgets[] = {
	/* For AIC31XX and AIC3110 as it is stereo both left and right channel
	 * class-D can be powered up/down
	 */
	SND_SOC_DAPM_OUT_DRV_E("SPL ClassD", AIC31XX_SPKAMP, 7, 0, NULL, 0,
			       aic31xx_power_up_event, SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("SPR ClassD", AIC31XX_SPKAMP, 6, 0, NULL, 0,
			       aic31xx_power_up_event, SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SWITCH("SP Left", SND_SOC_NOPM, 0, 0,
			    &aic31xx_dapm_spl_switch),
	SND_SOC_DAPM_SWITCH("SP Right", SND_SOC_NOPM, 0, 0,
			    &aic31xx_dapm_spr_switch),
	SND_SOC_DAPM_OUTPUT("SPL"),
	SND_SOC_DAPM_OUTPUT("SPR"),
};

static const struct snd_soc_dapm_widget aic310x_dapm_widgets[] = {
	SND_SOC_DAPM_OUT_DRV_E("SPK ClassD", AIC31XX_SPKAMP, 7, 0, NULL, 0,
			       aic31xx_power_up_event, SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SWITCH("Speaker", SND_SOC_NOPM, 0, 0,
			    &aic31xx_dapm_spl_switch),
	SND_SOC_DAPM_OUTPUT("SPK"),
};

static const struct snd_soc_dapm_route
aic31xx_audio_map[] = {
	{"CODEC_CLK_IN", NULL, "PLLCLK"},
	{"CODEC_CLK_IN", NULL, "BCLKN_DIV"},
	{"CODEC_CLK_IN", NULL, "NDAC_DIV"},
	{"CODEC_CLK_IN", NULL, "NADC_DIV"},
	{"CODEC_CLK_IN", NULL, "MDAC_DIV"},
	{"CODEC_CLK_IN", NULL, "MADC_DIV"},

	/* Clocks for ADC */
	{"ADC", NULL, "CODEC_CLK_IN"},

	/* DAC Input Routing */
	{"DAC Left Input", "Left Data", "DAC IN"},
	{"DAC Left Input", "Right Data", "DAC IN"},
	{"DAC Left Input", "Mono", "DAC IN"},
	{"DAC Right Input", "Left Data", "DAC IN"},
	{"DAC Right Input", "Right Data", "DAC IN"},
	{"DAC Right Input", "Mono", "DAC IN"},
	{"DAC Left", NULL, "DAC Left Input"},
	{"DAC Right", NULL, "DAC Right Input"},

	/* Mic input */
	{"MIC1LP P-Terminal", "FFR 10 Ohm", "MIC1LP"},
	{"MIC1LP P-Terminal", "FFR 20 Ohm", "MIC1LP"},
	{"MIC1LP P-Terminal", "FFR 40 Ohm", "MIC1LP"},
	{"MIC1RP P-Terminal", "FFR 10 Ohm", "MIC1RP"},
	{"MIC1RP P-Terminal", "FFR 20 Ohm", "MIC1RP"},
	{"MIC1RP P-Terminal", "FFR 40 Ohm", "MIC1RP"},
	{"MIC1LM P-Terminal", "FFR 10 Ohm", "MIC1LM"},
	{"MIC1LM P-Terminal", "FFR 20 Ohm", "MIC1LM"},
	{"MIC1LM P-Terminal", "FFR 40 Ohm", "MIC1LM"},

	{"MIC1LM M-Terminal", "FFR 10 Ohm", "MIC1LM"},
	{"MIC1LM M-Terminal", "FFR 20 Ohm", "MIC1LM"},
	{"MIC1LM M-Terminal", "FFR 40 Ohm", "MIC1LM"},

	{"MIC_GAIN_CTL", NULL, "MIC1LP P-Terminal"},
	{"MIC_GAIN_CTL", NULL, "MIC1RP P-Terminal"},
	{"MIC_GAIN_CTL", NULL, "MIC1LM P-Terminal"},
	{"MIC_GAIN_CTL", NULL, "MIC1LM M-Terminal"},

	{"ADC", NULL, "MIC_GAIN_CTL"},
	{"MIC_GAIN_CTL", NULL, "Mic Bias"},

	/* Clocks for DAC */
	{"DAC Left", NULL, "CODEC_CLK_IN" },
	{"DAC Right", NULL, "CODEC_CLK_IN"},

	/* Left Output */
	{"Output Left", "From Left DAC", "DAC Left"},
	{"Output Left", "From MIC1LP", "MIC1LP"},
	{"Output Left", "From MIC1RP", "MIC1RP"},

	/* Right Output */
	{"Output Right", "From Right DAC", "DAC Right"},
	{"Output Right", "From MIC1RP", "MIC1RP"},

	/* HPL path */
	{"HP Left", "Switch", "Output Left"},
	{"HPL Driver", NULL, "HP Left"},
	{"HPL", NULL, "HPL Driver"},

	/* HPR path */
	{"HP Right", "Switch", "Output Right"},
	{"HPR Driver", NULL, "HP Right"},
	{"HPR", NULL, "HPR Driver"},
};

static const struct snd_soc_dapm_route
aic311x_audio_map[] = {
	/* SP L path */
	{"SP Left", "Switch", "Output Left"},
	{"SPL ClassD", NULL, "SP Left"},
	{"SPL", NULL, "SPL ClassD"},

	/* SP R path */
	{"SP Right", "Switch", "Output Right"},
	{"SPR ClassD", NULL, "SP Right"},
	{"SPR", NULL, "SPR ClassD"},
};

static const struct snd_soc_dapm_route
aic310x_audio_map[] = {
	/* SP L path */
	{"Speaker", "Switch", "Output Left"},
	{"SPK ClassD", NULL, "Speaker"},
	{"SPK", NULL, "SPK ClassD"},
};

static int aic31xx_add_controls(struct snd_soc_codec *codec)
{
	int err = 0;
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(codec);

	if (aic31xx->pdata.codec_type == AIC311X) {
		err = snd_soc_add_codec_controls(
			codec, aic311x_snd_controls,
			ARRAY_SIZE(aic311x_snd_controls));
		if (err < 0)
			dev_dbg(codec->dev, "Invalid control\n");

	} else if (aic31xx->pdata.codec_type == AIC310X) {
		err = snd_soc_add_codec_controls(
			codec, aic310x_snd_controls,
			ARRAY_SIZE(aic310x_snd_controls));
		if (err < 0)
			dev_dbg(codec->dev, "Invalid Control\n");
	}
	return 0;
}

static int aic31xx_add_widgets(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	if (aic31xx->pdata.codec_type == AIC311X) {
		ret = snd_soc_dapm_new_controls(
			dapm, aic311x_dapm_widgets,
			ARRAY_SIZE(aic311x_dapm_widgets));
		if (ret)
			dev_err(codec->dev,
				"Adding %d dapm widgets failed: %d\n",
				ARRAY_SIZE(aic311x_dapm_widgets), ret);
		ret = snd_soc_dapm_add_routes(dapm, aic311x_audio_map,
					      ARRAY_SIZE(aic311x_audio_map));
		if (ret)
			dev_err(codec->dev,
				"Adding %d DAPM routes failed: %d\n",
				ARRAY_SIZE(aic311x_audio_map), ret);
	} else if (aic31xx->pdata.codec_type == AIC310X) {
		ret = snd_soc_dapm_new_controls(
			dapm, aic310x_dapm_widgets,
			ARRAY_SIZE(aic310x_dapm_widgets));
		if (ret)
			dev_err(codec->dev,
				"Adding %d dapm widgets failed: %d\n",
				ARRAY_SIZE(aic310x_dapm_widgets), ret);
		ret = snd_soc_dapm_add_routes(dapm, aic310x_audio_map,
					      ARRAY_SIZE(aic310x_audio_map));
		if (ret)
			dev_err(codec->dev,
				"Adding %d DAPM routes failed: %d\n",
				ARRAY_SIZE(aic310x_audio_map), ret);
	}

	return 0;
}

static int aic31xx_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *tmp)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(codec);
	u8 data = 0;
	int i;

	dev_dbg(codec->dev, "## %s: format %d rate %d\n",
		__func__, params_format(params), params_rate(params));

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		data = (AIC31XX_WORD_LEN_20BITS <<
			AIC31XX_IFACE1_DATALEN_SHIFT);
		break;
	case SNDRV_PCM_FORMAT_S24_3LE:
		data = (AIC31XX_WORD_LEN_24BITS <<
			AIC31XX_IFACE1_DATALEN_SHIFT);
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		data = (AIC31XX_WORD_LEN_32BITS <<
			AIC31XX_IFACE1_DATALEN_SHIFT);
	break;
	}

	snd_soc_update_bits(codec, AIC31XX_IFACE1,
			    AIC31XX_IFACE1_DATALEN_MASK,
			    data);

	/* Use PLL as CODEC_CLKIN and DAC_MOD_CLK as BDIV_CLKIN */
	snd_soc_update_bits(codec, AIC31XX_CLKMUX,
			    AIC31XX_CODEC_CLKIN_MASK, AIC31XX_CODEC_CLKIN_PLL);
	snd_soc_update_bits(codec, AIC31XX_IFACE2, AIC31XX_BDIVCLK_MASK,
			    AIC31XX_DACMOD2BCLK);

	for (i = 0; i < ARRAY_SIZE(aic31xx_divs); i++) {
		if ((aic31xx_divs[i].rate == params_rate(params))
		    && (aic31xx_divs[i].mclk == aic31xx->sysclk)) {
			break;
		}
	}

	if (i == ARRAY_SIZE(aic31xx_divs)) {
		dev_err(codec->dev, "%s: Sampling rate %u not supported\n",
			__func__, params_rate(params));
		return -EINVAL;
	}

	snd_soc_update_bits(codec, AIC31XX_PLLPR, AIC31XX_PLL_MASK,
			    (aic31xx_divs[i].p_val << 4) | 0x01);
	snd_soc_write(codec, AIC31XX_PLLJ, aic31xx_divs[i].pll_j);

	snd_soc_write(codec, AIC31XX_PLLDMSB, (aic31xx_divs[i].pll_d >> 8));
	snd_soc_write(codec, AIC31XX_PLLDLSB,
		      (aic31xx_divs[i].pll_d & 0xff));

	/* NDAC divider value */
	snd_soc_update_bits(codec, AIC31XX_NDAC, AIC31XX_PLL_MASK,
			    aic31xx_divs[i].ndac);

	/* MDAC divider value */
	snd_soc_update_bits(codec, AIC31XX_MDAC, AIC31XX_PLL_MASK,
			    aic31xx_divs[i].mdac);

	/* DOSR MSB & LSB values */
	snd_soc_write(codec, AIC31XX_DOSRMSB, aic31xx_divs[i].dosr >> 8);
	snd_soc_write(codec, AIC31XX_DOSRLSB,
		      (aic31xx_divs[i].dosr & 0xff));
	/* NADC divider value */
	snd_soc_update_bits(codec, AIC31XX_NADC, AIC31XX_PLL_MASK,
			    aic31xx_divs[i].nadc);
	/* MADC divider value */
	snd_soc_update_bits(codec, AIC31XX_MADC, AIC31XX_PLL_MASK,
			    aic31xx_divs[i].madc);
	/* AOSR value */
	snd_soc_write(codec, AIC31XX_AOSR, aic31xx_divs[i].aosr);
	/* BCLK N divider */
	snd_soc_update_bits(codec, AIC31XX_BCLKN, AIC31XX_PLL_MASK,
			    aic31xx_divs[i].bclk_n);

	return 0;
}

static int aic31xx_dac_mute(struct snd_soc_dai *codec_dai, int mute)
{
	struct snd_soc_codec *codec = codec_dai->codec;

	if (mute) {
		snd_soc_update_bits(codec, AIC31XX_DACMUTE,
				    AIC31XX_DACMUTE_MASK,
				    AIC31XX_DACMUTE_MASK);
	} else {
		snd_soc_update_bits(codec, AIC31XX_DACMUTE,
				    AIC31XX_DACMUTE_MASK, 0x0);
	}

	return 0;
}

static int aic31xx_set_dai_fmt(struct snd_soc_dai *codec_dai,
			       unsigned int fmt)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	u8 iface_reg1 = 0;
	u8 iface_reg3 = 0;
	u8 dsp_a_val = 0;

	dev_dbg(codec->dev, "## %s: fmt = 0x%x\n", __func__, fmt);

	/* set master/slave audio interface */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		iface_reg1 |= AIC31XX_BCLK_MASTER | AIC31XX_WCLK_MASTER;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		iface_reg1 &= ~(AIC31XX_BCLK_MASTER | AIC31XX_WCLK_MASTER);
		break;
	case SND_SOC_DAIFMT_CBS_CFM:
		iface_reg1 |= AIC31XX_BCLK_MASTER;
		iface_reg1 &= ~(AIC31XX_WCLK_MASTER);
		break;
	default:
		dev_alert(codec->dev, "Invalid DAI master/slave interface\n");
		return -EINVAL;
	}
	/* interface format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_DSP_A:
		dsp_a_val = 0x1;
	case SND_SOC_DAIFMT_DSP_B:
		/* NOTE: BCLKINV bit value 1 equas NB and 0 equals IB */
		switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
		case SND_SOC_DAIFMT_NB_NF:
			iface_reg3 |= AIC31XX_BCLKINV_MASK;
			break;
		case SND_SOC_DAIFMT_IB_NF:
			break;
		default:
			return -EINVAL;
		}
		iface_reg1 |= (AIC31XX_DSP_MODE <<
			       AIC31XX_IFACE1_DATATYPE_SHIFT);
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		iface_reg1 |= (AIC31XX_RIGHT_JUSTIFIED_MODE <<
			       AIC31XX_IFACE1_DATATYPE_SHIFT);
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface_reg1 |= (AIC31XX_LEFT_JUSTIFIED_MODE <<
			       AIC31XX_IFACE1_DATATYPE_SHIFT);
		break;
	default:
		dev_err(codec->dev, "Invalid DAI interface format\n");
		return -EINVAL;
	}

	snd_soc_update_bits(codec, AIC31XX_IFACE1,
			    AIC31XX_IFACE1_DATATYPE_MASK |
			    AIC31XX_IFACE1_MASTER_MASK,
			    iface_reg1);
	snd_soc_update_bits(codec, AIC31XX_DATA_OFFSET,
			    AIC31XX_DATA_OFFSET_MASK,
			    dsp_a_val);
	snd_soc_update_bits(codec, AIC31XX_IFACE2,
			    AIC31XX_BCLKINV_MASK,
			    iface_reg3);

	return 0;
}

static int aic31xx_set_dai_sysclk(struct snd_soc_dai *codec_dai,
				  int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(codec);
	int i;

	dev_dbg(codec->dev, "## %s: clk_id = %d, freq = %d, dir = %d\n",
		__func__, clk_id, freq, dir);

	for (i = 0; aic31xx_divs[i].mclk != freq; i++)
		if (i == ARRAY_SIZE(aic31xx_divs)) {
			dev_err(aic31xx->dev, "%s: Unsupported frequency %d\n",
				__func__, freq);
			return -EINVAL;
		}

	/* set clock on MCLK, BCLK, or GPIO1 as PLL input */
	snd_soc_update_bits(codec, AIC31XX_CLKMUX, AIC31XX_PLL_CLKIN_MASK,
			    clk_id << AIC31XX_PLL_CLKIN_SHIFT);

	aic31xx->sysclk = freq;
	return 0;
}

static int aic31xx_regulator_event(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct aic31xx_disable_nb *disable_nb =
		container_of(nb, struct aic31xx_disable_nb, nb);
	struct aic31xx_priv *aic31xx = disable_nb->aic31xx;

	if (event & REGULATOR_EVENT_DISABLE) {
		/*
		 * Put codec to reset and as at least one
		 * of the supplies was disabled
		 */
		dev_dbg(aic31xx->dev, "## %s: DISABLE received\n", __func__);
		if (gpio_is_valid(aic31xx->pdata.gpio_reset))
			gpio_set_value(aic31xx->pdata.gpio_reset, 0);
	}

	return 0;
}

static int aic31xx_set_power(struct snd_soc_codec *codec, int power)
{
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(codec);
	int ret;

	dev_dbg(codec->dev, "## %s: %d\n", __func__, power);
	if (power) {
		ret = regulator_bulk_enable(ARRAY_SIZE(aic31xx->supplies),
					    aic31xx->supplies);
		if (ret)
			return ret;
		aic31xx->power = 1;

		if (gpio_is_valid(aic31xx->pdata.gpio_reset)) {
			gpio_set_value(aic31xx->pdata.gpio_reset, 1);
			mdelay(10);
		}
	} else {
		/*
		 * Do soft reset to this codec instance in order to clear
		 * possible VDD leakage currents in case the supply regulators
		 * remain on
		 */
		snd_soc_write(codec, AIC31XX_RESET, 0x01);
		if (gpio_is_valid(aic31xx->pdata.gpio_reset))
			gpio_set_value(aic31xx->pdata.gpio_reset, 0);
		aic31xx->power = 0;
		ret = regulator_bulk_disable(ARRAY_SIZE(aic31xx->supplies),
					     aic31xx->supplies);
	}

	return ret;
}

static int aic31xx_set_bias_level(struct snd_soc_codec *codec,
				  enum snd_soc_bias_level level)
{
	dev_dbg(codec->dev, "## %s: %d (current = %d)\n", __func__,
		level, codec->dapm.bias_level);
	if (level == codec->dapm.bias_level)
		return 0;

	switch (level) {
	/* full On */
	case SND_SOC_BIAS_ON:
		/* All power is driven by DAPM system*/
		break;
	/* partial On */
	case SND_SOC_BIAS_PREPARE:
		break;
	/* Off, with power */
	case SND_SOC_BIAS_STANDBY:
		aic31xx_set_power(codec, 1);
		break;
	/* Off, without power */
	case SND_SOC_BIAS_OFF:
		aic31xx_set_power(codec, 0);
		break;
	}
	codec->dapm.bias_level = level;

	return 0;
}


static int aic31xx_suspend(struct snd_soc_codec *codec)
{
	aic31xx_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static int aic31xx_resume(struct snd_soc_codec *codec)
{
	aic31xx_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
	return 0;
}

void aic31xx_device_exit(struct aic31xx_priv *aic31xx)
{
	if (aic31xx->pdata.gpio_reset)
		gpio_free(aic31xx->pdata.gpio_reset);
	regulator_bulk_free(ARRAY_SIZE(aic31xx->supplies), aic31xx->supplies);
}

static int aic31xx_codec_probe(struct snd_soc_codec *codec)
{
	int ret = 0;
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(codec);
	int i;

	dev_dbg(aic31xx->dev, "## %s\n", __func__);

	aic31xx = snd_soc_codec_get_drvdata(codec);
	codec->control_data = aic31xx->regmap;

	aic31xx->codec = codec;

	ret = snd_soc_codec_set_cache_io(codec, 8, 8, SND_SOC_REGMAP);

	if (ret != 0) {
		dev_err(codec->dev, "snd_soc_codec_set_cache_io failed %d\n",
			ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(aic31xx->supplies); i++) {
		aic31xx->disable_nb[i].nb.notifier_call =
			aic31xx_regulator_event;
		aic31xx->disable_nb[i].aic31xx = aic31xx;
		ret = regulator_register_notifier(aic31xx->supplies[i].consumer,
						  &aic31xx->disable_nb[i].nb);
		if (ret) {
			dev_err(codec->dev,
				"Failed to request regulator notifier: %d\n",
				ret);
			return ret;
		}
	}

	/* off, with power on */
	aic31xx_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
	aic31xx_add_controls(codec);
	aic31xx_add_widgets(codec);

	return ret;
}

static int aic31xx_codec_remove(struct snd_soc_codec *codec)
{
	struct aic31xx_priv *aic31xx = snd_soc_codec_get_drvdata(codec);
	int i;
	/* power down chip */
	aic31xx_set_bias_level(codec, SND_SOC_BIAS_OFF);

	for (i = 0; i < ARRAY_SIZE(aic31xx->supplies); i++)
		regulator_unregister_notifier(aic31xx->supplies[i].consumer,
					      &aic31xx->disable_nb[i].nb);

	return 0;
}

static struct snd_soc_codec_driver soc_codec_driver_aic31xx = {
	.probe			= aic31xx_codec_probe,
	.remove			= aic31xx_codec_remove,
	.suspend		= aic31xx_suspend,
	.resume			= aic31xx_resume,
	.set_bias_level		= aic31xx_set_bias_level,
	.controls		= aic31xx_snd_controls,
	.num_controls		= ARRAY_SIZE(aic31xx_snd_controls),
	.dapm_widgets		= aic31xx_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(aic31xx_dapm_widgets),
	.dapm_routes		= aic31xx_audio_map,
	.num_dapm_routes	= ARRAY_SIZE(aic31xx_audio_map),
};

static struct snd_soc_dai_ops aic31xx_dai_ops = {
	.hw_params	= aic31xx_hw_params,
	.set_sysclk	= aic31xx_set_dai_sysclk,
	.set_fmt	= aic31xx_set_dai_fmt,
	.digital_mute	= aic31xx_dac_mute,
};

static struct snd_soc_dai_driver aic31xx_dai_driver[] = {
	{
		.name = "tlv320aic31xx-hifi",
		.playback = {
			.stream_name	 = "Playback",
			.channels_min	 = 1,
			.channels_max	 = 2,
			.rates		 = AIC31XX_RATES,
			.formats	 = AIC31XX_FORMATS,
		},
		.capture = {
			.stream_name	 = "Capture",
			.channels_min	 = 1,
			.channels_max	 = 2,
			.rates		 = AIC31XX_RATES,
			.formats	 = AIC31XX_FORMATS,
		},
		.ops = &aic31xx_dai_ops,
	}
};

#if defined(CONFIG_OF)
static const struct of_device_id tlv320aic31xx_of_match[] = {
	{ .compatible = "ti,tlv320aic310x" },
	{ .compatible = "ti,tlv320aic311x" },
	{},
};
MODULE_DEVICE_TABLE(of, tlv320aic31xx_of_match);

static void aic31xx_pdata_from_of(struct aic31xx_priv *aic31xx)
{
	struct device_node *np = aic31xx->dev->of_node;
	unsigned int value;
	int ret;

	if (!of_property_read_u32(np, "ai31xx-micbias-vg", &value)) {
		switch (value) {
		case 1:
			aic31xx->pdata.micbias_vg = AIC31XX_MICBIAS_2_0V;
			break;
		case 2:
			aic31xx->pdata.micbias_vg = AIC31XX_MICBIAS_2_5V;
			break;
		case 3:
			aic31xx->pdata.micbias_vg = AIC31XX_MICBIAS_AVDDV;
			break;
		default:
			dev_err(aic31xx->dev,
				"Bad ai31xx-micbias-vg value %d DT\n",
				value);
		case 0:
			aic31xx->pdata.micbias_vg = AIC31XX_MICBIAS_OFF;
		}
	}

	ret = of_get_named_gpio(np, "gpio-reset", 0);
	if (ret > 0)
		aic31xx->pdata.gpio_reset = ret;
}
#else /* CONFIG_OF */
static void aic31xx_pdata_from_of(struct aic31xx_priv *aic31xx)
{
}
#endif /* CONFIG_OF */

void aic31xx_device_init(struct aic31xx_priv *aic31xx)
{
	int ret, i;

	dev_set_drvdata(aic31xx->dev, aic31xx);

	if (dev_get_platdata(aic31xx->dev))
		memcpy(&aic31xx->pdata, dev_get_platdata(aic31xx->dev),
		       sizeof(aic31xx->pdata));
	else if (aic31xx->dev->of_node)
		aic31xx_pdata_from_of(aic31xx);

	if (aic31xx->pdata.gpio_reset) {
		ret = gpio_request_one(aic31xx->pdata.gpio_reset,
				       GPIOF_OUT_INIT_HIGH,
				       "aic31xx-reset-pin");
		if (ret < 0) {
			dev_err(aic31xx->dev, "not able to acquire gpio\n");
			return;
		}
	}

	for (i = 0; i < ARRAY_SIZE(aic31xx->supplies); i++)
		aic31xx->supplies[i].supply = aic31xx_supply_names[i];

	ret = devm_regulator_bulk_get(aic31xx->dev,
				      ARRAY_SIZE(aic31xx->supplies),
				      aic31xx->supplies);
	if (ret != 0) {
		dev_err(aic31xx->dev, "Failed to request supplies: %d\n", ret);
		goto gpio_free;
	}

gpio_free:
	if (aic31xx->pdata.gpio_reset)
		gpio_free(aic31xx->pdata.gpio_reset);

}

static int aic31xx_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	struct aic31xx_priv *aic31xx;
	int ret;
	const struct regmap_config *regmap_config;

	dev_dbg(&i2c->dev, "## %s: codec_type = %d\n", __func__,
		(int) id->driver_data);

	regmap_config = &aicxxx_i2c_regmap;

	aic31xx = devm_kzalloc(&i2c->dev, sizeof(*aic31xx), GFP_KERNEL);
	if (aic31xx == NULL)
		return -ENOMEM;

	aic31xx->regmap = devm_regmap_init_i2c(i2c, regmap_config);

	if (IS_ERR(aic31xx->regmap)) {
		ret = PTR_ERR(aic31xx->regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}
	aic31xx->dev = &i2c->dev;

	aic31xx->pdata.codec_type = id->driver_data;

	aic31xx_device_init(aic31xx);

	ret = snd_soc_register_codec(&i2c->dev, &soc_codec_driver_aic31xx,
				     aic31xx_dai_driver,
				     ARRAY_SIZE(aic31xx_dai_driver));

	return ret;
}

static int aic31xx_i2c_remove(struct i2c_client *i2c)
{
	struct aic31xx_priv *aic31xx = dev_get_drvdata(&i2c->dev);

	aic31xx_device_exit(aic31xx);
	kfree(aic31xx);
	return 0;
}

static const struct i2c_device_id aic31xx_i2c_id[] = {
	{ "tlv320aic311x", AIC311X },
	{ "tlv320aic310x", AIC310X },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aic31xx_i2c_id);

static struct i2c_driver aic31xx_i2c_driver = {
	.driver = {
		.name	= "tlv320aic31xx-codec",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(tlv320aic31xx_of_match),
	},
	.probe		= aic31xx_i2c_probe,
	.remove		= (aic31xx_i2c_remove),
	.id_table	= aic31xx_i2c_id,
};

module_i2c_driver(aic31xx_i2c_driver);

MODULE_DESCRIPTION("ASoC TLV320AIC3111 codec driver");
MODULE_AUTHOR("Jyri Sarha");
MODULE_LICENSE("GPL");
