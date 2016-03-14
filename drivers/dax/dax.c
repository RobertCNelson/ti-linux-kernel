/*
 * Copyright(c) 2016 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/pfn_t.h>
#include <linux/slab.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>

static int dax_major;
static struct class *dax_class;
static DEFINE_IDA(dax_minor_ida);

struct dax_region {
	int id;
	struct ida ida;
	void *base;
	struct kref kref;
	struct device *dev;
	unsigned int align;
	struct resource res;
	unsigned long pfn_flags;
};

struct dax_dev {
	struct dax_region *region;
	struct device *dev;
	int num_resources;
	struct resource res[0];
};

static void dax_region_release(struct kref *kref)
{
	struct dax_region *dax_region;

	dax_region = container_of(kref, struct dax_region, kref);
	kfree(dax_region);
}

void dax_region_put(struct dax_region *dax_region)
{
	kref_put(&dax_region->kref, dax_region_release);
}
EXPORT_SYMBOL_GPL(dax_region_put);

static void dax_release(struct device *dev)
{
	struct dax_dev *dax_dev = dev_get_drvdata(dev);
	struct dax_region *dax_region = dax_dev->region;
	int region_id, id, rc, minor;

	dev_dbg(dev, "%s\n", __func__);
	rc = sscanf(dev_name(dev), "dax%d.%d", &region_id, &id);
	WARN_ON(rc != 2 || dax_region->id != region_id);

	ida_simple_remove(&dax_region->ida, id);
	minor = MINOR(dev->devt);
	ida_simple_remove(&dax_minor_ida, minor);
	dax_region_put(dax_region);
}

struct dax_region *alloc_dax_region(struct device *parent,
		int region_id, struct resource *res, unsigned int align,
		void *addr, unsigned long pfn_flags)
{
	struct dax_region *dax_region;

	dax_region = kzalloc(sizeof(*dax_region), GFP_KERNEL);

	if (!dax_region)
		return NULL;

	memcpy(&dax_region->res, res, sizeof(*res));
	dax_region->pfn_flags = pfn_flags;
	kref_init(&dax_region->kref);
	dax_region->id = region_id;
	ida_init(&dax_region->ida);
	dax_region->align = align;
	dax_region->dev = parent;
	dax_region->base = addr;

	return dax_region;
}
EXPORT_SYMBOL_GPL(alloc_dax_region);

static ssize_t size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dax_dev *dax_dev = dev_get_drvdata(dev);
	unsigned long long size = 0;
	int i;

	for (i = 0; i < dax_dev->num_resources; i++)
		size += resource_size(&dax_dev->res[i]);

	return sprintf(buf, "%llu\n", size);
}
static DEVICE_ATTR_RO(size);

static struct attribute *dax_device_attributes[] = {
	&dev_attr_size.attr,
	NULL,
};

static const struct attribute_group dax_device_attribute_group = {
	.attrs = dax_device_attributes,
};

static const struct attribute_group *dax_attribute_groups[] = {
	&dax_device_attribute_group,
	NULL,
};

static void destroy_dax_dev(void *dev)
{
	dev_dbg(dev, "%s\n", __func__);
	device_unregister(dev);
}

int devm_create_dax_dev(struct dax_region *dax_region, struct resource *res,
		int count)
{
	struct device *parent = dax_region->dev;
	struct dax_dev *dax_dev;
	struct device *dev;
	int id, rc, minor;
	dev_t dev_t;

	dax_dev = kzalloc(sizeof(*dax_dev) + sizeof(*res) * count, GFP_KERNEL);
	if (!dax_dev)
		return -ENOMEM;
	memcpy(dax_dev->res, res, sizeof(*res) * count);
	dax_dev->num_resources = count;
	dax_dev->region = dax_region;
	kref_get(&dax_region->kref);

	id = ida_simple_get(&dax_region->ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		rc = id;
		goto err_id;
	}

	minor = ida_simple_get(&dax_minor_ida, 0, 0, GFP_KERNEL);
	if (minor < 0) {
		rc = minor;
		goto err_minor;
	}

	dev_t = MKDEV(dax_major, id);
	dev = device_create_with_groups(dax_class, parent, dev_t, dax_dev,
			dax_attribute_groups, "dax%d.%d", dax_region->id, id);
	if (IS_ERR(dev)) {
		rc = PTR_ERR(dev);
		goto err_create;
	}
	dax_dev->dev = dev;

	rc = devm_add_action(dax_region->dev, destroy_dax_dev, dev);
	if (rc) {
		destroy_dax_dev(dev);
		return rc;
	}

	return 0;

 err_create:
	ida_simple_remove(&dax_minor_ida, minor);
 err_minor:
	ida_simple_remove(&dax_region->ida, id);
 err_id:
	dax_region_put(dax_region);
	kfree(dax_dev);

	return rc;
}
EXPORT_SYMBOL_GPL(devm_create_dax_dev);

static const struct file_operations dax_fops = {
	.llseek = noop_llseek,
	.owner = THIS_MODULE,
};

static int __init dax_init(void)
{
	int rc;

	rc = register_chrdev(0, "dax", &dax_fops);
	if (rc < 0)
		return rc;
	dax_major = rc;

	dax_class = class_create(THIS_MODULE, "dax");
	if (IS_ERR(dax_class)) {
		unregister_chrdev(dax_major, "dax");
		return PTR_ERR(dax_class);
	}
	dax_class->dev_release = dax_release;

	return 0;
}

static void __exit dax_exit(void)
{
	class_destroy(dax_class);
	unregister_chrdev(dax_major, "dax");
}

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
subsys_initcall(dax_init);
module_exit(dax_exit);
