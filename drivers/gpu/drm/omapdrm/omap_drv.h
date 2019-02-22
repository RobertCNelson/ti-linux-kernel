/*
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Rob Clark <rob@ti.com>
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

#ifndef __OMAPDRM_DRV_H__
#define __OMAPDRM_DRV_H__

#include <linux/module.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem.h>
#include <drm/omap_drm.h>

#include "dss/omapdss.h"

#include "omap_connector.h"
#include "omap_crtc.h"
#include "omap_encoder.h"
#include "omap_fb.h"
#include "omap_fbdev.h"
#include "omap_gem.h"
#include "omap_irq.h"
#include "omap_plane.h"
#include "omap_overlay.h"

#define DBG(fmt, ...) DRM_DEBUG(fmt"\n", ##__VA_ARGS__)
#define VERB(fmt, ...) if (0) DRM_DEBUG(fmt, ##__VA_ARGS__) /* verbose debug */

#define MODULE_NAME     "omapdrm"

struct omap_drm_usergart;

struct omap_drm_pipeline {
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	struct omap_dss_device *output;
	unsigned int alias_id;
};

/*
 * Global private object state for tracking resources that are shared across
 * multiple kms objects (planes/crtcs/etc).
 */
#define to_omap_global_state(x) container_of(x, struct omap_global_state, base)
struct omap_global_state {
	struct drm_private_state base;

	struct drm_atomic_state *state;

	/* global atomic state of assignment between overlays and planes */
	struct drm_plane *hwoverlay_to_plane[8];
};

struct omap_drm_private {
	struct drm_device *ddev;
	struct device *dev;
	u32 omaprev;

	struct dss_device *dss;
	struct dispc_device *dispc;
	const struct dispc_ops *dispc_ops;

	unsigned int num_pipes;
	struct omap_drm_pipeline pipes[8];
	struct omap_drm_pipeline *channels[8];

	unsigned int num_planes;
	struct drm_plane *planes[8];

	unsigned int num_ovls;
	struct omap_hw_overlay *overlays[8];

	/*
	 * Global private object state, Do not access directly, use
	 * omap_global_get_state()
	 */
	struct drm_modeset_lock glob_obj_lock;
	struct drm_private_obj glob_obj;

	struct drm_fb_helper *fbdev;

	struct workqueue_struct *wq;

	/* lock for obj_list below */
	struct mutex list_lock;

	/* list of GEM objects: */
	struct list_head obj_list;

	struct omap_drm_usergart *usergart;
	bool has_dmm;

	/* properties: */
	struct drm_property *zorder_prop;
	struct drm_property *global_alpha_prop;
	struct drm_property *pre_mult_alpha_prop;

	/* crtc properties */
	struct drm_property *background_color_prop;
	struct drm_property *trans_key_mode_prop;
	struct drm_property *trans_key_prop;
	struct drm_property *alpha_blender_prop;

	/* irq handling: */
	spinlock_t wait_lock;		/* protects the wait_list */
	struct list_head wait_list;	/* list of omap_irq_wait */
	u32 irq_mask;			/* enabled irqs in addition to wait_list */

	/* memory bandwidth limit if it is needed on the platform */
	unsigned int max_bandwidth;

	void *wb_private;	      /* Write-back private data */
	bool wb_initialized;
};


int omap_debugfs_init(struct drm_minor *minor);
struct omap_global_state *__must_check
omap_get_global_state(struct drm_atomic_state *s);
struct omap_global_state *
omap_get_existing_global_state(struct omap_drm_private *priv);

#if IS_ENABLED(CONFIG_DRM_OMAP_WB)

#define OMAP_WB_IRQ_MASK (DISPC_IRQ_FRAMEDONEWB | \
			  DISPC_IRQ_WBBUFFEROVERFLOW | \
			  DISPC_IRQ_WBUNCOMPLETEERROR)

int omap_wb_init(struct drm_device *drmdev);
void omap_wb_cleanup(struct drm_device *drmdev);
void omap_wb_irq(void *priv, u32 irqstatus);

#else

#define OMAP_WB_IRQ_MASK (0)

static inline int omap_wb_init(struct drm_device *drmdev) { return 0; }
static inline void omap_wb_cleanup(struct drm_device *drmdev) { }
static inline void omap_wb_irq(void *priv, u32 irqstatus) { }

#endif

#endif /* __OMAPDRM_DRV_H__ */
