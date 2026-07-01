// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Texas Instruments Incorporated - https://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>

#include "tidss_crtc.h"
#include "tidss_dispc.h"
#include "tidss_drv.h"
#include "tidss_plane.h"

void tidss_plane_error_irq(struct drm_plane *plane, u64 irqstatus)
{
	struct tidss_plane *tplane = to_tidss_plane(plane);

	dev_err_ratelimited(plane->dev->dev, "Plane%u underflow (irq %llx)\n",
			    tplane->hw_plane_id, irqstatus);
}

/* drm_plane_helper_funcs */

static int tidss_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct tidss_plane_state *tstate = to_tidss_plane_state(new_plane_state);
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane *tplane = to_tidss_plane(plane);
	const struct drm_format_info *finfo;
	struct drm_crtc_state *crtc_state;
	u32 hw_plane = tplane->hw_plane_id;
	u32 hw_videoport;
	int ret;

	dev_dbg(ddev->dev, "%s\n", __func__);

	if (!new_plane_state->crtc) {
		/*
		 * The visible field is not reset by the DRM core but only
		 * updated by drm_atomic_helper_check_plane_state(), set it
		 * manually.
		 */
		new_plane_state->visible = false;
		return 0;
	}

	crtc_state = drm_atomic_get_crtc_state(state,
					       new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  0,
						  INT_MAX, true, true);
	if (ret < 0)
		return ret;

	/*
	 * The HW is only able to start drawing at subpixel boundary
	 * (the two first checks below). At the end of a row the HW
	 * can only jump integer number of subpixels forward to the
	 * beginning of the next row. So we can only show picture with
	 * integer subpixel width (the third check). However, after
	 * reaching the end of the drawn picture the drawing starts
	 * again at the absolute memory address where top left corner
	 * position of the drawn picture is (so there is no need to
	 * check for odd height).
	 */

	finfo = drm_format_info(new_plane_state->fb->format->format);

	if ((new_plane_state->src_x >> 16) % finfo->hsub != 0) {
		dev_dbg(ddev->dev,
			"%s: x-position %u not divisible subpixel size %u\n",
			__func__, (new_plane_state->src_x >> 16), finfo->hsub);
		return -EINVAL;
	}

	if ((new_plane_state->src_y >> 16) % finfo->vsub != 0) {
		dev_dbg(ddev->dev,
			"%s: y-position %u not divisible subpixel size %u\n",
			__func__, (new_plane_state->src_y >> 16), finfo->vsub);
		return -EINVAL;
	}

	if ((new_plane_state->src_w >> 16) % finfo->hsub != 0) {
		dev_dbg(ddev->dev,
			"%s: src width %u not divisible by subpixel size %u\n",
			 __func__, (new_plane_state->src_w >> 16),
			 finfo->hsub);
		return -EINVAL;
	}

	if (!new_plane_state->visible)
		return 0;

	hw_videoport = to_tidss_crtc(new_plane_state->crtc)->hw_videoport;

	ret = dispc_plane_check(tidss->dispc, hw_plane, new_plane_state,
				hw_videoport);
	if (ret)
		return ret;

	tstate->self_refresh_capable =
		dispc_plane_can_selfrefresh(tidss->dispc, hw_plane, new_plane_state);

	dev_dbg(ddev->dev,
		"%s: plane%u self_refresh requested=%d capable=%d\n",
		__func__, hw_plane,
		tstate->self_refresh_requested,
		tstate->self_refresh_capable);

	return 0;
}

