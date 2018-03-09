// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013-2018 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Jyri Sarha <jsarha@ti.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/backlight.h>

#include <video/of_display_timing.h>

#include "../dss/omapdss.h"

struct panel_drv_data {
	struct omap_dss_device dssdev;
	struct omap_dss_device *in;

	struct videomode vm;

	struct backlight_device *backlight;

	struct gpio_desc *enable_gpio;
	struct regulator *vcc_supply;
};

#define to_panel_data(p) container_of(p, struct panel_drv_data, dssdev)

static int panel_oldi_connect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	if (omapdss_device_is_connected(dssdev))
		return 0;

	r = in->ops.oldi->connect(in, dssdev);
	if (r)
		return r;

	return 0;
}

static void panel_oldi_disconnect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (!omapdss_device_is_connected(dssdev))
		return;

	in->ops.oldi->disconnect(in, dssdev);
}

static int panel_oldi_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	if (!omapdss_device_is_connected(dssdev))
		return -ENODEV;

	if (omapdss_device_is_enabled(dssdev))
		return 0;

	in->ops.oldi->set_timings(in, &ddata->vm);

	r = in->ops.oldi->enable(in);
	if (r)
		return r;

	r = regulator_enable(ddata->vcc_supply);
	if (r) {
		in->ops.oldi->disable(in);
		return r;
	}

	gpiod_set_value_cansleep(ddata->enable_gpio, 1);

	if (ddata->backlight) {
		ddata->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ddata->backlight);
	}

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	return 0;
}

static void panel_oldi_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (!omapdss_device_is_enabled(dssdev))
		return;

	if (ddata->backlight) {
		ddata->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ddata->backlight);
	}

	gpiod_set_value_cansleep(ddata->enable_gpio, 0);
	regulator_disable(ddata->vcc_supply);

	in->ops.oldi->disable(in);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static void panel_oldi_set_timings(struct omap_dss_device *dssdev,
				  struct videomode *vm)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	ddata->vm = *vm;
	dssdev->panel.vm = *vm;

	in->ops.oldi->set_timings(in, vm);
}

static void panel_oldi_get_timings(struct omap_dss_device *dssdev,
				  struct videomode *vm)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	*vm = ddata->vm;
}

static int panel_oldi_check_timings(struct omap_dss_device *dssdev,
				   struct videomode *vm)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	return in->ops.oldi->check_timings(in, vm);
}

static struct omap_dss_driver panel_oldi_ops = {
	.connect	= panel_oldi_connect,
	.disconnect	= panel_oldi_disconnect,

	.enable		= panel_oldi_enable,
	.disable	= panel_oldi_disable,

	.set_timings	= panel_oldi_set_timings,
	.get_timings	= panel_oldi_get_timings,
	.check_timings	= panel_oldi_check_timings,
};

static int panel_oldi_probe_of(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;
	struct device_node *bl_node;
	struct omap_dss_device *in;
	int r;
	struct display_timing timing;
	struct gpio_desc *gpio;

	gpio = devm_gpiod_get_optional(&pdev->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);

	ddata->enable_gpio = gpio;

	/*
	 * Many different panels are supported by this driver and there are
	 * probably very different needs for their reset pins in regards to
	 * timing and order relative to the enable gpio. So for now it's just
	 * ensured that the reset line isn't active.
	 */
	gpio = devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);

	ddata->vcc_supply = devm_regulator_get(&pdev->dev, "vcc");
	if (IS_ERR(ddata->vcc_supply))
		return PTR_ERR(ddata->vcc_supply);

	bl_node = of_parse_phandle(node, "backlight", 0);
	if (bl_node) {
		ddata->backlight = of_find_backlight_by_node(bl_node);
		of_node_put(bl_node);

		if (!ddata->backlight)
			return -EPROBE_DEFER;
	}

	r = of_get_display_timing(node, "panel-timing", &timing);
	if (r) {
		dev_err(&pdev->dev, "failed to get video timing\n");
		goto error_free_backlight;
	}

	videomode_from_timing(&timing, &ddata->vm);

	in = omapdss_of_find_source_for_first_ep(node);
	if (IS_ERR(in)) {
		dev_err(&pdev->dev, "failed to find video source\n");
		r = PTR_ERR(in);
		goto error_free_backlight;
	}

	ddata->in = in;

	return 0;

error_free_backlight:
	if (ddata->backlight)
		put_device(&ddata->backlight->dev);

	return r;
}

static int panel_oldi_probe(struct platform_device *pdev)
{
	struct panel_drv_data *ddata;
	struct omap_dss_device *dssdev;
	int r;

	if (!pdev->dev.of_node)
		return -ENODEV;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (ddata == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);

	r = panel_oldi_probe_of(pdev);
	if (r)
		return r;

	dssdev = &ddata->dssdev;
	dssdev->dev = &pdev->dev;
	dssdev->driver = &panel_oldi_ops;
	dssdev->type = OMAP_DISPLAY_TYPE_OLDI;
	dssdev->owner = THIS_MODULE;
	dssdev->panel.vm = ddata->vm;

	r = omapdss_register_display(dssdev);
	if (r) {
		dev_err(&pdev->dev, "Failed to register panel\n");
		goto err_reg;
	}

	return 0;

err_reg:
	omap_dss_put_device(ddata->in);
	return r;
}

static int __exit panel_oldi_remove(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct omap_dss_device *dssdev = &ddata->dssdev;
	struct omap_dss_device *in = ddata->in;

	omapdss_unregister_display(dssdev);

	panel_oldi_disable(dssdev);
	panel_oldi_disconnect(dssdev);

	omap_dss_put_device(in);

	if (ddata->backlight)
		put_device(&ddata->backlight->dev);

	return 0;
}

static const struct of_device_id panel_oldi_of_match[] = {
	{ .compatible = "omapdss,panel-oldi", },
	{},
};

MODULE_DEVICE_TABLE(of, panel_oldi_of_match);

static struct platform_driver panel_oldi_driver = {
	.probe = panel_oldi_probe,
	.remove = __exit_p(panel_oldi_remove),
	.driver = {
		.name = "panel-oldi",
		.of_match_table = panel_oldi_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(panel_oldi_driver);

MODULE_AUTHOR("Jyri Sarha <jsarha@ti.com>");
MODULE_DESCRIPTION("Generic OLDI Panel Driver");
MODULE_LICENSE("GPL");
