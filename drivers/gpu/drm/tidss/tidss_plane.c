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
#include "tidss_drv.h"
#include "tidss_plane.h"

dma_addr_t dispc7_plane_state_paddr(const struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *gem;
	u32 x = state->src_x >> 16;
	u32 y = state->src_y >> 16;

	gem = drm_fb_cma_get_gem_obj(state->fb, 0);

	return gem->paddr + fb->offsets[0] + x * fb->format->cpp[0] +
		y * fb->pitches[0];
}

dma_addr_t dispc7_plane_state_p_uv_addr(const struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *gem;
	u32 x = state->src_x >> 16;
	u32 y = state->src_y >> 16;

	if (WARN_ON(state->fb->format->num_planes != 2))
		return 0;

	gem = drm_fb_cma_get_gem_obj(fb, 1);

	return gem->paddr + fb->offsets[1] +
		(x * fb->format->cpp[1] / fb->format->hsub) +
		(y * fb->pitches[1] / fb->format->vsub);
}

static int tidss_plane_atomic_check(struct drm_plane *plane,
				    struct drm_plane_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = ddev->dev_private;
	struct drm_crtc_state *crtc_state;
	struct tidss_plane *tplane = to_tidss_plane(plane);
	const struct drm_format_info *finfo;
	u32 hw_videoport;
	int ret;

	dev_dbg(ddev->dev, "%s\n", __func__);

	if (tplane->reserved_wb)
		return -EBUSY;

	if (!state->crtc) {
		/*
		 * The visible field is not reset by the DRM core but only
		 * updated by drm_plane_helper_check_state(), set it manually.
		 */
		state->visible = false;
		return 0;
	}

	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  0,
						  INT_MAX,
						  true, true);
	if (ret < 0)
		return ret;

	/*
	 * The HW is only able to start drawing at subpixel boundary
	 * (the two first checks bellow). At the end of a row the HW
	 * can only jump integer number of subpixels forward to
	 * beginning of next row. So we can only show picture with
	 * integer subpixel width (the third check). However, after
	 * reaching the end of the drawn picture the drawing starts
	 * again at the absolute memory address where top left corner
	 * position of the drawn picture is (so there is no need to
	 * check for odd height).
	 */

	finfo = drm_format_info(state->fb->format->format);

	if ((state->src_x >> 16) % finfo->hsub != 0) {
		dev_dbg(ddev->dev,
			"%s: x-position %u not divisible subpixel size %u\n",
			__func__, (state->src_x >> 16), finfo->hsub);
		return -EINVAL;
	}

	if ((state->src_y >> 16) % finfo->vsub != 0) {
		dev_dbg(ddev->dev,
			"%s: y-position %u not divisible subpixel size %u\n",
			__func__, (state->src_y >> 16), finfo->vsub);
		return -EINVAL;
	}

	if ((state->src_w >> 16) % finfo->hsub != 0) {
		dev_dbg(ddev->dev,
			"%s: src width %u not divisible by subpixel size %u\n",
			 __func__, (state->src_w >> 16), finfo->hsub);
		return -EINVAL;
	}

	if (!state->visible)
		return 0;

	hw_videoport = to_tidss_crtc(state->crtc)->hw_videoport;

	return tidss->dispc_ops->plane_check(tidss->dispc,
					     tplane->hw_plane_id,
					     state, hw_videoport);
}

static void tidss_plane_atomic_update(struct drm_plane *plane,
				      struct drm_plane_state *old_state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = ddev->dev_private;
	struct tidss_plane *tplane = to_tidss_plane(plane);
	struct drm_plane_state *state = plane->state;
	u32 hw_videoport;
	int ret;

	dev_dbg(ddev->dev, "%s\n", __func__);

	if (!state->visible) {
		tidss->dispc_ops->plane_enable(tidss->dispc, tplane->hw_plane_id,
					       false);
		return;
	}

	hw_videoport = to_tidss_crtc(state->crtc)->hw_videoport;

	ret = tidss->dispc_ops->plane_setup(tidss->dispc, tplane->hw_plane_id,
					    state, hw_videoport);

	if (ret) {
		dev_err(plane->dev->dev, "Failed to setup plane %d\n",
			tplane->hw_plane_id);
		tidss->dispc_ops->plane_enable(tidss->dispc, tplane->hw_plane_id,
					       false);
		return;
	}

	tidss->dispc_ops->plane_enable(tidss->dispc, tplane->hw_plane_id, true);
}

