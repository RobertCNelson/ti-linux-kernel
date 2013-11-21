/*
 * Some code and ideas taken from drivers/video/omap/ driver
 * by Imre Deak.
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

#define DSS_SUBSYS_NAME "DRA7XX_DPI"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/of.h>

#include <video/omapdss.h>

#include "dss.h"
#include "dss_features.h"

struct dpi_data {
	enum dss_dpll dpll;

	struct mutex lock;

	u32 module_id;
	enum omap_channel channel;

	struct omap_video_timings timings;
	struct dss_lcd_mgr_config mgr_config;
	int data_lines;

	struct omap_dss_device output;
};

/*
 * On DRA7xx, we will try to use the DPLL_VIDEOx PLLs, only if we can't get one,
 * we will try to modify the DSS_FCLK to get the pixel clock. Leave HDMI PLL out
 * for now
 */
enum dss_dpll dpi_get_dpll(struct dpi_data *dpi)
{
	switch (dpi->module_id) {
	case 0:
		if (dss_dpll_disabled(DSS_DPLL_VIDEO1))
			return DSS_DPLL_VIDEO1;
		else
			return DSS_DPLL_NONE;
	case 1:
	case 2:
		if (dss_dpll_disabled(DSS_DPLL_VIDEO1))
			return DSS_DPLL_VIDEO1;
		else if (dss_dpll_disabled(DSS_DPLL_VIDEO2))
			return DSS_DPLL_VIDEO2;
		else
			return DSS_DPLL_NONE;
	default:
		return DSS_DPLL_NONE;
	}

	return DSS_DPLL_NONE;
}

struct dpi_clk_calc_ctx {
	enum dss_dpll dpll;

	/* inputs */
	unsigned long pck_min, pck_max;

	/* outputs */
	struct dss_dpll_cinfo dpll_cinfo;
	unsigned long long fck;
	struct dispc_clock_info dispc_cinfo;
};

static bool dpi_calc_dispc_cb(int lckd, int pckd, unsigned long lck,
		unsigned long pck, void *data)
{
	struct dpi_clk_calc_ctx *ctx = data;

	/*
	 * Odd dividers give us uneven duty cycle, causing problem when level
	 * shifted. So skip all odd dividers when the pixel clock is on the
	 * higher side.
	 */
	if (ctx->pck_min >= 100000000) {
		if (lckd > 1 && lckd % 2 != 0)
			return false;

		if (pckd > 1 && pckd % 2 != 0)
			return false;
	}

	ctx->dispc_cinfo.lck_div = lckd;
	ctx->dispc_cinfo.pck_div = pckd;
	ctx->dispc_cinfo.lck = lck;
	ctx->dispc_cinfo.pck = pck;

	return true;
}

static bool dpi_calc_hsdiv_cb(int regm_hsdiv, unsigned long dispc,
		void *data)
{
	struct dpi_clk_calc_ctx *ctx = data;

	/*
	 * Odd dividers give us uneven duty cycle, causing problem when level
	 * shifted. So skip all odd dividers when the pixel clock is on the
	 * higher side.
	 */
	if (regm_hsdiv > 1 && regm_hsdiv % 2 != 0 && ctx->pck_min >= 100000000)
		return false;

	ctx->dpll_cinfo.regm_hsdiv = regm_hsdiv;
	ctx->dpll_cinfo.hsdiv_clk = dispc;

	return dispc_div_calc(dispc, ctx->pck_min, ctx->pck_max,
			dpi_calc_dispc_cb, ctx);
}

static bool dpi_calc_pll_cb(int regn, int regm, unsigned long fint,
		unsigned long pll,
		void *data)
{
	struct dpi_clk_calc_ctx *ctx = data;

	ctx->dpll_cinfo.regn = regn;
	ctx->dpll_cinfo.regm = regm;
	ctx->dpll_cinfo.fint = fint;
	ctx->dpll_cinfo.clkout = pll;

	return dss_dpll_hsdiv_calc(ctx->dpll, pll, ctx->pck_min,
			dpi_calc_hsdiv_cb, ctx);
}

static bool dpi_calc_dss_cb(unsigned long fck, void *data)
{
	struct dpi_clk_calc_ctx *ctx = data;

	ctx->fck = fck;

	return dispc_div_calc(fck, ctx->pck_min, ctx->pck_max,
			dpi_calc_dispc_cb, ctx);
}


