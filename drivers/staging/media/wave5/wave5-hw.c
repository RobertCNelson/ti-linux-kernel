// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave5 series multi-standard codec IP - wave5 backend logic
 *
 * Copyright (C) 2021 CHIPS&MEDIA INC
 */

#include <linux/iopoll.h>
#include "wave5-vpu.h"
#include "wave5.h"
#include "wave5-regdefine.h"

#define FIO_TIMEOUT 10000000
#define FIO_CTRL_READY		BIT(31)
#define FIO_CTRL_WRITE		BIT(16)
#define VPU_BUSY_CHECK_TIMEOUT          10000000

static void wave5_print_reg_err(struct vpu_device *vpu_dev, u32 reg_fail_reason)
{
	char *caller = __builtin_return_address(0);
	struct device *dev = vpu_dev->dev;
	u32 reg_val;

	switch (reg_fail_reason) {
	case WAVE5_SYSERR_QUEUEING_FAIL:
		reg_val = vpu_read_reg(vpu_dev, W5_RET_QUEUE_FAIL_REASON);
		dev_dbg(dev, "%s: queueing failure 0x%x\n", caller, reg_val);
		break;
	case WAVE5_SYSERR_RESULT_NOT_READY:
		dev_err(dev, "%s: result not ready 0x%x\n", caller, reg_fail_reason);
		break;
	case WAVE5_SYSERR_ACCESS_VIOLATION_HW:
		dev_err(dev, "%s: access violation 0x%x\n", caller, reg_fail_reason);
		break;
	case WAVE5_SYSERR_WATCHDOG_TIMEOUT:
		dev_err(dev, "%s: watchdog timeout 0x%x\n", caller, reg_fail_reason);
		break;
	case WAVE5_SYSERR_BUS_ERROR:
		dev_err(dev, "%s: bus error 0x%x\n", caller, reg_fail_reason);
		break;
	case WAVE5_SYSERR_DOUBLE_FAULT:
		dev_err(dev, "%s: double fault 0x%x\n", caller, reg_fail_reason);
		break;
	case WAVE5_SYSERR_VPU_STILL_RUNNING:
		dev_err(dev, "%s: still running 0x%x\n", caller, reg_fail_reason);
		break;
	case WAVE5_SYSERR_VLC_BUF_FULL:
		dev_err(dev, "%s: vlc buf full 0x%x\n", caller, reg_fail_reason);
		break;
	default:
		dev_err(dev, "%s: failure: 0x%x\n", caller, reg_fail_reason);
		break;
	}
}

static int wave5_wait_fio_readl(struct vpu_device *vpu_dev, u32 addr, u32 val)
{
	u32 ctrl;
	int ret;

	ctrl = addr & 0xffff;
	wave5_vdi_write_register(vpu_dev, W5_VPU_FIO_CTRL_ADDR, ctrl);
	ret = read_poll_timeout(wave5_vdi_readl, ctrl, ctrl & FIO_CTRL_READY,
				0, FIO_TIMEOUT, false, vpu_dev, W5_VPU_FIO_CTRL_ADDR);
	if (ret)
		return ret;
	if (wave5_vdi_readl(vpu_dev, W5_VPU_FIO_DATA) != val)
		return -ETIMEDOUT;
	return 0;
}

static void wave5_fio_writel(struct vpu_device *vpu_dev, unsigned int addr, unsigned int data)
{
	unsigned int ctrl;

	wave5_vdi_write_register(vpu_dev, W5_VPU_FIO_DATA, data);
	ctrl = FIO_CTRL_WRITE | (addr & 0xffff);
	wave5_vdi_write_register(vpu_dev, W5_VPU_FIO_CTRL_ADDR, ctrl);
	read_poll_timeout(wave5_vdi_readl, ctrl, ctrl & FIO_CTRL_READY,
			  0, FIO_TIMEOUT, false, vpu_dev, W5_VPU_FIO_CTRL_ADDR);
}

static int wave5_wait_bus_busy(struct vpu_device *vpu_dev, unsigned int addr)
{
	u32 gdi_status_check_value = 0x3f;

	if (vpu_dev->product_code == WAVE521C_CODE ||
	    vpu_dev->product_code == WAVE521_CODE ||
	    vpu_dev->product_code == WAVE521E1_CODE)
		gdi_status_check_value = 0x00ff1f3f;

	return wave5_wait_fio_readl(vpu_dev, addr, gdi_status_check_value);
}

static int wave5_wait_vpu_busy(struct vpu_device *vpu_dev, unsigned int addr)
{
	u32 data;

	return read_poll_timeout(wave5_vdi_readl, data, data == 0,
				 0, VPU_BUSY_CHECK_TIMEOUT, false, vpu_dev, addr);
}

static int wave5_wait_vcpu_bus_busy(struct vpu_device *vpu_dev, unsigned int addr)
{
	return wave5_wait_fio_readl(vpu_dev, addr, 0);
}

bool wave5_vpu_is_init(struct vpu_device *vpu_dev)
{
	return vpu_read_reg(vpu_dev, W5_VCPU_CUR_PC) != 0;
}

static struct dma_vpu_buf *get_sram_memory(struct vpu_device *vpu_dev)
{
	u32 sram_size = 0;
	u32 val = vpu_read_reg(vpu_dev, W5_PRODUCT_NUMBER);

	if (vpu_dev->sram_buf.size)
		return &vpu_dev->sram_buf;

	switch (val) {
	case WAVE511_CODE:
		/* 10bit profile : 8_kx8_k -> 129024, 4_kx2_k -> 64512 */
		sram_size = 0x1F800;
		break;
	case WAVE517_CODE:
		/* 10bit profile : 8_kx8_k -> 272384, 4_kx2_k -> 104448 */
		sram_size = 0x42800;
		break;
	case WAVE537_CODE:
		/* 10bit profile : 8_kx8_k -> 272384, 4_kx2_k -> 104448 */
		sram_size = 0x42800;
		break;
	case WAVE521_CODE:
		/* 10bit profile : 8_kx8_k -> 126976, 4_kx2_k -> 63488 */
		sram_size = 0x1F000;
		break;
	case WAVE521E1_CODE:
		/* 10bit profile : 8_kx8_k -> 126976, 4_kx2_k -> 63488 */
		sram_size = 0x1F000;
		break;
	case WAVE521C_CODE:
		/* 10bit profile : 8_kx8_k -> 129024, 4_kx2_k -> 64512 */
		sram_size = 0x1F800;
		break;
	case WAVE521C_DUAL_CODE:
		/* 10bit profile : 8_kx8_k -> 129024, 4_kx2_k -> 64512 */
		sram_size = 0x1F800;
		break;
	default:
		dev_err(vpu_dev->dev, "invalid check product_code(%x)\n", val);
		break;
	}

	// if we can know the sram address directly in vdi layer, we use it first for sdram address
	vpu_dev->sram_buf.daddr = 0;
	vpu_dev->sram_buf.size = sram_size;

	return &vpu_dev->sram_buf;
}

int32_t wave_vpu_get_product_id(struct vpu_device *vpu_dev)
{
	u32 product_id = PRODUCT_ID_NONE;
	u32 val;

	val = vpu_read_reg(vpu_dev, W5_PRODUCT_NUMBER);

	switch (val) {
	case WAVE521_CODE:
		product_id = PRODUCT_ID_521; break;
	case WAVE521C_CODE:
		product_id = PRODUCT_ID_521; break;
	case WAVE511_CODE:
		product_id = PRODUCT_ID_511; break;
	case WAVE521C_DUAL_CODE:
		product_id = PRODUCT_ID_521; break;
	case WAVE517_CODE:
		product_id = PRODUCT_ID_517; break;
	case WAVE537_CODE:
		product_id = PRODUCT_ID_517; break;
	case WAVE521E1_CODE:
		product_id = PRODUCT_ID_521; break;
	default:
		dev_err(vpu_dev->dev, "check product_id(%x)\n", val);
		break;
	}
	return product_id;
}

void wave5_bit_issue_command(struct vpu_instance *vpu_inst, uint32_t cmd)
{
	u32 instance_index = vpu_inst->id;
	u32 codec_mode = vpu_inst->std;

	vpu_write_reg(vpu_inst->dev, W5_CMD_INSTANCE_INFO, (codec_mode << 16) |
			(instance_index & 0xffff));
	vpu_write_reg(vpu_inst->dev, W5_VPU_BUSY_STATUS, 1);
	vpu_write_reg(vpu_inst->dev, W5_COMMAND, cmd);

	dev_dbg(vpu_inst->dev->dev, "cmd=0x%x\n", cmd);

	vpu_write_reg(vpu_inst->dev, W5_VPU_HOST_INT_REQ, 1);
}

static int wave5_send_query(struct vpu_instance *vpu_inst, enum QUERY_OPT query_opt)
{
	int ret;

	vpu_write_reg(vpu_inst->dev, W5_QUERY_OPTION, query_opt);
	vpu_write_reg(vpu_inst->dev, W5_VPU_BUSY_STATUS, 1);
	wave5_bit_issue_command(vpu_inst, W5_QUERY);

	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(vpu_inst->dev->dev, "query timed out. opt=0x%x\n", query_opt);
		return ret;
	}

	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS))
		return -EIO;

	return 0;
}

static int setup_wave5_properties(struct device *dev)
{
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	struct vpu_attr *p_attr = &vpu_dev->attr;
	u32 reg_val;
	u8 *str;
	int ret;
	u32 hw_config_def0, hw_config_def1, hw_config_feature, hw_config_rev;

	vpu_write_reg(vpu_dev, W5_QUERY_OPTION, GET_VPU_INFO);
	vpu_write_reg(vpu_dev, W5_VPU_BUSY_STATUS, 1);
	vpu_write_reg(vpu_dev, W5_COMMAND, W5_QUERY);
	vpu_write_reg(vpu_dev, W5_VPU_HOST_INT_REQ, 1);
	ret = wave5_wait_vpu_busy(vpu_dev, W5_VPU_BUSY_STATUS);
	if (ret)
		return ret;

	if (!vpu_read_reg(vpu_dev, W5_RET_SUCCESS))
		return -EIO;

	reg_val = vpu_read_reg(vpu_dev, W5_RET_PRODUCT_NAME);
	str = (u8 *)&reg_val;
	p_attr->product_name[0] = str[3];
	p_attr->product_name[1] = str[2];
	p_attr->product_name[2] = str[1];
	p_attr->product_name[3] = str[0];
	p_attr->product_name[4] = 0;

	p_attr->product_id = wave_vpu_get_product_id(vpu_dev);
	p_attr->product_version = vpu_read_reg(vpu_dev, W5_RET_PRODUCT_VERSION);
	p_attr->fw_version = vpu_read_reg(vpu_dev, W5_RET_FW_VERSION);
	p_attr->customer_id = vpu_read_reg(vpu_dev, W5_RET_CUSTOMER_ID);
	hw_config_def0 = vpu_read_reg(vpu_dev, W5_RET_STD_DEF0);
	hw_config_def1 = vpu_read_reg(vpu_dev, W5_RET_STD_DEF1);
	hw_config_feature = vpu_read_reg(vpu_dev, W5_RET_CONF_FEATURE);
	hw_config_rev = vpu_read_reg(vpu_dev, W5_RET_CONF_REVISION);

	p_attr->support_hevc10bit_enc = (hw_config_feature >> 3) & 1;
	if (hw_config_rev > 167455) //20190321
		p_attr->support_avc10bit_enc = (hw_config_feature >> 11) & 1;
	else
		p_attr->support_avc10bit_enc = p_attr->support_hevc10bit_enc;

	p_attr->support_decoders = 0;
	p_attr->support_encoders = 0;
	if (p_attr->product_id == PRODUCT_ID_521) {
		p_attr->support_dual_core = ((hw_config_def1 >> 26) & 0x01);
		if (p_attr->support_dual_core || hw_config_rev < 206116) {
			p_attr->support_decoders = BIT(STD_AVC);
			p_attr->support_decoders |= BIT(STD_HEVC);
			p_attr->support_encoders = BIT(STD_AVC);
			p_attr->support_encoders |= BIT(STD_HEVC);
		} else {
			p_attr->support_decoders |= (((hw_config_def1 >> 3) & 0x01) << STD_AVC);
			p_attr->support_decoders |= (((hw_config_def1 >> 2) & 0x01) << STD_HEVC);
			p_attr->support_encoders = (((hw_config_def1 >> 1) & 0x01) << STD_AVC);
			p_attr->support_encoders |= (((hw_config_def1 >> 0) & 0x01) << STD_HEVC);
		}
	} else if (p_attr->product_id == PRODUCT_ID_511) {
		p_attr->support_decoders = BIT(STD_HEVC);
		p_attr->support_decoders |= BIT(STD_AVC);
	} else if (p_attr->product_id == PRODUCT_ID_517) {
		p_attr->support_decoders = (((hw_config_def1 >> 4) & 0x01) << STD_AV1);
		p_attr->support_decoders |= (((hw_config_def1 >> 3) & 0x01) << STD_AVS2);
		p_attr->support_decoders |= (((hw_config_def1 >> 2) & 0x01) << STD_AVC);
		p_attr->support_decoders |= (((hw_config_def1 >> 1) & 0x01) << STD_VP9);
		p_attr->support_decoders |= (((hw_config_def1 >> 0) & 0x01) << STD_HEVC);
	}

	p_attr->support_backbone = (hw_config_def0 >> 16) & 0x01;
	p_attr->support_vcpu_backbone = (hw_config_def0 >> 28) & 0x01;
	p_attr->support_vcore_backbone = (hw_config_def0 >> 22) & 0x01;
	p_attr->support_dual_core = (hw_config_def1 >> 26) & 0x01;
	p_attr->support_endian_mask = BIT(VDI_LITTLE_ENDIAN) |
				      BIT(VDI_BIG_ENDIAN) |
				      BIT(VDI_32BIT_LITTLE_ENDIAN) |
				      BIT(VDI_32BIT_BIG_ENDIAN) |
				      (0xffffUL << 16);
	p_attr->support_bitstream_mode = BIT(BS_MODE_INTERRUPT) |
		BIT(BS_MODE_PIC_END);

	return 0;
}

int wave5_vpu_get_version(struct vpu_device *vpu_dev, uint32_t *version_info,
			  uint32_t *revision)
{
	u32 reg_val;
	int ret;

	vpu_write_reg(vpu_dev, W5_QUERY_OPTION, GET_VPU_INFO);
	vpu_write_reg(vpu_dev, W5_VPU_BUSY_STATUS, 1);
	vpu_write_reg(vpu_dev, W5_COMMAND, W5_QUERY);
	vpu_write_reg(vpu_dev, W5_VPU_HOST_INT_REQ, 1);
	ret = wave5_wait_vpu_busy(vpu_dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(vpu_dev->dev, "%s: timeout\n", __func__);
		return ret;
	}

	if (!vpu_read_reg(vpu_dev, W5_RET_SUCCESS)) {
		dev_err(vpu_dev->dev, "%s: failed\n", __func__);
		return -EIO;
	}

	reg_val = vpu_read_reg(vpu_dev, W5_RET_FW_VERSION);
	if (version_info)
		*version_info = 0;
	if (revision)
		*revision = reg_val;

	return 0;
}

int wave5_vpu_init(struct device *dev, u8 *firmware, uint32_t size)
{
	struct vpu_buf *common_vb;
	struct dma_vpu_buf *sram_vb;
	dma_addr_t code_base, temp_base;
	u32 code_size, temp_size;
	u32 i, reg_val, remap_size;
	int ret;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);

	common_vb = &vpu_dev->common_mem;

	code_base = common_vb->daddr;
	/* ALIGN TO 4KB */
	code_size = (WAVE5_MAX_CODE_BUF_SIZE & ~0xfff);
	if (code_size < size * 2)
		return -EINVAL;

	temp_base = common_vb->daddr + WAVE5_TEMPBUF_OFFSET;
	temp_size = WAVE5_TEMPBUF_SIZE;

	wave5_vdi_write_memory(vpu_dev, common_vb, 0, firmware, size, VDI_128BIT_LITTLE_ENDIAN);

	vpu_write_reg(vpu_dev, W5_PO_CONF, 0);

	/* clear registers */

	for (i = W5_CMD_REG_BASE; i < W5_CMD_REG_END; i += 4)
		vpu_write_reg(vpu_dev, i, 0x00);

	/* remap page size 0*/
	remap_size = (W5_REMAP_MAX_SIZE >> 12) & 0x1ff;
	reg_val = 0x80000000 | (WAVE5_UPPER_PROC_AXI_ID << 20) | (0 << 16)
		| (W5_REMAP_INDEX0 << 12) | BIT(11) | remap_size;
	vpu_write_reg(vpu_dev, W5_VPU_REMAP_CTRL, reg_val);
	vpu_write_reg(vpu_dev, W5_VPU_REMAP_VADDR, W5_REMAP_INDEX0 * W5_REMAP_MAX_SIZE);
	vpu_write_reg(vpu_dev, W5_VPU_REMAP_PADDR, code_base + W5_REMAP_INDEX0 * W5_REMAP_MAX_SIZE);

	/* remap page size 1*/
	remap_size = (W5_REMAP_MAX_SIZE >> 12) & 0x1ff;
	reg_val = 0x80000000 | (WAVE5_UPPER_PROC_AXI_ID << 20) | (0 << 16)
		| (W5_REMAP_INDEX1 << 12) | BIT(11) | remap_size;
	vpu_write_reg(vpu_dev, W5_VPU_REMAP_CTRL, reg_val);
	vpu_write_reg(vpu_dev, W5_VPU_REMAP_VADDR, W5_REMAP_INDEX1 * W5_REMAP_MAX_SIZE);
	vpu_write_reg(vpu_dev, W5_VPU_REMAP_PADDR, code_base + W5_REMAP_INDEX1 * W5_REMAP_MAX_SIZE);

	vpu_write_reg(vpu_dev, W5_ADDR_CODE_BASE, code_base);
	vpu_write_reg(vpu_dev, W5_CODE_SIZE, code_size);
	vpu_write_reg(vpu_dev, W5_CODE_PARAM, (WAVE5_UPPER_PROC_AXI_ID << 4) | 0);
	vpu_write_reg(vpu_dev, W5_ADDR_TEMP_BASE, temp_base);
	vpu_write_reg(vpu_dev, W5_TEMP_SIZE, temp_size);

	vpu_write_reg(vpu_dev, W5_HW_OPTION, 0);

	/* interrupt */
	// encoder
	reg_val = BIT(INT_WAVE5_ENC_SET_PARAM);
	reg_val |= BIT(INT_WAVE5_ENC_PIC);
	reg_val |= BIT(INT_WAVE5_BSBUF_FULL);
	// decoder
	reg_val |= BIT(INT_WAVE5_INIT_SEQ);
	reg_val |= BIT(INT_WAVE5_DEC_PIC);
	reg_val |= BIT(INT_WAVE5_BSBUF_EMPTY);
	vpu_write_reg(vpu_dev, W5_VPU_VINT_ENABLE, reg_val);

	reg_val = vpu_read_reg(vpu_dev, W5_VPU_RET_VPU_CONFIG0);
	if ((reg_val >> 16) & 1) {
		reg_val = ((WAVE5_PROC_AXI_ID << 28) |
			   (WAVE5_PRP_AXI_ID << 24) |
			   (WAVE5_FBD_Y_AXI_ID << 20) |
			   (WAVE5_FBC_Y_AXI_ID << 16) |
			   (WAVE5_FBD_C_AXI_ID << 12) |
			   (WAVE5_FBC_C_AXI_ID << 8) |
			   (WAVE5_PRI_AXI_ID << 4) |
			   (WAVE5_SEC_AXI_ID << 0));
		wave5_fio_writel(vpu_dev, W5_BACKBONE_PROG_AXI_ID, reg_val);
	}

	sram_vb = get_sram_memory(vpu_dev);

	vpu_write_reg(vpu_dev, W5_ADDR_SEC_AXI, sram_vb->daddr);
	vpu_write_reg(vpu_dev, W5_SEC_AXI_SIZE, sram_vb->size);
	vpu_write_reg(vpu_dev, W5_VPU_BUSY_STATUS, 1);
	vpu_write_reg(vpu_dev, W5_COMMAND, W5_INIT_VPU);
	vpu_write_reg(vpu_dev, W5_VPU_REMAP_CORE_START, 1);
	ret = wave5_wait_vpu_busy(vpu_dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(vpu_dev->dev, "VPU init(W5_VPU_REMAP_CORE_START) timeout\n");
		return ret;
	}

	reg_val = vpu_read_reg(vpu_dev, W5_RET_SUCCESS);
	if (!reg_val) {
		u32 reason_code = vpu_read_reg(vpu_dev, W5_RET_FAIL_REASON);

		wave5_print_reg_err(vpu_dev, reason_code);
		return -EIO;
	}

	return setup_wave5_properties(dev);
}

