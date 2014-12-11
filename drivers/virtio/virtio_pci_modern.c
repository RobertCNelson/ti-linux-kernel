/*
 * Virtio PCI driver - legacy device support
 *
 * This module allows virtio devices to be used over a virtual PCI device.
 * This can be used with QEMU based VMMs like KVM or Xen.
 *
 * Copyright IBM Corp. 2007
 * Copyright Red Hat, Inc. 2014
 *
 * Authors:
 *  Anthony Liguori  <aliguori@us.ibm.com>
 *  Rusty Russell <rusty@rustcorp.com.au>
 *  Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#define VIRTIO_PCI_NO_LEGACY
#include "virtio_pci_common.h"

static void __iomem *map_capability(struct pci_dev *dev, int off,
				    size_t minlen,
				    u32 align,
				    u32 start, u32 size,
				    size_t *len)
{
	u8 type_and_bar, bar;
	u32 offset, length;
	void __iomem *p;

	pci_read_config_byte(dev, off + offsetof(struct virtio_pci_cap,
						 type_and_bar),
			     &type_and_bar);
	pci_read_config_dword(dev, off + offsetof(struct virtio_pci_cap, offset),
			     &offset);
	pci_read_config_dword(dev, off + offsetof(struct virtio_pci_cap, length),
			      &length);

	if (length < minlen) {
		dev_err(&dev->dev,
			"virtio_pci: small capability len %u (%zu expected)\n",
			length, minlen);
		return NULL;
	}

	if (offset & (align - 1)) {
		dev_err(&dev->dev,
			"virtio_pci: offset %u not aligned to %u\n",
			offset, align);
		return NULL;
	}

	if (length > size)
		length = size;

	if (len)
		*len = length;

	bar = (type_and_bar >> VIRTIO_PCI_CAP_BAR_SHIFT) &
		VIRTIO_PCI_CAP_BAR_MASK;

	if (minlen + offset < minlen ||
	    minlen + offset > pci_resource_len(dev, bar))
		dev_err(&dev->dev,
			"virtio_pci: map virtio %zu@%u "
			"out of range on bar %i length %lu\n",
			minlen, offset,
			bar, (unsigned long)pci_resource_len(dev, bar));

	p = pci_iomap_range(dev, bar, offset, length);
	if (!p)
		dev_err(&dev->dev,
			"virtio_pci: unable to map virtio %u@%u on bar %i\n",
			length, offset, bar);
	return p;
}

static void iowrite64_twopart(u64 val, __le32 __iomem *lo, __le32 __iomem *hi)
{
	iowrite32((u32)val, lo);
	iowrite32(val >> 32, hi);
}

/* virtio config->get_features() implementation */
static u64 vp_get_features(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	u64 features;

	iowrite32(0, &vp_dev->common->device_feature_select);
	features = ioread32(&vp_dev->common->device_feature);
	iowrite32(1, &vp_dev->common->device_feature_select);
	features |= ((u64)ioread32(&vp_dev->common->device_feature) << 32);

	return features;
}

/* virtio config->finalize_features() implementation */
static int vp_finalize_features(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	/* Give virtio_ring a chance to accept features. */
	vring_transport_features(vdev);

	if (!__virtio_test_bit(vdev, VIRTIO_F_VERSION_1)) {
		dev_err(&vdev->dev, "virtio: device uses modern interface "
			"but does not have VIRTIO_F_VERSION_1\n");
		return -EINVAL;
	}

	iowrite32(0, &vp_dev->common->guest_feature_select);
	iowrite32((u32)vdev->features, &vp_dev->common->guest_feature);
	iowrite32(1, &vp_dev->common->guest_feature_select);
	iowrite32(vdev->features >> 32, &vp_dev->common->guest_feature);

	return 0;
}

