/*
 * Silicon image Sil9022 DPI-to-HDMI encoder driver
 *
 * Copyright (C) 2013 Texas Instruments
 * Author: Sathya Prakash M R <sathyap@ti.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/of_gpio.h>

#include <video/omapdss.h>
#include <video/omap-panel-data.h>
#include "encoder-sil9022.h"

static struct regmap_config sil9022_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

struct panel_drv_data {
	struct omap_dss_device dssdev;
	struct omap_dss_device *in;
	struct i2c_client *i2c_client;
	int reset_gpio;
	int data_lines;
	struct regmap *regmap;
	struct omap_video_timings timings;
};

#define to_panel_data(x) container_of(x, struct panel_drv_data, dssdev)

static int sil9022_hw_enable(struct omap_dss_device *dssdev)
{
	int		err;
	u8		vals[14];
	unsigned int val;
	u16		horizontal_res;
	u16		vertical_res;
	u16		pixel_clk;

	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_video_timings *hdmi_timings = &ddata->timings;
	struct i2c_client *sil9022_client = ddata->i2c_client;
	struct regmap *regmap = ddata->regmap;

	memset(vals, 0, 14);

	horizontal_res = hdmi_timings->x_res;
	vertical_res = hdmi_timings->y_res;
	pixel_clk = hdmi_timings->pixel_clock;

	dev_info(dssdev->dev,
			 "HW_ENABLE -> Timings\n"
			 "pixel_clk			= %d\n"
			 "horizontal res		= %d\n"
			 "vertical res			= %d\n",
			 hdmi_timings->pixel_clock,
			 hdmi_timings->x_res,
			 hdmi_timings->y_res
			 );

	/*  Fill the TPI Video Mode Data structure */
	vals[0] = (pixel_clk & 0xFF);                  /* Pixel clock */
	vals[1] = ((pixel_clk & 0xFF00) >> 8);
	vals[2] = VERTICAL_FREQ;                    /* Vertical freq */
	/* register programming information on how vertical freq is to be
	programmed to Sil9022 not clear. Hence setting to 60 for now */
	vals[3] = 0x00;
	vals[4] = (horizontal_res & 0xFF);         /* Horizontal pixels*/
	vals[5] = ((horizontal_res & 0xFF00) >> 8);
	vals[6] = (vertical_res & 0xFF);           /* Vertical pixels */
	vals[7] = ((vertical_res & 0xFF00) >> 8);

	/*  Write out the TPI Video Mode Data */

	err = regmap_bulk_write(ddata->regmap, HDMI_TPI_VIDEO_DATA_BASE_REG,
		&vals, 8);
	if (err < 0) {
		dev_err(dssdev->dev, "ERROR: writing TPI video mode data\n");
		return err;
	}

	/* Write out the TPI Input bus and pixel repetition Data:
	(24 bit wide bus, falling edge, no pixel replication, 1:1 CLK ration) */
	val = TPI_AVI_PIXEL_REP_BUS_24BIT |
		TPI_AVI_PIXEL_REP_FALLING_EDGE |
		TPI_AVI_PIXEL_REP_NONE | TPI_CLK_RATIO_1X;
	err = regmap_write(regmap, HDMI_TPI_PIXEL_REPETITION_REG, val);

	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR: writing TPI pixel repetition data\n");
		return err;
	}

	 /*  Write out the TPI AVI Input Format */
	val = TPI_AVI_INPUT_BITMODE_8BIT |
		TPI_AVI_INPUT_RANGE_AUTO |
		TPI_AVI_INPUT_COLORSPACE_RGB;
	err = regmap_write(regmap, HDMI_TPI_AVI_IN_FORMAT_REG, val);
	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR: writing TPI AVI Input format\n");
		return err;
	}

	/*  Write out the TPI AVI Output Format */
	val = TPI_AVI_OUTPUT_CONV_BT709 |
		TPI_AVI_OUTPUT_RANGE_AUTO |
		TPI_AVI_OUTPUT_COLORSPACE_RGBHDMI;
	err = regmap_write(regmap, HDMI_TPI_AVI_OUT_FORMAT_REG, val);
	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR: writing TPI AVI output format\n");
		return err;
	}

	/* Write out the TPI System Control Data to power down */
	val = TPI_SYS_CTRL_POWER_DOWN;
	err = regmap_write(regmap, HDMI_SYS_CTRL_DATA_REG, val);
	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR: writing TPI power down control data\n");
		return err;
	}

	/* Move from ENABLED -> FULLY ENABLED Power State  */
	val = TPI_AVI_POWER_STATE_D0;
	err = regmap_write(regmap, HDMI_TPI_POWER_STATE_CTRL_REG, val);
	if (err < 0) {
		dev_err(&sil9022_client->dev,
			"<%s> ERROR: Setting device power state to D0\n",
			__func__);
		return err;
	}

	/* Write out the TPI System Control Data to power up and
	 * select output mode
	 */
	val = TPI_SYS_CTRL_POWER_ACTIVE | TPI_SYS_CTRL_OUTPUT_MODE_HDMI;
	err = regmap_write(regmap, HDMI_SYS_CTRL_DATA_REG, val);
	if (err < 0) {
		dev_err(&sil9022_client->dev,
			"<%s> ERROR: Writing system control data\n", __func__);
		return err;
	}

	/*  Read back TPI System Control Data to latch settings */
	msleep(20);
	err = regmap_read(regmap, HDMI_SYS_CTRL_DATA_REG, &val);
	if (err < 0) {
		dev_err(&sil9022_client->dev,
			"<%s> ERROR: Writing system control data\n",
			__func__);
		return err;
	}

	/* HDCP */
	val = 0; /* DISABLED */
	err = regmap_write(regmap, HDMI_TPI_HDCP_CONTROLDATA_REG, val);
	if (err < 0) {
		dev_err(&sil9022_client->dev,
			"<%s> ERROR: Enable (1) / Disable (0) => HDCP: %d\n",
			__func__, val);
		return err;
	}

	dev_info(&sil9022_client->dev, "<%s> hdmi enabled\n", __func__);
	return 0;

}