int wave5_vpu_build_up_dec_param(struct vpu_instance *vpu_inst,
				 struct dec_open_param *param)
{
	int ret;
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;
	u32 bs_endian;
	struct dma_vpu_buf *sram_vb;
	struct vpu_device *vpu_dev = vpu_inst->dev;

	switch (vpu_inst->std) {
	case W_HEVC_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_HEVC;
		break;
	case W_VP9_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_VP9;
		break;
	case W_AVS2_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_AVS2;
		break;
	case W_AVC_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_AVC;
		break;
	case W_AV1_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_AV1;
		break;
	default:
		return -EINVAL;
	}

	if (vpu_dev->product == PRODUCT_ID_517)
		p_dec_info->vb_work.size = WAVE517_WORKBUF_SIZE;
	else if (vpu_dev->product == PRODUCT_ID_521)
		p_dec_info->vb_work.size = WAVE521DEC_WORKBUF_SIZE;
	else if (vpu_dev->product == PRODUCT_ID_511)
		p_dec_info->vb_work.size = WAVE521DEC_WORKBUF_SIZE;

	ret = wave5_vdi_allocate_dma_memory(vpu_inst->dev, &p_dec_info->vb_work);
	if (ret)
		return ret;

	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_VCORE_INFO, 1);

	sram_vb = get_sram_memory(vpu_inst->dev);
	p_dec_info->sec_axi_info.buf_base = sram_vb->daddr;
	p_dec_info->sec_axi_info.buf_size = sram_vb->size;

	wave5_vdi_clear_memory(vpu_inst->dev, &p_dec_info->vb_work);

	vpu_write_reg(vpu_inst->dev, W5_ADDR_WORK_BASE, p_dec_info->vb_work.daddr);
	vpu_write_reg(vpu_inst->dev, W5_WORK_SIZE, p_dec_info->vb_work.size);

	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_BS_START_ADDR, p_dec_info->stream_buf_start_addr);
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_BS_SIZE, p_dec_info->stream_buf_size);

	/* NOTE: when endian mode is 0, SDMA reads MSB first */
	bs_endian = wave5_vdi_convert_endian(vpu_inst->dev, param->stream_endian);
	bs_endian = (~bs_endian & VDI_128BIT_ENDIAN_MASK);
	vpu_write_reg(vpu_inst->dev, W5_CMD_BS_PARAM, bs_endian);

	vpu_write_reg(vpu_inst->dev, W5_CMD_NUM_CQ_DEPTH_M1, (COMMAND_QUEUE_DEPTH - 1));
	vpu_write_reg(vpu_inst->dev, W5_CMD_ERR_CONCEAL, (param->error_conceal_unit << 2) |
			(param->error_conceal_mode));

	wave5_bit_issue_command(vpu_inst, W5_CREATE_INSTANCE);
	// check QUEUE_DONE
	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(vpu_inst->dev->dev, "create instance timed out\n");
		goto free_vb_work;
	}

	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS)) { // failed adding into VCPU QUEUE
		ret = -EIO;
		goto free_vb_work;
	}

	p_dec_info->product_code = vpu_read_reg(vpu_inst->dev, W5_PRODUCT_NUMBER);

	return 0;
free_vb_work:
	wave5_vdi_free_dma_memory(vpu_dev, &p_dec_info->vb_work);
	return ret;
}

int wave5_vpu_dec_init_seq(struct vpu_instance *vpu_inst)
{
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;
	u32 cmd_option = INIT_SEQ_NORMAL, bs_option;
	u32 reg_val;
	int ret;

	if (!vpu_inst->codec_info)
		return -EINVAL;

	if (p_dec_info->thumbnail_mode)
		cmd_option = INIT_SEQ_W_THUMBNAIL;

	/* set attributes of bitstream buffer controller */
	bs_option = 0;
	switch (p_dec_info->open_param.bitstream_mode) {
	case BS_MODE_INTERRUPT:
		if (p_dec_info->seq_init_escape)
			bs_option = BSOPTION_ENABLE_EXPLICIT_END;
		break;
	case BS_MODE_PIC_END:
		bs_option = BSOPTION_ENABLE_EXPLICIT_END;
		break;
	default:
		return -EINVAL;
	}

	vpu_write_reg(vpu_inst->dev, W5_BS_RD_PTR, p_dec_info->stream_rd_ptr);
	vpu_write_reg(vpu_inst->dev, W5_BS_WR_PTR, p_dec_info->stream_wr_ptr);

	if (p_dec_info->stream_endflag == 1)
		bs_option = 3;
	if (vpu_inst->std == W_AV1_DEC)
		bs_option |= ((p_dec_info->open_param.av1_format) << 2);
	vpu_write_reg(vpu_inst->dev, W5_BS_OPTION, BIT(31) | bs_option);

	vpu_write_reg(vpu_inst->dev, W5_COMMAND_OPTION, cmd_option);
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_USER_MASK, p_dec_info->user_data_enable);

	wave5_bit_issue_command(vpu_inst, W5_INIT_SEQ);

	// check QUEUE_DONE
	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(vpu_inst->dev->dev, "init seq timed out\n");
		return ret;
	}

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_QUEUE_STATUS);

	p_dec_info->instance_queue_count = (reg_val >> 16) & 0xff;
	p_dec_info->report_queue_count = (reg_val & 0xffff);

	// FAILED for adding a command into VCPU QUEUE
	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS)) {
		reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
		wave5_print_reg_err(vpu_inst->dev, reg_val);
		return -EIO;
	}

	return 0;
}

static void wave5_get_dec_seq_result(struct vpu_instance *vpu_inst, struct dec_initial_info *info)
{
	u32 reg_val, sub_layer_info;
	u32 profile_compatibility_flag;
	u32 left, right, top, bottom;
	u32 output_bit_depth_minus8;
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;

	p_dec_info->stream_rd_ptr = wave5_vpu_dec_get_rd_ptr(vpu_inst);
	info->rd_ptr = p_dec_info->stream_rd_ptr;

	p_dec_info->frame_display_flag = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_DISP_IDC);

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PIC_SIZE);
	info->pic_width = ((reg_val >> 16) & 0xffff);
	info->pic_height = (reg_val & 0xffff);
	info->min_frame_buffer_count = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_NUM_REQUIRED_FB);
	info->frame_buf_delay = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_NUM_REORDER_DELAY);

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_CROP_LEFT_RIGHT);
	left = (reg_val >> 16) & 0xffff;
	right = reg_val & 0xffff;
	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_CROP_TOP_BOTTOM);
	top = (reg_val >> 16) & 0xffff;
	bottom = reg_val & 0xffff;

	info->pic_crop_rect.left = left;
	info->pic_crop_rect.right = info->pic_width - right;
	info->pic_crop_rect.top = top;
	info->pic_crop_rect.bottom = info->pic_height - bottom;

	info->f_rate_numerator = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_FRAME_RATE_NR);
	info->f_rate_denominator = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_FRAME_RATE_DR);

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_COLOR_SAMPLE_INFO);
	info->luma_bitdepth = (reg_val >> 0) & 0x0f;
	info->chroma_bitdepth = (reg_val >> 4) & 0x0f;
	info->chroma_format_idc = (reg_val >> 8) & 0x0f;
	info->aspect_rate_info = (reg_val >> 16) & 0xff;
	info->is_ext_sar = (info->aspect_rate_info == 255 ? true : false);
	/* [0:15] - vertical size, [16:31] - horizontal size */
	if (info->is_ext_sar)
		info->aspect_rate_info = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_ASPECT_RATIO);
	info->bit_rate = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_BIT_RATE);

	sub_layer_info = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_SUB_LAYER_INFO);
	info->max_temporal_layers = (sub_layer_info >> 8) & 0x7;

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_SEQ_PARAM);
	info->level = reg_val & 0xff;
	profile_compatibility_flag = (reg_val >> 12) & 0xff;
	info->profile = (reg_val >> 24) & 0x1f;
	info->tier = (reg_val >> 29) & 0x01;
	output_bit_depth_minus8 = (reg_val >> 30) & 0x03;

	if (vpu_inst->std == W_HEVC_DEC) {
		/* guessing profile */
		if (!info->profile) {
			if ((profile_compatibility_flag & 0x06) == 0x06)
				info->profile = HEVC_PROFILE_MAIN; /* main profile */
			else if ((profile_compatibility_flag & 0x04) == 0x04)
				info->profile = HEVC_PROFILE_MAIN10; /* main10 profile */
			else if ((profile_compatibility_flag & 0x08) == 0x08)
				/* main still picture profile */
				info->profile = HEVC_PROFILE_STILLPICTURE;
			else
				info->profile = HEVC_PROFILE_MAIN; /* for old version HM */
		}

	} else if (vpu_inst->std == W_AVS2_DEC) {
		if (info->luma_bitdepth == 10 && output_bit_depth_minus8 == 2)
			info->output_bit_depth = 10;
		else
			info->output_bit_depth = 8;

	} else if (vpu_inst->std == W_AVC_DEC) {
		info->profile = (reg_val >> 24) & 0x7f;
	}

	info->vlc_buf_size = vpu_read_reg(vpu_inst->dev, W5_RET_VLC_BUF_SIZE);
	info->param_buf_size = vpu_read_reg(vpu_inst->dev, W5_RET_PARAM_BUF_SIZE);
	p_dec_info->vlc_buf_size = info->vlc_buf_size;
	p_dec_info->param_buf_size = info->param_buf_size;
}

int wave5_vpu_dec_get_seq_info(struct vpu_instance *vpu_inst, struct dec_initial_info *info)
{
	int ret;
	u32 reg_val;
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;

	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_ADDR_REPORT_BASE, p_dec_info->user_data_buf_addr);
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_REPORT_SIZE, p_dec_info->user_data_buf_size);
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_REPORT_PARAM,
		      VPU_USER_DATA_ENDIAN & VDI_128BIT_ENDIAN_MASK);

	// send QUERY cmd
	ret = wave5_send_query(vpu_inst, GET_RESULT);
	if (ret) {
		if (ret == -EIO) {
			reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
			wave5_print_reg_err(vpu_inst->dev, reg_val);
		}
		return ret;
	}

	dev_dbg(vpu_inst->dev->dev, "init seq complete\n");

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_QUEUE_STATUS);

	p_dec_info->instance_queue_count = (reg_val >> 16) & 0xff;
	p_dec_info->report_queue_count = (reg_val & 0xffff);

	/* this is not a fatal error, set ret to -EIO but don't return immediately */
	if (vpu_read_reg(vpu_inst->dev, W5_RET_DEC_DECODING_SUCCESS) != 1) {
		info->seq_init_err_reason = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_ERR_INFO);
		ret = -EIO;
	} else {
		info->warn_info = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_WARN_INFO);
	}

	// get sequence info
	info->user_data_size = 0;
	info->user_data_buf_full = false;
	info->user_data_header = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_USERDATA_IDC);
	if (info->user_data_header) {
		if (info->user_data_header & BIT(USERDATA_FLAG_BUFF_FULL))
			info->user_data_buf_full = true;
		info->user_data_size = p_dec_info->user_data_buf_size;
	}

	wave5_get_dec_seq_result(vpu_inst, info);

	return ret;
}

int wave5_vpu_dec_register_framebuffer(struct vpu_instance *vpu_inst, struct frame_buffer *fb_arr,
				       enum tiled_map_type map_type, u32 count)
{
	int ret;
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;
	size_t remain, idx, j, i, cnt_8_chunk;
	u32 start_no, end_no;
	u32 reg_val, cbcr_interleave, nv21, pic_size;
	u32 endian, yuv_format;
	u32 addr_y, addr_cb, addr_cr;
	u32 mv_col_size, fbc_y_tbl_size, fbc_c_tbl_size;
	struct vpu_buf vb_buf;
	u32 color_format = 0;
	u32 init_pic_width = 0, init_pic_height = 0;
	u32 pixel_order = 1;
	u32 bwb_flag = (map_type == LINEAR_FRAME_MAP) ? 1 : 0;

	cbcr_interleave = p_dec_info->open_param.cbcr_interleave;
	nv21 = p_dec_info->open_param.nv21;
	mv_col_size = 0;
	fbc_y_tbl_size = 0;
	fbc_c_tbl_size = 0;

	init_pic_width = p_dec_info->initial_info.pic_width;
	init_pic_height = p_dec_info->initial_info.pic_height;

	if (map_type >= COMPRESSED_FRAME_MAP) {
		cbcr_interleave = 0;
		nv21 = 0;

		switch (vpu_inst->std) {
		case W_HEVC_DEC:
			mv_col_size = WAVE5_DEC_HEVC_BUF_SIZE(init_pic_width, init_pic_height);
			break;
		case W_VP9_DEC:
			mv_col_size = WAVE5_DEC_VP9_BUF_SIZE(init_pic_width, init_pic_height);
			break;
		case W_AVS2_DEC:
			mv_col_size = WAVE5_DEC_AVS2_BUF_SIZE(init_pic_width, init_pic_height);
			break;
		case W_AVC_DEC:
			mv_col_size = WAVE5_DEC_AVC_BUF_SIZE(init_pic_width, init_pic_height);
			break;
		case W_AV1_DEC:
			mv_col_size = WAVE5_DEC_AV1_BUF_SZ_1(init_pic_width, init_pic_height) +
				WAVE5_DEC_AV1_BUF_SZ_2(init_pic_width, init_pic_width,
						       init_pic_height);
			break;
		default:
			return -EINVAL;
		}

		mv_col_size = ALIGN(mv_col_size, 16);
		vb_buf.daddr = 0;
		if (vpu_inst->std == W_HEVC_DEC || vpu_inst->std == W_AVS2_DEC || vpu_inst->std ==
				W_VP9_DEC || vpu_inst->std == W_AVC_DEC || vpu_inst->std ==
				W_AV1_DEC) {
			/* 4096 is a margin */
			vb_buf.size = ALIGN(mv_col_size, 4096) + 4096;

			for (i = 0 ; i < count ; i++) {
				if (p_dec_info->vb_mv[i].size == 0) {
					ret = wave5_vdi_allocate_dma_memory(vpu_inst->dev, &vb_buf);
					if (ret)
						return ret;
					p_dec_info->vb_mv[i] = vb_buf;
				}
			}
		}

		if (p_dec_info->product_code == WAVE521C_DUAL_CODE) {
			u32 bgs_width = (p_dec_info->initial_info.luma_bitdepth > 8 ? 256 :
					512);
			u32 ot_bg_width = 1024;
			u32 frm_width = ALIGN(init_pic_width, 16);
			u32 frm_height = ALIGN(init_pic_height, 16);
			// valid_width = align(width, 16),
			// comp_frm_width = align(valid_width+pad_x, 16)
			u32 comp_frm_width = ALIGN(ALIGN(frm_width, 16) + 16, 16);
			// 1024 = offset table BG width
			u32 ot_frm_width = ALIGN(comp_frm_width, ot_bg_width);

			// sizeof_offset_table()
			u32 ot_bg_height = 32;
			u32 bgs_height = BIT(14) / bgs_width /
				(p_dec_info->initial_info.luma_bitdepth > 8 ? 2 : 1);
			u32 comp_frm_height = ALIGN(ALIGN(frm_height, 4) + 4, bgs_height);
			u32 ot_frm_height = ALIGN(comp_frm_height, ot_bg_height);

			fbc_y_tbl_size = (ot_frm_width / 16) * (ot_frm_height / 4) * 2;
		} else {
			switch (vpu_inst->std) {
			case W_HEVC_DEC:
				fbc_y_tbl_size = WAVE5_FBC_LUMA_TABLE_SIZE(init_pic_width,
									   init_pic_height);
				break;
			case W_VP9_DEC:
				fbc_y_tbl_size =
					WAVE5_FBC_LUMA_TABLE_SIZE(ALIGN(init_pic_width, 64),
								  ALIGN(init_pic_height, 64));
				break;
			case W_AVS2_DEC:
				fbc_y_tbl_size = WAVE5_FBC_LUMA_TABLE_SIZE(init_pic_width,
									   init_pic_height);
				break;
			case W_AVC_DEC:
				fbc_y_tbl_size = WAVE5_FBC_LUMA_TABLE_SIZE(init_pic_width,
									   init_pic_height);
				break;
			case W_AV1_DEC:
				fbc_y_tbl_size =
					WAVE5_FBC_LUMA_TABLE_SIZE(ALIGN(init_pic_width, 16),
								  ALIGN(init_pic_height, 8));
				break;
			default:
				return -EINVAL;
			}
			fbc_y_tbl_size = ALIGN(fbc_y_tbl_size, 16);
		}

		vb_buf.daddr = 0;
		vb_buf.size = ALIGN(fbc_y_tbl_size, 4096) + 4096;
		for (i = 0 ; i < count ; i++) {
			if (p_dec_info->vb_fbc_y_tbl[i].size == 0) {
				ret = wave5_vdi_allocate_dma_memory(vpu_inst->dev, &vb_buf);
				if (ret)
					return ret;
				p_dec_info->vb_fbc_y_tbl[i] = vb_buf;
			}
		}

		if (p_dec_info->product_code == WAVE521C_DUAL_CODE) {
			u32 bgs_width = (p_dec_info->initial_info.chroma_bitdepth > 8 ? 256 : 512);
			u32 ot_bg_width = 1024;
			u32 frm_width = ALIGN(init_pic_width, 16);
			u32 frm_height = ALIGN(init_pic_height, 16);
			u32 comp_frm_width = ALIGN(ALIGN(frm_width / 2, 16) + 16, 16);
			// valid_width = align(width, 16),
			// comp_frm_width = align(valid_width+pad_x, 16)
			// 1024 = offset table BG width
			u32 ot_frm_width = ALIGN(comp_frm_width, ot_bg_width);

			// sizeof_offset_table()
			u32 ot_bg_height = 32;
			u32 bgs_height = BIT(14) / bgs_width /
				(p_dec_info->initial_info.chroma_bitdepth > 8 ? 2 : 1);
			u32 comp_frm_height = ALIGN(ALIGN(frm_height, 4) + 4, bgs_height);
			u32 ot_frm_height = ALIGN(comp_frm_height, ot_bg_height);

			fbc_c_tbl_size = (ot_frm_width / 16) * (ot_frm_height / 4) * 2;
		} else {
			switch (vpu_inst->std) {
			case W_HEVC_DEC:
				fbc_c_tbl_size = WAVE5_FBC_CHROMA_TABLE_SIZE(init_pic_width,
									     init_pic_height);
				break;
			case W_VP9_DEC:
				fbc_c_tbl_size =
					WAVE5_FBC_CHROMA_TABLE_SIZE(ALIGN(init_pic_width, 64),
								    ALIGN(init_pic_height, 64));
				break;
			case W_AVS2_DEC:
				fbc_c_tbl_size = WAVE5_FBC_CHROMA_TABLE_SIZE(init_pic_width,
									     init_pic_height);
				break;
			case W_AVC_DEC:
				fbc_c_tbl_size = WAVE5_FBC_CHROMA_TABLE_SIZE(init_pic_width,
									     init_pic_height);
				break;
			case W_AV1_DEC:
				fbc_c_tbl_size =
					WAVE5_FBC_CHROMA_TABLE_SIZE(ALIGN(init_pic_width, 16),
								    ALIGN(init_pic_height, 8));
				break;
			default:
				return -EINVAL;
			}
			fbc_c_tbl_size = ALIGN(fbc_c_tbl_size, 16);
		}

		vb_buf.daddr = 0;
		vb_buf.size = ALIGN(fbc_c_tbl_size, 4096) + 4096;
		for (i = 0 ; i < count ; i++) {
			if (p_dec_info->vb_fbc_c_tbl[i].size == 0) {
				ret = wave5_vdi_allocate_dma_memory(vpu_inst->dev, &vb_buf);
				if (ret)
					return ret;
				p_dec_info->vb_fbc_c_tbl[i] = vb_buf;
			}
		}
		pic_size = (init_pic_width << 16) | (init_pic_height);

		// allocate task_buffer
		vb_buf.size = (p_dec_info->vlc_buf_size * VLC_BUF_NUM) +
				(p_dec_info->param_buf_size * COMMAND_QUEUE_DEPTH);
		vb_buf.daddr = 0;
		ret = wave5_vdi_allocate_dma_memory(vpu_inst->dev, &vb_buf);
		if (ret)
			return ret;

		p_dec_info->vb_task = vb_buf;

		vpu_write_reg(vpu_inst->dev, W5_CMD_SET_FB_ADDR_TASK_BUF,
			      p_dec_info->vb_task.daddr);
		vpu_write_reg(vpu_inst->dev, W5_CMD_SET_FB_TASK_BUF_SIZE, vb_buf.size);
	} else {
		pic_size = (init_pic_width << 16) | (init_pic_height);
	}
	endian = wave5_vdi_convert_endian(vpu_inst->dev, fb_arr[0].endian);
	vpu_write_reg(vpu_inst->dev, W5_PIC_SIZE, pic_size);

