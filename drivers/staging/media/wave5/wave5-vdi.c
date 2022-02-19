// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave5 series multi-standard codec IP - low level access functions
 *
 * Copyright (C) 2021 CHIPS&MEDIA INC
 */

#include <linux/bug.h>
#include "wave5-vdi.h"
#include "wave5-vpu.h"
#include "wave5-regdefine.h"
#include <linux/delay.h>

#define VDI_SRAM_BASE_ADDR 0x00

#define VDI_SYSTEM_ENDIAN VDI_LITTLE_ENDIAN
#define VDI_128BIT_BUS_SYSTEM_ENDIAN VDI_128BIT_LITTLE_ENDIAN

static int wave5_vdi_allocate_common_memory(struct device *dev)
{
	int ret;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);

	if (!vpu_dev->common_mem.vaddr) {
		vpu_dev->common_mem.size = SIZE_COMMON;
		ret = wave5_vdi_allocate_dma_memory(vpu_dev, &vpu_dev->common_mem);
		if (ret) {
			dev_err(dev, "unable to allocate common buffer\n");
			return ret;
		}
	}

	dev_dbg(dev, "common_mem: daddr=%pad size=%zu vaddr=0x%p\n",
		&vpu_dev->common_mem.daddr, vpu_dev->common_mem.size,
			vpu_dev->common_mem.vaddr);

	return 0;
}

int wave5_vdi_init(struct device *dev)
{
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	int i;
	int ret;

	ret = wave5_vdi_allocate_common_memory(dev);
	if (ret < 0) {
		dev_err(dev, "[VDI] fail to get vpu common buffer from driver\n");
		return ret;
	}

	if (PRODUCT_CODE_W_SERIES(vpu_dev->product_code)) {
		// if BIT processor is not running.
		if (wave5_vdi_readl(vpu_dev, W5_VCPU_CUR_PC) == 0) {
			for (i = 0; i < 64; i++)
				wave5_vdi_write_register(vpu_dev, (i * 4) + 0x100, 0x0);
		}
	} else {
		WARN_ONCE(1, "unsupported product code 0x%x\n", vpu_dev->product_code);
	}

	dev_dbg(dev, "[VDI] success to init driver\n");

	return 0;
}

int wave5_vdi_release(struct device *dev)
{
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);

	vpu_dev->vdb_register = NULL;
	wave5_vdi_free_dma_memory(vpu_dev, &vpu_dev->common_mem);

	return 0;
}

void wave5_vdi_write_register(struct vpu_device *vpu_dev, unsigned int addr, unsigned int data)
{
	writel(data, vpu_dev->vdb_register + addr);
}

unsigned int wave5_vdi_readl(struct vpu_device *vpu_dev, u32 addr)
{
	return readl(vpu_dev->vdb_register + addr);
}

int wave5_vdi_clear_memory(struct vpu_device *vpu_dev, struct vpu_buf *vb)
{
	if (!vb || !vb->vaddr) {
		dev_err(vpu_dev->dev, "%s(): unable to clear unmapped buffer\n", __func__);
		return -EINVAL;
	}

	memset(vb->vaddr, 0, vb->size);
	return vb->size;
}

static void wave5_swap_endian(struct vpu_device *vpu_dev, u8 *data, int len, int endian);

int wave5_vdi_write_memory(struct vpu_device *vpu_dev, struct vpu_buf *vb, size_t offset,
			   u8 *data, int len, int endian)
{
	if (!vb || !vb->vaddr) {
		dev_err(vpu_dev->dev, "%s(): unable to write to unmapped buffer\n", __func__);
		return -EINVAL;
	}

	if (offset > vb->size || len > vb->size || offset + len > vb->size) {
		dev_err(vpu_dev->dev, "%s(): buffer too small\n", __func__);
		return -ENOSPC;
	}

	wave5_swap_endian(vpu_dev, data, len, endian);
	memcpy(vb->vaddr + offset, data, len);

	return len;
}