static int sil9022_hw_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	unsigned int val = 0;
	int err = 0;

	/*  Write out the TPI System Control Data to power down  */
	val = TPI_SYS_CTRL_POWER_DOWN;
	err = regmap_write(ddata->regmap, HDMI_SYS_CTRL_DATA_REG, val);
	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR: writing control data - power down\n");
		return err;
	}

	/*  Move from FULLY ENABLED -> ENABLED Power state */
	val = TPI_AVI_POWER_STATE_D2;
	err = regmap_write(ddata->regmap, HDMI_TPI_DEVICE_POWER_STATE_DATA,
			val);
	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR: Setting device power state to D2\n");
		return err;
	}

	/*  Read back TPI System Control Data to latch settings */
	mdelay(10);
	err = regmap_read(ddata->regmap, HDMI_SYS_CTRL_DATA_REG, &val);
	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR:  Reading System control data "
			"- latch settings\n");
		return err;
	}

	dev_info(dssdev->dev, "hdmi disabled\n");
	return 0;

}

static int sil9022_probe_chip_version(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	int err = 0;
	unsigned int ver;

	/* probe for sil9022 chip version*/
	err = regmap_write(ddata->regmap, SIL9022_REG_TPI_RQB, 0x00);
	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR: Writing HDMI configuration to "
			"reg - SI9022_REG_TPI_RQB\n");
		err = -ENODEV;
		return err;
	}

	err = regmap_read(ddata->regmap, SIL9022_REG_CHIPID0, &ver);
	if (err < 0) {
		dev_err(dssdev->dev,
			"ERROR: Reading HDMI version Id\n");
		err = -ENODEV;
	} else if (ver != SIL9022_CHIPID_902x) {
		dev_err(dssdev->dev,
			"Not a valid verId: 0x%x\n", ver);
		err = -ENODEV;
	} else {
		dev_info(dssdev->dev,
			 "sil9022 HDMI Chip version = %x\n", ver);
	}
	return err;
}

/* Hdmi ops */

static int sil9022_connect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int err;

	dev_err(dssdev->dev, "CONNECT\n");

	if (omapdss_device_is_connected(dssdev))
		return -EBUSY;

	err = in->ops.dpi->connect(in, dssdev);
	if (err)
		return err;

	dst->src = dssdev;
	dssdev->dst = dst;

	/* Move from LOW -> ENABLED Power state */
	err = regmap_write(ddata->regmap, HDMI_TPI_POWER_STATE_CTRL_REG,
			TPI_AVI_POWER_STATE_D2);
	if (err < 0) {
		dev_err(dssdev->dev, "ERROR: Setting device power state to D2\n");
		goto err_pwr;
	}

	return 0;

