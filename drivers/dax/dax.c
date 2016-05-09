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

/* return an unmapped area aligned to the dax region specified alignment */
static unsigned long dax_dev_get_unmapped_area(struct file *filp,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long flags)
{
	struct dax_dev *dax_dev;
	struct device *dev = filp ? filp->private_data : NULL;
	unsigned long off, off_end, off_align, len_align, addr_align, align = 0;

	if (!filp || addr)
		goto out;

	device_lock(dev);
	dax_dev = dev_get_drvdata(dev);
	if (dax_dev) {
		struct dax_region *dax_region = dax_dev->region;

		align = dax_region->align;
	}
	device_unlock(dev);

	if (!align)
		goto out;

	off = pgoff << PAGE_SHIFT;
	off_end = off + len;
	off_align = round_up(off, align);

	if ((off_end <= off_align) || ((off_end - off_align) < align))
		goto out;

	len_align = len + align;
	if ((off + len_align) < off)
		goto out;

	addr_align = current->mm->get_unmapped_area(filp, addr, len_align,
			pgoff, flags);
	if (!IS_ERR_VALUE(addr_align)) {
		addr_align += (off - addr_align) & (align - 1);
		return addr_align;
	}
 out:
	return current->mm->get_unmapped_area(filp, addr, len, pgoff, flags);
}

static int __match_devt(struct device *dev, const void *data)
{
	const dev_t *devt = data;

	return dev->devt == *devt;
}

static int dax_dev_open(struct inode *inode, struct file *filp)
{
	struct device *dev;

	dev = class_find_device(dax_class, NULL, &inode->i_rdev, __match_devt);
	if (dev) {
		dev_dbg(dev, "%s\n", __func__);
		filp->private_data = dev;
		inode->i_flags = S_DAX;
		return 0;
	}
	return -ENXIO;
}

static int dax_dev_release(struct inode *inode, struct file *filp)
{
	struct device *dev = filp->private_data;

	dev_dbg(dev, "%s\n", __func__);
	put_device(dev);
	return 0;
}

static struct dax_dev *to_dax_dev(struct device *dev)
{
	WARN_ON(dev->class != dax_class);
	device_lock_assert(dev);
	return dev_get_drvdata(dev);
}

static int dax_dev_check_vma(struct device *dev, struct vm_area_struct *vma,
		const char *func)
{
	struct dax_dev *dax_dev = to_dax_dev(dev);
	struct dax_region *dax_region;
	unsigned long mask;

	if (!dax_dev)
		return -ENXIO;

	/* prevent private / writable mappings from being established */
	if ((vma->vm_flags & (VM_NORESERVE|VM_SHARED|VM_WRITE)) == VM_WRITE) {
		dev_dbg(dev, "%s: fail, attempted private mapping\n", func);
		return -EINVAL;
	}

	dax_region = dax_dev->region;
	mask = dax_region->align - 1;
	if (vma->vm_start & mask || vma->vm_end & mask) {
		dev_dbg(dev, "%s: fail, unaligned vma (%#lx - %#lx, %#lx)\n",
				func, vma->vm_start, vma->vm_end, mask);
		return -EINVAL;
	}

	if ((dax_region->pfn_flags & (PFN_DEV|PFN_MAP)) == PFN_DEV
			&& (vma->vm_flags & VM_DONTCOPY) == 0) {
		dev_dbg(dev, "%s: fail, dax range requires MADV_DONTFORK\n",
				func);
		return -EINVAL;
	}

	if (!vma_is_dax(vma)) {
		dev_dbg(dev, "%s: fail, vma is not DAX capable\n", func);
		return -EINVAL;
	}

	return 0;
}

static phys_addr_t pgoff_to_phys(struct dax_dev *dax_dev, pgoff_t pgoff,
		unsigned long size)
{
	struct resource *res;
	phys_addr_t phys;
	int i;

	for (i = 0; i < dax_dev->num_resources; i++) {
		res = &dax_dev->res[i];
		phys = pgoff * PAGE_SIZE + res->start;
		if (phys >= res->start && phys <= res->end)
			break;
		pgoff -= PHYS_PFN(resource_size(res));
	}

	if (i < dax_dev->num_resources) {
		res = &dax_dev->res[i];
		if (phys + size - 1 <= res->end)
			return phys;
	}

	return -1;
}

static int __dax_dev_fault(struct address_space *mapping, struct device *dev,
		struct vm_area_struct *vma, struct vm_fault *vmf)
{
	unsigned long vaddr = (unsigned long)vmf->virtual_address;
	struct dax_dev *dax_dev = to_dax_dev(dev);
	struct dax_region *dax_region;
	phys_addr_t phys;
	pfn_t pfn;
	int rc;

	if (!dax_dev)
		return VM_FAULT_SIGBUS;

	if (dax_dev_check_vma(dev, vma, __func__))
		return VM_FAULT_SIGBUS;

