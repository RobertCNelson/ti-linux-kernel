/*
 * ALSA SoC TLV320AIC31XX codec driver
 *
 * Copyright (C) 2013 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */
#ifndef _TLV320AIC31XX_H
#define _TLV320AIC31XX_H

/* AIC31XX supported sample rate are 8k to 192k */
#define AIC31XX_RATES	SNDRV_PCM_RATE_8000_192000

/* AIC31XX supports the word formats 16bits, 20bits, 24bits and 32 bits */
#if 0
#define AIC31XX_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE \
			 | SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S32_LE)
#else
#define AIC31XX_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE)
#endif

/* Audio data word length = 16-bits (default setting) */
#define AIC31XX_WORD_LEN_16BITS		0x00
#define AIC31XX_WORD_LEN_20BITS		0x01
#define AIC31XX_WORD_LEN_24BITS		0x02
#define AIC31XX_WORD_LEN_32BITS		0x03

/* Masks used for updating register bits */
#define AIC31XX_IFACE1_DATALEN_MASK	0x30
#define AIC31XX_IFACE1_DATALEN_SHIFT	(4)
#define AIC31XX_IFACE1_DATATYPE_MASK	0xC0
#define AIC31XX_IFACE1_DATATYPE_SHIFT	(6)
/* Serial data bus uses I2S mode (Default mode) */
#define AIC31XX_I2S_MODE		0x00
#define AIC31XX_DSP_MODE		0x01
#define AIC31XX_RIGHT_JUSTIFIED_MODE	0x02
#define AIC31XX_LEFT_JUSTIFIED_MODE	0x03

#define AIC31XX_IFACE1_MASTER_MASK	0x0C
/* The values bellow have already been shifted */
#define AIC31XX_BCLK_MASTER	0x08
#define AIC31XX_WCLK_MASTER	0x04


#define AIC31XX_DATA_OFFSET_MASK	0xFF
#define AIC31XX_BCLKINV_MASK		0x08
#define AIC31XX_BDIVCLK_MASK		0x03

#define AIC31XX_DAC2BCLK		0x00
#define AIC31XX_DACMOD2BCLK		0x01
#define AIC31XX_ADC2BCLK		0x02
#define AIC31XX_ADCMOD2BCLK		0x03

enum aic31xx_type {
	AIC311X = 0,
	AIC310X = 1,
};

enum aic31xx_micbias_voltage {
	AIC31XX_MICBIAS_OFF = 0,
	AIC31XX_MICBIAS_2_0V = 1,
	AIC31XX_MICBIAS_2_5V = 2,
	AIC31XX_MICBIAS_AVDDV = 3,
};

struct aic31xx_pdata {
	enum aic31xx_type codec_type;
	unsigned int gpio_reset;
	enum aic31xx_micbias_voltage micbias_vg;
};

/* Page 0 Registers */
/* Software reset register */
#define AIC31XX_RESET				0x81
/* OT FLAG register */
#define AIC31XX_OT_FLAG				0x83
/* Clock clock Gen muxing, Multiplexers*/
#define AIC31XX_CLKMUX				0x84
#define AIC31XX_PLL_CLKIN_MASK			0x0c
#define AIC31XX_PLL_CLKIN_SHIFT			2
#define AIC31XX_PLL_CLKIN_MCLK			0
#define AIC31XX_CODEC_CLKIN_MASK		0x03
#define AIC31XX_CODEC_CLKIN_SHIFT		0
#define AIC31XX_CODEC_CLKIN_PLL			0x3
/* PLL P and R-VAL register */
#define AIC31XX_PLLPR				0x85
#define AIC31XX_PLL_MASK			0x7f
/* PLL J-VAL register */
#define AIC31XX_PLLJ				0x86
/* PLL D-VAL MSB register */
#define AIC31XX_PLLDMSB				0x87
/* PLL D-VAL LSB register */
#define AIC31XX_PLLDLSB				0x88
/* DAC NDAC_VAL register*/
#define AIC31XX_NDAC				0x8B
/* DAC MDAC_VAL register */
#define AIC31XX_MDAC				0x8C
/* DAC OSR setting register 1, MSB value */
#define AIC31XX_DOSRMSB				0x8D
/* DAC OSR setting register 2, LSB value */
#define AIC31XX_DOSRLSB				0x8E
#define AIC31XX_MINI_DSP_INPOL			0x90
/* Clock setting register 8, PLL */
#define AIC31XX_NADC				0x92
/* Clock setting register 9, PLL */
#define AIC31XX_MADC				0x93
/* ADC Oversampling (AOSR) Register */
#define AIC31XX_AOSR				0x94
/* Clock setting register 9, Multiplexers */
#define AIC31XX_CLKOUTMUX			0x99
/* Clock setting register 10, CLOCKOUT M divider value */
#define AIC31XX_CLKOUTMVAL			0x9A
/* Audio Interface Setting Register 1 */
#define AIC31XX_IFACE1				0x9B
/* Audio Data Slot Offset Programming */
#define AIC31XX_DATA_OFFSET			0x9C
/* Audio Interface Setting Register 2 */
#define AIC31XX_IFACE2				0x9D
/* Clock setting register 11, BCLK N Divider */
#define AIC31XX_BCLKN				0x9E
/* Audio Interface Setting Register 3, Secondary Audio Interface */
#define AIC31XX_IFACESEC1			0x9F
/* Audio Interface Setting Register 4 */
#define AIC31XX_IFACESEC2			0xA0
/* Audio Interface Setting Register 5 */
#define AIC31XX_IFACESEC3			0xA1
/* I2C Bus Condition */
#define AIC31XX_I2C				0xA2
/* ADC FLAG */
#define AIC31XX_ADCFLAG				0xA4
#define AIC31XX_ADCPWRSTATUS_MASK		0x40
/* DAC Flag Registers */
#define AIC31XX_DACFLAG1			0xA5
#define AIC31XX_LDACPWRSTATUS_MASK		0x80
#define AIC31XX_RDACPWRSTATUS_MASK		0x08
#define AIC31XX_HPLDRVPWRSTATUS_MASK		0x20
#define AIC31XX_HPRDRVPWRSTATUS_MASK		0x02
#define AIC31XX_SPLDRVPWRSTATUS_MASK		0x10
#define AIC31XX_SPRDRVPWRSTATUS_MASK		0x01
#define AIC31XX_DACFLAG2			0xA6

