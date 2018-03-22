/*
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Rob Clark <rob.clark@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "omap_drv.h"

struct omap_irq_wait {
	struct list_head node;
	wait_queue_head_t wq;
	u64 irqmask;
	int count;
};

/* call with wait_lock and dispc runtime held */
static u64 omap_irq_full_mask(struct drm_device *dev)
{
	struct omap_drm_private *priv = dev->dev_private;
	struct omap_irq_wait *wait;
	u64 irqmask;

	assert_spin_locked(&priv->wait_lock);

	irqmask = priv->irq_mask;

	list_for_each_entry(wait, &priv->wait_list, node)
		irqmask |= wait->irqmask;

	DBG("irqmask 0x%016llx", irqmask);

	return irqmask;
}

static void omap_irq_wait_handler(struct omap_irq_wait *wait)
{
	wait->count--;
	wake_up(&wait->wq);
}

struct omap_irq_wait * omap_irq_wait_init(struct drm_device *dev,
		u64 waitmask, int count)
{
	struct omap_drm_private *priv = dev->dev_private;
	struct omap_irq_wait *wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	unsigned long flags;
	u64 irqmask;

	if (!wait)
		return NULL;

	init_waitqueue_head(&wait->wq);
	wait->irqmask = waitmask;
	wait->count = count;

	spin_lock_irqsave(&priv->wait_lock, flags);
	list_add(&wait->node, &priv->wait_list);
	irqmask = omap_irq_full_mask(dev);
	priv->dispc_ops->write_irqenable(irqmask);
	spin_unlock_irqrestore(&priv->wait_lock, flags);

	return wait;
}

int omap_irq_wait(struct drm_device *dev, struct omap_irq_wait *wait,
		unsigned long timeout)
{
	struct omap_drm_private *priv = dev->dev_private;
	unsigned long flags;
	u64 irqmask;
	int ret;

	ret = wait_event_timeout(wait->wq, (wait->count <= 0), timeout);

	spin_lock_irqsave(&priv->wait_lock, flags);
	list_del(&wait->node);
	irqmask = omap_irq_full_mask(dev);
	priv->dispc_ops->write_irqenable(irqmask);
	spin_unlock_irqrestore(&priv->wait_lock, flags);

	kfree(wait);

	return ret == 0 ? -1 : 0;
}

/**
 * enable_vblank - enable vblank interrupt events
 * @dev: DRM device
 * @pipe: which irq to enable
 *
 * Enable vblank interrupts for @crtc.  If the device doesn't have
 * a hardware vblank counter, this routine should be a no-op, since
 * interrupts will have to stay on to keep the count accurate.
 *
 * RETURNS
 * Zero on success, appropriate errno if the given @crtc's vblank
 * interrupt cannot be enabled.
 */
int omap_irq_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct omap_drm_private *priv = dev->dev_private;
	unsigned long flags;
	enum omap_channel channel = omap_crtc_channel(crtc);
	u64 irqmask;

	DBG("dev=%p, crtc=%u", dev, channel);

	spin_lock_irqsave(&priv->wait_lock, flags);
	priv->irq_mask |= DSS_IRQ_MGR_VSYNC_EVEN(channel) |
		DSS_IRQ_MGR_VSYNC_ODD(channel);
	irqmask = omap_irq_full_mask(dev);
	priv->dispc_ops->write_irqenable(irqmask);
	spin_unlock_irqrestore(&priv->wait_lock, flags);

	return 0;
}

/**
 * disable_vblank - disable vblank interrupt events
 * @dev: DRM device
 * @pipe: which irq to enable
 *
 * Disable vblank interrupts for @crtc.  If the device doesn't have
 * a hardware vblank counter, this routine should be a no-op, since
 * interrupts will have to stay on to keep the count accurate.
 */
void omap_irq_disable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct omap_drm_private *priv = dev->dev_private;
	unsigned long flags;
	enum omap_channel channel = omap_crtc_channel(crtc);
	u64 irqmask;

	DBG("dev=%p, crtc=%u", dev, channel);

	spin_lock_irqsave(&priv->wait_lock, flags);
	priv->irq_mask &= ~(DSS_IRQ_MGR_VSYNC_EVEN(channel) |
			    DSS_IRQ_MGR_VSYNC_ODD(channel));
	irqmask = omap_irq_full_mask(dev);
	priv->dispc_ops->write_irqenable(irqmask);
	spin_unlock_irqrestore(&priv->wait_lock, flags);
}

