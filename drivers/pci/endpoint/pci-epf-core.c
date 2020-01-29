// SPDX-License-Identifier: GPL-2.0
/**
 * PCI Endpoint *Function* (EPF) library
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci-ep-cfs.h>

static DEFINE_MUTEX(pci_epf_mutex);

static struct bus_type pci_epf_bus_type;
static const struct device_type pci_epf_type;

static void pci_epf_dma_callback(void *param)
{
	struct pci_epf *epf = param;

	complete(&epf->transfer_complete);
}

/**
 * pci_epf_data_transfer() - Helper to use dmaengine API to transfer data
 *			     between PCIe EP and remote PCIe RC
 * @epf: the EPF device that performs the data transfer operation
 * @dma_dst: The destination address of the data transfer. It can be a physical
 *	     address given by pci_epc_mem_alloc_addr or DMA mapping APIs.
 * @dma_src: The source address of the data transfer. It can be a physical
 *	     address given by pci_epc_mem_alloc_addr or DMA mapping APIs.
 * @len: The size of the data transfer
 *
 * Helper to use dmaengine API to transfer data between PCIe EP and remote PCIe
 * RC. The source and destination address can be a physical address given by
 * pci_epc_mem_alloc_addr or the one obtained using DMA mapping APIs.
 *
 * The function returns '0' on success and negative value on failure.
 */