/* virtio config->get() implementation */
static void vp_get(struct virtio_device *vdev, unsigned offset,
		   void *buf, unsigned len)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	u8 b;
	__le16 w;
	__le32 l;

	switch (len) {
	case 1:
		b = ioread8(vp_dev->device + offset);
		memcpy(buf, &b, sizeof b);
		break;
	case 2:
		w = cpu_to_le16(ioread16(vp_dev->device + offset));
		memcpy(buf, &w, sizeof w);
		break;
	case 4:
		l = cpu_to_le32(ioread32(vp_dev->device + offset));
		memcpy(buf, &l, sizeof l);
		break;
	case 8:
		l = cpu_to_le32(ioread32(vp_dev->device + offset));
		memcpy(buf, &l, sizeof l);
		l = cpu_to_le32(ioread32(vp_dev->device + offset + sizeof l));
		memcpy(buf + sizeof l, &l, sizeof l);
		break;
	default:
		BUG();
	}
}

/* the config->set() implementation.  it's symmetric to the config->get()
 * implementation */
static void vp_set(struct virtio_device *vdev, unsigned offset,
		   const void *buf, unsigned len)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	u8 b;
	__le16 w;
	__le32 l;

	switch (len) {
	case 1:
		memcpy(&b, buf, sizeof b);
		iowrite8(b, vp_dev->device + offset);
		break;
	case 2:
		memcpy(&w, buf, sizeof w);
		iowrite16(le16_to_cpu(w), vp_dev->device + offset);
		break;
	case 4:
		memcpy(&l, buf, sizeof l);
		iowrite32(le32_to_cpu(l), vp_dev->device + offset);
		break;
	case 8:
		memcpy(&l, buf, sizeof l);
		iowrite32(le32_to_cpu(l), vp_dev->device + offset);
		memcpy(&l, buf + sizeof l, sizeof l);
		iowrite32(le32_to_cpu(l), vp_dev->device + offset + sizeof l);
		break;
	default:
		BUG();
	}
}

static u32 vp_generation(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	return ioread8(&vp_dev->common->config_generation);
}

/* config->{get,set}_status() implementations */
static u8 vp_get_status(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	return ioread8(&vp_dev->common->device_status);
}

static void vp_set_status(struct virtio_device *vdev, u8 status)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	/* We should never be setting status to 0. */
	BUG_ON(status == 0);
	iowrite8(status, &vp_dev->common->device_status);
}

static void vp_reset(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	/* 0 status means a reset. */
	iowrite8(0, &vp_dev->common->device_status);
	/* Flush out the status write, and flush in device writes,
	 * including MSI-X interrupts, if any. */
	ioread8(&vp_dev->common->device_status);
	/* Flush pending VQ/configuration callbacks. */
	vp_synchronize_vectors(vdev);
}

static u16 vp_config_vector(struct virtio_pci_device *vp_dev, u16 vector)
{
	/* Setup the vector used for configuration events */
	iowrite16(vector, &vp_dev->common->msix_config);
	/* Verify we had enough resources to assign the vector */
	/* Will also flush the write out to device */
	return ioread16(&vp_dev->common->msix_config);
}

static size_t vring_pci_size(u16 num)
{
	/* We only need a cacheline separation. */
	return PAGE_ALIGN(vring_size(num, SMP_CACHE_BYTES));
}

static void *alloc_virtqueue_pages(int *num)
{
	void *pages;

	/* TODO: allocate each queue chunk individually */
	for (; *num && vring_pci_size(*num) > PAGE_SIZE; *num /= 2) {
		pages = alloc_pages_exact(vring_pci_size(*num),
					  GFP_KERNEL|__GFP_ZERO|__GFP_NOWARN);
		if (pages)
			return pages;
	}

	if (!*num)
		return NULL;

	/* Try to get a single page. You are my only hope! */
	return alloc_pages_exact(vring_pci_size(*num), GFP_KERNEL|__GFP_ZERO);
}