static void omap_irq_fifo_underflow(struct omap_drm_private *priv,
				    u64 irqstatus)
{
	static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	unsigned int i;
	u64 masked;

	spin_lock(&priv->wait_lock);
	masked = irqstatus & priv->irq_uf_mask & priv->irq_mask;
	spin_unlock(&priv->wait_lock);

	if (!masked)
		return;

	if (!__ratelimit(&_rs))
		return;

	DRM_ERROR("FIFO underflow on ");

	for (i = 0; i < DSS_MAX_OVLS; ++i) {
		if (masked & DSS_IRQ_OVL_FIFO_UNDERFLOW(i))
			pr_cont("%u:%s ", i, priv->dispc_ops->ovl_name(i));
	}

	pr_cont("(%016llx)\n", irqstatus);
}

static void omap_irq_ocp_error_handler(struct drm_device *dev,
				       u64 irqstatus)
{
	if (irqstatus & DSS_IRQ_DEVICE_OCP_ERR)
		dev_err_ratelimited(dev->dev, "OCP error\n");
}

static irqreturn_t omap_irq_handler(int irq, void *arg)
{
	struct drm_device *dev = (struct drm_device *) arg;
	struct omap_drm_private *priv = dev->dev_private;
	struct omap_irq_wait *wait, *n;
	unsigned long flags;
	unsigned int id;
	u64 irqstatus;

	irqstatus = priv->dispc_ops->read_and_clear_irqstatus();

	VERB("irqs: 0x%016llx\n", irqstatus);

	for (id = 0; id < priv->num_crtcs; id++) {
		struct drm_crtc *crtc = priv->crtcs[id];
		enum omap_channel channel = omap_crtc_channel(crtc);

		if (irqstatus & (DSS_IRQ_MGR_VSYNC_EVEN(channel) |
				 DSS_IRQ_MGR_VSYNC_ODD(channel))) {
			drm_handle_vblank(dev, id);
			omap_crtc_vblank_irq(crtc);
		}

		if (irqstatus & DSS_IRQ_MGR_SYNC_LOST(channel))
			omap_crtc_error_irq(crtc, irqstatus);
	}

	omap_irq_ocp_error_handler(dev, irqstatus);
	omap_irq_fifo_underflow(priv, irqstatus);

	spin_lock_irqsave(&priv->wait_lock, flags);
	list_for_each_entry_safe(wait, n, &priv->wait_list, node) {
		if (irqstatus & wait->irqmask)
			omap_irq_wait_handler(wait);
	}
	spin_unlock_irqrestore(&priv->wait_lock, flags);

	return IRQ_HANDLED;
}

/*
 * We need a special version, instead of just using drm_irq_install(),
 * because we need to register the irq via omapdss.  Once omapdss and
 * omapdrm are merged together we can assign the dispc hwmod data to
 * ourselves and drop these and just use drm_irq_{install,uninstall}()
 */

int omap_drm_irq_install(struct drm_device *dev)
{
	struct omap_drm_private *priv = dev->dev_private;
	unsigned int i;
	int ret;

	spin_lock_init(&priv->wait_lock);
	INIT_LIST_HEAD(&priv->wait_list);

	priv->irq_mask = DSS_IRQ_DEVICE_OCP_ERR;

	priv->irq_uf_mask = 0;
	for (i = 0; i < priv->num_planes; ++i)
		priv->irq_uf_mask |= DSS_IRQ_OVL_FIFO_UNDERFLOW(
			omap_plane_get_id(priv->planes[i]));
	priv->irq_mask |= priv->irq_uf_mask;

	for (i = 0; i < priv->num_crtcs; ++i)
		priv->irq_mask |= DSS_IRQ_MGR_SYNC_LOST(
			omap_crtc_channel(priv->crtcs[i]));

	ret = priv->dispc_ops->request_irq(omap_irq_handler, dev);
	if (ret < 0)
		return ret;

	dev->irq_enabled = true;

	return 0;
}

void omap_drm_irq_uninstall(struct drm_device *dev)
{
	struct omap_drm_private *priv = dev->dev_private;

	if (!dev->irq_enabled)
		return;

	dev->irq_enabled = false;

	priv->dispc_ops->free_irq(dev);
}