err_pwr:
	dst->src = NULL;
	dssdev->dst = NULL;
	in->ops.dpi->disconnect(in, dssdev);
	return err;
}

static void sil9022_disconnect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	WARN_ON(!omapdss_device_is_connected(dssdev));
	if (!omapdss_device_is_connected(dssdev))
		return;

	WARN_ON(dst != dssdev->dst);
	if (dst != dssdev->dst)
		return;

	/* XXX we don't control the RESET pin, so we can't wake up from D3 */
#if 0
	/* Move from ENABLED -> LOW Power state */
	err = regmap_write(ddata->regmap, HDMI_TPI_POWER_STATE_CTRL_REG,
			TPI_AVI_POWER_STATE_D3);
	if (err < 0)
		dev_err(dssdev->dev, "ERROR: Setting device power state to D3\n");
#endif

	dst->src = NULL;
	dssdev->dst = NULL;

	in->ops.dpi->disconnect(in, dssdev);
}

static int sil9022_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	dev_err(dssdev->dev, "ENABLE\n");

	if (!omapdss_device_is_connected(dssdev))
		return -ENODEV;

	if (omapdss_device_is_enabled(dssdev))
		return 0;

	in->ops.dpi->set_timings(in, &ddata->timings);
	in->ops.dpi->set_data_lines(in, ddata->data_lines);

	r = in->ops.dpi->enable(in);
	if (r)
		return r;

	if (gpio_is_valid(ddata->reset_gpio))
		gpio_set_value_cansleep(ddata->reset_gpio, 0);

	r = sil9022_hw_enable(dssdev);
	if (r)
		goto err_hw_enable;

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	return 0;

err_hw_enable:
	if (gpio_is_valid(ddata->reset_gpio))
		gpio_set_value_cansleep(ddata->reset_gpio, 1);

	in->ops.dpi->disable(in);

	return r;
}

static void sil9022_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (!omapdss_device_is_enabled(dssdev))
		return;

	sil9022_hw_disable(dssdev);

	if (gpio_is_valid(ddata->reset_gpio))
		gpio_set_value_cansleep(ddata->reset_gpio, 1);

	in->ops.dpi->disable(in);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static void sil9022_set_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	struct omap_video_timings *sil9022_timings = timings;

       /* update DPI specific timing info */
	sil9022_timings->data_pclk_edge  = OMAPDSS_DRIVE_SIG_RISING_EDGE;
	sil9022_timings->de_level		  = OMAPDSS_SIG_ACTIVE_HIGH;
	sil9022_timings->sync_pclk_edge = OMAPDSS_DRIVE_SIG_OPPOSITE_EDGES;
	ddata->timings = *sil9022_timings;
	dssdev->panel.timings = *sil9022_timings;

	in->ops.dpi->set_timings(in, sil9022_timings);
}

static void sil9022_get_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	*timings = ddata->timings;
}

static int sil9022_check_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	struct omap_video_timings *sil9022_timings = timings;

	/* update DPI specific timing info */
	sil9022_timings->data_pclk_edge  = OMAPDSS_DRIVE_SIG_RISING_EDGE;
	sil9022_timings->de_level		  = OMAPDSS_SIG_ACTIVE_HIGH;
	sil9022_timings->sync_pclk_edge = OMAPDSS_DRIVE_SIG_OPPOSITE_EDGES;

	return in->ops.dpi->check_timings(in, sil9022_timings);
}

