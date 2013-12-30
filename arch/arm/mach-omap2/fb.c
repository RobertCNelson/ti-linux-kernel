/*
 * Framebuffer device registration for TI OMAP platforms
 *
 * Copyright (C) 2006 Nokia Corporation
 * Author: Imre Deak <imre.deak@nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/memblock.h>
#include <linux/io.h>
#include <linux/omapfb.h>
#include <linux/dma-mapping.h>

#include <asm/mach/map.h>

#include "soc.h"
#include "display.h"
#include "common.h"

#ifdef CONFIG_OMAP2_VRFB

/*
 * The first memory resource is the register region for VRFB,
 * the rest are VRFB virtual memory areas for each VRFB context.
 */

static const struct resource omap2_vrfb_resources[] = {
	DEFINE_RES_MEM_NAMED(0x68008000u, 0x40, "vrfb-regs"),
	DEFINE_RES_MEM_NAMED(0x70000000u, 0x4000000, "vrfb-area-0"),
	DEFINE_RES_MEM_NAMED(0x74000000u, 0x4000000, "vrfb-area-1"),
	DEFINE_RES_MEM_NAMED(0x78000000u, 0x4000000, "vrfb-area-2"),
	DEFINE_RES_MEM_NAMED(0x7c000000u, 0x4000000, "vrfb-area-3"),
};

static const struct resource omap3_vrfb_resources[] = {
	DEFINE_RES_MEM_NAMED(0x6C000180u, 0xc0, "vrfb-regs"),
	DEFINE_RES_MEM_NAMED(0x70000000u, 0x4000000, "vrfb-area-0"),
	DEFINE_RES_MEM_NAMED(0x74000000u, 0x4000000, "vrfb-area-1"),
	DEFINE_RES_MEM_NAMED(0x78000000u, 0x4000000, "vrfb-area-2"),
	DEFINE_RES_MEM_NAMED(0x7c000000u, 0x4000000, "vrfb-area-3"),
	DEFINE_RES_MEM_NAMED(0xe0000000u, 0x4000000, "vrfb-area-4"),
	DEFINE_RES_MEM_NAMED(0xe4000000u, 0x4000000, "vrfb-area-5"),
	DEFINE_RES_MEM_NAMED(0xe8000000u, 0x4000000, "vrfb-area-6"),
	DEFINE_RES_MEM_NAMED(0xec000000u, 0x4000000, "vrfb-area-7"),
	DEFINE_RES_MEM_NAMED(0xf0000000u, 0x4000000, "vrfb-area-8"),
	DEFINE_RES_MEM_NAMED(0xf4000000u, 0x4000000, "vrfb-area-9"),
	DEFINE_RES_MEM_NAMED(0xf8000000u, 0x4000000, "vrfb-area-10"),
	DEFINE_RES_MEM_NAMED(0xfc000000u, 0x4000000, "vrfb-area-11"),
};

int __init omap_init_vrfb(void)
{
	struct platform_device *pdev;
	const struct resource *res;
	unsigned int num_res;

	if (cpu_is_omap24xx()) {
		res = omap2_vrfb_resources;
		num_res = ARRAY_SIZE(omap2_vrfb_resources);
	} else if (cpu_is_omap34xx()) {
		res = omap3_vrfb_resources;
		num_res = ARRAY_SIZE(omap3_vrfb_resources);
	} else {
		return 0;
	}

	pdev = platform_device_register_resndata(NULL, "omapvrfb", -1,
			res, num_res, NULL, 0);

	return PTR_RET(pdev);
}
#else
int __init omap_init_vrfb(void) { return 0; }
#endif

#if defined(CONFIG_FB_OMAP2) || defined(CONFIG_FB_OMAP2_MODULE)

static u64 omap_fb_dma_mask = ~(u32)0;
static struct omapfb_platform_data omapfb_config;

static struct platform_device omap_fb_device = {
	.name		= "omapfb",
	.id		= -1,
	.dev = {
		.dma_mask		= &omap_fb_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
		.platform_data		= &omapfb_config,
	},
	.num_resources = 0,
};

static phys_addr_t omapfb_mem_base __initdata;
static phys_addr_t omapfb_mem_size __initdata;

static int __init early_omapfb_vram(char *p)
{
	omapfb_mem_size = memparse(p, &p);

	if (!omapfb_mem_size) {
		pr_err("omapfb: bad size for 'omapfb_vram' param\n");
		return 0;
	}

	if (*p == '@') {
		omapfb_mem_base = memparse(p + 1, NULL);

		if (!omapfb_mem_base) {
			pr_err("omapfb: bad addr for 'omapfb_vram' param\n");
			omapfb_mem_size = 0;
			return 0;
		}
	}

	return 0;
}
early_param("omapfb_vram", early_omapfb_vram);

void __init omap_fb_reserve_memblock(void)
{
	int r;

	if (!omapfb_mem_size)
		return;

	if (omapfb_mem_base) {
		r = memblock_reserve(omapfb_mem_base, omapfb_mem_size);

		if (r) {
			pr_err("omapfb: memblock_reserve failed: %d\n", r);
			return;
		}
	} else {
		omapfb_mem_base = memblock_alloc(omapfb_mem_size, SZ_1M);

		if (!omapfb_mem_base) {
			pr_err("omapfb: memblock_alloc failed\n");
			return;
		}
	}

	memblock_free(omapfb_mem_base, omapfb_mem_size);
	memblock_remove(omapfb_mem_base, omapfb_mem_size);

	pr_info("omapfb: reserved %pa bytes at %pa\n",
		&omapfb_mem_size, &omapfb_mem_base);
}

int __init omap_init_fb(void)
{
	int r;

	r = platform_device_register(&omap_fb_device);

	if (r)
		return r;

	if (!omapfb_mem_base)
		return 0;

	r = dma_declare_coherent_memory(&omap_fb_device.dev,
					omapfb_mem_base, omapfb_mem_base,
					omapfb_mem_size, DMA_MEMORY_MAP);
	if (!(r & DMA_MEMORY_MAP))
		pr_err("omapfb: dma_declare_coherent_memory failed\n");

	return 0;
}

#else
void __init omap_fb_reserve_memblock(void) { }
int __init omap_init_fb(void) { return 0; }
#endif
