// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016-2018 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Jyri Sarha <jsarha@ti.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/dma-mapping.h>
#include <linux/of_graph.h>
#include <drm/drm_fourcc.h>

#include "dss.h"
#include "dss7.h"

#define REG_GET(dev, idx, start, end) \
	FLD_GET(dispc7_read(dev, idx), start, end)

#define REG_FLD_MOD(dev, idx, val, start, end)				\
	dispc7_write(dev, idx, FLD_MOD(dispc7_read(dev, idx), val, start, end))

#define VID_REG_GET(dev, plane, idx, start, end) \
	FLD_GET(dispc7_vid_read(dev, plane, idx), start, end)

#define VID_REG_FLD_MOD(dev, plane, idx, val, start, end)				\
	dispc7_vid_write(dev, plane, idx, FLD_MOD(dispc7_vid_read(dev, plane, idx), val, start, end))

#define VP_REG_GET(dev, vp, idx, start, end) \
	FLD_GET(dispc7_vp_read(dev, vp, idx), start, end)

#define VP_REG_FLD_MOD(dev, vp, idx, val, start, end)				\
	dispc7_vp_write(dev, vp, idx, FLD_MOD(dispc7_vp_read(dev, vp, idx), val, start, end))

#define OVR_REG_GET(dev, ovr, idx, start, end) \
	FLD_GET(dispc7_ovr_read(dev, ovr, idx), start, end)

#define OVR_REG_FLD_MOD(dev, ovr, idx, val, start, end)				\
	dispc7_ovr_write(dev, ovr, idx, FLD_MOD(dispc7_ovr_read(dev, ovr, idx), val, start, end))

struct dss_features {
	int num_ports;

	/* XXX should these come from the .dts? Min pclk is not feature of DSS IP */
	unsigned long min_pclk;
	unsigned long max_pclk;

	u32 num_mgrs;
	u32 num_ovls;
};

static const struct dss_features k3_dss_feats = {
	.num_ports = 2,

	.min_pclk = 1000,
	.max_pclk = 200000000,

	.num_mgrs = 2,
	.num_ovls = 2,
};

static const struct of_device_id dss7_of_match[];

struct dss_mgr_data {
	u32 gamma_table[256];
};

struct dss_plane_data {
	uint zorder;
	uint channel;
};

struct dss_data
{
	struct platform_device *pdev;

	void __iomem *base_common;
	void __iomem *base_vid[2];
	void __iomem *base_ovr[2];
	void __iomem *base_vp[2];

	int irq;
	irq_handler_t user_handler;
	void *user_data;

	const struct dss_features *feat;

	struct clk *fclk;
	struct clk *vp_clk;

	bool is_enabled;

	struct dss_mgr_data mgr_data[2];

	struct dss_plane_data plane_data[2];
};

static struct dss_data *dispcp; // XXX A hack for dispc_ops without dev context

static struct dss_data *dssdata(struct device *dev)
{
	return dev_get_drvdata(dev);
}

/* omapdrm device */

/*
 * HACK. For OMAP, we create the omapdrm device in platform code. That will
 * be removed when omapdss and omapdrm are merged. To avoid creating such
 * platform code for K3, we create omapdrm device after omapdss's probe
 * has succeeded.
 */

static void omapdrm_release(struct device *dev)
{
}

static struct platform_device omap_drm_device = {
	.dev = {
		.release = omapdrm_release,
		.coherent_dma_mask = DMA_BIT_MASK(32),
	},

	.name = "omapdrm",
	.id = 0,
};

static int initialize_omapdrm_device(void)
{
	int r;

	r = platform_device_register(&omap_drm_device);

	if (r)
		return r;

	dma_set_mask_and_coherent(&omap_drm_device.dev, DMA_BIT_MASK(48));
	arch_setup_dma_ops(&omap_drm_device.dev, 0, DMA_BIT_MASK(48), NULL, true);

	return 0;
}

static void uninitialize_omapdrm_device(void)
{
	platform_device_unregister(&omap_drm_device);
}

/* omapdrm device end */




/***********************************************************************/
// DISPC START
/***********************************************************************/

static void dispc7_write(struct device *dev, u16 reg, u32 val)
{
	iowrite32(val, dssdata(dev)->base_common + reg);
}

static u32 dispc7_read(struct device *dev, u16 reg)
{
	return ioread32(dssdata(dev)->base_common + reg);
}

static void dispc7_vid_write(struct device *dev, enum omap_plane_id plane, u16 reg, u32 val)
{
	void __iomem *base = dssdata(dev)->base_vid[plane];
	iowrite32(val, base + reg);
}

static u32 dispc7_vid_read(struct device *dev, enum omap_plane_id plane, u16 reg)
{
	void __iomem *base = dssdata(dev)->base_vid[plane];
	return ioread32(base + reg);
}

static void dispc7_ovr_write(struct device *dev, enum omap_channel channel, u16 reg, u32 val)
{
	void __iomem *base = dssdata(dev)->base_ovr[channel];
	iowrite32(val, base + reg);
}

static u32 dispc7_ovr_read(struct device *dev, enum omap_channel channel, u16 reg)
{
	void __iomem *base = dssdata(dev)->base_ovr[channel];
	return ioread32(base + reg);
}

static void dispc7_vp_write(struct device *dev, enum omap_channel channel, u16 reg, u32 val)
{
	void __iomem *base = dssdata(dev)->base_vp[channel];
	iowrite32(val, base + reg);
}

static u32 dispc7_vp_read(struct device *dev, enum omap_channel channel, u16 reg)
{
	void __iomem *base = dssdata(dev)->base_vp[channel];
	return ioread32(base + reg);
}


int dispc7_runtime_get(void)
{
	struct device *dev = &dispcp->pdev->dev;
	int r;

	dev_dbg(dev, "dispc_runtime_get\n");

	r = pm_runtime_get_sync(dev);
	WARN_ON(r < 0);
	return r < 0 ? r : 0;
}

void dispc7_runtime_put(void)
{
	struct device *dev = &dispcp->pdev->dev;
	int r;

	dev_dbg(dev, "dispc_runtime_put\n");

	r = pm_runtime_put_sync(dev);
	WARN_ON(r < 0);
}

static void dispc7_save_context(struct device *dev)
{
	/* XXX: Implementation missing */
}

static void dispc7_restore_context(struct device *dev)
{
	/* XXX: Implementation missing */
}

static irqreturn_t dispc7_irq_handler(int irq, void *arg)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);
	u32 stat;

	if (!dssdata(dev)->is_enabled)
		return IRQ_NONE;

	stat = dispc7_read(dev, DISPC_IRQSTATUS);

	if (stat == 0)
		return IRQ_NONE;

	return dss_data->user_handler(irq, dss_data->user_data);
}

