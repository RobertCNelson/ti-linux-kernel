/*
 * Texas Instruments ION Driver
 *
 * Copyright (C) 2017 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include "../ion_priv.h"

static struct ion_device *idev;
static struct ion_heap *heap;

static struct ion_platform_heap ti_ion_heap = {
	.id	= ION_HEAP_TYPE_UNMAPPED,
	.type	= ION_HEAP_TYPE_UNMAPPED,
	.name	= "unmapped",
	.base   = 0xbf300000,
	.size   = 0x00400000,
	.align	= SZ_4K,
};

static int __init ti_ion_init(void)
{
	struct device_node *node;

	if (!of_have_populated_dt())
		return -ENODEV;

	node = of_find_node_by_path("/reserved-memory/secure_reserved");
	if (!node)
		return -ENODEV;

	idev = ion_device_create(NULL);
	if (IS_ERR(idev))
		return PTR_ERR(idev);

	heap = ion_heap_create(&ti_ion_heap);
	if (IS_ERR_OR_NULL(heap)) {
		ion_device_destroy(idev);
		return PTR_ERR(heap);
	}
	ion_device_add_heap(idev, heap);

	return 0;
}
module_init(ti_ion_init);

static void __exit ti_ion_exit(void)
{
	ion_device_destroy(idev);
	ion_heap_destroy(heap);
}
module_exit(ti_ion_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_DESCRIPTION("Texas Instruments SDP ION Driver");