	yuv_format = 0;
	color_format = 0;

	reg_val =
		(bwb_flag << 28) |
		(pixel_order << 23) | /* PIXEL ORDER in 128bit. first pixel in low address */
		(yuv_format << 20) |
		(color_format << 19) |
		(nv21 << 17) |
		(cbcr_interleave << 16) |
		(fb_arr[0].stride);
	vpu_write_reg(vpu_inst->dev, W5_COMMON_PIC_INFO, reg_val); //// 0x008012c0

	remain = count;
	cnt_8_chunk = (count + 7) / 8;
	idx = 0;
	for (j = 0; j < cnt_8_chunk; j++) {
		reg_val = (endian << 16) | (j == cnt_8_chunk - 1) << 4 | ((j == 0) << 3);
		reg_val |= (p_dec_info->open_param.enable_non_ref_fbc_write << 26);
		vpu_write_reg(vpu_inst->dev, W5_SFB_OPTION, reg_val);
		start_no = j * 8;
		end_no = start_no + (remain >= 8 ? 8 : remain) - 1;

		vpu_write_reg(vpu_inst->dev, W5_SET_FB_NUM, (start_no << 8) | end_no);

		for (i = 0; i < 8 && i < remain; i++) {
			if (map_type == LINEAR_FRAME_MAP && p_dec_info->open_param.cbcr_order ==
					CBCR_ORDER_REVERSED) {
				addr_y = fb_arr[i + start_no].buf_y;
				addr_cb = fb_arr[i + start_no].buf_cr;
				addr_cr = fb_arr[i + start_no].buf_cb;
			} else {
				addr_y = fb_arr[i + start_no].buf_y;
				addr_cb = fb_arr[i + start_no].buf_cb;
				addr_cr = fb_arr[i + start_no].buf_cr;
			}
			vpu_write_reg(vpu_inst->dev, W5_ADDR_LUMA_BASE0 + (i << 4), addr_y);
			vpu_write_reg(vpu_inst->dev, W5_ADDR_CB_BASE0 + (i << 4), addr_cb);
			if (map_type >= COMPRESSED_FRAME_MAP) {
				/* luma FBC offset table */
				vpu_write_reg(vpu_inst->dev, W5_ADDR_FBC_Y_OFFSET0 + (i << 4),
					      p_dec_info->vb_fbc_y_tbl[idx].daddr);
				/* chroma FBC offset table */
				vpu_write_reg(vpu_inst->dev, W5_ADDR_FBC_C_OFFSET0 + (i << 4),
					      p_dec_info->vb_fbc_c_tbl[idx].daddr);
				vpu_write_reg(vpu_inst->dev, W5_ADDR_MV_COL0 + (i << 2),
					      p_dec_info->vb_mv[idx].daddr);
			} else {
				vpu_write_reg(vpu_inst->dev, W5_ADDR_CR_BASE0 + (i << 4), addr_cr);
				vpu_write_reg(vpu_inst->dev, W5_ADDR_FBC_C_OFFSET0 + (i << 4), 0);
				vpu_write_reg(vpu_inst->dev, W5_ADDR_MV_COL0 + (i << 2), 0);
			}
			idx++;
		}
		remain -= i;

		wave5_bit_issue_command(vpu_inst, W5_SET_FB);
		ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
		if (ret)
			return ret;
	}

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS);
	if (!reg_val)
		return -EIO;

	return 0;
}

int wave5_vpu_decode(struct vpu_instance *vpu_inst, struct dec_param *option, u32 *fail_res)
{
	u32 mode_option = DEC_PIC_NORMAL, bs_option, reg_val;
	u32 force_latency = 0;
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;
	struct dec_open_param *p_open_param = &p_dec_info->open_param;
	int ret;

	if (p_dec_info->thumbnail_mode) {
		mode_option = DEC_PIC_W_THUMBNAIL;
	} else if (option->skipframe_mode) {
		switch (option->skipframe_mode) {
		case WAVE_SKIPMODE_NON_IRAP:
			mode_option = SKIP_NON_IRAP;
			force_latency = 1;
			break;
		case WAVE_SKIPMODE_NON_REF:
			mode_option = SKIP_NON_REF_PIC;
			break;
		default:
			// skip off
			break;
		}
	}

	// set disable reorder
	if (!p_dec_info->reorder_enable)
		force_latency = 1;

	/* set attributes of bitstream buffer controller */
	bs_option = 0;
	switch (p_open_param->bitstream_mode) {
	case BS_MODE_INTERRUPT:
		break;
	case BS_MODE_PIC_END:
		bs_option = BSOPTION_ENABLE_EXPLICIT_END;
		break;
	default:
		return -EINVAL;
	}

	vpu_write_reg(vpu_inst->dev, W5_BS_RD_PTR, p_dec_info->stream_rd_ptr);
	vpu_write_reg(vpu_inst->dev, W5_BS_WR_PTR, p_dec_info->stream_wr_ptr);
	if (p_dec_info->stream_endflag == 1)
		bs_option = 3; // (stream_end_flag<<1) | EXPLICIT_END
	if (p_open_param->bitstream_mode == BS_MODE_PIC_END)
		bs_option |= BIT(31);
	if (vpu_inst->std == W_AV1_DEC)
		bs_option |= ((p_open_param->av1_format) << 2);
	vpu_write_reg(vpu_inst->dev, W5_BS_OPTION, bs_option);

	/* secondary AXI */
	reg_val = (p_dec_info->sec_axi_info.wave.use_bit_enable << 0) |
		(p_dec_info->sec_axi_info.wave.use_ip_enable << 9) |
		(p_dec_info->sec_axi_info.wave.use_lf_row_enable << 15);
	vpu_write_reg(vpu_inst->dev, W5_USE_SEC_AXI, reg_val);

	/* set attributes of user buffer */
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_USER_MASK, p_dec_info->user_data_enable);

	vpu_write_reg(vpu_inst->dev, W5_COMMAND_OPTION,
		      ((option->disable_film_grain << 6) | (option->cra_as_bla_flag << 5) |
		      mode_option));
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_TEMPORAL_ID_PLUS1,
		      (p_dec_info->target_spatial_id << 9) |
		      (p_dec_info->temp_id_select_mode << 8) | p_dec_info->target_temp_id);
	vpu_write_reg(vpu_inst->dev, W5_CMD_SEQ_CHANGE_ENABLE_FLAG, p_dec_info->seq_change_mask);
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_FORCE_FB_LATENCY_PLUS1, force_latency);

	wave5_bit_issue_command(vpu_inst, W5_DEC_PIC);
	// check QUEUE_DONE
	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(vpu_inst->dev->dev, "dec pic timed out\n");
		return -ETIMEDOUT;
	}

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_QUEUE_STATUS);

	p_dec_info->instance_queue_count = (reg_val >> 16) & 0xff;
	p_dec_info->report_queue_count = (reg_val & 0xffff);
	// FAILED for adding a command into VCPU QUEUE
	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS)) {
		*fail_res = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
		wave5_print_reg_err(vpu_inst->dev, *fail_res);
		return -EIO;
	}

	return 0;
}

int wave5_vpu_dec_get_result(struct vpu_instance *vpu_inst, struct dec_output_info *result)
{
	int ret;
	u32 reg_val, sub_layer_info, index, nal_unit_type;
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;
	struct vpu_device *vpu_dev = vpu_inst->dev;

	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_ADDR_REPORT_BASE, p_dec_info->user_data_buf_addr);
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_REPORT_SIZE, p_dec_info->user_data_buf_size);
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_REPORT_PARAM,
		      VPU_USER_DATA_ENDIAN & VDI_128BIT_ENDIAN_MASK);

	// send QUERY cmd
	ret = wave5_send_query(vpu_inst, GET_RESULT);
	if (ret) {
		if (ret == -EIO) {
			reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
			wave5_print_reg_err(vpu_inst->dev, reg_val);
		}

		return ret;
	}

	dev_dbg(vpu_inst->dev->dev, "dec pic complete\n");

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_QUEUE_STATUS);

	p_dec_info->instance_queue_count = (reg_val >> 16) & 0xff;
	p_dec_info->report_queue_count = (reg_val & 0xffff);

	result->decoding_success = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_DECODING_SUCCESS);
	if (!result->decoding_success)
		result->error_reason = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_ERR_INFO);
	else
		result->warn_info = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_WARN_INFO);

	result->dec_output_ext_data.user_data_size = 0;
	result->dec_output_ext_data.user_data_buf_full = false;
	result->dec_output_ext_data.user_data_header = vpu_read_reg(vpu_inst->dev,
								    W5_RET_DEC_USERDATA_IDC);
	if (result->dec_output_ext_data.user_data_header) {
		reg_val = result->dec_output_ext_data.user_data_header;
		if (reg_val & BIT(USERDATA_FLAG_BUFF_FULL))
			result->dec_output_ext_data.user_data_buf_full = true;
		result->dec_output_ext_data.user_data_size = p_dec_info->user_data_buf_size;
	}

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PIC_TYPE);

	nal_unit_type = (reg_val & 0x3f0) >> 4;
	result->nal_type = nal_unit_type;

	if (vpu_inst->std == W_VP9_DEC) {
		if (reg_val & 0x01)
			result->pic_type = PIC_TYPE_I;
		else if (reg_val & 0x02)
			result->pic_type = PIC_TYPE_P;
		else if (reg_val & 0x04)
			result->pic_type = PIC_TYPE_REPEAT;
		else
			result->pic_type = PIC_TYPE_MAX;
	} else if (vpu_inst->std == W_HEVC_DEC) {
		if (reg_val & 0x04)
			result->pic_type = PIC_TYPE_B;
		else if (reg_val & 0x02)
			result->pic_type = PIC_TYPE_P;
		else if (reg_val & 0x01)
			result->pic_type = PIC_TYPE_I;
		else
			result->pic_type = PIC_TYPE_MAX;
		if ((nal_unit_type == 19 || nal_unit_type == 20) && result->pic_type == PIC_TYPE_I)
			/* IDR_W_RADL, IDR_N_LP */
			result->pic_type = PIC_TYPE_IDR;
	} else if (vpu_inst->std == W_AVC_DEC) {
		if (reg_val & 0x04)
			result->pic_type = PIC_TYPE_B;
		else if (reg_val & 0x02)
			result->pic_type = PIC_TYPE_P;
		else if (reg_val & 0x01)
			result->pic_type = PIC_TYPE_I;
		else
			result->pic_type = PIC_TYPE_MAX;
		if (nal_unit_type == 5 && result->pic_type == PIC_TYPE_I)
			result->pic_type = PIC_TYPE_IDR;
	} else if (vpu_inst->std == W_AV1_DEC) {
		switch (reg_val & 0x07) {
		case 0:
			result->pic_type = PIC_TYPE_KEY; break;
		case 1:
			result->pic_type = PIC_TYPE_INTER; break;
		case 2:
			result->pic_type = PIC_TYPE_AV1_INTRA; break;
		case 3:
			result->pic_type = PIC_TYPE_AV1_SWITCH; break;
		default:
			result->pic_type = PIC_TYPE_MAX; break;
		}
	} else { // AVS2
		switch (reg_val & 0x07) {
		case 0:
			result->pic_type = PIC_TYPE_I; break;
		case 1:
			result->pic_type = PIC_TYPE_P; break;
		case 2:
			result->pic_type = PIC_TYPE_B; break;
		case 3:
			result->pic_type = PIC_TYPE_AVS2_F; break;
		case 4:
			result->pic_type = PIC_TYPE_AVS2_S; break;
		case 5:
			result->pic_type = PIC_TYPE_AVS2_G; break;
		case 6:
			result->pic_type = PIC_TYPE_AVS2_GB; break;
		default:
			result->pic_type = PIC_TYPE_MAX; break;
		}
	}
	result->output_flag = (reg_val >> 31) & 0x1;
	result->ctu_size = 16 << ((reg_val >> 10) & 0x3);
	index = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_DISPLAY_INDEX);
	result->index_frame_display = index;
	result->index_frame_display_for_tiled = index;
	index = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_DECODED_INDEX);
	result->index_frame_decoded = index;
	result->index_frame_decoded_for_tiled = index;

	sub_layer_info = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_SUB_LAYER_INFO);
	result->temporal_id = sub_layer_info & 0x7;

	if (vpu_inst->std == W_HEVC_DEC) {
		result->decoded_poc = -1;
		result->display_poc = -1;
		if (result->index_frame_decoded >= 0 ||
		    result->index_frame_decoded == DECODED_IDX_FLAG_SKIP)
			result->decoded_poc = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PIC_POC);
	} else if (vpu_inst->std == W_AVS2_DEC) {
		result->avs2_info.decoded_poi = -1;
		result->avs2_info.display_poi = -1;
		if (result->index_frame_decoded >= 0)
			result->avs2_info.decoded_poi =
				vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PIC_POC);
	} else if (vpu_inst->std == W_AVC_DEC) {
		result->decoded_poc = -1;
		result->display_poc = -1;
		if (result->index_frame_decoded >= 0 ||
		    result->index_frame_decoded == DECODED_IDX_FLAG_SKIP)
			result->decoded_poc = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PIC_POC);
	} else if (vpu_inst->std == W_AV1_DEC) {
		result->decoded_poc = -1;
		result->display_poc = -1;
		if (result->index_frame_decoded >= 0 ||
		    result->index_frame_decoded == DECODED_IDX_FLAG_SKIP)
			result->decoded_poc = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PIC_POC);

		result->av1_info.spatial_id = (sub_layer_info >> 19) & 0x3;
		reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PIC_PARAM);
		result->av1_info.allow_intra_bc = (reg_val >> 0) & 0x1;
		result->av1_info.allow_screen_content_tools = (reg_val >> 1) & 0x1;
	}

	result->sequence_changed = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_NOTIFICATION);
	if (result->sequence_changed & SEQ_CHANGE_INTER_RES_CHANGE)
		result->index_inter_frame_decoded = vpu_read_reg(vpu_inst->dev,
								 W5_RET_DEC_REALLOC_INDEX);

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PIC_SIZE);
	result->dec_pic_width = reg_val >> 16;
	result->dec_pic_height = reg_val & 0xffff;

	if (result->sequence_changed) {
		memcpy((void *)&p_dec_info->new_seq_info, (void *)&p_dec_info->initial_info,
		       sizeof(struct dec_initial_info));
		wave5_get_dec_seq_result(vpu_inst, &p_dec_info->new_seq_info);
	}

	result->num_of_err_m_bs = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_ERR_CTB_NUM) >> 16;
	result->num_of_tot_m_bs = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_ERR_CTB_NUM) & 0xffff;
	result->byte_pos_frame_start = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_AU_START_POS);
	result->byte_pos_frame_end = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_AU_END_POS);

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_RECOVERY_POINT);
	result->h265_rp_sei.recovery_poc_cnt = reg_val & 0xFFFF; // [15:0]
	result->h265_rp_sei.exact_match_flag = (reg_val >> 16) & 0x01; // [16]
	result->h265_rp_sei.broken_link_flag = (reg_val >> 17) & 0x01; // [17]
	result->h265_rp_sei.exist = (reg_val >> 18) & 0x01; // [18]
	if (!result->h265_rp_sei.exist) {
		result->h265_rp_sei.recovery_poc_cnt = 0;
		result->h265_rp_sei.exact_match_flag = 0;
		result->h265_rp_sei.broken_link_flag = 0;
	}

	result->dec_host_cmd_tick = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_HOST_CMD_TICK);
	result->dec_seek_start_tick = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_SEEK_START_TICK);
	result->dec_seek_end_tick = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_SEEK_END_TICK);
	result->dec_parse_start_tick = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PARSING_START_TICK);
	result->dec_parse_end_tick = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_PARSING_END_TICK);
	result->dec_decode_start_tick = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_DECODING_START_TICK);
	result->dec_decode_end_tick = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_DECODING_ENC_TICK);

	if (!p_dec_info->first_cycle_check) {
		result->frame_cycle =
			(result->dec_decode_end_tick - result->dec_host_cmd_tick) *
			p_dec_info->cycle_per_tick;
		vpu_dev->last_performance_cycles = result->dec_decode_end_tick;
		p_dec_info->first_cycle_check = true;
	} else if (result->index_frame_decoded_for_tiled != -1) {
		result->frame_cycle =
			(result->dec_decode_end_tick - vpu_dev->last_performance_cycles) *
			p_dec_info->cycle_per_tick;
		vpu_dev->last_performance_cycles = result->dec_decode_end_tick;
		if (vpu_dev->last_performance_cycles < result->dec_host_cmd_tick)
			result->frame_cycle =
				(result->dec_decode_end_tick - result->dec_host_cmd_tick);
	}
	result->seek_cycle =
		(result->dec_seek_end_tick - result->dec_seek_start_tick) *
		p_dec_info->cycle_per_tick;
	result->parse_cycle =
		(result->dec_parse_end_tick - result->dec_parse_start_tick) *
		p_dec_info->cycle_per_tick;
	result->decoded_cycle =
		(result->dec_decode_end_tick - result->dec_decode_start_tick) *
		p_dec_info->cycle_per_tick;

	// no remaining command. reset frame cycle.
	if (p_dec_info->instance_queue_count == 0 && p_dec_info->report_queue_count == 0)
		p_dec_info->first_cycle_check = false;

	return 0;
}

