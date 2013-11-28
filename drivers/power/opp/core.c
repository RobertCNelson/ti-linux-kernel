/*
 * OPP Modifier framework
 *
 * Copyright (C) 2013 Texas Instruments Inc.
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

#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/opp-modifier.h>

static DEFINE_MUTEX(opp_modifier_list_mutex);
static LIST_HEAD(opp_modifier_list);

static int opp_modify_dev_opp_table(struct opp_modifier_dev *opp_dev,
				    struct device *dev)
{
	if (opp_dev->ops->modify)
		return opp_dev->ops->modify(dev);

	return -EINVAL;
}

static struct opp_modifier_dev *opp_modifier_get(struct device *dev)
{
	struct opp_modifier_dev *o, *opp_dev;
	struct device_node *np;

	if (!dev)
		return ERR_PTR(-EINVAL);

	np = of_parse_phandle(dev->of_node, "platform-opp-modifier", 0);

	if (!np)
		return ERR_PTR(-ENOSYS);

	opp_dev = NULL;

	mutex_lock(&opp_modifier_list_mutex);

	list_for_each_entry(o, &opp_modifier_list, list) {
		if (of_get_parent(np) == o->of_node) {
			opp_dev = o;
			break;
		}
	}

	if (!opp_dev) {
		mutex_unlock(&opp_modifier_list_mutex);
		return ERR_PTR(-EINVAL);
	}

	of_node_put(np);

	try_module_get(opp_dev->owner);
	mutex_unlock(&opp_modifier_list_mutex);

	return opp_dev;
}

static void opp_modifier_put(struct opp_modifier_dev *opp_dev)
{
	if (IS_ERR(opp_dev))
		return;

	module_put(opp_dev->owner);
}

int opp_modifier_register(struct opp_modifier_dev *opp_dev)
{
	mutex_lock(&opp_modifier_list_mutex);
	list_add(&opp_dev->list, &opp_modifier_list);
	mutex_unlock(&opp_modifier_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(opp_modifier_register);

void opp_modifier_unregister(struct opp_modifier_dev *opp_dev)
{
	mutex_lock(&opp_modifier_list_mutex);
	list_del(&opp_dev->list);
	mutex_unlock(&opp_modifier_list_mutex);
}
EXPORT_SYMBOL_GPL(opp_modifier_unregister);

int opp_modify_dev_table(struct device *dev)
{
	struct opp_modifier_dev *opp_dev;
	int ret;

	opp_dev = opp_modifier_get(dev);

	/*
	 * It is a valid case for a device to not implement
	 * an OPP modifier table so return 0 if not present
	 */

	if (IS_ERR(opp_dev) && PTR_ERR(opp_dev) == -ENOSYS) {
		dev_dbg(dev, "No platform-opp-modifier entry present\n");
		return 0;
	} else if (IS_ERR(opp_dev)) {
		return PTR_ERR(opp_dev);
	}

	ret = opp_modify_dev_opp_table(opp_dev, dev);

	opp_modifier_put(opp_dev);

	return ret;
}
EXPORT_SYMBOL_GPL(opp_modify_dev_table);