/* Sticky Interrupt flag (overflow) */
#define AIC31XX_OFFLAG				0xA7

/* Sticky Interrupt flags 1 and 2 registers (DAC) */
#define AIC31XX_INTRDACFLAG			0xAC
#define AIC31XX_HPSCDETECT_MASK			0x80
#define AIC31XX_BUTTONPRESS_MASK		0x20
#define AIC31XX_HSPLUG_MASK			0x10
#define AIC31XX_LDRCTHRES_MASK			0x08
#define AIC31XX_RDRCTHRES_MASK			0x04
#define AIC31XX_DACSINT_MASK			0x02
#define AIC31XX_DACAINT_MASK			0x01

/* INT1 interrupt control */
#define AIC31XX_INT1CTRL			0xB0
#define AIC31XX_HSPLUGDET_MASK			0x80
#define AIC31XX_BUTTONPRESSDET_MASK		0x40
#define AIC31XX_DRCTHRES_MASK			0x20
#define AIC31XX_AGCNOISE_MASK			0x10
#define AIC31XX_OC_MASK				0x08
#define AIC31XX_ENGINE_MASK			0x04

/* INT2 interrupt control */
#define AIC31XX_INT2CTRL			0xB1

/* GPIO1 control */
#define AIC31XX_GPIO1				0xB3

#define AIC31XX_DACPRB				0xBC
/* ADC Instruction Set Register */
#define AIC31XX_ADCPRB				0xBD
/* DAC channel setup register */
#define AIC31XX_DACSETUP			0xBF
#define AIC31XX_SOFTSTEP_MASK			0x03
/* DAC Mute and volume control register */
#define AIC31XX_DACMUTE				0xC0
#define AIC31XX_DACMUTE_MASK			0x0C
/* Left DAC channel digital volume control */
#define AIC31XX_LDACVOL				0xC1
/* Right DAC channel digital volume control */
#define AIC31XX_RDACVOL				0xC2
/* Headset detection */
#define AIC31XX_HSDETECT			0xC3
#define AIC31XX_ADCSETUP			0xD1
#define AIC31XX_ADCFGA				0xD2
#define AIC31XX_ADCMUTE_MASK			0x80
#define AIC31XX_ADCVOL				0xD3


/* Page 1 Registers */
/* Headphone drivers */
#define AIC31XX_HPDRIVER			0x11F
/* Class-D Speakear Amplifier */
#define AIC31XX_SPKAMP				0x120
/* HP Output Drivers POP Removal Settings */
#define AIC31XX_HPPOP				0x121
/* Output Driver PGA Ramp-Down Period Control */
#define AIC31XX_SPPGARAMP			0x122
/* DAC_L and DAC_R Output Mixer Routing */
#define AIC31XX_DACMIXERROUTE			0x123
/* Left Analog Vol to HPL */
#define AIC31XX_LANALOGHPL			0x124
/* Right Analog Vol to HPR */
#define AIC31XX_RANALOGHPR			0x125
/* Left Analog Vol to SPL */
#define AIC31XX_LANALOGSPL			0x126
/* Right Analog Vol to SPR */
#define AIC31XX_RANALOGSPR			0x127
/* HPL Driver */
#define AIC31XX_HPLGAIN				0x128
/* HPR Driver */
#define AIC31XX_HPRGAIN				0x129
/* SPL Driver */
#define AIC31XX_SPLGAIN				0x12A
/* SPR Driver */
#define AIC31XX_SPRGAIN				0x12B
/* HP Driver Control */
#define AIC31XX_HPCONTROL			0x12C
/* MIC Bias Control */
#define AIC31XX_MICBIAS				0x12E
#define AIC31XX_MICBIAS_MASK			0x03
#define AIC31XX_MICBIAS_SHIFT			0
/* MIC PGA*/
#define AIC31XX_MICPGA				0x12F
/* Delta-Sigma Mono ADC Channel Fine-Gain Input Selection for P-Terminal */
#define AIC31XX_MICPGAPI			0x130
/* ADC Input Selection for M-Terminal */
#define AIC31XX_MICPGAMI			0x131
/* Input CM Settings */
#define AIC31XX_MICPGACM			0x132

#endif	/* _TLV320AIC31XX_H */
