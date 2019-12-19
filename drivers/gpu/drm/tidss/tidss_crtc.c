// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_plane_helper.h>

#include "tidss_crtc.h"
#include "tidss_dispc.h"
#include "tidss_drv.h"
#include "tidss_irq.h"

/* -----------------------------------------------------------------------------
 * Page Flip
 */

static void tidss_crtc_finish_page_flip(struct tidss_crtc *tcrtc)
{
	struct drm_device *ddev = tcrtc->crtc.dev;
	struct tidss_device *tidss = ddev->dev_private;
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	bool busy;

	spin_lock_irqsave(&ddev->event_lock, flags);

	/*
	 * New settings are taken into use at VFP, and GO bit is cleared at
	 * the same time. This happens before the vertical blank interrupt.
	 * So there is a small change that the driver sets GO bit after VFP, but
	 * before vblank, and we have to check for that case here.
	 */
	busy = tidss->dispc_ops->vp_go_busy(tidss->dispc, tcrtc->hw_videoport);
	if (busy) {
		spin_unlock_irqrestore(&ddev->event_lock, flags);
		return;
	}

	event = tcrtc->event;
	tcrtc->event = NULL;

	if (!event) {
		spin_unlock_irqrestore(&ddev->event_lock, flags);
		return;
	}

	drm_crtc_send_vblank_event(&tcrtc->crtc, event);

	spin_unlock_irqrestore(&ddev->event_lock, flags);

	drm_crtc_vblank_put(&tcrtc->crtc);
}

void tidss_crtc_vblank_irq(struct drm_crtc *crtc)
{
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);

	drm_crtc_handle_vblank(crtc);

	tidss_crtc_finish_page_flip(tcrtc);
}

void tidss_crtc_framedone_irq(struct drm_crtc *crtc)
{
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);

	complete(&tcrtc->framedone_completion);
}

void tidss_crtc_error_irq(struct drm_crtc *crtc, u64 irqstatus)
{
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);

	dev_err_ratelimited(crtc->dev->dev, "CRTC%u SYNC LOST: (irq %llx)\n",
			    tcrtc->hw_videoport, irqstatus);
}

/* -----------------------------------------------------------------------------
 * CRTC Functions
 */

static int tidss_crtc_atomic_check(struct drm_crtc *crtc,
				   struct drm_crtc_state *state)
{
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = ddev->dev_private;
	int r;

	dev_dbg(ddev->dev, "%s\n", __func__);

	if (!state->enable)
		return 0;

	r = tidss->dispc_ops->vp_check(tidss->dispc, tcrtc->hw_videoport,
				       state);

	return r;
}

static void tidss_crtc_atomic_flush(struct drm_crtc *crtc,
				    struct drm_crtc_state *old_crtc_state)
{
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = ddev->dev_private;
	unsigned long flags;

	dev_dbg(ddev->dev, "%s, crtc enabled %d, event %p\n",
		__func__, tcrtc->enabled, crtc->state->event);

	/* Only flush the CRTC if it is currently enabled. */
	if (!tcrtc->enabled)
		return;

	/* If the GO bit is stuck we better quit here. */
	if (WARN_ON(tidss->dispc_ops->vp_go_busy(tidss->dispc,
						 tcrtc->hw_videoport)))
		return;

	/* We should have event if CRTC is enabled through out this commit. */
	if (WARN_ON(!crtc->state->event))
		return;

	tidss->dispc_ops->vp_setup(tidss->dispc,
				   tcrtc->hw_videoport,
				   crtc->state);

	WARN_ON(drm_crtc_vblank_get(crtc) != 0);

	spin_lock_irqsave(&ddev->event_lock, flags);
	tidss->dispc_ops->vp_go(tidss->dispc, tcrtc->hw_videoport);

	WARN_ON(tcrtc->event);

	tcrtc->event = crtc->state->event;
	crtc->state->event = NULL;

	spin_unlock_irqrestore(&ddev->event_lock, flags);
}

static void tidss_crtc_atomic_enable(struct drm_crtc *crtc,
				     struct drm_crtc_state *old_state)
{
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = ddev->dev_private;
	const struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	int r;
	unsigned long flags;

	dev_dbg(ddev->dev, "%s, event %p\n", __func__, crtc->state->event);

	tidss->dispc_ops->runtime_get(tidss->dispc);

	r = tidss->dispc_ops->vp_set_clk_rate(tidss->dispc, tcrtc->hw_videoport,
					      mode->clock * 1000);
	if (r != 0)
		return;

	r = tidss->dispc_ops->vp_enable_clk(tidss->dispc, tcrtc->hw_videoport);
	if (r != 0)
		return;

	tidss->dispc_ops->vp_setup(tidss->dispc, tcrtc->hw_videoport,
				   crtc->state);

	/* Turn vertical blanking interrupt reporting on. */
	drm_crtc_vblank_on(crtc);

	if (tidss->dispc_ops->vp_prepare)
		tidss->dispc_ops->vp_prepare(tidss->dispc, tcrtc->hw_videoport,
					     crtc->state);

	tcrtc->enabled = true;

	tidss->dispc_ops->vp_enable(tidss->dispc, tcrtc->hw_videoport,
				    crtc->state);

	spin_lock_irqsave(&ddev->event_lock, flags);

	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}

	spin_unlock_irqrestore(&ddev->event_lock, flags);
}

static void tidss_crtc_atomic_disable(struct drm_crtc *crtc,
				      struct drm_crtc_state *old_state)
{
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = ddev->dev_private;
	unsigned long flags;

	dev_dbg(ddev->dev, "%s, event %p\n", __func__, crtc->state->event);

	reinit_completion(&tcrtc->framedone_completion);

