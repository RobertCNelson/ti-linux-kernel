/*
 * TLC59108 TFC-S9700 Panel Driver
 *
 * Copyright (C) 2013 Texas Instruments
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/delay.h>

#include <video/omapdss.h>
#include <video/omap-panel-data.h>
#include <video/of_display_timing.h>

#define TLC_NAME		"tlc59108"
#define TLC_I2C_ADDR		0x40

#define TLC59108_MODE1		0x00
#define TLC59108_PWM2		0x04
#define TLC59108_LEDOUT0	0x0c
#define TLC59108_LEDOUT1	0x0d

struct panel_drv_data {
	struct omap_dss_device dssdev;
	struct omap_dss_device *in;

	int data_lines;
	struct omap_video_timings videomode;

	int enable_gpio;
	struct regmap *regmap;
};

static const struct omap_video_timings tfc_s9700_timings = {
	.x_res		= 800,
	.y_res		= 480,

	.pixel_clock	= 29232,

	.hfp		= 41,
	.hsw		= 49,
	.hbp		= 41,

	.vfp		= 13,
	.vsw		= 4,
	.vbp		= 29,

	.vsync_level	= OMAPDSS_SIG_ACTIVE_LOW,
	.hsync_level	= OMAPDSS_SIG_ACTIVE_LOW,
	.data_pclk_edge	= OMAPDSS_DRIVE_SIG_RISING_EDGE,
	.de_level	= OMAPDSS_SIG_ACTIVE_HIGH,
	.sync_pclk_edge	= OMAPDSS_DRIVE_SIG_OPPOSITE_EDGES,
};

static int tlc_init(struct panel_drv_data *ddata)
{
	struct regmap *map = ddata->regmap;

	/* init the TLC chip */
	regmap_write(map, TLC59108_MODE1, 0x01);

	/*
	 * set LED1(AVDD) to ON state(default), enable LED2 in PWM mode, enable
	 * LED0 to OFF state
	 */
	regmap_write(map, TLC59108_LEDOUT0, 0x21);

	/* set LED2 PWM to full freq */
	regmap_write(map, TLC59108_PWM2, 0xff);

	/* set LED4(UPDN) and LED6(MODE3) to OFF state */
	regmap_write(map, TLC59108_LEDOUT1, 0x11);

	return 0;
}

static int tlc_uninit(struct panel_drv_data *ddata)
{
	struct regmap *map = ddata->regmap;

	/* clear TLC chip regs */
	regmap_write(map, TLC59108_PWM2, 0x0);
	regmap_write(map, TLC59108_LEDOUT0, 0x0);
	regmap_write(map, TLC59108_LEDOUT1, 0x0);

	regmap_write(map, TLC59108_MODE1, 0x0);

	return 0;
}

#define to_panel_data(p) container_of(p, struct panel_drv_data, dssdev)

static int panel_dpi_connect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	if (omapdss_device_is_connected(dssdev))
		return 0;

	r = in->ops.dpi->connect(in, dssdev);
	if (r)
		return r;

	return 0;
}

static void panel_dpi_disconnect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (!omapdss_device_is_connected(dssdev))
		return;

	in->ops.dpi->disconnect(in, dssdev);
}

static int panel_dpi_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	if (!omapdss_device_is_connected(dssdev))
		return -ENODEV;

	if (omapdss_device_is_enabled(dssdev))
		return 0;

	in->ops.dpi->set_data_lines(in, ddata->data_lines);
	in->ops.dpi->set_timings(in, &ddata->videomode);

	r = in->ops.dpi->enable(in);
	if (r)
		return r;

	tlc_init(ddata);

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	return 0;
}

static void panel_dpi_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (!omapdss_device_is_enabled(dssdev))
		return;

	tlc_uninit(ddata);

	in->ops.dpi->disable(in);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static void panel_dpi_set_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	ddata->videomode = *timings;
	dssdev->panel.timings = *timings;

	in->ops.dpi->set_timings(in, timings);
}

static void panel_dpi_get_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	*timings = ddata->videomode;
}

static int panel_dpi_check_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	return in->ops.dpi->check_timings(in, timings);
}

static struct omap_dss_driver panel_dpi_ops = {
	.connect	= panel_dpi_connect,
	.disconnect	= panel_dpi_disconnect,

	.enable		= panel_dpi_enable,
	.disable	= panel_dpi_disable,

	.set_timings	= panel_dpi_set_timings,
	.get_timings	= panel_dpi_get_timings,
	.check_timings	= panel_dpi_check_timings,

	.get_resolution	= omapdss_default_get_resolution,
};