static int dispc7_request_irq(irq_handler_t handler, void *dev_id)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);
	int r;

	if (dssdata(dev)->user_handler != NULL)
		return -EBUSY;

	dssdata(dev)->user_handler = handler;
	dssdata(dev)->user_data = dev_id;

	/* ensure the dispc7_irq_handler sees the values above */
	smp_wmb();

	r = devm_request_irq(dev, dssdata(dev)->irq, dispc7_irq_handler,
			     IRQF_SHARED, "DISPC", dss_data);
	if (r) {
		dss_data->user_handler = NULL;
		dss_data->user_data = NULL;
	}

	return r;
}

static void dispc7_free_irq(void *dev_id)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);

	dispc7_write(dev, DISPC_IRQENABLE_CLR, 0xffffffff);

	devm_free_irq(dev, dss_data->irq, dss_data);

	dss_data->user_handler = NULL;
	dss_data->user_data = NULL;
}

static u64 dispc7_vp_irq_from_raw(u32 stat, enum omap_channel channel)
{
	u64 vp_stat = 0;

	if (stat & BIT(0))
		vp_stat |= DSS_IRQ_MGR_FRAME_DONE(channel);
	if (stat & BIT(1))
		vp_stat |= DSS_IRQ_MGR_VSYNC_EVEN(channel);
	if (stat & BIT(2))
		vp_stat |= DSS_IRQ_MGR_VSYNC_ODD(channel);
	if (stat & BIT(4))
		vp_stat |= DSS_IRQ_MGR_SYNC_LOST(channel);

	return vp_stat;
}

static u32 dispc7_vp_irq_to_raw(u64 vpstat, enum omap_channel channel)
{
	u32 stat = 0;

	if (vpstat & DSS_IRQ_MGR_FRAME_DONE(channel))
		stat |= BIT(0);
	if (vpstat & DSS_IRQ_MGR_VSYNC_EVEN(channel))
		stat |= BIT(1);
	if (vpstat & DSS_IRQ_MGR_VSYNC_ODD(channel))
		stat |= BIT(2);
	if (vpstat & DSS_IRQ_MGR_SYNC_LOST(channel))
		stat |= BIT(4);

	return stat;
}

static u64 dispc7_vid_irq_from_raw(u32 stat, enum omap_plane_id plane)
{
	u64 vid_stat = 0;

	if (stat & BIT(0))
		vid_stat |= DSS_IRQ_OVL_FIFO_UNDERFLOW(plane);

	return vid_stat;
}

static u32 dispc7_vid_irq_to_raw(u64 vidstat, enum omap_plane_id plane)
{
	u32 stat = 0;

	if (vidstat & DSS_IRQ_OVL_FIFO_UNDERFLOW(plane))
		stat |= BIT(0);

	return stat;
}

static u64 dispc7_vp_read_irqstatus(struct device *dev,
				    enum omap_channel channel)
{
	u32 stat = dispc7_read(dev, DISPC_VP_IRQSTATUS(channel));

	return dispc7_vp_irq_from_raw(stat, channel);
}

static void dispc7_vp_write_irqstatus(struct device *dev,
				      enum omap_channel channel,
				      u64 vpstat)
{
	u32 stat = dispc7_vp_irq_to_raw(vpstat, channel);

	dispc7_write(dev, DISPC_VP_IRQSTATUS(channel), stat);
}

static u64 dispc7_vid_read_irqstatus(struct device *dev,
				     enum omap_plane_id plane)
{
	u32 stat = dispc7_read(dev, DISPC_VID_IRQSTATUS(plane));

	return dispc7_vid_irq_from_raw(stat, plane);
}

static void dispc7_vid_write_irqstatus(struct device *dev,
				       enum omap_plane_id plane,
				       u64 vidstat)
{
	u32 stat = dispc7_vid_irq_to_raw(vidstat, plane);

	dispc7_write(dev, DISPC_VID_IRQSTATUS(plane), stat);
}

static u64 dispc7_vp_read_irqenable(struct device *dev,
				    enum omap_channel channel)
{
	u32 stat = dispc7_read(dev, DISPC_VP_IRQENABLE(channel));

	return dispc7_vp_irq_from_raw(stat, channel);
}

static void dispc7_vp_write_irqenable(struct device *dev,
				      enum omap_channel channel,
				      u64 vpstat)
{
	u32 stat = dispc7_vp_irq_to_raw(vpstat, channel);

	dispc7_write(dev, DISPC_VP_IRQENABLE(channel), stat);
}


static u64 dispc7_vid_read_irqenable(struct device *dev,
				     enum omap_plane_id plane)
{
	u32 stat = dispc7_read(dev, DISPC_VID_IRQENABLE(plane));

	return dispc7_vid_irq_from_raw(stat, plane);
}

static void dispc7_vid_write_irqenable(struct device *dev,
				       enum omap_plane_id plane,
				       u64 vidstat)
{
	u32 stat = dispc7_vid_irq_to_raw(vidstat, plane);

	dispc7_write(dev, DISPC_VID_IRQENABLE(plane), stat);
}

static void dispc7_clear_irqstatus(struct device *dev, u64 clearmask)
{
	struct dss_data *dss_data = dssdata(dev);
	unsigned i;
	u32 top_clear = 0;

	for (i = 0; i < dss_data->feat->num_mgrs; ++i) {
		if (clearmask & DSS_IRQ_MGR_MASK(i)) {
			dispc7_vp_write_irqstatus(dev, i, clearmask);
			top_clear |= BIT(i);
		}
	}
	for (i = 0; i < dss_data->feat->num_ovls; ++i) {
		if (clearmask & DSS_IRQ_OVL_MASK(i)) {
			dispc7_vid_write_irqstatus(dev, i, clearmask);
			top_clear |= BIT(4+i);
		}
	}
	dispc7_write(dev, DISPC_IRQSTATUS, top_clear);

	/* Flush posted writes */
	dispc7_read(dev, DISPC_IRQSTATUS);
}

static u64 dispc7_read_and_clear_irqstatus(void)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);
	u64 status = 0;
	unsigned i;

	for (i = 0; i < dss_data->feat->num_mgrs; ++i)
		status |= dispc7_vp_read_irqstatus(dev, i);

	for (i = 0; i < dss_data->feat->num_ovls; ++i)
		status |= dispc7_vid_read_irqstatus(dev, i);

	dispc7_clear_irqstatus(dev, status);

	return status;
}

static u64 dispc7_read_irqenable(void)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);
	u64 enable = 0;
	unsigned i;

	for (i = 0; i < dss_data->feat->num_mgrs; ++i)
		enable |= dispc7_vp_read_irqenable(dev, i);

	for (i = 0; i < dss_data->feat->num_ovls; ++i)
		enable |= dispc7_vid_read_irqenable(dev, i);

	return enable;
}

