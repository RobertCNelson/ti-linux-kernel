/*
 * Allwinner SoCs SRAM Controller Driver
 *
 * Copyright (C) 2015 Maxime Ripard
 *
 * Author: Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _SUNXI_SRAM_H_
#define _SUNXI_SRAM_H_

enum sunxi_sram_type {
	SUNXI_SRAM_USB_OTG,
	SUNXI_SRAM_EMAC,
};

int sunxi_sram_claim(enum sunxi_sram_type type, const char *function);
int sunxi_sram_release(enum sunxi_sram_type type);

#endif /* _SUNXI_SRAM_H_ */