static int tlc_probe_of(struct device *dev)
{
	struct panel_drv_data *ddata = dev_get_drvdata(dev);
	struct device_node *node = dev->of_node;
	struct omap_dss_device *in;
	struct device_node *src_node;
	int r, datalines;
	int gpio;

	src_node = of_parse_phandle(node, "video-source", 0);
	if (!src_node) {
		dev_err(dev, "failed to parse video source\n");
		return -ENODEV;
	}

	in = omap_dss_find_output_by_node(src_node);
	if (in == NULL) {
		dev_err(dev, "failed to find video source\n");
		return -EPROBE_DEFER;
	}

	ddata->in = in;

	r = of_property_read_u32(node, "data-lines", &datalines);
	if (r) {
		dev_err(dev, "failed to parse datalines\n");
		return r;
	}

	ddata->data_lines = datalines;

	gpio = of_get_gpio(node, 0);
	if (gpio_is_valid(gpio) || gpio == -ENOENT) {
		ddata->enable_gpio = gpio;
	} else {
		dev_err(dev, "failed to parse enable gpio\n");
		return gpio;
	}

	return 0;
}

struct regmap_config tlc59108_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int tlc59108_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int r;
	struct regmap *regmap;
	struct panel_drv_data *ddata;
	struct device *dev = &client->dev;
	struct omap_dss_device *dssdev;
	unsigned int val;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (ddata == NULL)
		return -ENOMEM;

	dev_set_drvdata(dev, ddata);

	r = tlc_probe_of(dev);
	if (r)
		return r;

	ddata->videomode = tfc_s9700_timings;

	if (gpio_is_valid(ddata->enable_gpio)) {
		r = devm_gpio_request_one(dev, ddata->enable_gpio,
				GPIOF_OUT_INIT_LOW, "panel enable");
		if (r)
			goto err_gpio;
	}

	regmap = devm_regmap_init_i2c(client, &tlc59108_regmap_config);
	if (IS_ERR(regmap)) {
		r = PTR_ERR(regmap);
		dev_err(dev, "Failed to init regmap: %d\n", r);
		goto err_gpio;
	}

	ddata->regmap = regmap;

	msleep(10);

	/* Try to read a TLC register to verify if i2c works */
	r = regmap_read(ddata->regmap, TLC59108_MODE1, &val);
	if (r < 0) {
		dev_err(dev, "Failed to set MODE1: %d\n", r);
		return r;
	}

	dssdev = &ddata->dssdev;
	dssdev->dev = dev;
	dssdev->driver = &panel_dpi_ops;
	dssdev->type = OMAP_DISPLAY_TYPE_DPI;
	dssdev->owner = THIS_MODULE;
	dssdev->panel.timings = ddata->videomode;
	dssdev->phy.dpi.data_lines = ddata->data_lines;

	r = omapdss_register_display(dssdev);
	if (r) {
		dev_err(dev, "Failed to register panel\n");
		goto err_reg;
	}

	dev_info(dev, "Successfully initialized %s\n", TLC_NAME);

	return 0;
err_reg:
err_gpio:
	omap_dss_put_device(ddata->in);
	return r;
}

static int tlc59108_i2c_remove(struct i2c_client *client)
{
	struct panel_drv_data *ddata = dev_get_drvdata(&client->dev);
	struct omap_dss_device *dssdev = &ddata->dssdev;
	struct omap_dss_device *in = ddata->in;

	if (gpio_is_valid(ddata->enable_gpio))
		gpio_set_value_cansleep(ddata->enable_gpio, 1);

	omapdss_unregister_display(dssdev);

	panel_dpi_disable(dssdev);
	panel_dpi_disconnect(dssdev);

	omap_dss_put_device(in);

	return 0;
}

static const struct i2c_device_id tlc59108_id[] = {
	{ TLC_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tlc59108_id);

static const struct of_device_id tlc59108_of_match[] = {
	{ .compatible = "ti,tlc59108", },
	{ },
};
MODULE_DEVICE_TABLE(of, tlc59108_of_match);

static struct i2c_driver tlc59108_i2c_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= TLC_NAME,
		.of_match_table = tlc59108_of_match,
	},
	.id_table	= tlc59108_id,
	.probe		= tlc59108_i2c_probe,
	.remove		= tlc59108_i2c_remove,
};

static int __init tfc_s9700_init(void)
{
	return i2c_add_driver(&tlc59108_i2c_driver);
}

static void __exit tfc_s9700_exit(void)
{
}
module_init(tfc_s9700_init);
module_exit(tfc_s9700_exit);

MODULE_AUTHOR("Archit Taneja  <archit@ti.com>");
MODULE_DESCRIPTION("TFC-S9700 DPI Panel Driver");
MODULE_LICENSE("GPL");
