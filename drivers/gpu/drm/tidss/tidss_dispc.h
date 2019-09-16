/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#ifndef __TIDSS_DISPC_H__
#define __TIDSS_DISPC_H__

struct dispc_device;
struct tidss_device;

struct drm_crtc_state;

#define DSS_MAX_CHANNELS 8
#define DSS_MAX_PLANES 8

/*
 * Based on the above 2 defines the bellow defines describe following
 * u64 IRQ bits:
 *
 * bit group |dev |mrg0|mrg1|mrg2|mrg3|mrg4|mrg5|mrg6|mrg7|plane 0-7|<unused> |
 * bit use   |Dfou|FEOL|FEOL|FEOL|FEOL|FEOL|FEOL|FEOL|FEOL|UUUU|UUUU| | | | | |
 * bit number|0-3 |4-7 |8-11|            12-35            |  36-43  |  44-63  |
 *
 * device bits:	D = OCP error
 * WB bits:	f = frame done wb, o = wb buffer overflow,
 *		u = wb buffer uncomplete
 * vp bits:	F = frame done, E = vsync even, O = vsync odd, L = sync lost
 * plane bits:	U = fifo underflow
 */

#define DSS_IRQ_DEVICE_OCP_ERR			BIT_ULL(0)

#define DSS_IRQ_DEVICE_FRAMEDONEWB		BIT_ULL(1)
#define DSS_IRQ_DEVICE_WBBUFFEROVERFLOW		BIT_ULL(2)
#define DSS_IRQ_DEVICE_WBUNCOMPLETEERROR	BIT_ULL(3)
#define DSS_IRQ_DEVICE_WBSECURITYVIOLATION	BIT_ULL(44)
#define DSS_IRQ_DEVICE_WBSYNC			BIT_ULL(45)
#define DSS_IRQ_DEVICE_WB_MASK			(GENMASK_ULL(3, 1) | BIT_ULL(44) | BIT_ULL(45))

#define DSS_IRQ_VP_BIT_N(ch, bit)	(4 + 4 * (ch) + (bit))
#define DSS_IRQ_PLANE_BIT_N(plane, bit) \
	(DSS_IRQ_VP_BIT_N(DSS_MAX_CHANNELS, 0) + 1 * (plane) + (bit))

#define DSS_IRQ_VP_BIT(ch, bit)	BIT_ULL(DSS_IRQ_VP_BIT_N((ch), (bit)))
#define DSS_IRQ_PLANE_BIT(plane, bit) \
	BIT_ULL(DSS_IRQ_PLANE_BIT_N((plane), (bit)))

#define DSS_IRQ_VP_MASK(ch) \
	GENMASK_ULL(DSS_IRQ_VP_BIT_N((ch), 3), DSS_IRQ_VP_BIT_N((ch), 0))
#define DSS_IRQ_PLANE_MASK(plane) \
	GENMASK_ULL(DSS_IRQ_PLANE_BIT_N((plane), 0), \
		    DSS_IRQ_PLANE_BIT_N((plane), 0))

#define DSS_IRQ_VP_FRAME_DONE(ch)	DSS_IRQ_VP_BIT((ch), 0)
#define DSS_IRQ_VP_VSYNC_EVEN(ch)	DSS_IRQ_VP_BIT((ch), 1)
#define DSS_IRQ_VP_VSYNC_ODD(ch)	DSS_IRQ_VP_BIT((ch), 2)
#define DSS_IRQ_VP_SYNC_LOST(ch)	DSS_IRQ_VP_BIT((ch), 3)

#define DSS_IRQ_PLANE_FIFO_UNDERFLOW(plane)	DSS_IRQ_PLANE_BIT((plane), 0)

enum tidss_gamma_type { TIDSS_GAMMA_8BIT, TIDSS_GAMMA_10BIT };