static int sil9022_read_edid(struct omap_dss_device *dssdev,
	       u8 *edid, int len)
{

	int err =  0;
	unsigned int val = 0;
	int retries = 0;
	int i2c_client_addr;
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct i2c_client *client = ddata->i2c_client;

	len = (len < HDMI_EDID_MAX_LENGTH) ? len : HDMI_EDID_MAX_LENGTH;

	/* Request DDC bus access to read EDID info from HDTV */
	dev_info(&client->dev, "Reading HDMI EDID\n");

	val = 0;
	err = regmap_read(ddata->regmap, 0x3D, &val);
		if (err < 0) {
			dev_err(&client->dev,
				"ERROR: Reading Monitor Status register\n");
			return err;
	}

	if (val & 0x2)
		dev_err(&client->dev, " MONITOR PRESENT \n");
	else
		dev_err(&client->dev, " MONITOR NOT PRESENT \n");

	/* Disable TMDS clock */
	val = 0x11;
	err = regmap_write(ddata->regmap, HDMI_SYS_CTRL_DATA_REG, val);
	if (err < 0) {
		dev_err(&client->dev,
			"ERROR: Failed to disable TMDS clock\n");
		return err;
	}

	val = 0;

	/* Read TPI system control register*/
	err = regmap_read(ddata->regmap, HDMI_SYS_CTRL_DATA_REG, &val);
	if (err < 0) {
		dev_err(&client->dev,
			"ERROR: Reading DDC BUS REQUEST\n");
		return err;
	}

	/* The host writes 0x1A[2]=1 to request the
	 * DDC(Display Data Channel) bus
	 */
	val |= TPI_SYS_CTRL_DDC_BUS_REQUEST;
	err = regmap_write(ddata->regmap, HDMI_SYS_CTRL_DATA_REG, val);
	if (err < 0) {
		dev_err(&client->dev,
			"ERROR: Writing DDC BUS REQUEST\n");
		return err;
	}

	 /*  Poll for bus DDC Bus control to be granted */
	dev_info(&client->dev, "Poll for DDC bus access\n");
	val = 0;
	do {
		err = regmap_read(ddata->regmap, HDMI_SYS_CTRL_DATA_REG, &val);
		if (retries++ > 100)
			return err;

	} while ((val & TPI_SYS_CTRL_DDC_BUS_GRANTED) == 0);

	/*  Close the switch to the DDC */
	val |= TPI_SYS_CTRL_DDC_BUS_REQUEST | TPI_SYS_CTRL_DDC_BUS_GRANTED;
	err = regmap_write(ddata->regmap, HDMI_SYS_CTRL_DATA_REG, val);
	if (err < 0) {
		dev_err(&client->dev,
			"<%s> ERROR: Close switch to DDC BUS REQUEST\n",
			__func__);
		return err;
	}

	memset(edid, 0, len);
	/* change I2C SetSlaveAddress to HDMI_I2C_MONITOR_ADDRESS */
	/*  Read the EDID structure from the monitor I2C address  */
	i2c_client_addr = client->addr;
	client->addr = HDMI_I2C_MONITOR_ADDRESS;

	err = regmap_raw_read(ddata->regmap, 0x00, edid, len);
	if (err < 0) {
		dev_err(&client->dev, "ERROR: Reading EDID\n");
		return err;
	}

	/* Release DDC bus access */
	client->addr = i2c_client_addr;
	val &= ~(TPI_SYS_CTRL_DDC_BUS_REQUEST | TPI_SYS_CTRL_DDC_BUS_GRANTED);

	retries = 0;
	do {
		err = regmap_write(ddata->regmap, HDMI_SYS_CTRL_DATA_REG, val);
		if (err >= 0)
			break;
		retries++;
	} while (retries < 5);
	if (err < 0) {
		dev_err(&client->dev, "ERROR: Releasing DDC Bus Access\n");
		return err;
		}

	print_hex_dump(KERN_ERR, "\t", DUMP_PREFIX_NONE, 16, 1, edid, len, 0);

	return 0;

}

static bool sil9022_detect(struct omap_dss_device *dssdev)
{
	/* Hot plug detection is not implemented */
	/* Hence we assume monitor connected */
	/* This will be fixed once HPD / polling is implemented */
	return true;
}

static bool sil9022_audio_supported(struct omap_dss_device *dssdev)
{
	/* Audio configuration not present, hence returning false */
	return false;
}

static const struct omapdss_hdmi_ops sil9022_hdmi_ops = {
	.connect			= sil9022_connect,
	.disconnect		= sil9022_disconnect,

	.enable			= sil9022_enable,
	.disable			= sil9022_disable,

	.check_timings	= sil9022_check_timings,
	.set_timings		= sil9022_set_timings,
	.get_timings		= sil9022_get_timings,

	.read_edid		= sil9022_read_edid,
	.detect			= sil9022_detect,

	.audio_supported	= sil9022_audio_supported,
	/* Yet to implement audio ops */
	/* For now audio_supported ops to return false */
};