int wave5_vpu_re_init(struct device *dev, u8 *fw, uint32_t size)
{
	struct dma_vpu_buf *sram_vb;
	struct vpu_buf *common_vb;
	dma_addr_t code_base, temp_base;
	dma_addr_t old_code_base, temp_size;
	u32 code_size;
	u32 reg_val, remap_size;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	int ret;

	common_vb = &vpu_dev->common_mem;

	code_base = common_vb->daddr;
	/* ALIGN TO 4KB */
	code_size = (WAVE5_MAX_CODE_BUF_SIZE & ~0xfff);
	if (code_size < size * 2)
		return -EINVAL;
	temp_base = common_vb->daddr + WAVE5_TEMPBUF_OFFSET;
	temp_size = WAVE5_TEMPBUF_SIZE;

	old_code_base = vpu_read_reg(vpu_dev, W5_VPU_REMAP_PADDR);

	if (old_code_base != code_base + W5_REMAP_INDEX1 * W5_REMAP_MAX_SIZE) {
		wave5_vdi_write_memory(vpu_dev, common_vb, 0, fw, size, VDI_128BIT_LITTLE_ENDIAN);

		vpu_write_reg(vpu_dev, W5_PO_CONF, 0);

		wave5_vpu_reset(dev, SW_RESET_ON_BOOT);

		/* remap page size 0*/
		remap_size = (W5_REMAP_MAX_SIZE >> 12) & 0x1ff;
		reg_val = 0x80000000 | (WAVE5_UPPER_PROC_AXI_ID << 20) | (0 << 16)
			| (W5_REMAP_INDEX0 << 12) | BIT(11) | remap_size;
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_CTRL, reg_val);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_VADDR, W5_REMAP_INDEX0 * W5_REMAP_MAX_SIZE);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_PADDR,
			      code_base + W5_REMAP_INDEX0 * W5_REMAP_MAX_SIZE);

		/* remap page size 1*/
		remap_size = (W5_REMAP_MAX_SIZE >> 12) & 0x1ff;
		reg_val = 0x80000000 | (WAVE5_UPPER_PROC_AXI_ID << 20) | (0 << 16)
			| (W5_REMAP_INDEX1 << 12) | BIT(11) | remap_size;
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_CTRL, reg_val);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_VADDR, W5_REMAP_INDEX1 * W5_REMAP_MAX_SIZE);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_PADDR,
			      code_base + W5_REMAP_INDEX1 * W5_REMAP_MAX_SIZE);

		vpu_write_reg(vpu_dev, W5_ADDR_CODE_BASE, code_base);
		vpu_write_reg(vpu_dev, W5_CODE_SIZE, code_size);
		vpu_write_reg(vpu_dev, W5_CODE_PARAM, (WAVE5_UPPER_PROC_AXI_ID << 4) | 0);
		vpu_write_reg(vpu_dev, W5_ADDR_TEMP_BASE, temp_base);
		vpu_write_reg(vpu_dev, W5_TEMP_SIZE, temp_size);

		vpu_write_reg(vpu_dev, W5_HW_OPTION, 0);

		/* interrupt */
		// encoder
		reg_val = BIT(INT_WAVE5_ENC_SET_PARAM);
		reg_val |= BIT(INT_WAVE5_ENC_PIC);
		reg_val |= BIT(INT_WAVE5_BSBUF_FULL);
		// decoder
		reg_val |= BIT(INT_WAVE5_INIT_SEQ);
		reg_val |= BIT(INT_WAVE5_DEC_PIC);
		reg_val |= BIT(INT_WAVE5_BSBUF_EMPTY);
		vpu_write_reg(vpu_dev, W5_VPU_VINT_ENABLE, reg_val);

		reg_val = vpu_read_reg(vpu_dev, W5_VPU_RET_VPU_CONFIG0);
		if ((reg_val >> 16) & 1) {
			reg_val = ((WAVE5_PROC_AXI_ID << 28) |
					(WAVE5_PRP_AXI_ID << 24) |
					(WAVE5_FBD_Y_AXI_ID << 20) |
					(WAVE5_FBC_Y_AXI_ID << 16) |
					(WAVE5_FBD_C_AXI_ID << 12) |
					(WAVE5_FBC_C_AXI_ID << 8) |
					(WAVE5_PRI_AXI_ID << 4) |
					(WAVE5_SEC_AXI_ID << 0));
			wave5_fio_writel(vpu_dev, W5_BACKBONE_PROG_AXI_ID, reg_val);
		}

		sram_vb = get_sram_memory(vpu_dev);

		vpu_write_reg(vpu_dev, W5_ADDR_SEC_AXI, sram_vb->daddr);
		vpu_write_reg(vpu_dev, W5_SEC_AXI_SIZE, sram_vb->size);
		vpu_write_reg(vpu_dev, W5_VPU_BUSY_STATUS, 1);
		vpu_write_reg(vpu_dev, W5_COMMAND, W5_INIT_VPU);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_CORE_START, 1);

		ret = wave5_wait_vpu_busy(vpu_dev, W5_VPU_BUSY_STATUS);
		if (ret) {
			dev_err(vpu_dev->dev, "VPU reinit(W5_VPU_REMAP_CORE_START) timeout\n");
			return ret;
		}

		reg_val = vpu_read_reg(vpu_dev, W5_RET_SUCCESS);
		if (!reg_val) {
			u32 reason_code = vpu_read_reg(vpu_dev, W5_RET_FAIL_REASON);

			wave5_print_reg_err(vpu_dev, reason_code);
			return -EIO;
		}
	}

	setup_wave5_properties(dev);
	return 0;
}

static int wave5_vpu_sleep_wake(struct device *dev, int i_sleep_wake, const uint16_t *code,
				uint32_t size)
{
	u32 reg_val;
	struct vpu_buf *common_vb;
	dma_addr_t code_base;
	u32 code_size;
	u32 remap_size;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	int ret;

	if (i_sleep_wake == 1) {
		ret = wave5_wait_vpu_busy(vpu_dev, W5_VPU_BUSY_STATUS);
		if (ret)
			return ret;

		vpu_write_reg(vpu_dev, W5_VPU_BUSY_STATUS, 1);
		vpu_write_reg(vpu_dev, W5_COMMAND, W5_SLEEP_VPU);
		vpu_write_reg(vpu_dev, W5_VPU_HOST_INT_REQ, 1);

		ret = wave5_wait_vpu_busy(vpu_dev, W5_VPU_BUSY_STATUS);
		if (ret)
			return ret;

		if (!vpu_read_reg(vpu_dev, W5_RET_SUCCESS)) {
			u32 reason = vpu_read_reg(vpu_dev, W5_RET_FAIL_REASON);

			wave5_print_reg_err(vpu_dev, reason);
			return -EIO;
		}
	} else { /* restore */
		common_vb = &vpu_dev->common_mem;

		code_base = common_vb->daddr;
		/* ALIGN TO 4KB */
		code_size = (WAVE5_MAX_CODE_BUF_SIZE & ~0xfff);
		if (code_size < size * 2) {
			dev_err(dev, "size too small\n");
			return -EINVAL;
		}

		vpu_write_reg(vpu_dev, W5_PO_CONF, 0);

		/* remap page size 0*/
		remap_size = (W5_REMAP_MAX_SIZE >> 12) & 0x1ff;
		reg_val = 0x80000000 | (WAVE5_UPPER_PROC_AXI_ID << 20) | (0 << 16)
			| (W5_REMAP_INDEX0 << 12) | BIT(11) | remap_size;
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_CTRL, reg_val);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_VADDR, W5_REMAP_INDEX0 * W5_REMAP_MAX_SIZE);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_PADDR,
			      code_base + W5_REMAP_INDEX0 * W5_REMAP_MAX_SIZE);

		/* remap page size 1*/
		remap_size = (W5_REMAP_MAX_SIZE >> 12) & 0x1ff;
		reg_val = 0x80000000 | (WAVE5_UPPER_PROC_AXI_ID << 20) | (0 << 16)
			| (W5_REMAP_INDEX1 << 12) | BIT(11) | remap_size;
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_CTRL, reg_val);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_VADDR, W5_REMAP_INDEX1 * W5_REMAP_MAX_SIZE);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_PADDR,
			      code_base + W5_REMAP_INDEX1 * W5_REMAP_MAX_SIZE);

		vpu_write_reg(vpu_dev, W5_ADDR_CODE_BASE, code_base);
		vpu_write_reg(vpu_dev, W5_CODE_SIZE, code_size);
		vpu_write_reg(vpu_dev, W5_CODE_PARAM, (WAVE5_UPPER_PROC_AXI_ID << 4) | 0);

		vpu_write_reg(vpu_dev, W5_HW_OPTION, 0);

		/* interrupt */
		// encoder
		reg_val = BIT(INT_WAVE5_ENC_SET_PARAM);
		reg_val |= BIT(INT_WAVE5_ENC_PIC);
		reg_val |= BIT(INT_WAVE5_BSBUF_FULL);
		// decoder
		reg_val |= BIT(INT_WAVE5_INIT_SEQ);
		reg_val |= BIT(INT_WAVE5_DEC_PIC);
		reg_val |= BIT(INT_WAVE5_BSBUF_EMPTY);
		vpu_write_reg(vpu_dev, W5_VPU_VINT_ENABLE, reg_val);

		reg_val = vpu_read_reg(vpu_dev, W5_VPU_RET_VPU_CONFIG0);
		if ((reg_val >> 16) & 1) {
			reg_val = ((WAVE5_PROC_AXI_ID << 28) |
					(WAVE5_PRP_AXI_ID << 24) |
					(WAVE5_FBD_Y_AXI_ID << 20) |
					(WAVE5_FBC_Y_AXI_ID << 16) |
					(WAVE5_FBD_C_AXI_ID << 12) |
					(WAVE5_FBC_C_AXI_ID << 8) |
					(WAVE5_PRI_AXI_ID << 4) |
					(WAVE5_SEC_AXI_ID << 0));
			wave5_fio_writel(vpu_dev, W5_BACKBONE_PROG_AXI_ID, reg_val);
		}

		vpu_write_reg(vpu_dev, W5_VPU_BUSY_STATUS, 1);
		vpu_write_reg(vpu_dev, W5_COMMAND, W5_WAKEUP_VPU);
		vpu_write_reg(vpu_dev, W5_VPU_REMAP_CORE_START, 1);

		ret = wave5_wait_vpu_busy(vpu_dev, W5_VPU_BUSY_STATUS);
		if (ret) {
			dev_err(vpu_dev->dev, "VPU wakeup(W5_VPU_REMAP_CORE_START) timeout\n");
			return ret;
		}

		reg_val = vpu_read_reg(vpu_dev, W5_RET_SUCCESS);
		if (!reg_val) {
			u32 reason_code = vpu_read_reg(vpu_dev, W5_RET_FAIL_REASON);

			wave5_print_reg_err(vpu_dev, reason_code);
			return -EIO;
		}
	}

	return 0;
}

int wave5_vpu_reset(struct device *dev, enum sw_reset_mode reset_mode)
{
	u32 val = 0;
	int ret = 0;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	struct vpu_attr *p_attr = &vpu_dev->attr;
	// VPU doesn't send response. force to set BUSY flag to 0.
	vpu_write_reg(vpu_dev, W5_VPU_BUSY_STATUS, 0);

	if (reset_mode == SW_RESET_SAFETY) {
		ret = wave5_vpu_sleep_wake(dev, true, NULL, 0);
		if (ret)
			return ret;
	}

	val = vpu_read_reg(vpu_dev, W5_VPU_RET_VPU_CONFIG0);
	if ((val >> 16) & 0x1)
		p_attr->support_backbone = true;
	if ((val >> 22) & 0x1)
		p_attr->support_vcore_backbone = true;
	if ((val >> 28) & 0x1)
		p_attr->support_vcpu_backbone = true;

	val = vpu_read_reg(vpu_dev, W5_VPU_RET_VPU_CONFIG1);
	if ((val >> 26) & 0x1)
		p_attr->support_dual_core = true;

	// waiting for completion of bus transaction
	if (p_attr->support_backbone) {
		dev_dbg(dev, "backbone supported\n");

		if (p_attr->support_dual_core) {
			// check CORE0
			wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE0, 0x7);

			ret = wave5_wait_bus_busy(vpu_dev, W5_BACKBONE_BUS_STATUS_VCORE0);
			if (ret) {
				wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE0, 0x00);
				return ret;
			}

			// check CORE1
			wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE1, 0x7);

			ret = wave5_wait_bus_busy(vpu_dev, W5_BACKBONE_BUS_STATUS_VCORE1);
			if (ret) {
				wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE1, 0x00);
				return ret;
			}

		} else if (p_attr->support_vcore_backbone) {
			if (p_attr->support_vcpu_backbone) {
				// step1 : disable request
				wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCPU, 0xFF);

				// step2 : waiting for completion of bus transaction
				ret = wave5_wait_vcpu_bus_busy(vpu_dev,
							       W5_BACKBONE_BUS_STATUS_VCPU);
				if (ret) {
					wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCPU, 0x00);
					return ret;
				}
			}
			// step1 : disable request
			wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE0, 0x7);

			// step2 : waiting for completion of bus transaction
			if (wave5_wait_bus_busy(vpu_dev, W5_BACKBONE_BUS_STATUS_VCORE0)) {
				wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE0, 0x00);
				return -EBUSY;
			}
		} else {
			// step1 : disable request
			wave5_fio_writel(vpu_dev, W5_COMBINED_BACKBONE_BUS_CTRL, 0x7);

			// step2 : waiting for completion of bus transaction
			if (wave5_wait_bus_busy(vpu_dev, W5_COMBINED_BACKBONE_BUS_STATUS)) {
				wave5_fio_writel(vpu_dev, W5_COMBINED_BACKBONE_BUS_CTRL, 0x00);
				return -EBUSY;
			}
		}
	} else {
		dev_dbg(dev, "backbone NOT supported\n");
		// step1 : disable request
		wave5_fio_writel(vpu_dev, W5_GDI_BUS_CTRL, 0x100);

		// step2 : waiting for completion of bus transaction
		ret = wave5_wait_bus_busy(vpu_dev, W5_GDI_BUS_STATUS);
		if (ret) {
			wave5_fio_writel(vpu_dev, W5_GDI_BUS_CTRL, 0x00);
			return ret;
		}
	}

	switch (reset_mode) {
	case SW_RESET_ON_BOOT:
	case SW_RESET_FORCE:
	case SW_RESET_SAFETY:
		val = W5_RST_BLOCK_ALL;
		break;
	default:
		return -EINVAL;
	}

	if (val) {
		vpu_write_reg(vpu_dev, W5_VPU_RESET_REQ, val);

		ret = wave5_wait_vpu_busy(vpu_dev, W5_VPU_RESET_STATUS);
		if (ret) {
			vpu_write_reg(vpu_dev, W5_VPU_RESET_REQ, 0);
			return ret;
		}
		vpu_write_reg(vpu_dev, W5_VPU_RESET_REQ, 0);
	}
	// step3 : must clear GDI_BUS_CTRL after done SW_RESET
	if (p_attr->support_backbone) {
		if (p_attr->support_dual_core) {
			wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE0, 0x00);
			wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE1, 0x00);
		} else if (p_attr->support_vcore_backbone) {
			if (p_attr->support_vcpu_backbone)
				wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCPU, 0x00);
			wave5_fio_writel(vpu_dev, W5_BACKBONE_BUS_CTRL_VCORE0, 0x00);
		} else {
			wave5_fio_writel(vpu_dev, W5_COMBINED_BACKBONE_BUS_CTRL, 0x00);
		}
	} else {
		wave5_fio_writel(vpu_dev, W5_GDI_BUS_CTRL, 0x00);
	}
	if (reset_mode == SW_RESET_SAFETY || reset_mode == SW_RESET_FORCE)
		ret = wave5_vpu_sleep_wake(dev, false, NULL, 0);

	return ret;
}

int wave5_vpu_dec_fini_seq(struct vpu_instance *vpu_inst, u32 *fail_res)
{
	int ret;

	wave5_bit_issue_command(vpu_inst, W5_DESTROY_INSTANCE);
	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret)
		return -ETIMEDOUT;

	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS)) {
		*fail_res = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
		wave5_print_reg_err(vpu_inst->dev, *fail_res);
		return -EIO;
	}

	return 0;
}