int wave5_vdi_allocate_dma_memory(struct vpu_device *vpu_dev, struct vpu_buf *vb)
{
	void *vaddr;
	dma_addr_t daddr;

	if (!vb->size) {
		dev_err(vpu_dev->dev, "%s(): requested size==0\n", __func__);
		return -EINVAL;
	}

	vaddr = dma_alloc_coherent(vpu_dev->dev, vb->size, &daddr, GFP_KERNEL);
	if (!vaddr)
		return -ENOMEM;
	vb->vaddr = vaddr;
	vb->daddr = daddr;

	return 0;
}

void wave5_vdi_free_dma_memory(struct vpu_device *vpu_dev, struct vpu_buf *vb)
{
	if (vb->size == 0)
		return;

	if (!vb->vaddr)
		dev_err(vpu_dev->dev, "%s(): requested free of unmapped buffer\n", __func__);
	else
		dma_free_coherent(vpu_dev->dev, vb->size, vb->vaddr, vb->daddr);

	memset(vb, 0, sizeof(*vb));
}

int wave5_vdi_convert_endian(struct vpu_device *vpu_dev, unsigned int endian)
{
	if (PRODUCT_CODE_W_SERIES(vpu_dev->product_code)) {
		switch (endian) {
		case VDI_LITTLE_ENDIAN:
			endian = 0x00;
			break;
		case VDI_BIG_ENDIAN:
			endian = 0x0f;
			break;
		case VDI_32BIT_LITTLE_ENDIAN:
			endian = 0x04;
			break;
		case VDI_32BIT_BIG_ENDIAN:
			endian = 0x03;
			break;
		}
	}

	return (endian & 0x0f);
}

static void byte_swap(unsigned char *data, int len)
{
	u8 temp;
	int i;

	for (i = 0; i < len; i += 2) {
		temp = data[i];
		data[i] = data[i + 1];
		data[i + 1] = temp;
	}
}

static void word_swap(unsigned char *data, int len)
{
	u16 temp;
	u16 *ptr = (u16 *)data;
	int i;
	s32 size = len / sizeof(uint16_t);

	for (i = 0; i < size; i += 2) {
		temp = ptr[i];
		ptr[i] = ptr[i + 1];
		ptr[i + 1] = temp;
	}
}

static void dword_swap(unsigned char *data, int len)
{
	u32 temp;
	u32 *ptr = (u32 *)data;
	s32 size = len / sizeof(uint32_t);
	int i;

	for (i = 0; i < size; i += 2) {
		temp = ptr[i];
		ptr[i] = ptr[i + 1];
		ptr[i + 1] = temp;
	}
}

static void lword_swap(unsigned char *data, int len)
{
	u64 temp;
	u64 *ptr = (u64 *)data;
	s32 size = len / sizeof(uint64_t);
	int i;

	for (i = 0; i < size; i += 2) {
		temp = ptr[i];
		ptr[i] = ptr[i + 1];
		ptr[i + 1] = temp;
	}
}

static void wave5_swap_endian(struct vpu_device *vpu_dev, u8 *data, int len, int endian)
{
	int changes;
	int sys_endian;
	bool byte_change, word_change, dword_change, lword_change;

	if (PRODUCT_CODE_W_SERIES(vpu_dev->product_code)) {
		sys_endian = VDI_128BIT_BUS_SYSTEM_ENDIAN;
	} else {
		dev_err(vpu_dev->dev, "unknown product id : %08x\n", vpu_dev->product_code);
		return;
	}

	endian = wave5_vdi_convert_endian(vpu_dev, endian);
	sys_endian = wave5_vdi_convert_endian(vpu_dev, sys_endian);
	if (endian == sys_endian)
		return;

	changes = endian ^ sys_endian;
	byte_change = changes & 0x01;
	word_change = ((changes & 0x02) == 0x02);
	dword_change = ((changes & 0x04) == 0x04);
	lword_change = ((changes & 0x08) == 0x08);

	if (byte_change)
		byte_swap(data, len);
	if (word_change)
		word_swap(data, len);
	if (dword_change)
		dword_swap(data, len);
	if (lword_change)
		lword_swap(data, len);
}