struct tidss_vp_feat {
	struct tidss_vp_color_feat {
		u32 gamma_size;
		enum tidss_gamma_type gamma_type;
		bool has_ctm;
	} color;
};

struct tidss_plane_feat {
	struct tidss_plane_color_feat {
		u32 encodings;
		u32 ranges;
		enum drm_color_encoding default_encoding;
		enum drm_color_range default_range;
	} color;
	struct tidss_plane_blend_feat {
		bool global_alpha;
	} blend;
};

struct tidss_dispc_ops {
	u64 (*read_and_clear_irqstatus)(struct dispc_device *dispc);
	void (*write_irqenable)(struct dispc_device *dispc, u64 enable);

	int (*get_num_vps)(struct dispc_device *dispc);
	const char *(*vp_name)(struct dispc_device *dispc,
			       u32 hw_videoport);
	const struct tidss_vp_feat *(*vp_feat)(struct dispc_device *dispc,
					       u32 hw_videoport);
	void (*vp_prepare)(struct dispc_device *dispc, u32 hw_videoport,
			   const struct drm_crtc_state *state);
	void (*vp_enable)(struct dispc_device *dispc, u32 hw_videoport,
			  const struct drm_crtc_state *state);
	void (*vp_disable)(struct dispc_device *dispc, u32 hw_videoport);
	void (*vp_unprepare)(struct dispc_device *dispc, u32 hw_videoport);
	bool (*vp_go_busy)(struct dispc_device *dispc,
			   u32 hw_videoport);
	void (*vp_go)(struct dispc_device *dispc, u32 hw_videoport);
	enum drm_mode_status (*vp_mode_valid)(struct dispc_device *dispc,
					u32 hw_videoport,
					const struct drm_display_mode *mode);
	int (*vp_check)(struct dispc_device *dispc, u32 hw_videoport,
			const struct drm_crtc_state *state);
	void (*vp_setup)(struct dispc_device *dispc, u32 hw_videoport,
			 const struct drm_crtc_state *state);

	int (*vp_set_clk_rate)(struct dispc_device *dispc,
			       u32 hw_videoport, unsigned long rate);
	int (*vp_enable_clk)(struct dispc_device *dispc, u32 hw_videoport);
	void (*vp_disable_clk)(struct dispc_device *dispc, u32 hw_videoport);

	int (*get_num_planes)(struct dispc_device *dispc);
	const char *(*plane_name)(struct dispc_device *dispc,
				  u32 hw_plane);
	const struct tidss_plane_feat *(*plane_feat)(struct dispc_device *dispc,
						     u32 hw_plane);
	int (*plane_enable)(struct dispc_device *dispc, u32 hw_plane,
			    bool enable);
	int (*plane_check)(struct dispc_device *dispc, u32 hw_plane,
			   const struct drm_plane_state *state,
			   u32 hw_videoport);
	int (*plane_setup)(struct dispc_device *dispc, u32 hw_plane,
			   const struct drm_plane_state *state,
			   u32 hw_videoport);

	int (*runtime_get)(struct dispc_device *dispc);
	void (*runtime_put)(struct dispc_device *dispc);

	int (*runtime_suspend)(struct dispc_device *dispc);
	int (*runtime_resume)(struct dispc_device *dispc);

	void (*remove)(struct dispc_device *dispc);

	int (*modeset_init)(struct dispc_device *dispc);

	int (*get_irq)(struct dispc_device *dispc);

	bool (*has_writeback)(struct dispc_device *dispc);
	int (*wb_setup)(struct dispc_device *dispc,
			const struct drm_plane_state *state,
			bool mem_to_mem, u32 src_hw_plane,
			u32 src_hw_videoport);
	int (*wb_enable)(struct dispc_device *dispc, bool enable);
	u32 (*wb_get_reserved_ovr)(struct dispc_device *dispc);
};

int dispc6_init(struct tidss_device *tidss);
int dispc7_init(struct tidss_device *tidss);

#endif