	dax_region = dax_dev->region;
	if (dax_region->align > PAGE_SIZE) {
		dev_dbg(dev, "%s: alignment > fault size\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	phys = pgoff_to_phys(dax_dev, vmf->pgoff, PAGE_SIZE);
	if (phys == -1) {
		dev_dbg(dev, "%s: phys_to_pgoff(%#lx) failed\n", __func__,
				vmf->pgoff);
		return VM_FAULT_SIGBUS;
	}

	pfn = phys_to_pfn_t(phys, dax_region->pfn_flags);

	i_mmap_lock_read(mapping);
	rc = vm_insert_mixed(vma, vaddr, pfn);
	i_mmap_unlock_read(mapping);

	if (rc == -ENOMEM)
		return VM_FAULT_OOM;
	if (rc < 0 && rc != -EBUSY)
		return VM_FAULT_SIGBUS;

	return VM_FAULT_NOPAGE;
}

static int dax_dev_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int rc;
	struct file *filp = vma->vm_file;
	struct device *dev = filp->private_data;
	struct address_space *mapping = filp->f_mapping;

	dev_dbg(dev, "%s: %s (%#lx - %#lx)\n", __func__,
			(vmf->flags & FAULT_FLAG_WRITE) ? "write" : "read",
			vma->vm_start, vma->vm_end);
	device_lock(dev);
	rc = __dax_dev_fault(mapping, dev, vma, vmf);
	device_unlock(dev);

	return rc;
}

static int __dax_dev_pmd_fault(struct address_space *mapping,
		struct device *dev, struct vm_area_struct *vma,
		unsigned long addr, pmd_t *pmd, unsigned int flags)
{
	struct dax_dev *dax_dev = to_dax_dev(dev);
	unsigned long pmd_addr = addr & PMD_MASK;
	struct dax_region *dax_region;
	phys_addr_t phys;
	pgoff_t pgoff;
	pfn_t pfn;
	int rc;

	if (!dax_dev)
		return VM_FAULT_SIGBUS;

	if (dax_dev_check_vma(dev, vma, __func__))
		return VM_FAULT_SIGBUS;

	dax_region = dax_dev->region;
	if (dax_region->align > PMD_SIZE) {
		dev_dbg(dev, "%s: alignment > fault size\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	/* dax pmd mappings require pfn_t_devmap() */
	if ((dax_region->pfn_flags & (PFN_DEV|PFN_MAP)) != (PFN_DEV|PFN_MAP)) {
		dev_dbg(dev, "%s: alignment > fault size\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	pgoff = linear_page_index(vma, pmd_addr);
	phys = pgoff_to_phys(dax_dev, pgoff, PAGE_SIZE);
	if (phys == -1) {
		dev_dbg(dev, "%s: phys_to_pgoff(%#lx) failed\n", __func__,
				pgoff);
		return VM_FAULT_SIGBUS;
	}

	pfn = phys_to_pfn_t(phys, dax_region->pfn_flags);

	i_mmap_lock_read(mapping);
	rc = vmf_insert_pfn_pmd(vma, addr, pmd, pfn,
			flags & FAULT_FLAG_WRITE);
	i_mmap_unlock_read(mapping);

	return rc;
}

static int dax_dev_pmd_fault(struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, unsigned int flags)
{
	int rc;
	struct file *filp = vma->vm_file;
	struct device *dev = filp->private_data;
	struct address_space *mapping = filp->f_mapping;

	dev_dbg(dev, "%s: %s (%#lx - %#lx)\n", __func__,
			(flags & FAULT_FLAG_WRITE) ? "write" : "read",
			vma->vm_start, vma->vm_end);
	device_lock(dev);
	rc = __dax_dev_pmd_fault(mapping, dev, vma, addr, pmd, flags);
	device_unlock(dev);

	return rc;
}

static void dax_dev_vm_open(struct vm_area_struct *vma)
{
	struct file *filp = vma->vm_file;
	struct device *dev = filp->private_data;

	dev_dbg(dev, "%s\n", __func__);
	get_device(dev);
}

static void dax_dev_vm_close(struct vm_area_struct *vma)
{
	struct file *filp = vma->vm_file;
	struct device *dev = filp->private_data;

	dev_dbg(dev, "%s\n", __func__);
	put_device(dev);
}

static const struct vm_operations_struct dax_dev_vm_ops = {
	.fault = dax_dev_fault,
	.pmd_fault = dax_dev_pmd_fault,
	.open = dax_dev_vm_open,
	.close = dax_dev_vm_close,
};

static int dax_dev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct device *dev = filp->private_data;
	int rc;

	dev_dbg(dev, "%s\n", __func__);

	device_lock(dev);
	rc = dax_dev_check_vma(dev, vma, __func__);
	device_unlock(dev);
	if (rc)
		return rc;

	get_device(dev);
	vma->vm_ops = &dax_dev_vm_ops;
	vma->vm_flags |= VM_MIXEDMAP | VM_HUGEPAGE;
	return 0;

}

static const struct file_operations dax_fops = {
	.llseek = noop_llseek,
	.owner = THIS_MODULE,
	.open = dax_dev_open,
	.release = dax_dev_release,
	.get_unmapped_area = dax_dev_get_unmapped_area,
	.mmap = dax_dev_mmap,
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
