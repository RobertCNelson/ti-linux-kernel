// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016-2018 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Jyri Sarha <jsarha@ti.com>
 */

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#include "dss.h"
#include "dss7.h"

struct oldi_data {
	struct platform_device *pdev;
	char name[16];

	struct mutex lock;

	struct videomode vm;
	int data_lines;
	int oldi_mode;

	struct omap_dss_device output;

	bool port_initialized;
};

static struct oldi_data *oldi7_get_data_from_dssdev(struct omap_dss_device *out)
{
	return container_of(out, struct oldi_data, output);
}

static void oldi7_config_lcd_manager(struct oldi_data *oldi)
{
	struct dss_lcd_mgr_config mgr_config = {
		.io_pad_mode = DSS_IO_PAD_MODE_BYPASS,
		.stallmode = false,
		.fifohandcheck = false,
		.video_port_width = oldi->data_lines,
		.lcden_sig_polarity = 0,
		.oldi = true,
		.oldi_mode = oldi->oldi_mode,
	};

	dss_mgr_set_lcd_config(oldi->output.dispc_channel, &mgr_config);
}

static int oldi7_connect(struct omap_dss_device *out,
			struct omap_dss_device *dst)
{
	struct oldi_data *oldi = oldi7_get_data_from_dssdev(out);
	enum omap_channel channel = out->dispc_channel;
	int r;

	r = dss_mgr_connect(channel, out);
	if (r)
		return r;

	r = omapdss_output_set_device(out, dst);
	if (r) {
		dev_err(&oldi->pdev->dev,
			"failed to connect output to new device: %s\n",
			dst->name);
		dss_mgr_disconnect(channel, out);
		return r;
	}

	return 0;
}

static void oldi7_disconnect(struct omap_dss_device *out,
			    struct omap_dss_device *dst)
{
	WARN_ON(dst != out->dst);

	if (dst != out->dst)
		return;

	omapdss_output_unset_device(out);

	dss_mgr_disconnect(out->dispc_channel, out);
}

static int oldi7_display_enable(struct omap_dss_device *out)
{
	struct oldi_data *oldi = oldi7_get_data_from_dssdev(out);
	enum omap_channel channel = out->dispc_channel;
	int r;

	mutex_lock(&oldi->lock);

	if (!out->dispc_channel_connected) {
		dev_err(&oldi->pdev->dev, "failed to enable display: no output channel set\n");
		r = -ENODEV;
		goto err_no_out_mgr;
	}

	r = dispc7_runtime_get();
	if (r)
		goto err_get_dispc;

	r = dispc7_vp_set_clk_rate(channel, oldi->vm.pixelclock);
	if (r)
		goto err_set_clk;

	r = dispc7_vp_enable_clk(channel);
	if (r)
		goto err_enable_clk;

	oldi7_config_lcd_manager(oldi);

	r = dss_mgr_enable(channel);
	if (r)
		goto err_mgr_enable;

	mutex_unlock(&oldi->lock);

	return 0;

err_mgr_enable:
	dispc7_vp_disable_clk(channel);
err_enable_clk:
err_set_clk:
	dispc7_runtime_put();
err_get_dispc:
err_no_out_mgr:
	mutex_unlock(&oldi->lock);
	return r;
}

static void oldi7_display_disable(struct omap_dss_device *out)
{
	struct oldi_data *oldi = oldi7_get_data_from_dssdev(out);
	enum omap_channel channel = out->dispc_channel;

	mutex_lock(&oldi->lock);

	dss_mgr_disable(channel);

	dispc7_vp_disable_clk(channel);

	dispc7_runtime_put();

	mutex_unlock(&oldi->lock);
}

static int oldi7_check_timings(struct omap_dss_device *out,
			      struct videomode *vm)
{
	enum omap_channel channel = out->dispc_channel;

	if (!dispc7_mgr_timings_ok(channel, vm))
		return -EINVAL;

	return 0;
}

static void oldi7_set_timings(struct omap_dss_device *out,
			     struct videomode *vm)
{
	struct oldi_data *oldi = oldi7_get_data_from_dssdev(out);

	mutex_lock(&oldi->lock);

	oldi->vm = *vm;

	mutex_unlock(&oldi->lock);
}

static void oldi7_get_timings(struct omap_dss_device *out,
			     struct videomode *vm)
{
	struct oldi_data *oldi = oldi7_get_data_from_dssdev(out);

	mutex_lock(&oldi->lock);

	*vm = oldi->vm;

	mutex_unlock(&oldi->lock);
}

static const struct omapdss_oldi_ops oldi7_ops = {
	.connect = oldi7_connect,
	.disconnect = oldi7_disconnect,

	.enable = oldi7_display_enable,
	.disable = oldi7_display_disable,

	.check_timings = oldi7_check_timings,
	.set_timings = oldi7_set_timings,
	.get_timings = oldi7_get_timings,
};

static void oldi7_setup_output_port(struct platform_device *pdev,
				   struct device_node *port)
{
	struct oldi_data *oldi = port->data;
	struct omap_dss_device *out = &oldi->output;
	int r;
	u32 port_num;

	r = of_property_read_u32(port, "reg", &port_num);
	if (r)
		port_num = 0;

	snprintf(oldi->name, sizeof(oldi->name), "oldi.%u", port_num);

	out->name = oldi->name;
	out->dispc_channel = port_num;

	out->dev = &pdev->dev;
	out->id = OMAP_DSS_OUTPUT_OLDI;
	out->output_type = OMAP_DISPLAY_TYPE_OLDI;
	out->port_num = port_num;
	out->ops.oldi = &oldi7_ops;
	out->owner = THIS_MODULE;

	omapdss_register_output(out);
}

int oldi7_init_port(struct platform_device *pdev, struct device_node *port)
{
	struct oldi_data *oldi;
	struct device_node *ep;
	u32 datalines;
	u32 oldi_mode;
	int r;

	oldi = devm_kzalloc(&pdev->dev, sizeof(*oldi), GFP_KERNEL);
	if (!oldi)
		return -ENOMEM;

	ep = of_get_next_child(port, NULL);
	if (!ep)
		return 0;

	r = of_property_read_u32(ep, "data-lines", &datalines);
	if (r) {
		dev_err(&oldi->pdev->dev, "failed to parse datalines\n");
		goto err_ep_put;
	}

	oldi->data_lines = datalines;

	r = of_property_read_u32(ep, "oldi-mode", &oldi_mode);
	if (r) {
		dev_err(&oldi->pdev->dev, "failed to parse oldi-mode\n");
		goto err_ep_put;
	}
	oldi->oldi_mode = oldi_mode;

	of_node_put(ep);

	oldi->pdev = pdev;
	port->data = oldi;

	mutex_init(&oldi->lock);

	oldi7_setup_output_port(pdev, port);

	oldi->port_initialized = true;

	return 0;

err_ep_put:
	of_node_put(ep);

	return r;
}

void oldi7_uninit_port(struct device_node *port)
{
	struct oldi_data *oldi = port->data;

	if (!oldi->port_initialized)
		return;

	omapdss_unregister_output(&oldi->output);
}