static void dispc7_write_irqenable(u64 mask)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);
	unsigned i;
	u32 main_enable = 0, main_disable = 0;
	u64 old_mask;

	old_mask = dispc7_read_irqenable();

	/* clear the irqstatus for newly enabled irqs */
	dispc7_clear_irqstatus(dev, (old_mask ^ mask) & mask);

	for (i = 0; i < dss_data->feat->num_mgrs; ++i) {
		dispc7_vp_write_irqenable(dev, i, mask);
		if (mask & DSS_IRQ_MGR_MASK(i))
			main_enable |= BIT(i);	/* VP IRQ */
		else
			main_disable |= BIT(i);	/* VP IRQ */
	}

	for (i = 0; i < dss_data->feat->num_ovls; ++i) {
		dispc7_vid_write_irqenable(dev, i, mask);
		if (mask & DSS_IRQ_OVL_MASK(i))
			main_enable |= BIT(i+4);	/* VID IRQ */
		else
			main_disable |= BIT(i+4);	/* VID IRQ */
	}

	if (main_enable)
		dispc7_write(dev, DISPC_IRQENABLE_SET, main_enable);

	if (main_disable)
		dispc7_write(dev, DISPC_IRQENABLE_CLR, main_disable);

	/* Flush posted writes */
	dispc7_read(dev, DISPC_IRQENABLE_SET);
}



static bool dispc7_mgr_go_busy(enum omap_channel channel)
{
	struct device *dev = &dispcp->pdev->dev;

	return VP_REG_GET(dev, channel, DISPC_VP_CONTROL, 5, 5);
}

static void dispc7_mgr_go(enum omap_channel channel)
{
	struct device *dev = &dispcp->pdev->dev;

	VP_REG_FLD_MOD(dev, channel, DISPC_VP_CONTROL, 1, 5, 5);
}

static void dispc7_mgr_enable(enum omap_channel channel, bool enable)
{
	struct device *dev = &dispcp->pdev->dev;

	VP_REG_FLD_MOD(dev, channel, DISPC_VP_CONTROL, !!enable, 0, 0);
}

static bool dispc7_mgr_is_enabled(enum omap_channel channel)
{
	struct device *dev = &dispcp->pdev->dev;

	return VP_REG_GET(dev, channel, DISPC_VP_CONTROL, 0, 0);
}

static u16 c8_to_c12(u8 c8)
{
	u16 c12;

	c12 = c8 << 4;

	/* Replication logic: Copy c8 4 MSB to 4 LSB for full scale c12 */
	c12 |= c8 >> 4;

	return c12;
}

static u64 argb8888_to_argb12121212(u32 argb8888)
{
	u8 a, r, g, b;
	u64 v;

	a = (argb8888 >> 24) & 0xff;
	r = (argb8888 >> 16) & 0xff;
	g = (argb8888 >> 8) & 0xff;
	b = (argb8888 >> 0) & 0xff;

	v = ((u64)c8_to_c12(a) << 36) | ((u64)c8_to_c12(r) << 24) |
	    ((u64)c8_to_c12(g) << 12) | (u64)c8_to_c12(b);

	return v;
}

static void dispc7_mgr_setup(enum omap_channel channel,
			     const struct omap_overlay_manager_info *info)
{
	struct device *dev = &dispcp->pdev->dev;
	u64 v;

	v = argb8888_to_argb12121212(info->default_color);

	dispc7_ovr_write(dev, channel, DISPC_OVR_DEFAULT_COLOR, v & 0xffffffff);
	dispc7_ovr_write(dev, channel, DISPC_OVR_DEFAULT_COLOR2, (v >> 32) & 0xffff);
}

static void dispc7_set_num_datalines(struct device *dev,
				     enum omap_channel channel, int num_lines)
{
	int v;

	switch (num_lines) {
	case 12:
		v = 0; break;
	case 16:
		v = 1; break;
	case 18:
		v = 2; break;
	case 24:
		v = 3; break;
	case 30:
		v = 4; break;
	case 36:
		v = 5; break;
	default:
		BUG();
	}

	VP_REG_FLD_MOD(dev, channel, DISPC_VP_CONTROL, v, 10, 8);
}

static void dispc7_mgr_set_lcd_config(enum omap_channel channel,
				      const struct dss_lcd_mgr_config *config)
{
	struct device *dev = &dispcp->pdev->dev;

	dispc7_set_num_datalines(dev, channel, config->video_port_width);
}

static bool dispc7_lcd_timings_ok(int hsw, int hfp, int hbp,
				  int vsw, int vfp, int vbp)
{
	if (hsw < 1 || hsw > 256 ||
	    hfp < 1 || hfp > 4096 ||
	    hbp < 1 || hbp > 4096 ||
	    vsw < 1 || vsw > 256 ||
	    vfp < 0 || vfp > 4095 ||
	    vbp < 0 || vbp > 4095)
		return false;
	return true;
}

bool dispc7_mgr_timings_ok(enum omap_channel channel,
			   const struct videomode *vm)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);

	if (vm->pixelclock < dss_data->feat->min_pclk && vm->pixelclock != 9000000)
		return false;

	if (vm->pixelclock > dss_data->feat->max_pclk)
		return false;

	if (vm->hactive > 4096)
		return false;

	if (vm->vactive > 4096)
		return false;

	/* TODO: add interlace support */
	if (vm->flags & DISPLAY_FLAGS_INTERLACED)
		return false;

	if (!dispc7_lcd_timings_ok(vm->hsync_len, vm->hfront_porch, vm->hback_porch,
				   vm->vsync_len, vm->vfront_porch, vm->vback_porch))
		return false;

	return true;
}

static void dispc7_mgr_set_timings(enum omap_channel channel,
				   const struct videomode *vm)
{
	struct device *dev = &dispcp->pdev->dev;
	bool align, onoff, rf, ieo, ipc, ihs, ivs;

	dispc7_vp_write(dev, channel, DISPC_VP_TIMING_H,
			FLD_VAL(vm->hsync_len - 1, 7, 0) |
			FLD_VAL(vm->hfront_porch - 1, 19, 8) |
			FLD_VAL(vm->hback_porch - 1, 31, 20));

	dispc7_vp_write(dev, channel, DISPC_VP_TIMING_V,
			FLD_VAL(vm->vsync_len - 1, 7, 0) |
			FLD_VAL(vm->vfront_porch, 19, 8) |
			FLD_VAL(vm->vback_porch, 31, 20));

	if (vm->flags & DISPLAY_FLAGS_VSYNC_HIGH)
		ivs = false;
	else
		ivs = true;

	if (vm->flags & DISPLAY_FLAGS_HSYNC_HIGH)
		ihs = false;
	else
		ihs = true;

	if (vm->flags & DISPLAY_FLAGS_DE_HIGH)
		ieo = false;
	else
		ieo = true;

	if (vm->flags & DISPLAY_FLAGS_PIXDATA_POSEDGE)
		ipc = false;
	else
		ipc = true;

	/* always use the 'rf' setting */
	onoff = true;

	if (vm->flags & DISPLAY_FLAGS_SYNC_POSEDGE)
		rf = true;
	else
		rf = false;

	/* always use aligned syncs */
	align = true;

	dispc7_vp_write(dev, channel, DISPC_VP_POL_FREQ,
			FLD_VAL(align, 18, 18) |
			FLD_VAL(onoff, 17, 17) |
			FLD_VAL(rf, 16, 16) |
			FLD_VAL(ieo, 15, 15) |
			FLD_VAL(ipc, 14, 14) |
			FLD_VAL(ihs, 13, 13) |
			FLD_VAL(ivs, 12, 12));

	dispc7_vp_write(dev, channel, DISPC_VP_SIZE_SCREEN,
			FLD_VAL(vm->hactive - 1, 11, 0) |
			FLD_VAL(vm->vactive - 1, 27, 16));
}