static struct virtqueue *setup_vq(struct virtio_pci_device *vp_dev,
				  struct virtio_pci_vq_info *info,
				  unsigned index,
				  void (*callback)(struct virtqueue *vq),
				  const char *name,
				  u16 msix_vec)
{
	struct virtio_pci_common_cfg __iomem *cfg = vp_dev->common;
	struct virtqueue *vq;
	u16 num, off;
	int err;

	if (index >= ioread16(&cfg->num_queues))
		return ERR_PTR(-ENOENT);

	/* Select the queue we're interested in */
	iowrite16(index, &cfg->queue_select);

	/* Check if queue is either not available or already active. */
	num = ioread16(&cfg->queue_size);
	if (!num || ioread8(&cfg->queue_enable))
		return ERR_PTR(-ENOENT);

	if (num & (num - 1)) {
		dev_warn(&vp_dev->pci_dev->dev, "bad queue size %u", num);
		return ERR_PTR(-EINVAL);
	}

	/* get offset of notification word for this vq (shouldn't wrap) */
	off = ioread16(&cfg->queue_notify_off);
	if ((u64)off * vp_dev->notify_offset_multiplier + 2
	    > vp_dev->notify_len) {
		dev_warn(&vp_dev->pci_dev->dev,
			 "bad notification offset %u (x %u) for queue %u > %zd",
			 off, vp_dev->notify_offset_multiplier, 
			 index, vp_dev->notify_len);
		return ERR_PTR(-EINVAL);
	}

	info->num = num;
	info->msix_vector = msix_vec;

	info->queue = alloc_virtqueue_pages(&info->num);
	if (info->queue == NULL)
		return ERR_PTR(-ENOMEM);

	/* create the vring */
	vq = vring_new_virtqueue(index, info->num,
				 SMP_CACHE_BYTES, &vp_dev->vdev,
				 true, info->queue, vp_notify, callback, name);
	if (!vq) {
		err = -ENOMEM;
		goto out_activate_queue;
	}

	/* activate the queue */
	iowrite16(num, &cfg->queue_size);
	iowrite64_twopart(virt_to_phys(info->queue),
			  &cfg->queue_desc_lo, &cfg->queue_desc_hi);
	iowrite64_twopart(virt_to_phys(virtqueue_get_avail(vq)),
			  &cfg->queue_avail_lo, &cfg->queue_avail_hi);
	iowrite64_twopart(virt_to_phys(virtqueue_get_avail(vq)),
			  &cfg->queue_used_lo, &cfg->queue_used_hi);

	if (vp_dev->notify_map_cap)
		vq->priv = (void __force *)map_capability(vp_dev->pci_dev,
					  vp_dev->notify_map_cap, 2, 2,
					  off * vp_dev->notify_offset_multiplier, 2,
					  NULL);
	else
		vq->priv = (void __force *)vp_dev->notify_base +
			off * vp_dev->notify_offset_multiplier;

	if (!vq->priv) {
		err = -ENOMEM;
		goto out_map;
	}

	if (msix_vec != VIRTIO_MSI_NO_VECTOR) {
		iowrite16(msix_vec, &cfg->queue_msix_vector);
		msix_vec = ioread16(&cfg->queue_msix_vector);
		if (msix_vec == VIRTIO_MSI_NO_VECTOR) {
			err = -EBUSY;
			goto out_assign;
		}
	}

	return vq;

out_assign:
	if (vp_dev->notify_map_cap)
		pci_iounmap(vp_dev->pci_dev, (void __force *)vq->priv);
out_map:
	vring_del_virtqueue(vq);
out_activate_queue:
	free_pages_exact(info->queue, vring_pci_size(info->num));
	return ERR_PTR(err);
}

static int vp_modern_find_vqs(struct virtio_device *vdev, unsigned nvqs,
			      struct virtqueue *vqs[],
			      vq_callback_t *callbacks[],
			      const char *names[])
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	struct virtqueue *vq;
	int rc = vp_find_vqs(vdev, nvqs, vqs, callbacks, names);

	if (rc)
		return rc;

	/* Select and activate all queues. Has to be done last: once we do
	 * this, there's no way to go back except reset.
	 */
	list_for_each_entry(vq, &vdev->vqs, list) {
		iowrite16(vq->index, &vp_dev->common->queue_select);
		iowrite8(1, &vp_dev->common->queue_enable);
	}

	return 0;
}

static void del_vq(struct virtio_pci_vq_info *info)
{
	struct virtqueue *vq = info->vq;
	struct virtio_pci_device *vp_dev = to_vp_device(vq->vdev);

	iowrite16(vq->index, &vp_dev->common->queue_select);

	if (vp_dev->msix_enabled) {
		iowrite16(VIRTIO_MSI_NO_VECTOR,
			  &vp_dev->common->queue_msix_vector);
		/* Flush the write out to device */
		ioread16(&vp_dev->common->queue_msix_vector);
	}

	if (vp_dev->notify_map_cap)
		pci_iounmap(vp_dev->pci_dev, (void __force *)vq->priv);

	vring_del_virtqueue(vq);

	free_pages_exact(info->queue, vring_pci_size(info->num));
}