int wave5_vpu_dec_set_bitstream_flag(struct vpu_instance *vpu_inst, bool eos)
{
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;
	enum bit_stream_mode bs_mode = (enum bit_stream_mode)p_dec_info->open_param.bitstream_mode;
	int ret;

	p_dec_info->stream_endflag = eos ? 1 : 0;

	if (bs_mode == BS_MODE_INTERRUPT) {
		vpu_write_reg(vpu_inst->dev, W5_BS_OPTION, (p_dec_info->stream_endflag << 1));
		vpu_write_reg(vpu_inst->dev, W5_BS_WR_PTR, p_dec_info->stream_wr_ptr);

		wave5_bit_issue_command(vpu_inst, W5_UPDATE_BS);
		ret = wave5_wait_vpu_busy(vpu_inst->dev,
					  W5_VPU_BUSY_STATUS);
		if (ret)
			return ret;

		if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS))
			return -EIO;
	}

	return 0;
}

int wave5_dec_clr_disp_flag(struct vpu_instance *vpu_inst, unsigned int index)
{
	struct dec_info *p_dec_info = &vpu_inst->codec_info->dec_info;
	int ret;

	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_CLR_DISP_IDC, BIT(index));
	vpu_write_reg(vpu_inst->dev, W5_CMD_DEC_SET_DISP_IDC, 0);
	ret = wave5_send_query(vpu_inst, UPDATE_DISP_FLAG);

	if (ret) {
		if (ret == -EIO) {
			u32 reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);

			wave5_print_reg_err(vpu_inst->dev, reg_val);
		}
		return ret;
	}

	p_dec_info->frame_display_flag = vpu_read_reg(vpu_inst->dev, W5_RET_DEC_DISP_IDC);

	return 0;
}

int wave5_vpu_clear_interrupt(struct vpu_instance *vpu_inst, uint32_t flags)
{
	u32 interrupt_reason;

	interrupt_reason = vpu_read_reg(vpu_inst->dev, W5_VPU_VINT_REASON_USR);
	interrupt_reason &= ~flags;
	vpu_write_reg(vpu_inst->dev, W5_VPU_VINT_REASON_USR, interrupt_reason);

	return 0;
}

dma_addr_t wave5_vpu_dec_get_rd_ptr(struct vpu_instance *vpu_inst)
{
	int ret;

	ret = wave5_send_query(vpu_inst, GET_BS_RD_PTR);

	if (ret)
		return vpu_inst->codec_info->dec_info.stream_rd_ptr;

	return vpu_read_reg(vpu_inst->dev, W5_RET_QUERY_DEC_BS_RD_PTR);
}

/************************************************************************/
/* ENCODER functions */
/************************************************************************/

int wave5_vpu_build_up_enc_param(struct device *dev, struct vpu_instance *vpu_inst,
				 struct enc_open_param *param)
{
	int ret;
	struct enc_info *p_enc_info = &vpu_inst->codec_info->enc_info;
	u32 reg_val;
	struct dma_vpu_buf *sram_vb;
	u32 bs_endian;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);

	sram_vb = get_sram_memory(vpu_dev);
	p_enc_info->sec_axi_info.buf_base = sram_vb->daddr;
	p_enc_info->sec_axi_info.buf_size = sram_vb->size;

	if (vpu_dev->product == PRODUCT_ID_521)
		p_enc_info->vb_work.size = WAVE521ENC_WORKBUF_SIZE;

	ret = wave5_vdi_allocate_dma_memory(vpu_dev, &p_enc_info->vb_work);
	if (ret) {
		memset(&p_enc_info->vb_work, 0, sizeof(p_enc_info->vb_work));
		return ret;
	}

	wave5_vdi_clear_memory(vpu_dev, &p_enc_info->vb_work);

	vpu_write_reg(vpu_inst->dev, W5_ADDR_WORK_BASE, p_enc_info->vb_work.daddr);
	vpu_write_reg(vpu_inst->dev, W5_WORK_SIZE, p_enc_info->vb_work.size);

	reg_val = wave5_vdi_convert_endian(vpu_dev, param->stream_endian);
	bs_endian = (~reg_val & VDI_128BIT_ENDIAN_MASK);

	reg_val = (param->line_buf_int_en << 6) | bs_endian;
	vpu_write_reg(vpu_inst->dev, W5_CMD_BS_PARAM, reg_val);
	vpu_write_reg(vpu_inst->dev, W5_CMD_NUM_CQ_DEPTH_M1, (COMMAND_QUEUE_DEPTH - 1));

	reg_val = 0;
	if (vpu_dev->product == PRODUCT_ID_521)
		reg_val |= (param->sub_frame_sync_enable | param->sub_frame_sync_mode << 1);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SRC_OPTIONS, reg_val);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_VCORE_INFO, 1);

	wave5_bit_issue_command(vpu_inst, W5_CREATE_INSTANCE);
	// check QUEUE_DONE
	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(vpu_inst->dev->dev, "create instance timed out\n");
		wave5_vdi_free_dma_memory(vpu_dev, &p_enc_info->vb_work);
		return ret;
	}

	// FAILED for adding into VCPU QUEUE
	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS)) {
		wave5_vdi_free_dma_memory(vpu_dev, &p_enc_info->vb_work);
		reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
		wave5_print_reg_err(vpu_inst->dev, reg_val);
		return -EIO;
	}

	p_enc_info->sub_frame_sync_config.sub_frame_sync_mode = param->sub_frame_sync_mode;
	p_enc_info->sub_frame_sync_config.sub_frame_sync_on = param->sub_frame_sync_enable;
	p_enc_info->stream_rd_ptr = param->bitstream_buffer;
	p_enc_info->stream_wr_ptr = param->bitstream_buffer;
	p_enc_info->line_buf_int_en = param->line_buf_int_en;
	p_enc_info->stream_buf_start_addr = param->bitstream_buffer;
	p_enc_info->stream_buf_size = param->bitstream_buffer_size;
	p_enc_info->stream_buf_end_addr = param->bitstream_buffer + param->bitstream_buffer_size;
	p_enc_info->stride = 0;
	p_enc_info->initial_info_obtained = false;
	p_enc_info->product_code = vpu_read_reg(vpu_inst->dev, W5_PRODUCT_NUMBER);

	return 0;
}

static int wave5_set_enc_crop_info(u32 codec, struct enc_wave_param *param, int rot_mode,
				   int src_width, int src_height)
{
	int aligned_width = (codec == W_HEVC_ENC) ? ALIGN(src_width, 32) : ALIGN(src_width, 16);
	int aligned_height = (codec == W_HEVC_ENC) ? ALIGN(src_height, 32) : ALIGN(src_height, 16);
	int pad_right, pad_bot;
	int crop_right, crop_left, crop_top, crop_bot;
	int prp_mode = rot_mode >> 1; // remove prp_enable bit

	if (codec == W_HEVC_ENC &&
	    (!rot_mode || prp_mode == 14)) // prp_mode 14 : hor_mir && ver_mir && rot_180
		return 0;

	pad_right = aligned_width - src_width;
	pad_bot = aligned_height - src_height;

	if (param->conf_win_right > 0)
		crop_right = param->conf_win_right + pad_right;
	else
		crop_right = pad_right;

	if (param->conf_win_bot > 0)
		crop_bot = param->conf_win_bot + pad_bot;
	else
		crop_bot = pad_bot;

	crop_top = param->conf_win_top;
	crop_left = param->conf_win_left;

	param->conf_win_top = crop_top;
	param->conf_win_left = crop_left;
	param->conf_win_bot = crop_bot;
	param->conf_win_right = crop_right;

	if (prp_mode == 1 || prp_mode == 15) {
		param->conf_win_top = crop_right;
		param->conf_win_left = crop_top;
		param->conf_win_bot = crop_left;
		param->conf_win_right = crop_bot;
	} else if (prp_mode == 2 || prp_mode == 12) {
		param->conf_win_top = crop_bot;
		param->conf_win_left = crop_right;
		param->conf_win_bot = crop_top;
		param->conf_win_right = crop_left;
	} else if (prp_mode == 3 || prp_mode == 13) {
		param->conf_win_top = crop_left;
		param->conf_win_left = crop_bot;
		param->conf_win_bot = crop_right;
		param->conf_win_right = crop_top;
	} else if (prp_mode == 4 || prp_mode == 10) {
		param->conf_win_top = crop_bot;
		param->conf_win_bot = crop_top;
	} else if (prp_mode == 8 || prp_mode == 6) {
		param->conf_win_left = crop_right;
		param->conf_win_right = crop_left;
	} else if (prp_mode == 5 || prp_mode == 11) {
		param->conf_win_top = crop_left;
		param->conf_win_left = crop_top;
		param->conf_win_bot = crop_right;
		param->conf_win_right = crop_bot;
	} else if (prp_mode == 7 || prp_mode == 9) {
		param->conf_win_top = crop_right;
		param->conf_win_left = crop_bot;
		param->conf_win_bot = crop_left;
		param->conf_win_right = crop_top;
	}

	return 0;
}

int wave5_vpu_enc_init_seq(struct vpu_instance *vpu_inst)
{
	u32 reg_val = 0, rot_mir_mode, fixed_cu_size_mode = 0x7;
	struct enc_info *p_enc_info = &vpu_inst->codec_info->enc_info;
	struct enc_open_param *p_open_param = &p_enc_info->open_param;
	struct enc_wave_param *p_param = &p_open_param->wave_param;
	int ret;

	if (vpu_inst->dev->product != PRODUCT_ID_521)
		return -EINVAL;

	/*==============================================*/
	/* OPT_CUSTOM_GOP */
	/*==============================================*/
	/*
	 * SET_PARAM + CUSTOM_GOP
	 * only when gop_preset_idx == custom_gop, custom_gop related registers should be set
	 */
	if (p_param->gop_preset_idx == PRESET_IDX_CUSTOM_GOP) {
		int i = 0, j = 0;

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_CUSTOM_GOP_PARAM,
			      p_param->gop_param.custom_gop_size);
		for (i = 0; i < p_param->gop_param.custom_gop_size; i++) {
			vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_CUSTOM_GOP_PIC_PARAM_0 + (i * 4),
				      (p_param->gop_param.pic_param[i].pic_type << 0) |
				      (p_param->gop_param.pic_param[i].poc_offset << 2) |
				      (p_param->gop_param.pic_param[i].pic_qp << 6) |
				      (p_param->gop_param.pic_param[i].use_multi_ref_p << 13) |
				      ((p_param->gop_param.pic_param[i].ref_poc_l0 & 0x1F) << 14) |
				      ((p_param->gop_param.pic_param[i].ref_poc_l1 & 0x1F) << 19) |
				      (p_param->gop_param.pic_param[i].temporal_id << 24));
		}

		for (j = i; j < MAX_GOP_NUM; j++)
			vpu_write_reg(vpu_inst->dev,
				      W5_CMD_ENC_CUSTOM_GOP_PIC_PARAM_0 + (j * 4), 0);

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_SET_PARAM_OPTION, OPT_CUSTOM_GOP);
		wave5_bit_issue_command(vpu_inst, W5_ENC_SET_PARAM);

		ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
		if (ret) {
			dev_warn(vpu_inst->dev->dev, "enc set param timeout op=0x%x\n",
				 OPT_CUSTOM_GOP);
			return ret;
		}
	}

	/*======================================================================*/
	/* OPT_COMMON */
	/* :								*/
	/*	the last SET_PARAM command should be called with OPT_COMMON */
	/*======================================================================*/
	rot_mir_mode = 0;
	if (p_enc_info->rotation_enable) {
		switch (p_enc_info->rotation_angle) {
		case 0:
			rot_mir_mode |= 0x0; break;
		case 90:
			rot_mir_mode |= 0x3; break;
		case 180:
			rot_mir_mode |= 0x5; break;
		case 270:
			rot_mir_mode |= 0x7; break;
		}
	}

	if (p_enc_info->mirror_enable) {
		switch (p_enc_info->mirror_direction) {
		case MIRDIR_NONE:
			rot_mir_mode |= 0x0; break;
		case MIRDIR_VER:
			rot_mir_mode |= 0x9; break;
		case MIRDIR_HOR:
			rot_mir_mode |= 0x11; break;
		case MIRDIR_HOR_VER:
			rot_mir_mode |= 0x19; break;
		}
	}

	wave5_set_enc_crop_info(vpu_inst->std, p_param, rot_mir_mode, p_open_param->pic_width,
				p_open_param->pic_height);

	/* SET_PARAM + COMMON */
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_SET_PARAM_OPTION, OPT_COMMON);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_SRC_SIZE, p_open_param->pic_height << 16
			| p_open_param->pic_width);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_MAP_ENDIAN, VDI_LITTLE_ENDIAN);

	if (vpu_inst->std == W_AVC_ENC) {
		reg_val = (p_param->profile << 0) |
			(p_param->level << 3) |
			(p_param->internal_bit_depth << 14) |
			(p_param->use_long_term << 21);
		if (p_param->scaling_list_enable == 2) {
			reg_val |= BIT(22) | BIT(23); // [23]=USE_DEFAULT_SCALING_LIST
		} else { // 0 or 1
			reg_val |= (p_param->scaling_list_enable << 22);
		}
	} else { // HEVC enc
		reg_val = (p_param->profile << 0) |
			(p_param->level << 3) |
			(p_param->tier << 12) |
			(p_param->internal_bit_depth << 14) |
			(p_param->use_long_term << 21) |
			(p_param->tmvp_enable << 23) |
			(p_param->sao_enable << 24) |
			(p_param->skip_intra_trans << 25) |
			(p_param->strong_intra_smooth_enable << 27) |
			(p_param->en_still_picture << 30);
		if (p_param->scaling_list_enable == 2)
			reg_val |= BIT(22) | BIT(31); // [31]=USE_DEFAULT_SCALING_LIST
		else
			reg_val |= (p_param->scaling_list_enable << 22);
	}

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_SPS_PARAM, reg_val);

	reg_val = (p_param->lossless_enable) |
		(p_param->const_intra_pred_flag << 1) |
		(p_param->lf_cross_slice_boundary_enable << 2) |
		(p_param->weight_pred_enable << 3) |
		(p_param->wpp_enable << 4) |
		(p_param->disable_deblk << 5) |
		((p_param->beta_offset_div2 & 0xF) << 6) |
		((p_param->tc_offset_div2 & 0xF) << 10) |
		((p_param->chroma_cb_qp_offset & 0x1F) << 14) |
		((p_param->chroma_cr_qp_offset & 0x1F) << 19) |
		(p_param->transform8x8_enable << 29) |
		(p_param->entropy_coding_mode << 30);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_PPS_PARAM, reg_val);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_GOP_PARAM, p_param->gop_preset_idx);

	if (vpu_inst->std == W_AVC_ENC)
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_INTRA_PARAM, (p_param->intra_qp << 0) |
				((p_param->intra_period & 0x7ff) << 6) |
				((p_param->avc_idr_period & 0x7ff) << 17) |
				((p_param->forced_idr_header_enable & 3) << 28));
	else
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_INTRA_PARAM,
			      (p_param->decoding_refresh_type << 0) | (p_param->intra_qp << 3) |
				(p_param->forced_idr_header_enable << 9) |
				(p_param->intra_period << 16));

	reg_val = (p_param->use_recommend_enc_param) |
		(p_param->rdo_skip << 2) |
		(p_param->lambda_scaling_enable << 3) |
		(p_param->coef_clear_disable << 4) |
		(fixed_cu_size_mode << 5) |
		(p_param->intra_nx_n_enable << 8) |
		(p_param->max_num_merge << 18) |
		(p_param->custom_md_enable << 20) |
		(p_param->custom_lambda_enable << 21) |
		(p_param->monochrome_enable << 22);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RDO_PARAM, reg_val);

	if (vpu_inst->std == W_AVC_ENC)
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_INTRA_REFRESH,
			      p_param->intra_mb_refresh_arg << 16 | p_param->intra_mb_refresh_mode);
	else
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_INTRA_REFRESH,
			      p_param->intra_refresh_arg << 16 | p_param->intra_refresh_mode);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_FRAME_RATE, p_open_param->frame_rate_info);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_TARGET_RATE, p_open_param->bit_rate);

	if (vpu_inst->std == W_AVC_ENC)
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_PARAM,
			      (p_open_param->rc_enable << 0) |
				(p_param->mb_level_rc_enable << 1) |
				(p_param->hvs_qp_enable << 2) |
				(p_param->hvs_qp_scale << 4) |
				(p_param->bit_alloc_mode << 8) |
				(p_param->roi_enable << 13) |
				((p_param->initial_rc_qp & 0x3F) << 14) |
				(p_open_param->vbv_buffer_size << 20));
	else
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_PARAM,
			      (p_open_param->rc_enable << 0) |
				(p_param->cu_level_rc_enable << 1) |
				(p_param->hvs_qp_enable << 2) |
				(p_param->hvs_qp_scale << 4) |
				(p_param->bit_alloc_mode << 8) |
				(p_param->roi_enable << 13) |
				((p_param->initial_rc_qp & 0x3F) << 14) |
				(p_open_param->vbv_buffer_size << 20));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_WEIGHT_PARAM,
		      p_param->rc_weight_buf << 8 | p_param->rc_weight_param);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_MIN_MAX_QP, (p_param->min_qp_i << 0) |
			(p_param->max_qp_i << 6) |
			(p_param->hvs_max_delta_qp << 12));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_INTER_MIN_MAX_QP, (p_param->min_qp_p << 0) |
			(p_param->max_qp_p << 6) |
			(p_param->min_qp_b << 12) |
			(p_param->max_qp_b << 18));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_BIT_RATIO_LAYER_0_3,
		      ((uint32_t)p_param->fixed_bit_ratio[0] << 0) |
			((uint32_t)p_param->fixed_bit_ratio[1] << 8) |
			((uint32_t)p_param->fixed_bit_ratio[2] << 16) |
			((uint32_t)p_param->fixed_bit_ratio[3] << 24));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_RC_BIT_RATIO_LAYER_4_7,
		      ((uint32_t)p_param->fixed_bit_ratio[4] << 0) |
			((uint32_t)p_param->fixed_bit_ratio[5] << 8) |
			((uint32_t)p_param->fixed_bit_ratio[6] << 16) |
			((uint32_t)p_param->fixed_bit_ratio[7] << 24));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_ROT_PARAM, rot_mir_mode);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_BG_PARAM, (p_param->bg_detect_enable) |
			(p_param->bg_thr_diff << 1) |
			(p_param->bg_thr_mean_diff << 10) |
			(p_param->bg_lambda_qp << 18) |
			((p_param->bg_delta_qp & 0x1F) << 24) |
			(vpu_inst->std == W_AVC_ENC ? p_param->s2fme_disable << 29 : 0));

	if (vpu_inst->std == W_HEVC_ENC || vpu_inst->std == W_AVC_ENC) {
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_LAMBDA_ADDR,
			      p_param->custom_lambda_addr);

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CONF_WIN_TOP_BOT,
			      p_param->conf_win_bot << 16 | p_param->conf_win_top);
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CONF_WIN_LEFT_RIGHT,
			      p_param->conf_win_right << 16 | p_param->conf_win_left);

		if (vpu_inst->std == W_AVC_ENC)
			vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_INDEPENDENT_SLICE,
				      p_param->avc_slice_arg << 16 | p_param->avc_slice_mode);
		else
			vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_INDEPENDENT_SLICE,
				      p_param->independ_slice_mode_arg << 16 |
				 p_param->independ_slice_mode);

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_USER_SCALING_LIST_ADDR,
			      p_param->user_scaling_list_addr);

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_NUM_UNITS_IN_TICK,
			      p_param->num_units_in_tick);
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_TIME_SCALE, p_param->time_scale);
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_NUM_TICKS_POC_DIFF_ONE,
			      p_param->num_ticks_poc_diff_one);
	}

	if (vpu_inst->std == W_HEVC_ENC) {
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_MD_PU04,
			      (p_param->pu04_delta_rate & 0xFF) |
				((p_param->pu04_intra_planar_delta_rate & 0xFF) << 8) |
				((p_param->pu04_intra_dc_delta_rate & 0xFF) << 16) |
				((p_param->pu04_intra_angle_delta_rate & 0xFF) << 24));

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_MD_PU08,
			      (p_param->pu08_delta_rate & 0xFF) |
				((p_param->pu08_intra_planar_delta_rate & 0xFF) << 8) |
				((p_param->pu08_intra_dc_delta_rate & 0xFF) << 16) |
				((p_param->pu08_intra_angle_delta_rate & 0xFF) << 24));

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_MD_PU16,
			      (p_param->pu16_delta_rate & 0xFF) |
				((p_param->pu16_intra_planar_delta_rate & 0xFF) << 8) |
				((p_param->pu16_intra_dc_delta_rate & 0xFF) << 16) |
				((p_param->pu16_intra_angle_delta_rate & 0xFF) << 24));

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_MD_PU32,
			      (p_param->pu32_delta_rate & 0xFF) |
				((p_param->pu32_intra_planar_delta_rate & 0xFF) << 8) |
				((p_param->pu32_intra_dc_delta_rate & 0xFF) << 16) |
				((p_param->pu32_intra_angle_delta_rate & 0xFF) << 24));

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_MD_CU08,
			      (p_param->cu08_intra_delta_rate & 0xFF) |
				((p_param->cu08_inter_delta_rate & 0xFF) << 8) |
				((p_param->cu08_merge_delta_rate & 0xFF) << 16));

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_MD_CU16,
			      (p_param->cu16_intra_delta_rate & 0xFF) |
				((p_param->cu16_inter_delta_rate & 0xFF) << 8) |
				((p_param->cu16_merge_delta_rate & 0xFF) << 16));

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_CUSTOM_MD_CU32,
			      (p_param->cu32_intra_delta_rate & 0xFF) |
				((p_param->cu32_inter_delta_rate & 0xFF) << 8) |
				((p_param->cu32_merge_delta_rate & 0xFF) << 16));

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_DEPENDENT_SLICE,
			      p_param->depend_slice_mode_arg << 16 | p_param->depend_slice_mode);

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_NR_PARAM, (p_param->nr_y_enable << 0) |
				(p_param->nr_cb_enable << 1) |
				(p_param->nr_cr_enable << 2) |
				(p_param->nr_noise_est_enable << 3) |
				(p_param->nr_noise_sigma_y << 4) |
				(p_param->nr_noise_sigma_cb << 12) |
				(p_param->nr_noise_sigma_cr << 20));

		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_NR_WEIGHT,
			      (p_param->nr_intra_weight_y << 0) |
				(p_param->nr_intra_weight_cb << 5) |
				(p_param->nr_intra_weight_cr << 10) |
				(p_param->nr_inter_weight_y << 15) |
				(p_param->nr_inter_weight_cb << 20) |
				(p_param->nr_inter_weight_cr << 25));
	}
	if (p_enc_info->open_param.encode_vui_rbsp || p_enc_info->open_param.enc_hrd_rbsp_in_vps) {
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_VUI_HRD_PARAM,
			      (p_enc_info->open_param.hrd_rbsp_data_size << 18) |
				(p_enc_info->open_param.vui_rbsp_data_size << 4) |
				(p_enc_info->open_param.enc_hrd_rbsp_in_vps << 2) |
				(p_enc_info->open_param.encode_vui_rbsp));
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_VUI_RBSP_ADDR,
			      p_enc_info->open_param.vui_rbsp_data_addr);
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_HRD_RBSP_ADDR,
			      p_enc_info->open_param.hrd_rbsp_data_addr);
	} else {
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_SEQ_VUI_HRD_PARAM, 0);
	}

	wave5_bit_issue_command(vpu_inst, W5_ENC_SET_PARAM);

	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(vpu_inst->dev->dev, "enc set param timed out\n");
		return ret;
	}

	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS)) {
		reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
		wave5_print_reg_err(vpu_inst->dev, reg_val);
		return -EIO;
	}

	return 0;
}