static void tidss_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane *tplane = to_tidss_plane(plane);
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	struct tidss_plane_state *old_tstate = to_tidss_plane_state(old_state);
	struct tidss_plane_state *new_tstate = to_tidss_plane_state(new_state);
	u32 hw_videoport;
	u32 hw_plane_id = tplane->hw_plane_id;

	dev_dbg(ddev->dev, "%s\n", __func__);

	if (!new_state->visible) {
		dispc_plane_enable(tidss->dispc, hw_plane_id, false);
		return;
	}

	hw_videoport = to_tidss_crtc(new_state->crtc)->hw_videoport;

	dev_dbg(ddev->dev,
		"plane%u: old(active=%d req=%d pend=%d) new(req=%d cap=%d)\n",
		hw_plane_id,
		tplane->self_refresh_active,
		old_tstate->self_refresh_requested,
		old_tstate->self_refresh_pending_enable,
		new_tstate->self_refresh_requested,
		new_tstate->self_refresh_capable);

	/*
	 * Self-refresh lock logic:
	 *
	 * When self-refresh is active and userspace still has SELF_REFRESH=1,
	 * the display is intentionally frozen. Any new framebuffer submitted
	 * by the application is silently discarded, the shadow registers are NOT
	 * updated and the hardware continues replaying from the FIFO buffer.
	 * The display only resumes normal operation when userspace explicitly
	 * sets SELF_REFRESH=0.
	 */
	if (tplane->self_refresh_active) {
		if (!new_tstate->self_refresh_requested) {
			/* User disabled self-refresh â exit and update HW */
			dev_dbg(ddev->dev,
				"plane%u: self-refresh disabled by userspace\n",
				hw_plane_id);
			dispc_plane_set_self_refresh(tidss->dispc, hw_plane_id, 0);
			tplane->self_refresh_active = false;
			new_tstate->self_refresh_pending_enable = false;
		} else {
			/*
			 * Self-refresh still requested â ignore new buffer.
			 * Hardware continues replaying from FIFO.
			 */
			dev_dbg(ddev->dev,
				"plane%u: self-refresh active, ignoring new FB\n",
				hw_plane_id);
			return;
		}
	}

	/* Write plane registers to shadow (skipped when self-refresh locked) */
	dispc_plane_setup(tidss->dispc, hw_plane_id, new_state, hw_videoport);

	/*
	 * Fresh self-refresh enable path (was not active before):
	 * Mark pending as actual enable happens in vblank IRQ after GO clears.
	 */
	if (!tplane->self_refresh_active &&
	    new_tstate->self_refresh_requested &&
	    new_tstate->self_refresh_capable &&
	    !new_tstate->self_refresh_pending_enable) {
		dev_dbg(ddev->dev,
			"plane%u: self-refresh requested, pending enable after GO clears\n",
			hw_plane_id);
		new_tstate->self_refresh_pending_enable = true;
	}
}

static void tidss_plane_atomic_enable(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane *tplane = to_tidss_plane(plane);

	dev_dbg(ddev->dev, "%s\n", __func__);

	dispc_plane_enable(tidss->dispc, tplane->hw_plane_id, true);
}

static void tidss_plane_atomic_disable(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane *tplane = to_tidss_plane(plane);

	dev_dbg(ddev->dev, "%s\n", __func__);

	dispc_plane_enable(tidss->dispc, tplane->hw_plane_id, false);
}

static void drm_plane_destroy(struct drm_plane *plane)
{
	struct tidss_plane *tplane = to_tidss_plane(plane);

	drm_plane_cleanup(plane);
	kfree(tplane);
}

/* Property handling for self-refresh */
static int tidss_plane_atomic_set_property(struct drm_plane *plane,
					   struct drm_plane_state *state,
					   struct drm_property *property,
					   uint64_t val)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane_state *tstate = to_tidss_plane_state(state);

	if (property == tidss->self_refresh_property) {
		tstate->self_refresh_requested = !!val;
		return 0;
	}

	return -EINVAL;
}

static int tidss_plane_atomic_get_property(struct drm_plane *plane,
					   const struct drm_plane_state *state,
					   struct drm_property *property,
					   uint64_t *val)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	const struct tidss_plane_state *tstate = to_tidss_plane_state(state);

	if (property == tidss->self_refresh_property) {
		*val = tstate->self_refresh_requested;
		return 0;
	}

	return -EINVAL;
}

/* State management for self-refresh tracking */
static void tidss_plane_reset(struct drm_plane *plane)
{
	struct tidss_plane_state *tstate;

	if (plane->state) {
		__drm_atomic_helper_plane_destroy_state(plane->state);
		kfree(to_tidss_plane_state(plane->state));
	}

	tstate = kzalloc(sizeof(*tstate), GFP_KERNEL);
	if (!tstate) {
		plane->state = NULL;
		return;
	}

	__drm_atomic_helper_plane_reset(plane, &tstate->base);

	/* Initialize self-refresh state */
	tstate->self_refresh_requested = false;
	tstate->self_refresh_capable = false;
	tstate->self_refresh_pending_enable = false;
}

