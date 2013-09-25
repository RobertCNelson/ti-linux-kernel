/*
 * IRQ/DMA CROSSBAR DRIVER
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 *	Sricharan <r.sricharan@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/platform_device.h>

/*
 * @base: base address of the crossbar device
 * @dev:  device ptr
 * @name: name of the crossbar device
 * @node: list node for the crossbar devices linked list
 * @cb_entries: list of entries that belong to the crossbar
 * @cb_lock: mutex
 * @regmap pointer
 */
struct cb_device {
	void __iomem *base;
	struct device *dev;
	const char *name;
	struct list_head node;
	struct list_head cb_entries;
	struct mutex cb_lock;
	struct regmap *cb_regmap;
};

/*
 * @cb_name: name of crossbar target to which this line is mapped
 * @dev_name: mapped device input request name
 * @cb_no: crossbar device input number
 * @int_no: request number to which this input should be routed
 * @offset: register offset address
 */
struct cb_line {
	const char *cb_name;
	const char *dev_name;
	unsigned cb_no;
	unsigned int_no;
	unsigned offset;
};

struct cb_entry {
	struct cb_line line;
	struct list_head cb_list;
};

int crossbar_map(struct device_node *cbdev_node);
int crossbar_unmap(struct device_node *cbdev_node, unsigned index);