int wave5_vpu_enc_get_seq_info(struct vpu_instance *vpu_inst, struct enc_initial_info *info)
{
	int ret;
	u32 reg_val;
	struct enc_info *p_enc_info = &vpu_inst->codec_info->enc_info;

	if (vpu_inst->dev->product != PRODUCT_ID_521)
		return -EINVAL;

	// send QUERY cmd
	ret = wave5_send_query(vpu_inst, GET_RESULT);
	if (ret) {
		if (ret == -EIO) {
			reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
			wave5_print_reg_err(vpu_inst->dev, reg_val);
		}
		return ret;
	}

	dev_dbg(vpu_inst->dev->dev, "init seq\n");

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_QUEUE_STATUS);

	p_enc_info->instance_queue_count = (reg_val >> 16) & 0xff;
	p_enc_info->report_queue_count = (reg_val & 0xffff);

	if (vpu_read_reg(vpu_inst->dev, W5_RET_ENC_ENCODING_SUCCESS) != 1) {
		info->seq_init_err_reason = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_ERR_INFO);
		ret = -EIO;
	} else {
		info->warn_info = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_WARN_INFO);
	}

	info->min_frame_buffer_count = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_NUM_REQUIRED_FB);
	info->min_src_frame_count = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_MIN_SRC_BUF_NUM);
	info->max_latency_pictures = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_MAX_LATENCY_PICS);
	info->vlc_buf_size = vpu_read_reg(vpu_inst->dev, W5_RET_VLC_BUF_SIZE);
	info->param_buf_size = vpu_read_reg(vpu_inst->dev, W5_RET_PARAM_BUF_SIZE);
	p_enc_info->vlc_buf_size = info->vlc_buf_size;
	p_enc_info->param_buf_size = info->param_buf_size;

	return ret;
}

int wave5_vpu_enc_register_framebuffer(struct device *dev, struct vpu_instance *vpu_inst,
				       struct frame_buffer *fb_arr, enum tiled_map_type map_type,
				       uint32_t count)
{
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	int ret = 0;
	u32 stride;
	u32 start_no, end_no;
	size_t remain, idx, j, i, cnt_8_chunk;
	u32 reg_val = 0, pic_size = 0, mv_col_size, fbc_y_tbl_size, fbc_c_tbl_size;
	u32	sub_sampled_size = 0;
	u32 endian, nv21 = 0, cbcr_interleave = 0, luma_stride, chroma_stride;
	u32	buf_height = 0, buf_width = 0;
	struct vpu_buf vb_mv = {0,};
	struct vpu_buf vb_fbc_y_tbl = {0,};
	struct vpu_buf vb_fbc_c_tbl = {0,};
	struct vpu_buf vb_sub_sam_buf = {0,};
	struct vpu_buf vb_task = {0,};
	struct enc_open_param *p_open_param;
	struct enc_info *p_enc_info = &vpu_inst->codec_info->enc_info;

	p_open_param = &p_enc_info->open_param;
	mv_col_size = 0;
	fbc_y_tbl_size = 0;
	fbc_c_tbl_size = 0;
	stride = p_enc_info->stride;

	if (vpu_inst->std == W_AVC_ENC) {
		buf_width = ALIGN(p_open_param->pic_width, 16);
		buf_height = ALIGN(p_open_param->pic_height, 16);

		if ((p_enc_info->rotation_angle || p_enc_info->mirror_direction) &&
		    !(p_enc_info->rotation_angle == 180 &&
					p_enc_info->mirror_direction == MIRDIR_HOR_VER)) {
			buf_width = ALIGN(p_open_param->pic_width, 16);
			buf_height = ALIGN(p_open_param->pic_height, 16);
		}

		if (p_enc_info->rotation_angle == 90 || p_enc_info->rotation_angle == 270) {
			buf_width = ALIGN(p_open_param->pic_height, 16);
			buf_height = ALIGN(p_open_param->pic_width, 16);
		}
	} else {
		buf_width = ALIGN(p_open_param->pic_width, 8);
		buf_height = ALIGN(p_open_param->pic_height, 8);

		if ((p_enc_info->rotation_angle || p_enc_info->mirror_direction) &&
		    !(p_enc_info->rotation_angle == 180 &&
					p_enc_info->mirror_direction == MIRDIR_HOR_VER)) {
			buf_width = ALIGN(p_open_param->pic_width, 32);
			buf_height = ALIGN(p_open_param->pic_height, 32);
		}

		if (p_enc_info->rotation_angle == 90 || p_enc_info->rotation_angle == 270) {
			buf_width = ALIGN(p_open_param->pic_height, 32);
			buf_height = ALIGN(p_open_param->pic_width, 32);
		}
	}

	pic_size = (buf_width << 16) | buf_height;

	if (vpu_inst->std == W_HEVC_ENC) {
		mv_col_size = WAVE5_ENC_HEVC_BUF_SIZE(buf_width, buf_height);
		mv_col_size = ALIGN(mv_col_size, 16);
		vb_mv.daddr = 0;
		/* 4096 is a margin */
		vb_mv.size = ALIGN(mv_col_size * count, 4096) + 4096;
	} else if (vpu_inst->std == W_AVC_ENC) {
		mv_col_size = WAVE5_ENC_AVC_BUF_SIZE(buf_width, buf_height);
		vb_mv.daddr = 0;
		/* 4096 is a margin */
		vb_mv.size = ALIGN(mv_col_size * count, 4096) + 4096;
	}

	ret = wave5_vdi_allocate_dma_memory(vpu_dev, &vb_mv);
	if (ret)
		return ret;

	p_enc_info->vb_mv = vb_mv;

	if (p_enc_info->product_code == WAVE521C_DUAL_CODE) {
		u32 bgs_width, ot_bg_width, comp_frm_width, ot_frm_width, ot_bg_height,
		 bgs_height, comp_frm_height, ot_frm_height;
		u32 frm_width, frm_height;
		u32 dual_width = buf_width;
		u32 dual_height = buf_height;

		bgs_width = (p_open_param->wave_param.internal_bit_depth > 8 ? 256 : 512);

		if (vpu_inst->std == W_AVC_ENC)
			ot_bg_width = 1024;
		else // if (vpu_inst->std == W_HEVC_ENC)
			ot_bg_width = 512;

		frm_width = ALIGN(dual_width, 16);
		frm_height = ALIGN(dual_height, 16);
		// valid_width = align(width, 16), comp_frm_width = align(valid_width+pad_x, 16)
		comp_frm_width = ALIGN(ALIGN(frm_width, 16) + 16, 16);
		// 1024 = offset table BG width
		ot_frm_width = ALIGN(comp_frm_width, ot_bg_width);

		// sizeof_offset_table()
		ot_bg_height = 32;
		bgs_height = BIT(14) / bgs_width;
		if (p_open_param->wave_param.internal_bit_depth > 8)
			bgs_height /= 2;
		comp_frm_height = ALIGN(ALIGN(frm_height, 4) + 4, bgs_height);
		ot_frm_height = ALIGN(comp_frm_height, ot_bg_height);
		fbc_y_tbl_size = (ot_frm_width / 16) * (ot_frm_height / 4) * 2;
	} else {
		fbc_y_tbl_size = WAVE5_FBC_LUMA_TABLE_SIZE(buf_width, buf_height);
		fbc_y_tbl_size = ALIGN(fbc_y_tbl_size, 16);
	}

	vb_fbc_y_tbl.daddr = 0;
	vb_fbc_y_tbl.size = ALIGN(fbc_y_tbl_size * count, 4096) + 4096;
	ret = wave5_vdi_allocate_dma_memory(vpu_dev, &vb_fbc_y_tbl);
	if (ret)
		return ret;

	p_enc_info->vb_fbc_y_tbl = vb_fbc_y_tbl;

	if (p_enc_info->product_code == WAVE521C_DUAL_CODE) {
		u32 bgs_width, ot_bg_width, comp_frm_width, ot_frm_width, ot_bg_height,
		 bgs_height, comp_frm_height, ot_frm_height;
		u32 frm_width, frm_height;
		u32 dual_width = buf_width;
		u32 dual_height = buf_height;

		bgs_width = (p_open_param->wave_param.internal_bit_depth > 8 ? 256 : 512);

		if (vpu_inst->std == W_AVC_ENC)
			ot_bg_width = 1024;
		else // if (vpu_inst->std == W_HEVC_ENC)
			ot_bg_width = 512;

		frm_width = ALIGN(dual_width, 16);
		frm_height = ALIGN(dual_height, 16);
		// valid_width = align(width, 16), comp_frm_width = align(valid_width+pad_x, 16)
		comp_frm_width = ALIGN(ALIGN(frm_width / 2, 16) + 16, 16);
		// 1024 = offset table BG width
		ot_frm_width = ALIGN(comp_frm_width, ot_bg_width);

		// sizeof_offset_table()
		ot_bg_height = 32;
		bgs_height = BIT(14) / bgs_width;
		if (p_open_param->wave_param.internal_bit_depth > 8)
			bgs_height /= 2;

		comp_frm_height = ALIGN(ALIGN(frm_height, 4) + 4, bgs_height);
		ot_frm_height = ALIGN(comp_frm_height, ot_bg_height);
		fbc_c_tbl_size = (ot_frm_width / 16) * (ot_frm_height / 4) * 2;
	} else {
		fbc_c_tbl_size = WAVE5_FBC_CHROMA_TABLE_SIZE(buf_width, buf_height);
		fbc_c_tbl_size = ALIGN(fbc_c_tbl_size, 16);
	}

	vb_fbc_c_tbl.daddr = 0;
	vb_fbc_c_tbl.size = ALIGN(fbc_c_tbl_size * count, 4096) + 4096;
	ret = wave5_vdi_allocate_dma_memory(vpu_dev, &vb_fbc_c_tbl);
	if (ret)
		return ret;

	p_enc_info->vb_fbc_c_tbl = vb_fbc_c_tbl;

	if (vpu_inst->std == W_AVC_ENC)
		sub_sampled_size = WAVE5_SUBSAMPLED_ONE_SIZE_AVC(buf_width, buf_height);
	else
		sub_sampled_size = WAVE5_SUBSAMPLED_ONE_SIZE(buf_width, buf_height);
	vb_sub_sam_buf.size = ALIGN(sub_sampled_size * count, 4096) + 4096;
	vb_sub_sam_buf.daddr = 0;
	ret = wave5_vdi_allocate_dma_memory(vpu_dev, &vb_sub_sam_buf);
	if (ret)
		return ret;

	p_enc_info->vb_sub_sam_buf = vb_sub_sam_buf;

	vb_task.size = (p_enc_info->vlc_buf_size * VLC_BUF_NUM) +
			(p_enc_info->param_buf_size * COMMAND_QUEUE_DEPTH);
	vb_task.daddr = 0;
	if (p_enc_info->vb_task.size == 0) {
		ret = wave5_vdi_allocate_dma_memory(vpu_dev, &vb_task);
		if (ret)
			return ret;

		p_enc_info->vb_task = vb_task;

		vpu_write_reg(vpu_inst->dev, W5_CMD_SET_FB_ADDR_TASK_BUF,
			      p_enc_info->vb_task.daddr);
		vpu_write_reg(vpu_inst->dev, W5_CMD_SET_FB_TASK_BUF_SIZE, vb_task.size);
	}

	// set sub-sampled buffer base addr
	vpu_write_reg(vpu_inst->dev, W5_ADDR_SUB_SAMPLED_FB_BASE, vb_sub_sam_buf.daddr);
	// set sub-sampled buffer size for one frame
	vpu_write_reg(vpu_inst->dev, W5_SUB_SAMPLED_ONE_FB_SIZE, sub_sampled_size);

	endian = wave5_vdi_convert_endian(vpu_dev, fb_arr[0].endian);

	vpu_write_reg(vpu_inst->dev, W5_PIC_SIZE, pic_size);

	// set stride of luma/chroma for compressed buffer
	if ((p_enc_info->rotation_angle || p_enc_info->mirror_direction) &&
	    !(p_enc_info->rotation_angle == 180 &&
				p_enc_info->mirror_direction == MIRDIR_HOR_VER)) {
		luma_stride = ALIGN(buf_width, 16) *
			(p_open_param->wave_param.internal_bit_depth > 8 ? 5 : 4);
		luma_stride = ALIGN(luma_stride, 32);
		chroma_stride = ALIGN(buf_width / 2, 16) *
			(p_open_param->wave_param.internal_bit_depth > 8 ? 5 : 4);
		chroma_stride = ALIGN(chroma_stride, 32);
	} else {
		luma_stride = ALIGN(p_open_param->pic_width, 16) *
			(p_open_param->wave_param.internal_bit_depth > 8 ? 5 : 4);
		luma_stride = ALIGN(luma_stride, 32);
		chroma_stride = ALIGN(p_open_param->pic_width / 2, 16) *
			(p_open_param->wave_param.internal_bit_depth > 8 ? 5 : 4);
		chroma_stride = ALIGN(chroma_stride, 32);
	}

	vpu_write_reg(vpu_inst->dev, W5_FBC_STRIDE, luma_stride << 16 | chroma_stride);

	cbcr_interleave = p_open_param->cbcr_interleave;
	reg_val = (nv21 << 29) |
		(cbcr_interleave << 16) |
		(stride);

	vpu_write_reg(vpu_inst->dev, W5_COMMON_PIC_INFO, reg_val);

	remain = count;
	cnt_8_chunk = (count + 7) / 8;
	idx = 0;
	for (j = 0; j < cnt_8_chunk; j++) {
		reg_val = (endian << 16) | (j == cnt_8_chunk - 1) << 4 | ((j == 0) << 3);
		reg_val |= (p_open_param->enable_non_ref_fbc_write << 26);
		vpu_write_reg(vpu_inst->dev, W5_SFB_OPTION, reg_val);
		start_no = j * 8;
		end_no = start_no + (remain >= 8 ? 8 : remain) - 1;

		vpu_write_reg(vpu_inst->dev, W5_SET_FB_NUM, (start_no << 8) | end_no);

		for (i = 0; i < 8 && i < remain; i++) {
			vpu_write_reg(vpu_inst->dev, W5_ADDR_LUMA_BASE0 + (i << 4), fb_arr[i +
					start_no].buf_y);
			vpu_write_reg(vpu_inst->dev, W5_ADDR_CB_BASE0 + (i << 4),
				      fb_arr[i + start_no].buf_cb);
			/* luma FBC offset table */
			vpu_write_reg(vpu_inst->dev, W5_ADDR_FBC_Y_OFFSET0 + (i << 4),
				      vb_fbc_y_tbl.daddr + idx * fbc_y_tbl_size);
			/* chroma FBC offset table */
			vpu_write_reg(vpu_inst->dev, W5_ADDR_FBC_C_OFFSET0 + (i << 4),
				      vb_fbc_c_tbl.daddr + idx * fbc_c_tbl_size);

			vpu_write_reg(vpu_inst->dev, W5_ADDR_MV_COL0 + (i << 2),
				      vb_mv.daddr + idx * mv_col_size);
			idx++;
		}
		remain -= i;

		wave5_bit_issue_command(vpu_inst, W5_SET_FB);
		ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
		if (ret)
			return ret;
	}

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS);
	if (!reg_val)
		return -EIO;

	return ret;
}

