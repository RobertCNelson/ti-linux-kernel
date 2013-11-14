/*
 * TI OPP Modifier Core API
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 * Dave Gerlach <d-gerlach@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __LINUX_OPP_MODIFIER_H__
#define __LINUX_OPP_MODIFIER_H__

struct opp_modifier_dev {
	struct opp_modifier_ops *ops;
	struct module *owner;
	struct list_head list;
	struct device_node *of_node;
};

struct opp_modifier_ops {
	int (*modify)(struct device *dev);
};

int opp_modifier_register(struct opp_modifier_dev *opp_dev);
void opp_modifier_unregister(struct opp_modifier_dev *opp_dev);
int opp_modify_dev_table(struct device *dev);

#endif          /* __LINUX_OPP_MODIFIER_H__ */