static int sil9022_probe_of(struct i2c_client *client)
{
	struct panel_drv_data *ddata = dev_get_drvdata(&client->dev);
	struct device_node *node = client->dev.of_node;
	struct device_node *src_node;
	struct omap_dss_device *dssdev, *in;

	int r, reset_gpio, datalines;

	src_node = of_parse_phandle(node, "video-source", 0);
	if (!src_node) {
		dev_err(&client->dev, "failed to parse video source\n");
		return -ENODEV;
	}

	in = omap_dss_find_output_by_node(src_node);
	if (in == NULL) {
		dev_err(&client->dev, "failed to find video source\n");
		return -EPROBE_DEFER;
	}
	ddata->in = in;

	reset_gpio = of_get_named_gpio(node, "reset-gpio", 0);

	if (gpio_is_valid(reset_gpio) || reset_gpio == -ENOENT) {
		ddata->reset_gpio = reset_gpio;
	} else {
		dev_err(&client->dev, "failed to parse lcdorhdmi gpio\n");
		return reset_gpio;
	}

	r = of_property_read_u32(node, "data-lines", &datalines);
	if (r) {
		dev_err(&client->dev, "failed to parse datalines\n");
		return r;
	}

	ddata->data_lines = datalines;
	ddata->reset_gpio = reset_gpio;
	dssdev = &ddata->dssdev;

	return 0;

}

static int sil9022_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct panel_drv_data *ddata;
	struct omap_dss_device *dssdev;
	struct regmap *regmap;
	int err = 0;

	regmap = devm_regmap_init_i2c(client, &sil9022_regmap_config);
	if (IS_ERR(regmap)) {
		err = PTR_ERR(regmap);
		dev_err(&client->dev, "Failed to init regmap: %d\n", err);
		return err;
	}

	ddata = devm_kzalloc(&client->dev, sizeof(*ddata), GFP_KERNEL);
	if (ddata == NULL)
		return -ENOMEM;

	dev_set_drvdata(&client->dev, ddata);

	if (client->dev.of_node) {
		err = sil9022_probe_of(client);
		if (err)
			return err;
	} else {
		return -ENODEV;
	}

	if (gpio_is_valid(ddata->reset_gpio)) {
		err = devm_gpio_request_one(&client->dev, ddata->reset_gpio,
				GPIOF_OUT_INIT_HIGH, "Sil9022-Encoder");
		if (err)
			goto err_gpio;
	}

	ddata->regmap = regmap;
	ddata->i2c_client = client;
	dssdev = &ddata->dssdev;
	dssdev->dev = &client->dev;
	dssdev->ops.hdmi = &sil9022_hdmi_ops;
	dssdev->type = OMAP_DISPLAY_TYPE_DPI;
	dssdev->output_type = OMAP_DISPLAY_TYPE_HDMI;
	dssdev->owner = THIS_MODULE;
	dssdev->phy.dpi.data_lines = ddata->data_lines;

	/* Read sil9022 chip version */
	err = sil9022_probe_chip_version(dssdev);
	if (err) {
		dev_err(&client->dev, "Failed to read CHIP VERSION\n");
		goto err_i2c;
	}

	err = omapdss_register_output(dssdev);
	if (err) {
		dev_err(&client->dev, "Failed to register output\n");
		goto err_reg;
	}

	return 0;

err_reg:
err_i2c:
err_gpio:
	omap_dss_put_device(ddata->in);
	return err;
}


static int sil9022_remove(struct i2c_client *client)
{
	struct panel_drv_data *ddata = dev_get_drvdata(&client->dev);
	struct omap_dss_device *dssdev = &ddata->dssdev;

	omapdss_unregister_output(dssdev);

	WARN_ON(omapdss_device_is_enabled(dssdev));
	if (omapdss_device_is_enabled(dssdev))
		sil9022_disable(dssdev);

	WARN_ON(omapdss_device_is_connected(dssdev));
	if (omapdss_device_is_connected(dssdev))
		sil9022_disconnect(dssdev, dssdev->dst);

	omap_dss_put_device(ddata->in);

	return 0;
}

static const struct i2c_device_id sil9022_id[] = {
	{ SIL9022_DRV_NAME, 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, sil9022_id);

static struct i2c_driver sil9022_driver = {
	.driver = {
		.name  = SIL9022_DRV_NAME,
		.owner = THIS_MODULE,
		},
	.probe		= sil9022_probe,
	.remove		= sil9022_remove,
	.id_table	= sil9022_id,
};

module_i2c_driver(sil9022_driver);

MODULE_AUTHOR("Sathya Prakash M R <sathyap@ti.com>");
MODULE_DESCRIPTION("Sil9022 DPI to HDMI encoder Driver");
MODULE_LICENSE("GPL");