int dispc7_vp_enable_clk(enum omap_channel channel)
{
	struct device *dev = &dispcp->pdev->dev;

	return clk_prepare_enable(dssdata(dev)->vp_clk);
}
void dispc7_vp_disable_clk(enum omap_channel channel)
{
	struct device *dev = &dispcp->pdev->dev;

	clk_disable_unprepare(dssdata(dev)->vp_clk);
}

int dispc7_vp_set_clk_rate(enum omap_channel channel, unsigned long rate)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);
	int r;
	unsigned long new_rate;

	return 0; // XXX

	/* NOTE: This code has not been tested on k3 HW, only with k2g */

	r = clk_set_rate(dss_data->vp_clk, rate);
	if (r) {
		dev_err(dev, "Failed to set vp clk rate to %lu\n",
			rate);
		return r;
	}

	new_rate = clk_get_rate(dss_data->vp_clk);

	if (rate != new_rate)
		dev_warn(dev,
			 "Failed to get exact pix clock %lu != %lu\n",
			 rate, new_rate);

	dev_dbg(dev, "New VP rate %lu Hz (requested %lu Hz)\n",
		clk_get_rate(dss_data->vp_clk), rate);

	return 0;
}

/* CSC */

struct color_conv_coef {
	int ry, rcb, rcr;
	int gy, gcb, gcr;
	int by, bcb, bcr;
	int roffset, goffset, boffset;
	bool full_range;
};

static void dispc7_vid_write_color_conv_coefs(struct device *dev,
					      enum omap_plane_id plane,
					      const struct color_conv_coef *ct)
{
#define CVAL(x, y) (FLD_VAL(x, 26, 16) | FLD_VAL(y, 10, 0))

	dispc7_vid_write(dev, plane, DISPC_VID_CSC_COEF(0), CVAL(ct->rcr, ct->ry));
	dispc7_vid_write(dev, plane, DISPC_VID_CSC_COEF(1), CVAL(ct->gy,  ct->rcb));
	dispc7_vid_write(dev, plane, DISPC_VID_CSC_COEF(2), CVAL(ct->gcb, ct->gcr));
	dispc7_vid_write(dev, plane, DISPC_VID_CSC_COEF(3), CVAL(ct->bcr, ct->by));
	dispc7_vid_write(dev, plane, DISPC_VID_CSC_COEF(4), CVAL(0, ct->bcb));

	dispc7_vid_write(dev, plane, DISPC_VID_CSC_COEF(5),
			 FLD_VAL(ct->roffset, 15, 3) | FLD_VAL(ct->goffset, 31, 19));
	dispc7_vid_write(dev, plane, DISPC_VID_CSC_COEF(6),
			 FLD_VAL(ct->boffset, 15, 3));

	VID_REG_FLD_MOD(dev, plane, DISPC_VID_ATTRIBUTES, ct->full_range, 11, 11);

#undef CVAL
}

static void dispc7_vid_csc_setup(struct device *dev)
{
	struct dss_data *dss_data = dssdata(dev);
	/* YUV -> RGB, ITU-R BT.601, full range */
	const struct color_conv_coef yuv2rgb_bt601_full = {
		256,   0,  358,
		256, -88, -182,
		256, 452,    0,
		0, -2048, -2048,
		true,
	};
	int i;

	for (i = 0; i < dss_data->feat->num_ovls; i++)
		dispc7_vid_write_color_conv_coefs(dev, i, &yuv2rgb_bt601_full);
}

static void dispc7_vid_csc_enable(struct device *dev, enum omap_plane_id plane,
				  bool enable)
{
	VID_REG_FLD_MOD(dev, plane, DISPC_VID_ATTRIBUTES, !!enable, 9, 9);
}

/* SCALER */

static u32 dispc7_calc_fir_inc(unsigned in, unsigned out)
{
	return (u32)div_u64(0x200000ull * in, out);
}

struct dispc7_vid_fir_coefs {
	s16 c2[16];
	s16 c1[16];
	u16 c0[9];
};

static const struct dispc7_vid_fir_coefs dispc7_fir_coefs_null = {
	.c2 = {	0 },
	.c1 = { 0 },
	.c0 = { 512, 512, 512, 512, 512, 512, 512, 512, 256,  },
};

/* M=8, Upscale x >= 1 */
static const struct dispc7_vid_fir_coefs dispc7_fir_coefs_m8 = {
	.c2 = {	0, -4, -8, -16, -24, -32, -40, -48, 0, 2, 4, 6, 8, 6, 4, 2,  },
	.c1 = { 0, 28, 56, 94, 132, 176, 220, 266, -56, -60, -64, -62, -60, -50, -40, -20,  },
	.c0 = { 512, 506, 500, 478, 456, 424, 392, 352, 312,  },
};

/* 5-tap, M=22, Downscale Ratio 2.5 < x < 3 */
static const struct dispc7_vid_fir_coefs dispc7_fir_coefs_m22_5tap = {
	.c2 = { 16, 20, 24, 30, 36, 42, 48, 56, 0, 0, 0, 2, 4, 8, 12, 14,  },
	.c1 = { 132, 140, 148, 156, 164, 172, 180, 186, 64, 72, 80, 88, 96, 104, 112, 122,  },
	.c0 = { 216, 216, 216, 214, 212, 208, 204, 198, 192,  },
};

/* 3-tap, M=22, Downscale Ratio 2.5 < x < 3 */
static const struct dispc7_vid_fir_coefs dispc7_fir_coefs_m22_3tap = {
	.c1 = { 100, 118, 136, 156, 176, 196, 216, 236, 0, 10, 20, 30, 40, 54, 68, 84,  },
	.c0 = { 312, 310, 308, 302, 296, 286, 276, 266, 256,  },
};

enum dispc7_vid_fir_coef_set {
	DISPC7_VID_FIR_COEF_HORIZ,
	DISPC7_VID_FIR_COEF_HORIZ_UV,
	DISPC7_VID_FIR_COEF_VERT,
	DISPC7_VID_FIR_COEF_VERT_UV,
};

