/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Texas Instruments Incorporated - https://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#ifndef __TIDSS_DRV_H__
#define __TIDSS_DRV_H__

#include <linux/spinlock.h>

#include <drm/drm_device.h>

#define TIDSS_MAX_PORTS 4
#define TIDSS_MAX_PLANES 4
#define TIDSS_MAX_AOD_DEVS_PER_PORT 10
#define TIDSS_MAX_OLDI_TXES 2

typedef u32 dispc_irq_t;
struct tidss_oldi;

struct tidss_device {
	struct drm_device ddev;		/* DRM device for DSS */
	struct device *dev;		/* Underlying DSS device */

	const struct dispc_features *feat;
	struct dispc_device *dispc;
	bool is_ext_vp_clk[TIDSS_MAX_PORTS];


	unsigned int num_crtcs;
	struct drm_crtc *crtcs[TIDSS_MAX_PORTS];

	unsigned int num_planes;
	struct drm_plane *planes[TIDSS_MAX_PLANES];

	unsigned int num_oldis;
	struct tidss_oldi *oldis[TIDSS_MAX_OLDI_TXES];

	unsigned int irq;

	/* protects the irq masks field and irqenable/irqstatus registers */
	spinlock_t irq_lock;
	dispc_irq_t irq_mask;	/* enabled irqs */

	bool shared_mode; /* DSS resources shared between remote core and Linux */
	/* 1: VP owned by Linux 0: VP is owned by remote and shared with Linux */
	u32 shared_mode_owned_vps[TIDSS_MAX_PORTS];
	bool shared_mode_own_oldi; /* Linux needs to configure OLDI in shared mode */

	int num_domains; /* Handle attached PM domains */
	struct device **pd_dev;
	struct device_link **pd_link;

	u32 boot_enabled_vp_mask;
	bool simplefb_enabled;

	/* Custom properties */
	struct drm_property *self_refresh_property;
	struct drm_property *always_on_display_property;

	/*
	 * Per-VP flag tracking whether GENPD_FLAG_ALWAYS_ON has been set for
	 * this video port's display pipeline.
	 */
	bool always_on_pd[TIDSS_MAX_PORTS];

	/*
	 * Platform devices for external bridges (e.g. cdns-dsi) and their
	 * PM suppliers (e.g. D-PHY) attached to respective TIDSS video port.
	 */
	struct device_node *aod_bridge_of_node[TIDSS_MAX_PORTS];
	bool aod_bridge_devs_collected[TIDSS_MAX_PORTS];
	struct device *aod_bridge_devs[TIDSS_MAX_PORTS][TIDSS_MAX_AOD_DEVS_PER_PORT];
	unsigned int aod_num_bridge_devs[TIDSS_MAX_PORTS];
};

#define to_tidss(__dev) container_of(__dev, struct tidss_device, ddev)

int tidss_runtime_get(struct tidss_device *tidss);
void tidss_runtime_put(struct tidss_device *tidss);
void tidss_aod_set_genpd_always_on(struct tidss_device *tidss, u32 hw_videoport, bool on);

#endif