static bool dpi_dpll_clk_calc(enum dss_dpll dpll, unsigned long pck,
		struct dpi_clk_calc_ctx *ctx)
{
	unsigned long clkin;
	unsigned long pll_min, pll_max;

	clkin = dpll_get_clkin(dpll);

	memset(ctx, 0, sizeof(*ctx));
	ctx->dpll = dpll;
	ctx->pck_min = pck - 1000;
	ctx->pck_max = pck + 1000;
	ctx->dpll_cinfo.clkin = clkin;

	pll_min = 0;
	pll_max = 0;

	return dss_dpll_calc(dpll, clkin, pll_min, pll_max, dpi_calc_pll_cb,
			ctx);
}

static bool dpi_dss_clk_calc(unsigned long pck, struct dpi_clk_calc_ctx *ctx)
{
	int i;

	/*
	 * DSS fck gives us very few possibilities, so finding a good pixel
	 * clock may not be possible. We try multiple times to find the clock,
	 * each time widening the pixel clock range we look for, up to
	 * +/- ~15MHz.
	 */

	for (i = 0; i < 25; ++i) {
		bool ok;

		memset(ctx, 0, sizeof(*ctx));
		if (pck > 1000 * i * i * i)
			ctx->pck_min = max(pck - 1000 * i * i * i, 0lu);
		else
			ctx->pck_min = 0;
		ctx->pck_max = pck + 1000 * i * i * i;

		ok = dss_div_calc(pck, ctx->pck_min, dpi_calc_dss_cb, ctx);
		if (ok)
			return ok;
	}

	return false;
}

static int dpi_set_dss_dpll_clk(struct dpi_data *dpi, unsigned long pck_req,
		unsigned long *fck, u16 *lck_div, u16 *pck_div)
{
	struct dpi_clk_calc_ctx ctx;
	int r;
	bool ok;

	ok = dpi_dpll_clk_calc(dpi->dpll, pck_req, &ctx);
	if (!ok)
		return -EINVAL;

	r = dss_dpll_set_clock_div(dpi->dpll, &ctx.dpll_cinfo);
	if (r)
		return r;

	dss_use_dpll_lcd(dpi->output.dispc_channel, true);

	dpi->mgr_config.clock_info = ctx.dispc_cinfo;

	*fck = ctx.dpll_cinfo.hsdiv_clk;
	*lck_div = ctx.dispc_cinfo.lck_div;
	*pck_div = ctx.dispc_cinfo.pck_div;

	return 0;
}

static int dpi_set_dispc_clk(struct dpi_data *dpi, unsigned long pck_req,
		unsigned long *fck, u16 *lck_div, u16 *pck_div)
{
	struct dpi_clk_calc_ctx ctx;
	int r;
	bool ok;

	ok = dpi_dss_clk_calc(pck_req, &ctx);
	if (!ok)
		return -EINVAL;

	r = dss_set_fck_rate(ctx.fck);
	if (r)
		return r;

	dpi->mgr_config.clock_info = ctx.dispc_cinfo;

	*fck = ctx.fck;
	*lck_div = ctx.dispc_cinfo.lck_div;
	*pck_div = ctx.dispc_cinfo.pck_div;

	return 0;
}

static int dpi_set_mode(struct dpi_data *dpi)
{
	struct omap_overlay_manager *mgr = dpi->output.manager;
	struct omap_video_timings *t = &dpi->timings;
	u16 lck_div = 0, pck_div = 0;
	unsigned long fck = 0;
	unsigned long pck;
	int r = 0;

	if (dpi->dpll != DSS_DPLL_NONE)
		r = dpi_set_dss_dpll_clk(dpi, t->pixel_clock * 1000, &fck,
			&lck_div, &pck_div);
	else
		r = dpi_set_dispc_clk(dpi, t->pixel_clock * 1000, &fck,
			&lck_div, &pck_div);
	if (r)
		return r;

	pck = fck / lck_div / pck_div / 1000;

	if (pck != t->pixel_clock) {
		DSSWARN("Could not find exact pixel clock. "
				"Requested %d kHz, got %lu kHz\n",
				t->pixel_clock, pck);

		t->pixel_clock = pck;
	}

	dss_mgr_set_timings(mgr, t);

	return 0;
}

static void dpi_config_lcd_manager(struct dpi_data *dpi)
{
	struct omap_overlay_manager *mgr = dpi->output.manager;

	dpi->mgr_config.io_pad_mode = DSS_IO_PAD_MODE_BYPASS;

	dpi->mgr_config.stallmode = false;
	dpi->mgr_config.fifohandcheck = false;

	dpi->mgr_config.video_port_width = dpi->data_lines;

	dpi->mgr_config.lcden_sig_polarity = 0;

	dss_mgr_set_lcd_config(mgr, &dpi->mgr_config);
}