static void dispc7_vid_write_fir_coefs(struct device *dev,
				       enum omap_plane_id plane,
				       enum dispc7_vid_fir_coef_set coef_set,
				       const struct dispc7_vid_fir_coefs *coefs)
{
	static const u16 c0_regs[] = {
		[DISPC7_VID_FIR_COEF_HORIZ] = DISPC_VID_FIR_COEFS_H0,
		[DISPC7_VID_FIR_COEF_HORIZ_UV] = DISPC_VID_FIR_COEFS_H0_C,
		[DISPC7_VID_FIR_COEF_VERT] = DISPC_VID_FIR_COEFS_V0,
		[DISPC7_VID_FIR_COEF_VERT_UV] = DISPC_VID_FIR_COEFS_V0_C,
	};

	static const u16 c12_regs[] = {
		[DISPC7_VID_FIR_COEF_HORIZ] = DISPC_VID_FIR_COEFS_H12,
		[DISPC7_VID_FIR_COEF_HORIZ_UV] = DISPC_VID_FIR_COEFS_H12_C,
		[DISPC7_VID_FIR_COEF_VERT] = DISPC_VID_FIR_COEFS_V12,
		[DISPC7_VID_FIR_COEF_VERT_UV] = DISPC_VID_FIR_COEFS_V12_C,
	};

	const u16 c0_base = c0_regs[coef_set];
	const u16 c12_base = c12_regs[coef_set];
	int phase;

	for (phase = 0; phase <= 8; ++phase) {
		u16 reg = c0_base + phase * 4;
		u16 c0 = coefs->c0[phase];

		dispc7_vid_write(dev, plane, reg, c0);
	}

	for (phase = 0; phase <= 15; ++phase) {
		u16 reg = c12_base + phase * 4;
		s16 c1, c2;
		u32 c12;

		c1 = coefs->c1[phase];
		c2 = coefs->c2[phase];
		c12 = FLD_VAL(c1, 19, 10) | FLD_VAL(c2, 29, 20);

		dispc7_vid_write(dev, plane, reg, c12);
	}
}

static void dispc7_vid_write_scale_coefs(struct device *dev,
					 enum omap_plane_id plane)
{
	dispc7_vid_write_fir_coefs(dev, plane, DISPC7_VID_FIR_COEF_HORIZ, &dispc7_fir_coefs_null);
	dispc7_vid_write_fir_coefs(dev, plane, DISPC7_VID_FIR_COEF_HORIZ_UV, &dispc7_fir_coefs_null);
	dispc7_vid_write_fir_coefs(dev, plane, DISPC7_VID_FIR_COEF_VERT, &dispc7_fir_coefs_null);
	dispc7_vid_write_fir_coefs(dev, plane, DISPC7_VID_FIR_COEF_VERT_UV, &dispc7_fir_coefs_null);
}

static void dispc7_vid_set_scaling(struct device *dev, enum omap_plane_id plane,
				   unsigned orig_width, unsigned orig_height,
				   unsigned out_width, unsigned out_height,
				   u32 fourcc)
{
	unsigned in_w, in_h, in_w_uv, in_h_uv;
	unsigned fir_hinc, fir_vinc, fir_hinc_uv, fir_vinc_uv;
	bool scale_x, scale_y;
	bool five_taps = false;		/* XXX always 3-tap for now */

	in_w = in_w_uv = orig_width;
	in_h = in_h_uv = orig_height;

	switch (fourcc) {
	case DRM_FORMAT_NV12:
		/* UV is subsampled by 2 horizontally and vertically */
		in_h_uv >>= 1;
		in_w_uv >>= 1;
		break;

	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_UYVY:
		/* UV is subsampled by 2 horizontally */
		in_w_uv >>= 1;
		break;

	default:
		break;
	}

	scale_x = in_w != out_width || in_w_uv != out_width;
	scale_y = in_h != out_height || in_h_uv != out_height;

	/* HORIZONTAL RESIZE ENABLE */
	VID_REG_FLD_MOD(dev, plane, DISPC_VID_ATTRIBUTES, scale_x, 7, 7);

	/* VERTICAL RESIZE ENABLE */
	VID_REG_FLD_MOD(dev, plane, DISPC_VID_ATTRIBUTES, scale_y, 8, 8);

	/* Skip the rest if no scaling is used */
	if (!scale_x && !scale_y)
		return;

	/* VERTICAL 5-TAPS  */
	VID_REG_FLD_MOD(dev, plane, DISPC_VID_ATTRIBUTES, five_taps, 21, 21);

	/* FIR INC */

	fir_hinc = dispc7_calc_fir_inc(in_w, out_width);
	fir_vinc = dispc7_calc_fir_inc(in_h, out_height);
	fir_hinc_uv = dispc7_calc_fir_inc(in_w_uv, out_width);
	fir_vinc_uv = dispc7_calc_fir_inc(in_h_uv, out_height);

	dispc7_vid_write(dev, plane, DISPC_VID_FIRH, fir_hinc);
	dispc7_vid_write(dev, plane, DISPC_VID_FIRV, fir_vinc);
	dispc7_vid_write(dev, plane, DISPC_VID_FIRH2, fir_hinc_uv);
	dispc7_vid_write(dev, plane, DISPC_VID_FIRV2, fir_vinc_uv);

	dispc7_vid_write_scale_coefs(dev, plane);
}

/* OTHER */

static const struct {
	u32 fourcc;
	u8 dss_code;
	u8 bytespp;
} dispc7_color_formats[] = {
	{ DRM_FORMAT_RGB565, 0x3, 3, },

	{ DRM_FORMAT_XRGB8888, 0x27, 4, },
	{ DRM_FORMAT_ARGB8888, 0x7, 4, },

	{ DRM_FORMAT_RGBX8888, 0x29, 4, },
	{ DRM_FORMAT_RGBA8888, 0x9, 4, },

	{ DRM_FORMAT_YUYV, 0x3e, 2, },
	{ DRM_FORMAT_UYVY, 0x3f, 2, },

	{ DRM_FORMAT_NV12, 0x3d, 2, },
};

static bool dispc7_fourcc_is_yuv(u32 fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_NV12:
		return true;
	default:
		return false;
	}
}

static void dispc7_ovl_set_pixel_format(struct device *dev,
					enum omap_plane_id plane, u32 fourcc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dispc7_color_formats); ++i) {
		if (dispc7_color_formats[i].fourcc == fourcc) {
			VID_REG_FLD_MOD(dev, plane, DISPC_VID_ATTRIBUTES,
					dispc7_color_formats[i].dss_code,
					6, 1);
			return;
		}
	}

	WARN_ON(1);
}

static int dispc7_fourcc_to_bytespp(u32 fourcc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dispc7_color_formats); ++i) {
		if (dispc7_color_formats[i].fourcc == fourcc)
			return dispc7_color_formats[i].bytespp;
	}

	WARN_ON(1);
	return 4;
}

static s32 pixinc(int pixels, u8 ps)
{
	if (pixels == 1)
		return 1;
	else if (pixels > 1)
		return 1 + (pixels - 1) * ps;
	else if (pixels < 0)
		return 1 - (-pixels + 1) * ps;

	BUG();
	return 0;
}