int pci_epf_data_transfer(struct pci_epf *epf, dma_addr_t dma_dst,
			  dma_addr_t dma_src, size_t len)
{
	enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	struct dma_chan *chan = epf->dma_chan;
	struct dma_async_tx_descriptor *tx;
	struct device *dev = &epf->dev;
	dma_cookie_t cookie;
	int ret;

	if (IS_ERR_OR_NULL(epf)) {
		dev_err(dev, "Invalid EPF device\n");
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(chan)) {
		dev_err(dev, "Invalid DMA memcpy channel\n");
		return -EINVAL;
	}

	tx = dmaengine_prep_dma_memcpy(chan, dma_dst, dma_src, len, flags);
	if (!tx) {
		dev_err(dev, "Failed to prepare DMA memcpy\n");
		return -EIO;
	}

	tx->callback = pci_epf_dma_callback;
	tx->callback_param = epf;
	cookie = tx->tx_submit(tx);
	reinit_completion(&epf->transfer_complete);

	ret = dma_submit_error(cookie);
	if (ret) {
		dev_err(dev, "Failed to do DMA tx_submit %d\n", cookie);
		return -EIO;
	}

	dma_async_issue_pending(chan);
	ret = wait_for_completion_interruptible(&epf->transfer_complete);
	if (ret < 0) {
		dmaengine_terminate_sync(chan);
		dev_err(dev, "DMA wait_for_completion_timeout\n");
		return -ETIMEDOUT;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pci_epf_data_transfer);

/**
 * pci_epf_init_dma_chan() - Helper to initialize EPF DMA channel
 * @epf: the EPF device that has to perform the data transfer operation
 *
 * Helper to initialize EPF DMA channel.
 */
int pci_epf_init_dma_chan(struct pci_epf *epf)
{
	struct device *dev = &epf->dev;
	struct dma_chan *dma_chan;
	dma_cap_mask_t mask;
	int ret;

	if (IS_ERR_OR_NULL(epf)) {
		dev_err(dev, "Invalid EPF device\n");
		return -EINVAL;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	dma_chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(dma_chan)) {
		ret = PTR_ERR(dma_chan);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get DMA channel\n");
		return ret;
	}
	init_completion(&epf->transfer_complete);

	epf->dma_chan = dma_chan;

	return 0;
}
EXPORT_SYMBOL_GPL(pci_epf_init_dma_chan);

/**
 * pci_epf_clean_dma_chan() - Helper to cleanup EPF DMA channel
 * @epf: the EPF device that performed the data transfer operation
 *
 * Helper to cleanup EPF DMA channel.
 */
void pci_epf_clean_dma_chan(struct pci_epf *epf)
{
	struct device *dev = &epf->dev;

	if (IS_ERR_OR_NULL(epf)) {
		dev_err(dev, "Invalid EPF device\n");
		return;
	}

	dma_release_channel(epf->dma_chan);
	epf->dma_chan = NULL;
}
EXPORT_SYMBOL_GPL(pci_epf_clean_dma_chan);

/**
 * pci_epf_tx() - transfer data between EPC and remote PCIe RC
 * @epf: the EPF device that performs the data transfer operation
 * @dma_dst: The destination address of the data transfer. It can be a physical
 *	     address given by pci_epc_mem_alloc_addr or DMA mapping APIs.
 * @dma_src: The source address of the data transfer. It can be a physical
 *	     address given by pci_epc_mem_alloc_addr or DMA mapping APIs.
 * @len: The size of the data transfer
 *
 * Invoke to transfer data between EPC and remote PCIe RC. The source and
 * destination address can be a physical address given by pci_epc_mem_alloc_addr
 * or the one obtained using DMA mapping APIs.
 */
int pci_epf_tx(struct pci_epf *epf, dma_addr_t dma_dst,
	       dma_addr_t dma_src, size_t len)
{
	int ret;
	struct pci_epc *epc = epf->epc;

	if (IS_ERR_OR_NULL(epc) || IS_ERR_OR_NULL(epf))
		return -EINVAL;

	if (!epc->ops->data_transfer)
		return -EINVAL;

	mutex_lock(&epf->lock);
	ret = epc->ops->data_transfer(epc, epf, dma_dst, dma_src, len);
	mutex_unlock(&epf->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epf_tx);

/**
 * pci_epf_unbind() - Notify the function driver that the binding between the
 *		      EPF device and EPC device has been lost
 * @epf: the EPF device which has lost the binding with the EPC device
 *
 * Invoke to notify the function driver that the binding between the EPF device
 * and EPC device has been lost.
 */
void pci_epf_unbind(struct pci_epf *epf)
{
	struct pci_epf *epf_vf;

	if (!epf->driver) {
		dev_WARN(&epf->dev, "epf device not bound to driver\n");
		return;
	}

	mutex_lock(&epf->lock);
	list_for_each_entry(epf_vf, &epf->pci_vepf, list) {
		if (epf_vf->is_bound)
			epf_vf->driver->ops->unbind(epf_vf);
	}
	if (epf->is_bound)
		epf->driver->ops->unbind(epf);
	mutex_unlock(&epf->lock);
	module_put(epf->driver->owner);
}
EXPORT_SYMBOL_GPL(pci_epf_unbind);

/**
 * pci_epf_bind() - Notify the function driver that the EPF device has been
 *		    bound to a EPC device
 * @epf: the EPF device which has been bound to the EPC device
 *
 * Invoke to notify the function driver that it has been bound to a EPC device
 */
int pci_epf_bind(struct pci_epf *epf)
{
	struct pci_epf *epf_vf;
	int ret;

	if (!epf->driver) {
		dev_WARN(&epf->dev, "epf device not bound to driver\n");
		return -EINVAL;
	}

	if (!try_module_get(epf->driver->owner))
		return -EAGAIN;

	mutex_lock(&epf->lock);
	list_for_each_entry(epf_vf, &epf->pci_vepf, list) {
		epf_vf->func_no = epf->func_no;
		epf_vf->epc = epf->epc;
		ret = epf_vf->driver->ops->bind(epf_vf);
		if (ret)
			goto ret;
		epf_vf->is_bound = true;
	}

	ret = epf->driver->ops->bind(epf);
	if (ret)
		goto ret;
	epf->is_bound = true;

	mutex_unlock(&epf->lock);
	return 0;

ret:
	mutex_unlock(&epf->lock);
	pci_epf_unbind(epf);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epf_bind);

/**
 * pci_epf_add_vepf() - associate virtual EP function to physical EP function
 * @epf_pf: the physical EP function to which the virtual EP function should be
 *   associated
 * @epf_vf: the virtual EP function to be added
 *
 * A physical endpoint function can be associated with multiple virtual
 * endpoint functions. Invoke pci_epf_add_epf() to add a virtual PCI endpoint
 * function to a physical PCI endpoint function.
 */
int pci_epf_add_vepf(struct pci_epf *epf_pf, struct pci_epf *epf_vf)
{
	u32 vfunc_no;

	if (IS_ERR_OR_NULL(epf_pf) || IS_ERR_OR_NULL(epf_vf))
		return -EINVAL;

	if (epf_pf->epc || epf_vf->epc || epf_vf->epf_pf)
		return -EBUSY;

	mutex_lock(&epf_pf->lock);
	vfunc_no = find_first_zero_bit(&epf_pf->vfunction_num_map,
				       BITS_PER_LONG);
	if (vfunc_no >= BITS_PER_LONG)
		return -EINVAL;

	set_bit(vfunc_no, &epf_pf->vfunction_num_map);
	epf_vf->vfunc_no = vfunc_no;

	epf_vf->epf_pf = epf_pf;
	epf_vf->is_vf = true;

	list_add_tail(&epf_vf->list, &epf_pf->pci_vepf);
	mutex_unlock(&epf_pf->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_epf_add_vepf);

/**
 * pci_epf_remove_vepf() - remove virtual EP function from physical EP function
 * @epf_pf: the physical EP function from which the virtual EP function should
 *   be removed
 * @epf_vf: the virtual EP function to be removed
 *
 * Invoke to remove a virtual endpoint function from the physcial endpoint
 * function.
 */
void pci_epf_remove_vepf(struct pci_epf *epf_pf, struct pci_epf *epf_vf)
{
	if (IS_ERR_OR_NULL(epf_pf) || IS_ERR_OR_NULL(epf_vf))
		return;

	mutex_lock(&epf_pf->lock);
	clear_bit(epf_vf->vfunc_no, &epf_pf->vfunction_num_map);
	list_del(&epf_vf->list);
	mutex_unlock(&epf_pf->lock);
}
EXPORT_SYMBOL_GPL(pci_epf_remove_vepf);

/**
 * pci_epf_free_space() - free the allocated PCI EPF register space
 * @addr: the virtual address of the PCI EPF register space
 * @bar: the BAR number corresponding to the register space
 * @type: Identifies if the allocated space is for primary EPC or secondary EPC
 *
 * Invoke to free the allocated PCI EPF register space.
 */
void pci_epf_free_space(struct pci_epf *epf, void *addr, enum pci_barno bar,
			enum pci_epc_interface_type type)
{
	struct device *dev = epf->epc->dev.parent;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc;

	if (!addr)
		return;

	if (type == PRIMARY_INTERFACE) {
		epc = epf->epc;
		epf_bar = epf->bar;
	} else {
		epc = epf->sec_epc;
		epf_bar = epf->sec_epc_bar;
	}

	dev = epc->dev.parent;
	dma_free_coherent(dev, epf_bar[bar].size, addr,
			  epf_bar[bar].phys_addr);

	epf_bar[bar].phys_addr = 0;
	epf_bar[bar].addr = NULL;
	epf_bar[bar].size = 0;
	epf_bar[bar].barno = 0;
	epf_bar[bar].flags = 0;
}
EXPORT_SYMBOL_GPL(pci_epf_free_space);

/**
 * pci_epf_alloc_space() - allocate memory for the PCI EPF register space
 * @size: the size of the memory that has to be allocated
 * @bar: the BAR number corresponding to the allocated register space
 * @align: alignment size for the allocation region
 * @type: Identifies if the allocation is for primary EPC or secondary EPC
 *
 * Invoke to allocate memory for the PCI EPF register space.
 */
void *pci_epf_alloc_space(struct pci_epf *epf, size_t size, enum pci_barno bar,
			  size_t align, enum pci_epc_interface_type type)
{
	struct pci_epf_bar *epf_bar;
	dma_addr_t phys_addr;
	struct pci_epc *epc;
	struct device *dev;
	void *space;

	if (size < 128)
		size = 128;

	if (align)
		size = ALIGN(size, align);
	else
		size = roundup_pow_of_two(size);

	if (type == PRIMARY_INTERFACE) {
		epc = epf->epc;
		epf_bar = epf->bar;
	} else {
		epc = epf->sec_epc;
		epf_bar = epf->sec_epc_bar;
	}

	dev = epc->dev.parent;
	space = dma_alloc_coherent(dev, size, &phys_addr, GFP_KERNEL);
	if (!space) {
		dev_err(dev, "failed to allocate mem space\n");
		return NULL;
	}

	epf_bar[bar].phys_addr = phys_addr;
	epf_bar[bar].addr = space;
	epf_bar[bar].size = size;
	epf_bar[bar].barno = bar;
	epf_bar[bar].flags |= upper_32_bits(size) ?
				PCI_BASE_ADDRESS_MEM_TYPE_64 :
				PCI_BASE_ADDRESS_MEM_TYPE_32;

	return space;
}
EXPORT_SYMBOL_GPL(pci_epf_alloc_space);

static void pci_epf_remove_cfs(struct pci_epf_driver *driver)
{
	struct config_group *group, *tmp;

	if (!IS_ENABLED(CONFIG_PCI_ENDPOINT_CONFIGFS))
		return;

	mutex_lock(&pci_epf_mutex);
	list_for_each_entry_safe(group, tmp, &driver->epf_group, group_entry)
		pci_ep_cfs_remove_epf_group(group);
	list_del(&driver->epf_group);
	mutex_unlock(&pci_epf_mutex);
}

/**
 * pci_epf_unregister_driver() - unregister the PCI EPF driver
 * @driver: the PCI EPF driver that has to be unregistered
 *
 * Invoke to unregister the PCI EPF driver.
 */
void pci_epf_unregister_driver(struct pci_epf_driver *driver)
{
	pci_epf_remove_cfs(driver);
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL_GPL(pci_epf_unregister_driver);

static int pci_epf_add_cfs(struct pci_epf_driver *driver)
{
	struct config_group *group;
	const struct pci_epf_device_id *id;

	if (!IS_ENABLED(CONFIG_PCI_ENDPOINT_CONFIGFS))
		return 0;

	INIT_LIST_HEAD(&driver->epf_group);

	id = driver->id_table;
	while (id->name[0]) {
		group = pci_ep_cfs_add_epf_group(id->name);
		if (IS_ERR(group)) {
			pci_epf_remove_cfs(driver);
			return PTR_ERR(group);
		}

		mutex_lock(&pci_epf_mutex);
		list_add_tail(&group->group_entry, &driver->epf_group);
		mutex_unlock(&pci_epf_mutex);
		id++;
	}

	return 0;
}

/**
 * __pci_epf_register_driver() - register a new PCI EPF driver
 * @driver: structure representing PCI EPF driver
 * @owner: the owner of the module that registers the PCI EPF driver
 *
 * Invoke to register a new PCI EPF driver.
 */
int __pci_epf_register_driver(struct pci_epf_driver *driver,
			      struct module *owner)
{
	int ret;

	if (!driver->ops || !driver->ops->bind || !driver->ops->unbind)
		pr_debug("%s: Supports only pci_epf device created using DT\n",
			 driver->driver.name);

	driver->driver.bus = &pci_epf_bus_type;
	driver->driver.owner = owner;

	ret = driver_register(&driver->driver);
	if (ret)
		return ret;

	pci_epf_add_cfs(driver);

	return 0;
}
EXPORT_SYMBOL_GPL(__pci_epf_register_driver);

/**
 * pci_epf_destroy() - destroy the created PCI EPF device
 * @epf: the PCI EPF device that has to be destroyed.
 *
 * Invoke to destroy the PCI EPF device created by invoking pci_epf_create().
 */
void pci_epf_destroy(struct pci_epf *epf)
{
	device_unregister(&epf->dev);
}
EXPORT_SYMBOL_GPL(pci_epf_destroy);

/**
 * pci_epf_create() - create a new PCI EPF device
 * @name: the name of the PCI EPF device. This name will be used to bind the
 *	  the EPF device to a EPF driver
 *
 * Invoke to create a new PCI EPF device by providing the name of the function
 * device.
 */
struct pci_epf *pci_epf_create(const char *name)
{
	int ret;
	struct pci_epf *epf;
	struct device *dev;
	int len;

	epf = kzalloc(sizeof(*epf), GFP_KERNEL);
	if (!epf)
		return ERR_PTR(-ENOMEM);

	len = strchrnul(name, '.') - name;
	epf->name = kstrndup(name, len, GFP_KERNEL);
	if (!epf->name) {
		kfree(epf);
		return ERR_PTR(-ENOMEM);
	}

	/* VFs are numbered starting with 1. So set BIT(0) by default */
	epf->vfunction_num_map = 1;
	INIT_LIST_HEAD(&epf->pci_vepf);

	dev = &epf->dev;
	device_initialize(dev);
	dev->bus = &pci_epf_bus_type;
	dev->type = &pci_epf_type;
	mutex_init(&epf->lock);

	ret = dev_set_name(dev, "%s", name);
	if (ret) {
		put_device(dev);
		return ERR_PTR(ret);
	}

	ret = device_add(dev);
	if (ret) {
		put_device(dev);
		return ERR_PTR(ret);
	}

	return epf;
}
EXPORT_SYMBOL_GPL(pci_epf_create);

/**
 * pci_epf_of_create() - create a new PCI EPF device from device tree node
 * @node: the device node of the PCI EPF device.
 *
 * Invoke to create a new PCI EPF device by providing a device tree node
 * with compatible property.
 */
struct pci_epf *pci_epf_of_create(struct device_node *node)
{
	struct pci_epf *epf;
	const char *compat;
	int ret;

	of_node_get(node);

	ret = of_property_read_string(node, "compatible", &compat);
	if (ret) {
		of_node_put(node);
		return ERR_PTR(ret);
	}

	epf = pci_epf_create(compat);
	if (!IS_ERR(epf))
		epf->node = node;

	return epf;
}
EXPORT_SYMBOL_GPL(pci_epf_of_create);

static void devm_epf_release(struct device *dev, void *res)
{
	struct pci_epf *epf = *(struct pci_epf **)res;

	pci_epf_destroy(epf);
}

/**
 * devm_pci_epf_of_create() - create a new PCI EPF device from device tree node
 * @dev: device that is creating the new EPF
 * @node: the device node of the PCI EPF device.
 *
 * Invoke to create a new PCI EPF device by providing a device tree node with
 * compatible property. While at that, it also associates the device with the
 * EPF using devres. On driver detach, release function is invoked on the devres
 * data, where devres data is freed.
 */
struct pci_epf *devm_pci_epf_of_create(struct device *dev,
				       struct device_node *node)
{
	struct pci_epf **ptr, *epf;

	ptr = devres_alloc(devm_epf_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	epf = pci_epf_of_create(node);
	if (!IS_ERR(epf)) {
		*ptr = epf;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return epf;
}
EXPORT_SYMBOL_GPL(devm_pci_epf_of_create);

static void pci_epf_dev_release(struct device *dev)
{
	struct pci_epf *epf = to_pci_epf(dev);

	of_node_put(epf->node);
	kfree(epf->name);
	kfree(epf);
}

static const struct device_type pci_epf_type = {
	.release	= pci_epf_dev_release,
};

static int
pci_epf_match_id(const struct pci_epf_device_id *id, const struct pci_epf *epf)
{
	while (id->name[0]) {
		if (strcmp(epf->name, id->name) == 0)
			return true;
		id++;
	}

	return false;
}

static int pci_epf_device_match(struct device *dev, struct device_driver *drv)
{
	struct pci_epf *epf = to_pci_epf(dev);
	struct pci_epf_driver *driver = to_pci_epf_driver(drv);

	if (driver->id_table)
		return pci_epf_match_id(driver->id_table, epf);

	return !strcmp(epf->name, drv->name);
}

static int pci_epf_device_probe(struct device *dev)
{
	struct pci_epf *epf = to_pci_epf(dev);
	struct pci_epf_driver *driver = to_pci_epf_driver(dev->driver);

	if (!driver->probe)
		return -ENODEV;

	epf->driver = driver;

	return driver->probe(epf);
}

static int pci_epf_device_remove(struct device *dev)
{
	int ret = 0;
	struct pci_epf *epf = to_pci_epf(dev);
	struct pci_epf_driver *driver = to_pci_epf_driver(dev->driver);

	if (driver->remove)
		ret = driver->remove(epf);
	epf->driver = NULL;

	return ret;
}

static struct bus_type pci_epf_bus_type = {
	.name		= "pci-epf",
	.match		= pci_epf_device_match,
	.probe		= pci_epf_device_probe,
	.remove		= pci_epf_device_remove,
};

static int __init pci_epf_init(void)
{
	int ret;

	ret = bus_register(&pci_epf_bus_type);
	if (ret) {
		pr_err("failed to register pci epf bus --> %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(pci_epf_init);

static void __exit pci_epf_exit(void)
{
	bus_unregister(&pci_epf_bus_type);
}
module_exit(pci_epf_exit);

MODULE_DESCRIPTION("PCI EPF Library");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_LICENSE("GPL v2");