static void tidss_plane_atomic_disable(struct drm_plane *plane,
				       struct drm_plane_state *old_state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = ddev->dev_private;
	struct tidss_plane *tplane = to_tidss_plane(plane);

	dev_dbg(ddev->dev, "%s\n", __func__);

	tidss->dispc_ops->plane_enable(tidss->dispc, tplane->hw_plane_id, false);
}

static const struct drm_plane_helper_funcs tidss_plane_helper_funcs = {
	.atomic_check = tidss_plane_atomic_check,
	.atomic_update = tidss_plane_atomic_update,
	.atomic_disable = tidss_plane_atomic_disable,
};

static const struct drm_plane_funcs tidss_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = drm_atomic_helper_plane_reset,
	.destroy = drm_plane_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

struct tidss_plane *tidss_plane_create(struct tidss_device *tidss,
				       u32 hw_plane_id,	u32 plane_type,
				       u32 crtc_mask, const u32 *formats,
				       u32 num_formats)
{
	const struct tidss_plane_feat *pfeat;
	struct tidss_plane *tplane;
	enum drm_plane_type type;
	u32 possible_crtcs;
	u32 num_planes = tidss->dispc_ops->get_num_planes(tidss->dispc);
	int ret;

	pfeat = tidss->dispc_ops->plane_feat(tidss->dispc, hw_plane_id);

	tplane = devm_kzalloc(tidss->dev, sizeof(*tplane), GFP_KERNEL);
	if (!tplane)
		return ERR_PTR(-ENOMEM);

	tplane->hw_plane_id = hw_plane_id;

	possible_crtcs = crtc_mask;
	type = plane_type;

	ret = drm_universal_plane_init(tidss->ddev, &tplane->plane,
				       possible_crtcs,
				       &tidss_plane_funcs,
				       formats, num_formats,
				       NULL, type, NULL);
	if (ret < 0)
		return ERR_PTR(ret);

	drm_plane_helper_add(&tplane->plane, &tidss_plane_helper_funcs);
	if (num_planes > 1)
		drm_plane_create_zpos_property(&tplane->plane, hw_plane_id, 0,
					       num_planes - 1);

	ret = drm_plane_create_color_properties(&tplane->plane,
						pfeat->color.encodings,
						pfeat->color.ranges,
						pfeat->color.default_encoding,
						pfeat->color.default_range);
	if (ret)
		return ERR_PTR(ret);

	if (pfeat->blend.global_alpha) {
		ret = drm_plane_create_alpha_property(&tplane->plane);
		if (ret)
			return ERR_PTR(ret);
	}

	return tplane;
}

struct drm_plane *tidss_plane_reserve_wb(struct drm_device *dev)
{
	struct tidss_device *tidss = dev->dev_private;
	int i;
	u32 ovr_id = tidss->dispc_ops->wb_get_reserved_ovr(tidss->dispc);

	for (i = tidss->num_planes - 1; i >= 0; --i) {
		struct drm_plane *plane = tidss->planes[i];
		struct tidss_plane *tplane = to_tidss_plane(plane);

		if (plane->state->crtc || plane->state->fb)
			continue;

		if (tplane->reserved_wb)
			continue;

		/*
		 * We found an available plane so just mark the
		 * associated video port as the one found in the last step
		 */
		tplane->reserved_wb = true;

		dev_dbg(dev->dev, "%s: found plane %s (%d) on %s (%d)\n",
			__func__,
			tidss->dispc_ops->plane_name(tidss->dispc, tplane->hw_plane_id),
			tplane->hw_plane_id,
			tidss->dispc_ops->vp_name(tidss->dispc, ovr_id), ovr_id);

		return plane;
	}
	return NULL;
}

void tidss_plane_release_wb(struct drm_plane *plane)
{
	struct tidss_plane *tplane = to_tidss_plane(plane);

	WARN_ON(!tplane->reserved_wb);

	tplane->reserved_wb = false;
}