static int dispc7_ovl_setup(enum omap_plane_id plane, const struct omap_overlay_info *oi,
			    const struct videomode *vm, bool mem_to_mem,
			    enum omap_channel channel)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);
	bool lite = plane != 0; // XXX vid lite doesn't have all the regs

	u32 fourcc = oi->fourcc;
	int bytespp = dispc7_fourcc_to_bytespp(fourcc);

	if (dispc7_fourcc_is_yuv(fourcc) && (oi->width & 1)) {
		dev_err(dev, "input width %d is not even for YUV format\n",
			oi->width);
		return -EINVAL;
	}

	dispc7_ovl_set_pixel_format(dev, plane, fourcc);

	dispc7_vid_write(dev, plane, DISPC_VID_BA_0, oi->paddr & 0xffffffff);
	dispc7_vid_write(dev, plane, DISPC_VID_BA_EXT_0, (u64)oi->paddr >> 32);
	dispc7_vid_write(dev, plane, DISPC_VID_BA_1, oi->paddr & 0xffffffff);
	dispc7_vid_write(dev, plane, DISPC_VID_BA_EXT_1, (u64)oi->paddr >> 32);

	dispc7_vid_write(dev, plane, DISPC_VID_BA_UV_0, oi->p_uv_addr & 0xffffffff);
	dispc7_vid_write(dev, plane, DISPC_VID_BA_UV_EXT_0, (u64)oi->p_uv_addr >> 32);
	dispc7_vid_write(dev, plane, DISPC_VID_BA_UV_1, oi->p_uv_addr & 0xffffffff);
	dispc7_vid_write(dev, plane, DISPC_VID_BA_UV_EXT_1, (u64)oi->p_uv_addr >> 32);

	dispc7_vid_write(dev, plane, DISPC_VID_PICTURE_SIZE,
			 (oi->width - 1) | ((oi->height - 1) << 16));

	dispc7_vid_write(dev, plane, DISPC_VID_PIXEL_INC, pixinc(1, bytespp));
	dispc7_vid_write(dev, plane, DISPC_VID_ROW_INC,
			 pixinc(1 + oi->screen_width - oi->width, bytespp));

	if (fourcc == DRM_FORMAT_NV12)
		dispc7_vid_write(dev, plane, DISPC_VID_ROW_INC_UV,
				 pixinc(1 + oi->screen_width - oi->width,
					bytespp));

	if (!lite) {
		dispc7_vid_write(dev, plane, DISPC_VID_SIZE,
				 (oi->out_width - 1) |
				 ((oi->out_height - 1) << 16));

		dispc7_vid_set_scaling(dev, plane,
				       oi->width, oi->height,
				       oi->out_width, oi->out_height,
				       fourcc);
	}

	/* enable YUV->RGB color conversion */
	if (dispc7_fourcc_is_yuv(fourcc))
		dispc7_vid_csc_enable(dev, plane, true);
	else
		dispc7_vid_csc_enable(dev, plane, false);

	OVR_REG_FLD_MOD(dev, channel, DISPC_OVR_ATTRIBUTES(oi->zorder),
			plane, 4, 1);
	OVR_REG_FLD_MOD(dev, channel, DISPC_OVR_ATTRIBUTES(oi->zorder),
			oi->pos_x, 17, 6);
	OVR_REG_FLD_MOD(dev, channel, DISPC_OVR_ATTRIBUTES(oi->zorder),
			oi->pos_y, 30, 19);

	OVR_REG_FLD_MOD(dev, channel, DISPC_OVR_ATTRIBUTES(oi->zorder), 1, 0, 0);
	dss_data->plane_data[plane].zorder = oi->zorder;
	dss_data->plane_data[plane].channel = channel;

	return 0;
}

static int dispc7_ovl_enable(enum omap_plane_id plane, bool enable)
{
	struct device *dev = &dispcp->pdev->dev;
	struct dss_data *dss_data = dssdata(dev);

	OVR_REG_FLD_MOD(dev, dss_data->plane_data[plane].channel,
			DISPC_OVR_ATTRIBUTES(dss_data->plane_data[plane].zorder),
			!!enable, 0, 0);
	VID_REG_FLD_MOD(dev, plane, DISPC_VID_ATTRIBUTES, !!enable, 0, 0);
	return 0;
}

static u32 dispc7_vid_get_fifo_size(enum omap_plane_id plane)
{
	struct device *dev = &dispcp->pdev->dev;
	const u32 unit_size = 16;	/* 128-bits */

	return VID_REG_GET(dev, plane, DISPC_VID_BUF_SIZE_STATUS, 15, 0) * unit_size;
}

static void dispc7_vid_set_mflag_threshold(struct device *dev,
					   enum omap_plane_id plane,
					   unsigned low, unsigned high)
{
	dispc7_vid_write(dev, plane, DISPC_VID_MFLAG_THRESHOLD,
			 FLD_VAL(high, 31, 16) | FLD_VAL(low, 15, 0));
}

static void __maybe_unused dispc7_mflag_setup(struct device *dev)
{
	enum omap_plane_id plane = 0;
	const u32 unit_size = 16;	/* 128-bits */
	u32 size = dispc7_vid_get_fifo_size(plane);
	u32 low, high;

	/* MFLAG_CTRL = MFLAGFORCE */
	REG_FLD_MOD(dev, DISPC_GLOBAL_MFLAG_ATTRIBUTE, 1, 1, 0);
	/* MFLAG_START = MFLAGNORMALSTARTMODE */
	REG_FLD_MOD(dev, DISPC_GLOBAL_MFLAG_ATTRIBUTE, 0, 6, 6);

	/*
	 * Simulation team suggests below thesholds:
	 * HT = fifosize * 5 / 8;
	 * LT = fifosize * 4 / 8;
	 */

	low = size * 4 / 8 / unit_size;
	high = size * 5 / 8 / unit_size;

	dispc7_vid_set_mflag_threshold(dev, plane, low, high);
}

static void dispc7_vp_setup(struct device *dev)
{
	struct dss_data *dss_data = dssdata(dev);
	unsigned int i;

	dev_dbg(dev, "%s()\n", __func__);

	/* Enable the gamma Shadow bit-field for all VPs*/
	for (i = 0; i < dss_data->feat->num_mgrs; i++)
		VP_REG_FLD_MOD(dev, i, DISPC_VP_CONFIG, 1, 2, 2);
}

static void dispc7_initial_config(struct device *dev)
{
	dispc7_vid_csc_setup(dev);
	//dispc7_mflag_setup(dev);
	dispc7_vp_setup(dev);
}

/***********************************************************************/
// DISPC END
/***********************************************************************/

static int dss7_init_features(struct platform_device *pdev)
{
	struct dss_data *dss_data = dssdata(&pdev->dev);
	const struct of_device_id *match;

	match = of_match_node(dss7_of_match, pdev->dev.of_node);
	if (!match) {
		dev_err(&pdev->dev, "Unsupported DSS version\n");
		return -ENODEV;
	}

	dss_data->feat = match->data;

	return 0;
}

static int dss7_init_ports(struct platform_device *pdev)
{
	struct dss_data *dss_data = dssdata(&pdev->dev);
	struct device_node *parent = pdev->dev.of_node;
	struct device_node *port;
	int i;

	for (i = 0; i < dss_data->feat->num_ports; i++) {
		port = of_graph_get_port_by_id(parent, i);
		if (!port)
			continue;

		dpi7_init_port(pdev, port);
	}

	return 0;
}