int wave5_vpu_encode(struct vpu_instance *vpu_inst, struct enc_param *option, u32 *fail_res)
{
	s32 src_frame_format;
	u32 reg_val = 0, bs_endian;
	u32 src_stride_c = 0;
	struct enc_info *p_enc_info = &vpu_inst->codec_info->enc_info;
	struct frame_buffer *p_src_frame = option->source_frame;
	struct enc_open_param *p_open_param = &p_enc_info->open_param;
	bool justified = WTL_RIGHT_JUSTIFIED;
	u32 format_no = WTL_PIXEL_8BIT;
	int ret;

	if (vpu_inst->dev->product != PRODUCT_ID_521)
		return -EINVAL;

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_BS_START_ADDR, option->pic_stream_buffer_addr);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_BS_SIZE, option->pic_stream_buffer_size);
	p_enc_info->stream_buf_start_addr = option->pic_stream_buffer_addr;
	p_enc_info->stream_buf_size = option->pic_stream_buffer_size;
	p_enc_info->stream_buf_end_addr =
		option->pic_stream_buffer_addr + option->pic_stream_buffer_size;

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_AXI_SEL, DEFAULT_SRC_AXI);
	/* secondary AXI */
	reg_val = (p_enc_info->sec_axi_info.wave.use_enc_rdo_enable << 11) |
		(p_enc_info->sec_axi_info.wave.use_enc_lf_enable << 15);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_USE_SEC_AXI, reg_val);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_REPORT_PARAM, 0);

	/*
	 * CODEOPT_ENC_VCL is used to implicitly encode a header(headers) for
	 * generating bitstream. (to encode a header
	 * only, use ENC_PUT_VIDEO_HEADER for
	 * give_command)
	 */

	if (option->code_option.implicit_header_encode == 1)
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_CODE_OPTION,
			      CODEOPT_ENC_HEADER_IMPLICIT | CODEOPT_ENC_VCL |
			      (option->code_option.encode_aud << 5) |
			      (option->code_option.encode_eos << 6) |
			      (option->code_option.encode_eob << 7));
	else
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_CODE_OPTION,
			      (option->code_option.implicit_header_encode << 0) |
				(option->code_option.encode_vcl << 1) |
				(option->code_option.encode_vps << 2) |
				(option->code_option.encode_sps << 3) |
				(option->code_option.encode_pps << 4) |
				(option->code_option.encode_aud << 5) |
				(option->code_option.encode_eos << 6) |
				(option->code_option.encode_eob << 7));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_PIC_PARAM, (option->skip_picture << 0) |
			(option->force_pic_qp_enable << 1) |
			(option->force_pic_qp_i << 2) |
			(option->force_pic_qp_p << 8) |
			(option->force_pic_qp_b << 14) |
			(option->force_pic_type_enable << 20) |
			(option->force_pic_type << 21) |
			(option->force_all_ctu_coef_drop_enable << 24));

	if (option->src_end_flag == 1)
		// no more source image.
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_PIC_IDX, 0xFFFFFFFF);
	else
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_PIC_IDX, option->src_idx);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_ADDR_Y, p_src_frame->buf_y);
	if (p_open_param->cbcr_order == CBCR_ORDER_NORMAL) {
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_ADDR_U, p_src_frame->buf_cb);
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_ADDR_V, p_src_frame->buf_cr);
	} else {
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_ADDR_U, p_src_frame->buf_cr);
		vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_ADDR_V, p_src_frame->buf_cb);
	}

	switch (p_open_param->src_format) {
	case FORMAT_420:
	case FORMAT_422:
	case FORMAT_YUYV:
	case FORMAT_YVYU:
	case FORMAT_UYVY:
	case FORMAT_VYUY:
		justified = WTL_LEFT_JUSTIFIED;
		format_no = WTL_PIXEL_8BIT;
		src_stride_c = (p_src_frame->cbcr_interleave == 1) ? p_src_frame->stride :
			(p_src_frame->stride / 2);
		src_stride_c = (p_open_param->src_format == FORMAT_422) ? src_stride_c * 2 :
			src_stride_c;
		break;
	case FORMAT_420_P10_16BIT_MSB:
	case FORMAT_422_P10_16BIT_MSB:
	case FORMAT_YUYV_P10_16BIT_MSB:
	case FORMAT_YVYU_P10_16BIT_MSB:
	case FORMAT_UYVY_P10_16BIT_MSB:
	case FORMAT_VYUY_P10_16BIT_MSB:
		justified = WTL_RIGHT_JUSTIFIED;
		format_no = WTL_PIXEL_16BIT;
		src_stride_c = (p_src_frame->cbcr_interleave == 1) ? p_src_frame->stride :
			(p_src_frame->stride / 2);
		src_stride_c = (p_open_param->src_format ==
				FORMAT_422_P10_16BIT_MSB) ? src_stride_c * 2 : src_stride_c;
		break;
	case FORMAT_420_P10_16BIT_LSB:
	case FORMAT_422_P10_16BIT_LSB:
	case FORMAT_YUYV_P10_16BIT_LSB:
	case FORMAT_YVYU_P10_16BIT_LSB:
	case FORMAT_UYVY_P10_16BIT_LSB:
	case FORMAT_VYUY_P10_16BIT_LSB:
		justified = WTL_LEFT_JUSTIFIED;
		format_no = WTL_PIXEL_16BIT;
		src_stride_c = (p_src_frame->cbcr_interleave == 1) ? p_src_frame->stride :
			(p_src_frame->stride / 2);
		src_stride_c = (p_open_param->src_format ==
				FORMAT_422_P10_16BIT_LSB) ? src_stride_c * 2 : src_stride_c;
		break;
	case FORMAT_420_P10_32BIT_MSB:
	case FORMAT_422_P10_32BIT_MSB:
	case FORMAT_YUYV_P10_32BIT_MSB:
	case FORMAT_YVYU_P10_32BIT_MSB:
	case FORMAT_UYVY_P10_32BIT_MSB:
	case FORMAT_VYUY_P10_32BIT_MSB:
		justified = WTL_RIGHT_JUSTIFIED;
		format_no = WTL_PIXEL_32BIT;
		src_stride_c = (p_src_frame->cbcr_interleave == 1) ? p_src_frame->stride :
			ALIGN(p_src_frame->stride / 2, 16) * BIT(p_src_frame->cbcr_interleave);
		src_stride_c = (p_open_param->src_format ==
				FORMAT_422_P10_32BIT_MSB) ? src_stride_c * 2 : src_stride_c;
		break;
	case FORMAT_420_P10_32BIT_LSB:
	case FORMAT_422_P10_32BIT_LSB:
	case FORMAT_YUYV_P10_32BIT_LSB:
	case FORMAT_YVYU_P10_32BIT_LSB:
	case FORMAT_UYVY_P10_32BIT_LSB:
	case FORMAT_VYUY_P10_32BIT_LSB:
		justified = WTL_LEFT_JUSTIFIED;
		format_no = WTL_PIXEL_32BIT;
		src_stride_c = (p_src_frame->cbcr_interleave == 1) ? p_src_frame->stride :
			ALIGN(p_src_frame->stride / 2, 16) * BIT(p_src_frame->cbcr_interleave);
		src_stride_c = (p_open_param->src_format ==
				FORMAT_422_P10_32BIT_LSB) ? src_stride_c * 2 : src_stride_c;
		break;
	default:
		return -EINVAL;
	}

	src_frame_format = (p_open_param->cbcr_interleave << 1) | (p_open_param->nv21);
	switch (p_open_param->packed_format) {
	case PACKED_YUYV:
		src_frame_format = 4; break;
	case PACKED_YVYU:
		src_frame_format = 5; break;
	case PACKED_UYVY:
		src_frame_format = 6; break;
	case PACKED_VYUY:
		src_frame_format = 7; break;
	default:
		break;
	}

	reg_val = wave5_vdi_convert_endian(vpu_inst->dev, p_open_param->source_endian);
	bs_endian = (~reg_val & VDI_128BIT_ENDIAN_MASK);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_STRIDE,
		      (p_src_frame->stride << 16) | src_stride_c);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SRC_FORMAT, (src_frame_format << 0) |
			(format_no << 3) |
			(justified << 5) |
			(bs_endian << 6));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_CUSTOM_MAP_OPTION_ADDR,
		      option->custom_map_opt.addr_custom_map);

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_CUSTOM_MAP_OPTION_PARAM,
		      (option->custom_map_opt.custom_roi_map_enable << 0) |
			(option->custom_map_opt.roi_avg_qp << 1) |
			(option->custom_map_opt.custom_lambda_map_enable << 8) |
			(option->custom_map_opt.custom_mode_map_enable << 9) |
			(option->custom_map_opt.custom_coef_drop_enable << 10));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_LONGTERM_PIC,
		      (option->use_cur_src_as_longterm_pic << 0) | (option->use_longterm_ref << 1));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_WP_PIXEL_SIGMA_Y, option->wp_pix_sigma_y);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_WP_PIXEL_SIGMA_C,
		      (option->wp_pix_sigma_cr << 16) | option->wp_pix_sigma_cb);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_WP_PIXEL_MEAN_Y, option->wp_pix_mean_y);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_WP_PIXEL_MEAN_C,
		      (option->wp_pix_mean_cr << 16) | (option->wp_pix_mean_cb));

	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_PREFIX_SEI_INFO, 0);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_PREFIX_SEI_NAL_ADDR, 0);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SUFFIX_SEI_INFO, 0);
	vpu_write_reg(vpu_inst->dev, W5_CMD_ENC_PIC_SUFFIX_SEI_NAL_ADDR, 0);

	wave5_bit_issue_command(vpu_inst, W5_ENC_PIC);

	// check QUEUE_DONE
	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(vpu_inst->dev->dev, "enc pic timed out\n");
		return -ETIMEDOUT;
	}

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_QUEUE_STATUS);

	p_enc_info->instance_queue_count = (reg_val >> 16) & 0xff;
	p_enc_info->report_queue_count = (reg_val & 0xffff);

	// FAILED for adding a command into VCPU QUEUE
	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS)) {
		*fail_res = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
		wave5_print_reg_err(vpu_inst->dev, *fail_res);
		return -EIO;
	}

	return 0;
}

int wave5_vpu_enc_get_result(struct vpu_instance *vpu_inst, struct enc_output_info *result)
{
	int ret;
	u32 encoding_success;
	u32 reg_val;
	struct enc_info *p_enc_info = &vpu_inst->codec_info->enc_info;
	struct vpu_device *vpu_dev = vpu_inst->dev;

	if (vpu_dev->product != PRODUCT_ID_521)
		return -EINVAL;

	ret = wave5_send_query(vpu_inst, GET_RESULT);
	if (ret) {
		if (ret == -EIO) {
			reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
			wave5_print_reg_err(vpu_inst->dev, reg_val);
		}
		return ret;
	}
	dev_dbg(vpu_inst->dev->dev, "enc pic complete\n");

	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_QUEUE_STATUS);

	p_enc_info->instance_queue_count = (reg_val >> 16) & 0xff;
	p_enc_info->report_queue_count = (reg_val & 0xffff);

	encoding_success = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_ENCODING_SUCCESS);
	if (!encoding_success) {
		result->error_reason = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_ERR_INFO);
		return -EIO;
	}

	result->warn_info = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_WARN_INFO);

	result->enc_pic_cnt = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_NUM);
	reg_val = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_TYPE);
	result->pic_type = reg_val & 0xFFFF;

	result->enc_vcl_nut = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_VCL_NUT);
	result->recon_frame_index = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_IDX);

	if (result->recon_frame_index >= 0)
		result->recon_frame = vpu_inst->frame_buf[result->recon_frame_index];

	result->num_of_slices = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_SLICE_NUM);
	result->pic_skipped = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_SKIP);
	result->num_of_intra = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_NUM_INTRA);
	result->num_of_merge = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_NUM_MERGE);
	result->num_of_skip_block = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_NUM_SKIP);
	result->bitstream_wrap_around = 0; // only support line-buffer mode.

	result->avg_ctu_qp = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_AVG_CTU_QP);
	result->enc_pic_byte = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_BYTE);

	result->enc_gop_pic_idx = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_GOP_PIC_IDX);
	result->enc_pic_poc = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_POC);
	result->enc_src_idx = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_USED_SRC_IDX);
	result->release_src_flag = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_SRC_BUF_FLAG);
	p_enc_info->stream_wr_ptr = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_WR_PTR);
	p_enc_info->stream_rd_ptr = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_RD_PTR);

	result->pic_distortion_low = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_DIST_LOW);
	result->pic_distortion_high = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PIC_DIST_HIGH);

	result->bitstream_buffer = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_RD_PTR);
	result->rd_ptr = p_enc_info->stream_rd_ptr;
	result->wr_ptr = p_enc_info->stream_wr_ptr;

	//result for header only(no vcl) encoding
	if (result->recon_frame_index == RECON_IDX_FLAG_HEADER_ONLY)
		result->bitstream_size = result->enc_pic_byte;
	else if (result->recon_frame_index < 0)
		result->bitstream_size = 0;
	else
		result->bitstream_size = result->enc_pic_byte;

	result->enc_host_cmd_tick = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_HOST_CMD_TICK);
	result->enc_prepare_start_tick = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PREPARE_START_TICK);
	result->enc_prepare_end_tick = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_PREPARE_END_TICK);
	result->enc_processing_start_tick = vpu_read_reg(vpu_inst->dev,
							 W5_RET_ENC_PROCESSING_START_TICK);
	result->enc_processing_end_tick = vpu_read_reg(vpu_inst->dev,
						       W5_RET_ENC_PROCESSING_END_TICK);
	result->enc_encode_start_tick = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_ENCODING_START_TICK);
	result->enc_encode_end_tick = vpu_read_reg(vpu_inst->dev, W5_RET_ENC_ENCODING_END_TICK);

	if (!p_enc_info->first_cycle_check) {
		result->frame_cycle = (result->enc_encode_end_tick - result->enc_host_cmd_tick) *
			p_enc_info->cycle_per_tick;
		p_enc_info->first_cycle_check = true;
	} else {
		result->frame_cycle =
			(result->enc_encode_end_tick - vpu_dev->last_performance_cycles) *
			p_enc_info->cycle_per_tick;
		if (vpu_dev->last_performance_cycles < result->enc_host_cmd_tick)
			result->frame_cycle = (result->enc_encode_end_tick -
					result->enc_host_cmd_tick) * p_enc_info->cycle_per_tick;
	}
	vpu_dev->last_performance_cycles = result->enc_encode_end_tick;
	result->prepare_cycle = (result->enc_prepare_end_tick -
			result->enc_prepare_start_tick) * p_enc_info->cycle_per_tick;
	result->processing = (result->enc_processing_end_tick -
			result->enc_processing_start_tick) * p_enc_info->cycle_per_tick;
	result->encoded_cycle = (result->enc_encode_end_tick -
			result->enc_encode_start_tick) * p_enc_info->cycle_per_tick;

	return 0;
}

int wave5_vpu_enc_fini_seq(struct vpu_instance *vpu_inst, u32 *fail_res)
{
	int ret;

	if (vpu_inst->dev->product != PRODUCT_ID_521)
		return -EINVAL;

	wave5_bit_issue_command(vpu_inst, W5_DESTROY_INSTANCE);
	ret = wave5_wait_vpu_busy(vpu_inst->dev, W5_VPU_BUSY_STATUS);
	if (ret)
		return -ETIMEDOUT;

	if (!vpu_read_reg(vpu_inst->dev, W5_RET_SUCCESS)) {
		*fail_res = vpu_read_reg(vpu_inst->dev, W5_RET_FAIL_REASON);
		wave5_print_reg_err(vpu_inst->dev, *fail_res);
		return -EIO;
	}
	return 0;
}

static int wave5_vpu_enc_check_common_param_valid(struct vpu_instance *vpu_inst,
						  struct enc_open_param *pop)
{
	int i = 0;
	bool low_delay = true;
	struct enc_wave_param *param = &pop->wave_param;
	struct vpu_device *vpu_dev = vpu_inst->dev;
	struct device *dev = vpu_dev->dev;
	s32 num_ctu_row = (pop->pic_height + 64 - 1) / 64;
	s32 num_ctu_col = (pop->pic_width + 64 - 1) / 64;
	s32 ctu_sz = num_ctu_col * num_ctu_row;

	// check low-delay gop structure
	if (param->gop_preset_idx == PRESET_IDX_CUSTOM_GOP) { /* common gop */
		if (param->gop_param.custom_gop_size > 1) {
			s32 min_val = param->gop_param.pic_param[0].poc_offset;

			for (i = 1; i < param->gop_param.custom_gop_size; i++) {
				if (min_val > param->gop_param.pic_param[i].poc_offset) {
					low_delay = false;
					break;
				}
				min_val = param->gop_param.pic_param[i].poc_offset;
			}
		}
	} else if (param->gop_preset_idx == PRESET_IDX_ALL_I ||
			param->gop_preset_idx == PRESET_IDX_IPP ||
			param->gop_preset_idx == PRESET_IDX_IBBB ||
			param->gop_preset_idx == PRESET_IDX_IPPPP ||
			param->gop_preset_idx == PRESET_IDX_IBBBB ||
			// low-delay case (IPPP, IBBB)
			param->gop_preset_idx == PRESET_IDX_IPP_SINGLE) {
	}

	if (vpu_inst->std == W_HEVC_ENC && low_delay && param->decoding_refresh_type == 1) {
		dev_warn(dev, "WARN : dec_refresh_type (CRA) is supported if low delay GOP.\n");
		dev_warn(dev, "RECOMMEND CONFIG PARAMETER : decoding refresh type = IDR\n");
		param->decoding_refresh_type = 2;
	}