static int dra7xx_dpi_display_enable(struct omap_dss_device *dssdev)
{
	struct dpi_data *dpi = dev_get_drvdata(dssdev->dev);
	struct omap_dss_device *out = &dpi->output;
	int r;

	mutex_lock(&dpi->lock);

	if (out == NULL || out->manager == NULL) {
		DSSERR("failed to enable display: no output/manager\n");
		r = -ENODEV;
		goto err_no_out_mgr;
	}

	r = dispc_runtime_get();
	if (r)
		goto err_get_dispc;

	r = dss_dpi_select_source(dpi->module_id, out->dispc_channel);
	if (r)
		goto err_src_sel;

	if (dpi->dpll != DSS_DPLL_NONE) {
		DSSDBG("using DPLL %d for DPI%d\n", dpi->dpll, dpi->module_id);
		dss_dpll_activate(dpi->dpll);
		dss_dpll_set_control_mux(out->dispc_channel, dpi->dpll);
	}

	r = dpi_set_mode(dpi);
	if (r)
		goto err_set_mode;


	dpi_config_lcd_manager(dpi);

	mdelay(2);

	r = dss_mgr_enable(out->manager);
	if (r)
		goto err_mgr_enable;

	mutex_unlock(&dpi->lock);

	return 0;

err_mgr_enable:
err_set_mode:
	if (dpi->dpll != DSS_DPLL_NONE)
		dss_dpll_disable(dpi->dpll);
err_src_sel:
	dispc_runtime_put();
err_get_dispc:
err_no_out_mgr:
	mutex_unlock(&dpi->lock);
	return r;
}

static void dra7xx_dpi_display_disable(struct omap_dss_device *dssdev)
{
	struct dpi_data *dpi = dev_get_drvdata(dssdev->dev);
	struct omap_overlay_manager *mgr = dpi->output.manager;

	mutex_lock(&dpi->lock);

	dss_mgr_disable(mgr);

	if (dpi->dpll != DSS_DPLL_NONE) {
		dss_use_dpll_lcd(dssdev->dispc_channel, false);
		dss_dpll_disable(dpi->dpll);
	}

	dispc_runtime_put();

	mutex_unlock(&dpi->lock);
}

static void dra7xx_dpi_set_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct dpi_data *dpi = dev_get_drvdata(dssdev->dev);

	DSSDBG("set_timings\n");

	mutex_lock(&dpi->lock);

	dpi->timings = *timings;

	mutex_unlock(&dpi->lock);
}

static int dra7xx_dpi_check_timings(struct omap_dss_device *dssdev,
			struct omap_video_timings *timings)
{
	DSSDBG("check_timings\n");

	return 0;
}

static void dra7xx_dpi_get_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct dpi_data *dpi = dev_get_drvdata(dssdev->dev);

	DSSDBG("set_timings\n");

	mutex_lock(&dpi->lock);

	*timings = dpi->timings;

	mutex_unlock(&dpi->lock);
}

static void dra7xx_dpi_set_data_lines(struct omap_dss_device *dssdev,
		int data_lines)
{
	struct dpi_data *dpi = dev_get_drvdata(dssdev->dev);

	mutex_lock(&dpi->lock);

	dpi->data_lines = data_lines;

	mutex_unlock(&dpi->lock);
}

static int dra7xx_dpi_connect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct dpi_data *dpi = dev_get_drvdata(dssdev->dev);
	struct omap_overlay_manager *mgr;
	int r;

	/* try to get a free dpll */
	dpi->dpll = dpi_get_dpll(dpi);

	r = dss_dpll_init_regulator(dpi->dpll);
	if (r)
		return r;

	mgr = omap_dss_get_overlay_manager(dssdev->dispc_channel);
	if (!mgr)
		return -ENODEV;

	r = dss_mgr_connect(mgr, dssdev);
	if (r)
		return r;

	r = omapdss_output_set_device(dssdev, dst);
	if (r) {
		DSSERR("failed to connect output to new device: %s\n",
				dst->name);
		dss_mgr_disconnect(mgr, dssdev);
		return r;
	}

	return 0;
}