static const struct virtio_config_ops virtio_pci_config_ops = {
	.get		= vp_get,
	.set		= vp_set,
	.generation	= vp_generation,
	.get_status	= vp_get_status,
	.set_status	= vp_set_status,
	.reset		= vp_reset,
	.find_vqs	= vp_modern_find_vqs,
	.del_vqs	= vp_del_vqs,
	.get_features	= vp_get_features,
	.finalize_features = vp_finalize_features,
	.bus_name	= vp_bus_name,
	.set_vq_affinity = vp_set_vq_affinity,
};

/**
 * virtio_pci_find_capability - walk capabilities to find device info.
 * @dev: the pci device
 * @cfg_type: the VIRTIO_PCI_CAP_* value we seek
 * @ioresource_types: IORESOURCE_MEM and/or IORESOURCE_IO.
 *
 * Returns offset of the capability, or 0.
 */
static inline int virtio_pci_find_capability(struct pci_dev *dev, u8 cfg_type,
					     u32 ioresource_types)
{
	int pos;

	for (pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
	     pos > 0;
	     pos = pci_find_next_capability(dev, pos, PCI_CAP_ID_VNDR)) {
		u8 type_and_bar, type, bar;
		pci_read_config_byte(dev, pos + offsetof(struct virtio_pci_cap,
							 type_and_bar),
				     &type_and_bar);

		type = (type_and_bar >> VIRTIO_PCI_CAP_TYPE_SHIFT) &
			VIRTIO_PCI_CAP_TYPE_MASK;
		bar = (type_and_bar >> VIRTIO_PCI_CAP_BAR_SHIFT) &
			VIRTIO_PCI_CAP_BAR_MASK;

		if (type == cfg_type) {
			if (pci_resource_flags(dev, bar) & ioresource_types)
				return pos;
		}
	}
	return 0;
}