	if (param->gop_preset_idx == PRESET_IDX_CUSTOM_GOP) {
		for (i = 0; i < param->gop_param.custom_gop_size; i++) {
			if (param->gop_param.pic_param[i].temporal_id >= MAX_NUM_TEMPORAL_LAYER) {
				dev_err(dev, "temporal_id %d exceeds MAX_NUM_TEMPORAL_LAYER\n",
					param->gop_param.pic_param[i].temporal_id);
				return -EINVAL;
			}

			if (param->gop_param.pic_param[i].temporal_id < 0) {
				dev_err(dev, "must be %d-th temporal_id >= 0\n",
					param->gop_param.pic_param[i].temporal_id);
				return -EINVAL;
			}
		}
	}

	if (param->wpp_enable && param->independ_slice_mode) {
		int num_ctb_in_width = ALIGN(pop->pic_width, 64) >> 6;

		if (param->independ_slice_mode_arg % num_ctb_in_width) {
			dev_err(dev, "inde_slice_arg not multiple of num_ctb_in_width\n");
			return -EINVAL;
		}
	}

	// multi-slice & wpp
	if (param->wpp_enable && param->depend_slice_mode) {
		dev_err(dev, "param->wpp_enable == 1 && param->depend_slice_mode\n");
		return -EINVAL;
	}

	if (!param->independ_slice_mode && param->depend_slice_mode) {
		dev_err(dev, "independ_slice_mode && param->depend_slice_mode\n");
		return -EINVAL;
	} else if (param->independ_slice_mode && param->depend_slice_mode == 1 &&
		   param->independ_slice_mode_arg < param->depend_slice_mode_arg) {
		dev_err(dev, "independ_slice_mode_arg < depend_slice_mode_arg\n");
		return -EINVAL;
	}

	if (param->independ_slice_mode && param->independ_slice_mode_arg > 65535) {
		dev_err(dev, "param->independ_slice_mode_arg > 65535\n");
		return -EINVAL;
	}

	if (param->depend_slice_mode && param->depend_slice_mode_arg > 65535) {
		dev_err(dev, "param->depend_slice_mode_arg > 65535\n");
		return -EINVAL;
	}

	if (param->conf_win_top % 2) {
		dev_err(dev, "conf_win_top: %d not multiple of 2.\n", param->conf_win_top);
		return -EINVAL;
	}

	if (param->conf_win_bot % 2) {
		dev_err(dev, "conf_win_bot: %d not multiple of 2.\n", param->conf_win_bot);
		return -EINVAL;
	}

	if (param->conf_win_left % 2) {
		dev_err(dev, "conf_win_left: %d not multiple of 2.\n", param->conf_win_left);
		return -EINVAL;
	}

	if (param->conf_win_right % 2) {
		dev_err(dev, "conf_win_right : %d. not multiple of 2.\n", param->conf_win_right);
		return -EINVAL;
	}

	if (param->lossless_enable && (param->nr_y_enable || param->nr_cb_enable ||
				       param->nr_cr_enable)) {
		dev_err(dev, "CFG FAIL : lossless_coding and noise_reduction");
		dev_err(dev, "(en_nr_y, en_nr_cb, and en_nr_cr) cannot be used simultaneously.\n");
		return -EINVAL;
	}

	if (param->lossless_enable && param->bg_detect_enable) {
		dev_err(dev, "lossless_coding and bg_detect cannot be used simultaneously.\n");
		return -EINVAL;
	}

	if (param->lossless_enable && pop->rc_enable) {
		dev_err(dev, "ossless_coding and rate_control cannot be used simultaneously.\n");
		return -EINVAL;
	}

	if (param->lossless_enable && param->roi_enable) {
		dev_err(dev, "CFG FAIL : lossless_coding and roi cannot be used simultaneously.\n");
		return -EINVAL;
	}

	if (param->lossless_enable && !param->skip_intra_trans) {
		dev_err(dev, "intra_trans_skip must be enabled when lossless_coding is enabled.\n");
		return -EINVAL;
	}

	// intra refresh
	if (param->intra_refresh_mode && param->intra_refresh_arg <= 0) {
		dev_err(dev, "mode %d, refresh %d wxh = %dx%d\n", param->intra_refresh_mode,
			param->intra_refresh_arg, num_ctu_row, num_ctu_col);
		return -EINVAL;
	}
	if (param->intra_refresh_mode == 1 && param->intra_refresh_arg > num_ctu_row) {
		dev_err(dev, "mode %d, refresh %d wxh = %dx%d\n", param->intra_refresh_mode,
			param->intra_refresh_arg, num_ctu_row, num_ctu_col);
		return -EINVAL;
	}
	if (param->intra_refresh_mode == 2 && param->intra_refresh_arg > num_ctu_col) {
		dev_err(dev, "mode %d, refresh %d wxh = %dx%d\n", param->intra_refresh_mode,
			param->intra_refresh_arg, num_ctu_row, num_ctu_col);
		return -EINVAL;
	}
	if (param->intra_refresh_mode == 3 && param->intra_refresh_arg > ctu_sz) {
		dev_err(dev, "mode %d, refresh %d wxh = %dx%d\n", param->intra_refresh_mode,
			param->intra_refresh_arg, num_ctu_row, num_ctu_col);
		return -EINVAL;
	}
	if (param->intra_refresh_mode == 4 && param->intra_refresh_arg > ctu_sz) {
		dev_err(dev, "mode %d, refresh %d wxh = %dx%d\n", param->intra_refresh_mode,
			param->intra_refresh_arg, num_ctu_row, num_ctu_col);
		return -EINVAL;
	}
	if (param->intra_refresh_mode == 4 && param->lossless_enable) {
		dev_err(dev, "mode %d, and lossless_enable", param->intra_refresh_mode);
		return -EINVAL;
	}
	if (param->intra_refresh_mode == 4 && param->roi_enable) {
		dev_err(dev, "mode %d, and roi_enable", param->intra_refresh_mode);
		return -EINVAL;
	}
	return 0;
}

static int wave5_vpu_enc_check_param_valid(struct vpu_device *vpu_dev, struct enc_open_param *pop)
{
	struct enc_wave_param *param = &pop->wave_param;

	if (pop->rc_enable) {
		if (param->min_qp_i > param->max_qp_i || param->min_qp_p > param->max_qp_p ||
		    param->min_qp_b > param->max_qp_b) {
			dev_err(vpu_dev->dev, "CFG FAIL : not allowed min_qp > max_qp\n");
			dev_err(vpu_dev->dev, "RECOMMEND CONFIG PARAMETER : min_qp = max_qp\n");
			return -EINVAL;
		}

		if (pop->bit_rate <= (int)pop->frame_rate_info) {
			dev_err(vpu_dev->dev, "not allowed enc_bit_rate <= frame_rate\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int wave5_vpu_enc_check_custom_gop(struct vpu_device *vpu_dev, struct enc_open_param *pop)
{
	struct custom_gop_param *gop_param;
	struct custom_gop_pic_param *gop_pic_param;
	struct custom_gop_pic_param new_gop[MAX_GOP_NUM * 2 + 1];

	u32 i, ei, gi, gop_size;
	s32 curr_poc;
	s32 enc_tid[MAX_GOP_NUM * 2 + 1];

	gop_param = &pop->wave_param.gop_param;
	gop_size = gop_param->custom_gop_size;

	new_gop[0].poc_offset = 0;
	new_gop[0].temporal_id = 0;
	new_gop[0].pic_type = PIC_TYPE_I;
	new_gop[0].use_multi_ref_p = 0;
	enc_tid[0] = 0;

	for (i = 0; i < gop_size * 2; i++) {
		ei = i % gop_size;
		gi = i / gop_size;
		gop_pic_param = &gop_param->pic_param[ei];

		curr_poc = gi * gop_size + gop_pic_param->poc_offset;
		new_gop[i + 1].poc_offset = curr_poc;
		new_gop[i + 1].temporal_id = gop_pic_param->temporal_id;
		new_gop[i + 1].pic_type = gop_pic_param->pic_type;
		new_gop[i + 1].ref_poc_l0 = gop_pic_param->ref_poc_l0 + gi * gop_size;
		new_gop[i + 1].ref_poc_l1 = gop_pic_param->ref_poc_l1 + gi * gop_size;
		new_gop[i + 1].use_multi_ref_p = gop_pic_param->use_multi_ref_p;
		enc_tid[i + 1] = -1;
	}

	for (i = 0; i < gop_size; i++) {
		gop_pic_param = &gop_param->pic_param[i];

		if (gop_pic_param->poc_offset <= 0) {
			dev_err(vpu_dev->dev, "POC of the %d-th pic not greater then -1\n", i + 1);
			return -EINVAL;
		}
		if (gop_pic_param->poc_offset > gop_size) {
			dev_err(vpu_dev->dev, "POC of %dth pic bigger than gop_size\n", i + 1);
			return -EINVAL;
		}
		if (gop_pic_param->temporal_id < 0) {
			dev_err(vpu_dev->dev, "temporal_id of the %d-th  < 0\n", i + 1);
			return -EINVAL;
		}
	}

	for (ei = 1; ei < gop_size * 2 + 1; ei++) {
		struct custom_gop_pic_param *cur_pic = &new_gop[ei];

		if (ei <= gop_size) {
			enc_tid[cur_pic->poc_offset] = cur_pic->temporal_id;
			continue;
		}

		if (new_gop[ei].pic_type != PIC_TYPE_I) {
			s32 ref_poc = cur_pic->ref_poc_l0;

			/* reference picture is not encoded yet */
			if (enc_tid[ref_poc] < 0) {
				dev_err(vpu_dev->dev, "1st ref pic cant be ref of pic (POC %d)\n",
					cur_pic->poc_offset - gop_size);
				return -EINVAL;
			}
			if (enc_tid[ref_poc] > cur_pic->temporal_id) {
				dev_err(vpu_dev->dev, "worng temporal_id of pic (POC %d)\n",
					cur_pic->poc_offset - gop_size);
				return -EINVAL;
			}
			if (ref_poc >= cur_pic->poc_offset) {
				dev_err(vpu_dev->dev, "POC of 1st ref pic of %d-th pic is wrong\n",
					cur_pic->poc_offset - gop_size);
				return -EINVAL;
			}
		}
		if (new_gop[ei].pic_type != PIC_TYPE_P) {
			s32 ref_poc = cur_pic->ref_poc_l1;

			/* reference picture is not encoded yet */
			if (enc_tid[ref_poc] < 0) {
				dev_err(vpu_dev->dev, "2nd ref pic cant be ref of pic (POC %d)\n"
						, cur_pic->poc_offset - gop_size);
				dev_err(vpu_dev->dev,  "2nd ref pic cant be ref of pic (POC %d)\n"
						, cur_pic->poc_offset - gop_size);
				return -EINVAL;
			}
			if (enc_tid[ref_poc] > cur_pic->temporal_id) {
				dev_err(vpu_dev->dev,  "temporal_id of %d-th picture is wrong\n",
					cur_pic->poc_offset - gop_size);
				return -EINVAL;
			}
			if (new_gop[ei].pic_type == PIC_TYPE_P && new_gop[ei].use_multi_ref_p > 0) {
				if (ref_poc >= cur_pic->poc_offset) {
					dev_err(vpu_dev->dev,  "bad POC of 2nd ref pic of %dth pic\n",
						cur_pic->poc_offset - gop_size);
					return -EINVAL;
				}
			} else if (ref_poc == cur_pic->poc_offset) {
				/* HOST_PIC_TYPE_B */
				dev_err(vpu_dev->dev,  "POC of 2nd ref pic of %dth pic is wrong\n",
					cur_pic->poc_offset - gop_size);
				return -EINVAL;
			}
		}
		curr_poc = cur_pic->poc_offset;
		enc_tid[curr_poc] = cur_pic->temporal_id;
	}
	return 0;
}

int wave5_vpu_enc_check_open_param(struct vpu_instance *vpu_inst, struct enc_open_param *pop)
{
	s32 pic_width;
	s32 pic_height;
	s32 product_id = vpu_inst->dev->product;
	struct vpu_attr *p_attr = &vpu_inst->dev->attr;

	if (!pop)
		return -EINVAL;

	pic_width = pop->pic_width;
	pic_height = pop->pic_height;

	if (vpu_inst->std != W_HEVC_ENC && vpu_inst->std != W_AVC_ENC)
		return -EOPNOTSUPP;

	if (vpu_inst->std == W_AVC_ENC && pop->wave_param.internal_bit_depth == 10 &&
	    !p_attr->support_avc10bit_enc)
		return -EOPNOTSUPP;

	if (vpu_inst->std == W_HEVC_ENC && pop->wave_param.internal_bit_depth == 10 &&
	    !p_attr->support_hevc10bit_enc)
		return -EOPNOTSUPP;

	if (pop->ring_buffer_enable) {
		if (pop->bitstream_buffer % 8)
			return -EINVAL;

		if (product_id == PRODUCT_ID_521) {
			if (pop->bitstream_buffer % 16)
				return -EINVAL;
			if (pop->bitstream_buffer_size < (1024 * 64))
				return -EINVAL;
		}

		if (pop->bitstream_buffer_size % 1024 || pop->bitstream_buffer_size < 1024)
			return -EINVAL;
	}

	if (!pop->frame_rate_info) {
		return -EINVAL;
	} else if (vpu_inst->std == W_HEVC_ENC) {
		if (product_id == PRODUCT_ID_521) {
			if (pop->bit_rate > 700000000 || pop->bit_rate < 0)
				return -EINVAL;
		}
	} else {
		if (pop->bit_rate > 32767 || pop->bit_rate < 0)
			return -EINVAL;
	}

	if (vpu_inst->std == W_HEVC_ENC ||
	    (vpu_inst->std == W_AVC_ENC && product_id == PRODUCT_ID_521)) {
		struct enc_wave_param *param = &pop->wave_param;

		if (pic_width < W5_MIN_ENC_PIC_WIDTH || pic_width > W5_MAX_ENC_PIC_WIDTH)
			return -EINVAL;

		if (pic_height < W5_MIN_ENC_PIC_HEIGHT || pic_height > W5_MAX_ENC_PIC_HEIGHT)
			return -EINVAL;

		if (param->profile) {
			if (vpu_inst->std == W_HEVC_ENC) { // only for HEVC condition
				if (param->profile != HEVC_PROFILE_MAIN && param->profile
						!= HEVC_PROFILE_MAIN10 &&
						param->profile != HEVC_PROFILE_STILLPICTURE)
					return -EINVAL;
				if (param->internal_bit_depth > 8 &&
				    param->profile == HEVC_PROFILE_MAIN)
					return -EINVAL;
			} else if (vpu_inst->std == W_AVC_ENC) {
				if ((param->internal_bit_depth > 8 &&
				     param->profile != H264_PROFILE_HIGH10))
					return -EINVAL;
			}
		}

		if (param->internal_bit_depth != 8 && param->internal_bit_depth != 10)
			return -EINVAL;

		if (param->decoding_refresh_type < 0 || param->decoding_refresh_type > 2)
			return -EINVAL;

		if (param->gop_preset_idx == PRESET_IDX_CUSTOM_GOP) {
			if (param->gop_param.custom_gop_size < 1 ||
			    param->gop_param.custom_gop_size > MAX_GOP_NUM)
				return -EINVAL;
		}

		if (vpu_inst->std == W_AVC_ENC) {
			if (param->custom_lambda_enable == 1)
				return -EINVAL;
		}
		if (param->intra_refresh_mode < 0 || param->intra_refresh_mode > 4)
			return -EINVAL;

		if (vpu_inst->std == W_HEVC_ENC && param->independ_slice_mode &&
		    param->depend_slice_mode > 2)
			return -EINVAL;

		if (param->scaling_list_enable == 3)
			return -EINVAL;

		if (!param->disable_deblk) {
			if (param->beta_offset_div2 < -6 || param->beta_offset_div2 > 6)
				return -EINVAL;

			if (param->tc_offset_div2 < -6 || param->tc_offset_div2 > 6)
				return -EINVAL;
		}

		if (param->intra_qp < 0 || param->intra_qp > 63)
			return -EINVAL;

		if (pop->rc_enable) {
			if (param->min_qp_i < 0 || param->min_qp_i > 63)
				return -EINVAL;
			if (param->max_qp_i < 0 || param->max_qp_i > 63)
				return -EINVAL;

			if (param->min_qp_p < 0 || param->min_qp_p > 63)
				return -EINVAL;
			if (param->max_qp_p < 0 || param->max_qp_p > 63)
				return -EINVAL;

			if (param->min_qp_b < 0 || param->min_qp_b > 63)
				return -EINVAL;
			if (param->max_qp_b < 0 || param->max_qp_b > 63)
				return -EINVAL;

			if (param->hvs_qp_enable) {
				if (param->hvs_max_delta_qp < 0 || param->hvs_max_delta_qp > 51)
					return -EINVAL;
			}

			if (param->bit_alloc_mode > 2)
				return -EINVAL;

			if (pop->vbv_buffer_size < 10 || pop->vbv_buffer_size > 3000)
				return -EINVAL;
		}

		// packed format & cbcr_interleave & nv12 can't be set at the same time.
		if (pop->packed_format == 1 && pop->cbcr_interleave == 1)
			return -EINVAL;

		if (pop->packed_format == 1 && pop->nv21 == 1)
			return -EINVAL;

		// check valid for common param
		if (wave5_vpu_enc_check_common_param_valid(vpu_inst, pop))
			return -EINVAL;

		// check valid for RC param
		if (wave5_vpu_enc_check_param_valid(vpu_inst->dev, pop))
			return -EINVAL;

		if (param->gop_preset_idx == PRESET_IDX_CUSTOM_GOP) {
			if (wave5_vpu_enc_check_custom_gop(vpu_inst->dev, pop))
				return -EINVAL;
		}

		if (param->chroma_cb_qp_offset < -12 || param->chroma_cb_qp_offset > 12)
			return -EINVAL;

		if (param->chroma_cr_qp_offset < -12 || param->chroma_cr_qp_offset > 12)
			return -EINVAL;

		if (param->intra_refresh_mode == 3 && !param->intra_refresh_arg)
			return -EINVAL;

		if (vpu_inst->std == W_HEVC_ENC) {
			if (param->nr_noise_sigma_y > 255)
				return -EINVAL;

			if (param->nr_noise_sigma_cb > 255)
				return -EINVAL;

			if (param->nr_noise_sigma_cr > 255)
				return -EINVAL;

			if (param->nr_intra_weight_y > 31)
				return -EINVAL;

			if (param->nr_intra_weight_cb > 31)
				return -EINVAL;

			if (param->nr_intra_weight_cr > 31)
				return -EINVAL;

			if (param->nr_inter_weight_y > 31)
				return -EINVAL;

			if (param->nr_inter_weight_cb > 31)
				return -EINVAL;

			if (param->nr_inter_weight_cr > 31)
				return -EINVAL;

			if ((param->nr_y_enable == 1 || param->nr_cb_enable == 1 ||
			     param->nr_cr_enable == 1) &&
					param->lossless_enable == 1)
				return -EINVAL;
		}
	}

	return 0;
}