	tidss->dispc_ops->vp_disable(tidss->dispc, tcrtc->hw_videoport);

	if (!wait_for_completion_timeout(&tcrtc->framedone_completion,
					 msecs_to_jiffies(500)))
		dev_err(tidss->dev, "Timeout waiting for framedone on crtc %d",
			tcrtc->hw_videoport);

	if (tidss->dispc_ops->vp_unprepare)
		tidss->dispc_ops->vp_unprepare(tidss->dispc,
					       tcrtc->hw_videoport);

	spin_lock_irqsave(&ddev->event_lock, flags);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irqrestore(&ddev->event_lock, flags);

	tcrtc->enabled = false;

	drm_crtc_vblank_off(crtc);

	tidss->dispc_ops->vp_disable_clk(tidss->dispc, tcrtc->hw_videoport);

	tidss->dispc_ops->runtime_put(tidss->dispc);
}

static
enum drm_mode_status tidss_crtc_mode_valid(struct drm_crtc *crtc,
					   const struct drm_display_mode *mode)
{
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = ddev->dev_private;

	return tidss->dispc_ops->vp_mode_valid(tidss->dispc,
					       tcrtc->hw_videoport,
					       mode);
}

static const struct drm_crtc_helper_funcs crtc_helper_funcs = {
	.atomic_check = tidss_crtc_atomic_check,
	.atomic_flush = tidss_crtc_atomic_flush,
	.atomic_enable = tidss_crtc_atomic_enable,
	.atomic_disable = tidss_crtc_atomic_disable,

	.mode_valid = tidss_crtc_mode_valid,
};

static void tidss_crtc_reset(struct drm_crtc *crtc)
{
	struct tidss_crtc_state *tcrtc;

	if (crtc->state)
		__drm_atomic_helper_crtc_destroy_state(crtc->state);

	kfree(crtc->state);

	tcrtc = kzalloc(sizeof(*tcrtc), GFP_KERNEL);
	if (!tcrtc) {
		crtc->state = NULL;
		return;
	}

	crtc->state = &tcrtc->base;
	crtc->state->crtc = crtc;
}

static struct drm_crtc_state *
tidss_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct tidss_crtc_state *state, *current_state;

	if (WARN_ON(!crtc->state))
		return NULL;

	current_state = to_tidss_crtc_state(crtc->state);

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &state->base);

	state->bus_format = current_state->bus_format;
	state->bus_flags = current_state->bus_flags;

	return &state->base;
}


static int tidss_crtc_atomic_set_property(struct drm_crtc *crtc,
					  struct drm_crtc_state *state,
					  struct drm_property *property,
					  uint64_t val)
{
	return -EINVAL;
}

static int tidss_crtc_atomic_get_property(struct drm_crtc *crtc,
					  const struct drm_crtc_state *state,
					  struct drm_property *property,
					  uint64_t *val)
{
	return -EINVAL;
}

static int tidss_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = ddev->dev_private;

	dev_dbg(ddev->dev, "%s\n", __func__);

	tidss->dispc_ops->runtime_get(tidss->dispc);

	tidss_irq_enable_vblank(crtc);

	return 0;
}

static void tidss_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = ddev->dev_private;

	dev_dbg(ddev->dev, "%s\n", __func__);

	tidss_irq_disable_vblank(crtc);

	tidss->dispc_ops->runtime_put(tidss->dispc);
}

static const struct drm_crtc_funcs crtc_funcs = {
	.reset = tidss_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = tidss_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.atomic_set_property = tidss_crtc_atomic_set_property,
	.atomic_get_property = tidss_crtc_atomic_get_property,
	.enable_vblank = tidss_crtc_enable_vblank,
	.disable_vblank = tidss_crtc_disable_vblank,
};

static void tidss_crtc_install_properties(struct tidss_device *tidss,
					  const struct tidss_vp_feat *vp_feat,
					  struct drm_crtc *crtc)
{
}

struct tidss_crtc *tidss_crtc_create(struct tidss_device *tidss, u32 hw_videoport,
				     struct drm_plane *primary)
{
	struct tidss_crtc *tcrtc;
	struct drm_crtc *crtc;
	const struct tidss_vp_feat *vp_feat;
	unsigned int gamma_lut_size = 0;
	int ret;

	vp_feat = tidss->dispc_ops->vp_feat(tidss->dispc, hw_videoport);

	tcrtc = devm_kzalloc(tidss->dev, sizeof(*tcrtc), GFP_KERNEL);
	if (!tcrtc)
		return ERR_PTR(-ENOMEM);

	tcrtc->hw_videoport = hw_videoport;
	init_completion(&tcrtc->framedone_completion);

	crtc =  &tcrtc->crtc;

	ret = drm_crtc_init_with_planes(tidss->ddev, crtc, primary,
					NULL, &crtc_funcs, NULL);
	if (ret < 0)
		return ERR_PTR(ret);

	drm_crtc_helper_add(crtc, &crtc_helper_funcs);

	/*
	 * The dispc API adapts to what ever size we ask from it no
	 * matter what HW supports. X-server assumes 256 element gamma
	 * tables so lets use that. Size of HW gamma table size is
	 * found from struct tidss_vp_feat that is extracted with
	 * dispc_vp_feats(). If gamma_size is 0 gamma table is not
	 * supported.
	 */
	if (vp_feat->color.gamma_size)
		gamma_lut_size = 256;

	drm_crtc_enable_color_mgmt(crtc, 0, vp_feat->color.has_ctm,
				   gamma_lut_size);
	if (gamma_lut_size)
		drm_mode_crtc_set_gamma_size(crtc, gamma_lut_size);

	tidss_crtc_install_properties(tidss, vp_feat, crtc);

	return tcrtc;
}