static void dss7_uninit_ports(struct platform_device *pdev)
{
	struct dss_data *dss_data = dssdata(&pdev->dev);
	struct device_node *parent = pdev->dev.of_node;
	struct device_node *port;
	int i;

	for (i = 0; i < dss_data->feat->num_ports; i++) {
		port = of_graph_get_port_by_id(parent, i);
		if (!port)
			continue;

		dpi7_uninit_port(port);
	}
}

static enum omap_dss_output_id dispc7_mgr_get_supported_outputs(enum omap_channel channel)
{
	return OMAP_DSS_OUTPUT_DPI;
}

static const u32 dispc7_color_list[] = {
	DRM_FORMAT_RGB565,

	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,

	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_RGBA8888,

	DRM_FORMAT_YUYV,
	DRM_FORMAT_UYVY,

	DRM_FORMAT_NV12,

	0
};

static const u32 * dispc7_ovl_get_color_modes(enum omap_plane_id plane)
{
	return dispc7_color_list;
}

static int dispc7_get_num_ovls(void)
{
	struct device *dev = &dispcp->pdev->dev;

	return dssdata(dev)->feat->num_ovls;
}

static int dispc7_get_num_mgrs(void)
{
	struct device *dev = &dispcp->pdev->dev;

	return dssdata(dev)->feat->num_mgrs;
}

static u32 dispc7_mgr_gamma_size(enum omap_channel channel)
{
	struct device *dev = &dispcp->pdev->dev;

	return ARRAY_SIZE(dssdata(dev)->mgr_data[channel].gamma_table);
}

static void dispc7_mgr_write_gamma_table(struct device *dev,
					 enum omap_channel channel)
{
	u32 *table = dssdata(dev)->mgr_data[channel].gamma_table;
	uint hwlen = ARRAY_SIZE(dssdata(dev)->mgr_data[channel].gamma_table);
	unsigned int i;

	dev_dbg(dev, "%s: channel %d\n", __func__, channel);

	for (i = 0; i < hwlen; ++i) {
		u32 v = table[i];

		v |= i << 24;

		dispc7_vp_write(dev, channel, DISPC_VP_GAMMA_TABLE, v);
	}
}

static void dispc7_restore_gamma_tables(struct device *dev)
{
	struct dss_data *dss_data = dssdata(dev);
	unsigned int i;

	dev_dbg(dev, "%s()\n", __func__);

	for (i = 0; i < dss_data->feat->num_mgrs; i++)
		dispc7_mgr_write_gamma_table(dev, i);
}

static const struct drm_color_lut dispc7_mgr_gamma_default_lut[] = {
	{ .red = 0, .green = 0, .blue = 0, },
	{ .red = U16_MAX, .green = U16_MAX, .blue = U16_MAX, },
};

static void dispc7_mgr_set_gamma(enum omap_channel channel,
			 const struct drm_color_lut *lut,
			 unsigned int length)
{
	struct device *dev = &dispcp->pdev->dev;
	u32 *table = dssdata(dev)->mgr_data[channel].gamma_table;
	uint hwlen = ARRAY_SIZE(dssdata(dev)->mgr_data[channel].gamma_table);
	static const uint hwbits = 8;
	uint i;

	dev_dbg(dev, "%s: channel %d, lut len %u, hw len %u\n",
		__func__, channel, length, hwlen);

	if (lut == NULL || length < 2) {
		lut = dispc7_mgr_gamma_default_lut;
		length = ARRAY_SIZE(dispc7_mgr_gamma_default_lut);
	}

	for (i = 0; i < length - 1; ++i) {
		uint first = i * (hwlen - 1) / (length - 1);
		uint last = (i + 1) * (hwlen - 1) / (length - 1);
		uint w = last - first;
		u16 r, g, b;
		uint j;

		if (w == 0)
			continue;

		for (j = 0; j <= w; j++) {
			r = (lut[i].red * (w - j) + lut[i+1].red * j) / w;
			g = (lut[i].green * (w - j) + lut[i+1].green * j) / w;
			b = (lut[i].blue * (w - j) + lut[i+1].blue * j) / w;

			r >>= 16 - hwbits;
			g >>= 16 - hwbits;
			b >>= 16 - hwbits;

			table[first + j] = (r << (hwbits * 2)) |
				(g << hwbits) | b;
		}
	}

	if (dssdata(dev)->is_enabled)
		dispc7_mgr_write_gamma_table(dev, channel);
}

static int dispc7_init_gamma_tables(struct device *dev)
{
	struct dss_data *dss_data = dssdata(dev);
	unsigned int i;

	dev_dbg(dev, "%s()\n", __func__);

	for (i = 0; i < dss_data->feat->num_mgrs; i++)
		dispc7_mgr_set_gamma(i, NULL, 0);

	return 0;
}

static const char *dispc7_ovl_name(enum omap_plane_id plane)
{
	static const char *ovl_names[] = { "VID", "VIDL1" };

	if (plane < ARRAY_SIZE(ovl_names))
		return ovl_names[plane];
	else
		return "ERROR";
}

static const char *dispc7_mgr_name(enum omap_channel channel)
{
	static const char *mgr_names[] = { "VP1", "VP2" };

	if (channel < ARRAY_SIZE(mgr_names))
		return mgr_names[channel];
	else
		return "ERROR";
}

static bool dispc7_mgr_has_framedone(enum omap_channel channel)
{
	return true;
}

static const struct dispc_ops dispc7_ops = {
	.read_and_clear_irqstatus = dispc7_read_and_clear_irqstatus,
	.write_irqenable = dispc7_write_irqenable,

	.request_irq = dispc7_request_irq,
	.free_irq = dispc7_free_irq,

	.runtime_get = dispc7_runtime_get,
	.runtime_put = dispc7_runtime_put,

	.get_num_ovls = dispc7_get_num_ovls,
	.get_num_mgrs = dispc7_get_num_mgrs,

	.ovl_name = dispc7_ovl_name,
	.mgr_name = dispc7_mgr_name,

	.mgr_has_framedone = dispc7_mgr_has_framedone,

	.mgr_enable = dispc7_mgr_enable,
	.mgr_is_enabled = dispc7_mgr_is_enabled,
	.mgr_go_busy = dispc7_mgr_go_busy,
	.mgr_go = dispc7_mgr_go,
	.mgr_set_lcd_config = dispc7_mgr_set_lcd_config,
	.mgr_set_timings = dispc7_mgr_set_timings,
	.mgr_setup = dispc7_mgr_setup,
	.mgr_get_supported_outputs = dispc7_mgr_get_supported_outputs,
	.mgr_gamma_size = dispc7_mgr_gamma_size,
	.mgr_set_gamma = dispc7_mgr_set_gamma,

	.ovl_enable = dispc7_ovl_enable,
	.ovl_setup = dispc7_ovl_setup,
	.ovl_get_color_modes = dispc7_ovl_get_color_modes,
};

