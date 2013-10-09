/*
 * gpmc-nand.c
 *
 * Copyright (C) 2009 Texas Instruments
 * Vimal Singh <vimalsingh@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/mtd/nand.h>
#include <linux/platform_data/mtd-nand-omap2.h>

#include <asm/mach/flash.h>

#include "gpmc.h"
#include "soc.h"
#include "gpmc-nand.h"

/* minimum size for IO mapping */
#define	NAND_IO_SIZE	4

static struct resource gpmc_nand_resource[] = {
	{
		.flags		= IORESOURCE_MEM,
	},
	{
		.flags		= IORESOURCE_IRQ,
	},
	{
		.flags		= IORESOURCE_IRQ,
	},
};

static struct platform_device gpmc_nand_device = {
	.name		= "omap2-nand",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(gpmc_nand_resource),
	.resource	= gpmc_nand_resource,
};

int gpmc_nand_init(struct omap_nand_platform_data *gpmc_nand_data,
		   struct gpmc_timings *gpmc_t)
{
	int err	= 0;
	struct gpmc_settings s;
	struct device *dev = &gpmc_nand_device.dev;

	memset(&s, 0, sizeof(struct gpmc_settings));

	gpmc_nand_device.dev.platform_data = gpmc_nand_data;

	err = gpmc_cs_request(gpmc_nand_data->cs, NAND_IO_SIZE,
				(unsigned long *)&gpmc_nand_resource[0].start);
	if (err < 0) {
		dev_err(dev, "Cannot request GPMC CS %d, error %d\n",
			gpmc_nand_data->cs, err);
		return err;
	}

	gpmc_nand_resource[0].end = gpmc_nand_resource[0].start +
							NAND_IO_SIZE - 1;

	gpmc_nand_resource[1].start =
				gpmc_get_client_irq(GPMC_IRQ_FIFOEVENTENABLE);
	gpmc_nand_resource[2].start =
				gpmc_get_client_irq(GPMC_IRQ_COUNT_EVENT);

	if (gpmc_t) {
		err = gpmc_cs_set_timings(gpmc_nand_data->cs, gpmc_t);
		if (err < 0) {
			dev_err(dev, "Unable to set gpmc timings: %d\n", err);
			return err;
		}

		if (gpmc_nand_data->of_node) {
			gpmc_read_settings_dt(gpmc_nand_data->of_node, &s);
		} else {
			/* Enable RD PIN Monitoring Reg */
			if (gpmc_nand_data->dev_ready) {
				s.wait_on_read = true;
				s.wait_on_write = true;
			}
		}

		s.device_nand = true;

		if (gpmc_nand_data->devsize == NAND_BUSWIDTH_16)
			s.device_width = GPMC_DEVWIDTH_16BIT;
		else
			s.device_width = GPMC_DEVWIDTH_8BIT;

		err = gpmc_cs_program_settings(gpmc_nand_data->cs, &s);
		if (err < 0)
			goto out_free_cs;

		err = gpmc_configure(GPMC_CONFIG_WP, 0);
		if (err < 0)
			goto out_free_cs;
	}

	gpmc_update_nand_reg(&gpmc_nand_data->reg, gpmc_nand_data->cs);

	err = platform_device_register(&gpmc_nand_device);
	if (err < 0) {
		dev_err(dev, "Unable to register NAND device\n");
		goto out_free_cs;
	}

	return 0;

out_free_cs:
	gpmc_cs_free(gpmc_nand_data->cs);

	return err;
}