static struct drm_plane_state *
tidss_plane_duplicate_state(struct drm_plane *plane)
{
	struct tidss_plane_state *tstate, *current_state;

	if (WARN_ON(!plane->state))
		return NULL;

	current_state = to_tidss_plane_state(plane->state);

	tstate = kmemdup(current_state, sizeof(*tstate), GFP_KERNEL);
	if (!tstate)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &tstate->base);

	return &tstate->base;
}

static void tidss_plane_destroy_state(struct drm_plane *plane,
				      struct drm_plane_state *state)
{
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(to_tidss_plane_state(state));
}

static const struct drm_plane_helper_funcs tidss_plane_helper_funcs = {
	.atomic_check = tidss_plane_atomic_check,
	.atomic_update = tidss_plane_atomic_update,
	.atomic_enable = tidss_plane_atomic_enable,
	.atomic_disable = tidss_plane_atomic_disable,
};

static const struct drm_plane_helper_funcs tidss_primary_plane_helper_funcs = {
	.atomic_check = tidss_plane_atomic_check,
	.atomic_update = tidss_plane_atomic_update,
	.atomic_enable = tidss_plane_atomic_enable,
	.atomic_disable = tidss_plane_atomic_disable,
	.get_scanout_buffer = drm_fb_dma_get_scanout_buffer,
};

static const struct drm_plane_funcs tidss_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = tidss_plane_reset,
	.destroy = drm_plane_destroy,
	.atomic_duplicate_state = tidss_plane_duplicate_state,
	.atomic_destroy_state = tidss_plane_destroy_state,
	.atomic_set_property = tidss_plane_atomic_set_property,
	.atomic_get_property = tidss_plane_atomic_get_property,
};

struct tidss_plane *tidss_plane_create(struct tidss_device *tidss,
				       u32 hw_plane_id, u32 plane_type,
				       u32 crtc_mask, const u32 *formats,
				       u32 num_formats)
{
	struct tidss_plane *tplane;
	enum drm_plane_type type;
	u32 possible_crtcs;
	u32 num_planes = tidss->feat->num_vids;
	u32 color_encodings = (BIT(DRM_COLOR_YCBCR_BT601) |
			       BIT(DRM_COLOR_YCBCR_BT709));
	u32 color_ranges = (BIT(DRM_COLOR_YCBCR_FULL_RANGE) |
			    BIT(DRM_COLOR_YCBCR_LIMITED_RANGE));
	u32 default_encoding = DRM_COLOR_YCBCR_BT601;
	u32 default_range = DRM_COLOR_YCBCR_FULL_RANGE;
	u32 blend_modes = (BIT(DRM_MODE_BLEND_PREMULTI) |
			   BIT(DRM_MODE_BLEND_COVERAGE));
	int ret;

	tplane = kzalloc(sizeof(*tplane), GFP_KERNEL);
	if (!tplane)
		return ERR_PTR(-ENOMEM);

	tplane->hw_plane_id = hw_plane_id;

	possible_crtcs = crtc_mask;
	type = plane_type;

	ret = drm_universal_plane_init(&tidss->ddev, &tplane->plane,
				       possible_crtcs,
				       &tidss_plane_funcs,
				       formats, num_formats,
				       NULL, type, NULL);
	if (ret < 0)
		goto err;

	if (type == DRM_PLANE_TYPE_PRIMARY)
		drm_plane_helper_add(&tplane->plane, &tidss_primary_plane_helper_funcs);
	else
		drm_plane_helper_add(&tplane->plane, &tidss_plane_helper_funcs);

	drm_plane_create_zpos_property(&tplane->plane, tidss->num_planes, 0,
				       num_planes - 1);

	ret = drm_plane_create_color_properties(&tplane->plane,
						color_encodings,
						color_ranges,
						default_encoding,
						default_range);
	if (ret)
		goto err;

	ret = drm_plane_create_alpha_property(&tplane->plane);
	if (ret)
		goto err;

	ret = drm_plane_create_blend_mode_property(&tplane->plane, blend_modes);
	if (ret)
		goto err;

	/* Attach self-refresh property */
	if (tidss->self_refresh_property) {
		drm_object_attach_property(&tplane->plane.base,
					   tidss->self_refresh_property, 0);
	}

	return tplane;

err:
	kfree(tplane);
	return ERR_PTR(ret);
}
