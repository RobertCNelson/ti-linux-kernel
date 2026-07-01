/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Texas Instruments Incorporated - https://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#ifndef __TIDSS_PLANE_H__
#define __TIDSS_PLANE_H__

#include <drm/drm_plane.h>

#define to_tidss_plane(p) container_of((p), struct tidss_plane, plane)
#define to_tidss_plane_state(s) container_of((s), struct tidss_plane_state, base)

struct tidss_device;

struct tidss_plane {
	struct drm_plane plane;

	u32 hw_plane_id;
	bool self_refresh_active;
};

struct tidss_plane_state {
	struct drm_plane_state base;

	/* Self-refresh state tracking */
	bool self_refresh_requested;      /* User/driver wants self-refresh */
	bool self_refresh_capable;        /* Frame fits in DMA buffer */
	bool self_refresh_pending_enable; /* Waiting for buffer load to complete */
};

struct tidss_plane *tidss_plane_create(struct tidss_device *tidss,
				       u32 hw_plane_id, u32 plane_type,
				       u32 crtc_mask, const u32 *formats,
				       u32 num_formats);

void tidss_plane_error_irq(struct drm_plane *plane, u64 irqstatus);

#endif