static void dra7xx_dpi_disconnect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct dpi_data *dpi = dev_get_drvdata(dssdev->dev);

	WARN_ON(dst != dssdev->dst);

	if (dst != dssdev->dst)
		return;

	dpi->dpll = DSS_DPLL_NONE;

	omapdss_output_unset_device(dssdev);

	if (dssdev->manager)
		dss_mgr_disconnect(dssdev->manager, dssdev);
}

static const struct omapdss_dpi_ops dra7xx_dpi_ops = {
	.connect = dra7xx_dpi_connect,
	.disconnect = dra7xx_dpi_disconnect,

	.enable = dra7xx_dpi_display_enable,
	.disable = dra7xx_dpi_display_disable,

	.check_timings = dra7xx_dpi_check_timings,
	.set_timings = dra7xx_dpi_set_timings,
	.get_timings = dra7xx_dpi_get_timings,

	.set_data_lines = dra7xx_dpi_set_data_lines,
};

static enum omap_channel dra7xx_dpi_get_channel(struct dpi_data *dpi)
{
	switch (dpi->module_id) {
	case 0:
		return dpi->channel;
	case 1:
		return OMAP_DSS_CHANNEL_LCD2;
	case 2:
		return OMAP_DSS_CHANNEL_LCD3;
	default:
		DSSWARN("unknown DPI instance\n");
		return OMAP_DSS_CHANNEL_LCD;
	}
}

static void dra7xx_dpi_init_output(struct platform_device *pdev)
{
	struct dpi_data *dpi = dev_get_drvdata(&pdev->dev);
	struct omap_dss_device *out = &dpi->output;
	char *name;

	out->dev = &pdev->dev;
	name = devm_kzalloc(&pdev->dev, 5, GFP_KERNEL);

	switch (dpi->module_id) {
	case 0:
		out->id = OMAP_DSS_OUTPUT_DPI;
		break;
	case 1:
		out->id = OMAP_DSS_OUTPUT_DPI1;
		break;
	case 2:
		out->id = OMAP_DSS_OUTPUT_DPI2;
		break;
	};

	snprintf(name, 5, "dpi.%d", dpi->module_id);
	out->name = name;
	out->output_type = OMAP_DISPLAY_TYPE_DPI;
	out->dispc_channel = dra7xx_dpi_get_channel(dpi);
	out->ops.dpi = &dra7xx_dpi_ops;
	out->owner = THIS_MODULE;

	omapdss_register_output(out);
}

static void __exit dra7xx_dpi_uninit_output(struct platform_device *pdev)
{
	struct dpi_data *dpi = dev_get_drvdata(&pdev->dev);
	struct omap_dss_device *out = &dpi->output;

	omapdss_unregister_output(out);
}

static int dra7xx_dpi_probe(struct platform_device *pdev)
{
	int r;
	struct dpi_data *dpi;

	dpi = devm_kzalloc(&pdev->dev, sizeof(*dpi), GFP_KERNEL);
	if (!dpi)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, dpi);

	mutex_init(&dpi->lock);

	if (pdev->dev.of_node) {
		u32 id;
		enum omap_channel channel;

		r = of_property_read_u32(pdev->dev.of_node, "id", &id);
		if (r) {
			DSSERR("failed to read DPI module ID\n");
			return r;
		}

		r = of_property_read_u32(pdev->dev.of_node, "channel", &channel);
		if (r && id == 0) {
			DSSERR("failed to read DPI channel\n");
			return r;
		}

		dpi->module_id = id;
		dpi->channel = channel;
	} else {
		dpi->module_id = pdev->id;
	}

	dra7xx_dpi_init_output(pdev);

	return 0;
}

static int __exit dra7xx_dpi_remove(struct platform_device *pdev)
{
	dra7xx_dpi_uninit_output(pdev);

	return 0;
}

#if defined(CONFIG_OF)
static const struct of_device_id dpi_of_match[] = {
	{
		.compatible = "ti,dra7xx-dpi",
	},
	{},
};
#else
#define dpi_of_match NULL
#endif

static struct platform_driver dra7xx_dpi_driver = {
	.probe		= dra7xx_dpi_probe,
	.remove         = __exit_p(dra7xx_dpi_remove),
	.driver         = {
		.name   = "omapdss_dra7xx_dpi",
		.owner  = THIS_MODULE,
		.of_match_table = dpi_of_match,
	},
};

int __init dra7xx_dpi_init_platform_driver(void)
{
	return platform_driver_register(&dra7xx_dpi_driver);
}

void __exit dra7xx_dpi_uninit_platform_driver(void)
{
	platform_driver_unregister(&dra7xx_dpi_driver);
}