/* the PCI probing function */
int virtio_pci_modern_probe(struct pci_dev *pci_dev,
			    const struct pci_device_id *id)
{
	struct virtio_pci_device *vp_dev;
	int err, common, isr, notify, device;
	struct virtio_device_id virtio_id;
	u32 notify_length;

	/* We only own devices >= 0x1000 and <= 0x107f: leave the rest. */
	if (pci_dev->device < 0x1000 || pci_dev->device > 0x107f)
		return -ENODEV;

	if (pci_dev->device < 0x1040) {
		/* Transitional devices: use the PCI subsystem device id as
		 * virtio device id, same as legacy driver always did.
		 */
		virtio_id.device = pci_dev->subsystem_device;
	} else {
		/* Modern devices: simply use PCI device id, but start from 0x1040. */
		virtio_id.device = pci_dev->device - 0x1040;
	}
	virtio_id.vendor = pci_dev->subsystem_vendor;

	if (virtio_device_is_legacy_only(virtio_id))
		return -ENODEV;

	/* check for a common config: if not, use legacy mode (bar 0). */
	common = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_COMMON_CFG,
					    IORESOURCE_IO|IORESOURCE_MEM);
	if (!common) {
		dev_info(&pci_dev->dev,
			 "virtio_pci: leaving for legacy driver\n");
		return -ENODEV;
	}

	/* If common is there, these should be too... */
	isr = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_ISR_CFG,
					 IORESOURCE_IO|IORESOURCE_MEM);
	notify = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_NOTIFY_CFG,
					    IORESOURCE_IO|IORESOURCE_MEM);
	device = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_DEVICE_CFG,
					    IORESOURCE_IO|IORESOURCE_MEM);
	if (!isr || !notify || !device) {
		dev_err(&pci_dev->dev,
			"virtio_pci: missing capabilities %i/%i/%i/%i\n",
			common, isr, notify, device);
		return -EINVAL;
	}

	/* allocate our structure and fill it out */
	vp_dev = kzalloc(sizeof(struct virtio_pci_device), GFP_KERNEL);
	if (!vp_dev)
		return -ENOMEM;

	vp_dev->vdev.dev.parent = &pci_dev->dev;
	vp_dev->vdev.dev.release = virtio_pci_release_dev;
	vp_dev->vdev.config = &virtio_pci_config_ops;
	vp_dev->pci_dev = pci_dev;
	INIT_LIST_HEAD(&vp_dev->virtqueues);
	spin_lock_init(&vp_dev->lock);

	/* Disable MSI/MSIX to bring device to a known good state. */
	pci_msi_off(pci_dev);

	/* enable the device */
	err = pci_enable_device(pci_dev);
	if (err)
		goto out;

	err = pci_request_regions(pci_dev, "virtio-pci");
	if (err)
		goto out_enable_device;

	err = -EINVAL;
	vp_dev->common = map_capability(pci_dev, common,
					sizeof(struct virtio_pci_common_cfg), 4,
					0, sizeof(struct virtio_pci_common_cfg),
					NULL);
	if (!vp_dev->common)
		goto out_req_regions;
	vp_dev->isr = map_capability(pci_dev, isr, sizeof(u8), 1,
				     0, 1,
				     NULL);
	if (!vp_dev->isr)
		goto out_map_common;

	/* Read notify_off_multiplier from config space. */
	pci_read_config_dword(pci_dev,
			      notify + offsetof(struct virtio_pci_notify_cap,
						notify_off_multiplier),
			      &vp_dev->notify_offset_multiplier);
	/* Read notify length from config space. */
	pci_read_config_dword(pci_dev,
			      notify + offsetof(struct virtio_pci_notify_cap,
						cap.length),
			      &notify_length);

	/* We don't know how many VQs we'll map, ahead of the time.
	 * If notify length is small, map it all now.
	 * Otherwise, map each VQ individually later.
	 */
	if (notify_length <= PAGE_SIZE) {
		vp_dev->notify_base = map_capability(pci_dev, notify, 2, 2,
						     0, PAGE_SIZE,
						     &vp_dev->notify_len);
		if (!vp_dev->notify_len)
			goto out_map_isr;
	} else {
		vp_dev->notify_map_cap = notify;
	}

	/* Device capability is only mandatory for devices that have
	 * device-specific configuration.
	 * Again, we don't know how much we should map, but PAGE_SIZE
	 * is more than enough for all existing devices.
	 */
	vp_dev->device = map_capability(pci_dev, device, 0, 4,
					0, PAGE_SIZE,
					&vp_dev->device_len);

	pci_set_drvdata(pci_dev, vp_dev);
	pci_set_master(pci_dev);

	vp_dev->vdev.id = virtio_id;
	vp_dev->config_vector = vp_config_vector;
	vp_dev->setup_vq = setup_vq;
	vp_dev->del_vq = del_vq;

	/* finally register the virtio device */
	err = register_virtio_device(&vp_dev->vdev);
	if (err)
		goto out_set_drvdata;

	return 0;

out_set_drvdata:
	pci_set_drvdata(pci_dev, NULL);
	if (vp_dev->device)
		pci_iounmap(pci_dev, vp_dev->device);
	if (vp_dev->notify_base)
		pci_iounmap(pci_dev, vp_dev->notify_base);
out_map_isr:
	pci_iounmap(pci_dev, vp_dev->isr);
out_map_common:
	pci_iounmap(pci_dev, vp_dev->common);
out_req_regions:
	pci_release_regions(pci_dev);
out_enable_device:
	pci_disable_device(pci_dev);
out:
	kfree(vp_dev);
	return err;
}

void virtio_pci_modern_remove(struct pci_dev *pci_dev)
{
	struct virtio_pci_device *vp_dev = pci_get_drvdata(pci_dev);

	unregister_virtio_device(&vp_dev->vdev);

	vp_del_vqs(&vp_dev->vdev);
	pci_set_drvdata(pci_dev, NULL);
	if (vp_dev->device)
		pci_iounmap(pci_dev, vp_dev->device);
	if (vp_dev->notify_base)
		pci_iounmap(pci_dev, vp_dev->notify_base);
	pci_iounmap(pci_dev, vp_dev->isr);
	pci_iounmap(pci_dev, vp_dev->common);
	pci_release_regions(pci_dev);
	pci_disable_device(pci_dev);
	kfree(vp_dev);
}