static int dispc7_iomap_resource(struct platform_device *pdev, const char *name,
				 void __iomem **base)
{
	struct resource *res;
	void __iomem *b;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res) {
		dev_err(&pdev->dev, "cannot get mem resource '%s'\n", name);
		return -EINVAL;
	}

	b = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(b)) {
		dev_err(&pdev->dev, "cannot ioremap resource '%s'\n", name);
		return PTR_ERR(b);
	}

	*base = b;

	return 0;
}

static int dss7_probe(struct platform_device *pdev)
{
	struct dss_data *dss_data;
	int r;

	dev_dbg(&pdev->dev, "PROBE\n");

	dss_data = devm_kzalloc(&pdev->dev, sizeof(*dss_data), GFP_KERNEL);
	if (!dss_data)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, dss_data);
	dss_data->pdev = pdev;

	dispcp = dss_data; // XXX A hack for dispc_ops without dev context

	r = dss7_init_features(dss_data->pdev);
	if (r)
		return r;


	r = dispc7_iomap_resource(pdev, "common", &dss_data->base_common);
	if (r)
		return r;

	/* note: VIDL1 is plane 2 */
	r = dispc7_iomap_resource(pdev, "vidl1", &dss_data->base_vid[1]);
	if (r)
		return r;

	/* note: VID is plane 1 */
	r = dispc7_iomap_resource(pdev, "vid", &dss_data->base_vid[0]);
	if (r)
		return r;

	r = dispc7_iomap_resource(pdev, "ovr1", &dss_data->base_ovr[0]);
	if (r)
		return r;

	r = dispc7_iomap_resource(pdev, "ovr2", &dss_data->base_ovr[1]);
	if (r)
		return r;

	r = dispc7_iomap_resource(pdev, "vp1", &dss_data->base_vp[0]);
	if (r)
		return r;

	r = dispc7_iomap_resource(pdev, "vp2", &dss_data->base_vp[1]);
	if (r)
		return r;

	dss_data->irq = platform_get_irq(dss_data->pdev, 0);
	if (dss_data->irq < 0) {
		dev_err(&pdev->dev, "platform_get_irq failed\n");
		return -ENODEV;
	}

	r = dispc7_init_gamma_tables(&pdev->dev);
	if (r)
		return r;

	r = dss7_init_ports(pdev);
	if (r) {
		dev_err(&pdev->dev, "Failed to init ports %d\n", r);
		return r;
	}

	dss_data->fclk = devm_clk_get(&pdev->dev, "fck");
	if (IS_ERR(dss_data->fclk)) {
		dev_err(&pdev->dev, "Failed to get fclk\n");
		r = PTR_ERR(dss_data->fclk);
		goto err_uninit_ports;
	}

	dss_data->vp_clk = devm_clk_get(&pdev->dev, "vp1");
	if (IS_ERR(dss_data->vp_clk)) {
		dev_err(&pdev->dev, "Failed to get vp clk\n");
		r = PTR_ERR(dss_data->vp_clk);
		goto err_uninit_ports;
	}

	dev_dbg(&pdev->dev, "DSS fclk %lu Hz\n", clk_get_rate(dss_data->fclk));

	pm_runtime_enable(&pdev->dev);

	pm_runtime_set_autosuspend_delay(&pdev->dev, 200);
	pm_runtime_use_autosuspend(&pdev->dev);

	dispc_set_ops(&dispc7_ops);

	omapdss_gather_components(&pdev->dev);
	omapdss_set_is_initialized(true);

	dispc7_runtime_get();
	dev_info(&pdev->dev, "OMAP DSS7 rev 0x%x\n",
		 dispc7_read(&pdev->dev, DSS_REVISION));
	dispc7_runtime_put();

	r = initialize_omapdrm_device();
	if (r) {
		dev_err(&pdev->dev,
			"initialize_omapdrm_device() failed %d\n", r);
		goto err_runtime_disable;
	}

	return 0;

err_runtime_disable:
	pm_runtime_disable(&pdev->dev);
	dispc_set_ops(NULL);
	omapdss_set_is_initialized(false);
err_uninit_ports:
	dss7_uninit_ports(pdev);

	return r;
}

static int dss7_remove(struct platform_device *pdev)
{
	uninitialize_omapdrm_device();

	dispc_set_ops(NULL);

	omapdss_set_is_initialized(false);

	dss7_uninit_ports(pdev);

	pm_runtime_disable(&pdev->dev);

	return 0;
}

static int dss7_runtime_suspend(struct device *dev)
{
	struct dss_data *dss_data = dssdata(dev);

	dev_dbg(dev, "suspend\n");

	dss_data->is_enabled = false;
	/* ensure the dispc7_irq_handler sees the is_enabled value */
	smp_wmb();
	/* wait for current handler to finish before turning the DISPC off */
	synchronize_irq(dss_data->irq);

	dispc7_save_context(dev);

	clk_disable_unprepare(dss_data->fclk);

	return 0;
}

static int dss7_runtime_resume(struct device *dev)
{
	struct dss_data *dss_data = dssdata(dev);

	dev_dbg(dev, "resume\n");

	clk_prepare_enable(dss_data->fclk);

	if (REG_GET(dev, DSS_SYSSTATUS, 0, 0) == 0)
		dev_warn(dev, "DSS FUNC RESET not done!\n");

	dev_dbg(dev, "VP RESETDONE %d,%d,%d",
		REG_GET(dev, DSS_SYSSTATUS, 1, 1),
		REG_GET(dev, DSS_SYSSTATUS, 2, 2),
		REG_GET(dev, DSS_SYSSTATUS, 3, 3));

	dev_dbg(dev, "OLDI RESETDONE %d,%d,%d",
		REG_GET(dev, DSS_SYSSTATUS, 5, 5),
		REG_GET(dev, DSS_SYSSTATUS, 6, 6),
		REG_GET(dev, DSS_SYSSTATUS, 7, 7));

	dev_dbg(dev, "DISPC IDLE %d",
		REG_GET(dev, DSS_SYSSTATUS, 9, 9));


	dispc7_initial_config(dev);

	dispc7_restore_context(dev);

	dispc7_restore_gamma_tables(dev);

	dss_data->is_enabled = true;
	/* ensure the dispc7_irq_handler sees the is_enabled value */
	smp_wmb();

	return 0;
}

static const struct dev_pm_ops dss7_pm_ops = {
	.runtime_suspend = dss7_runtime_suspend,
	.runtime_resume = dss7_runtime_resume,
};

static const struct of_device_id dss7_of_match[] = {
	{ .compatible = "ti,k3-dss", .data = &k3_dss_feats, },
	{},
};

MODULE_DEVICE_TABLE(of, dss7_of_match);

static struct platform_driver dss7_driver = {
	.probe		= dss7_probe,
	.remove		= dss7_remove,
	.driver         = {
		.name   = "omap_dss7",
		.pm	= &dss7_pm_ops,
		.of_match_table = dss7_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(dss7_driver);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ti.com>");
MODULE_DESCRIPTION("OMAP7 Display Subsystem");
MODULE_LICENSE("GPL v2");
