/*
 * TPD12S015 HDMI ESD protection & level shifter chip driver
 *
 * Copyright (C) 2013 Texas Instruments
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>

#include <video/omapdss.h>
#include <video/omap-panel-data.h>

#define CLK_BASE			0x4a009000
#define MCASP2_BASE			0x48464000
#define	CTRL_BASE			0x4a003400
#define	PINMUX_BASE			0x4a003600

#define CM_L4PER2_MCASP2_CLKCTRL	0x860
#define	CM_L4PER2_CLKSTCTRL		0x8fc
#define MCASP_PFUNC			0x10
#define MCASP_PDIR			0x14
#define MCASP_PDOUT			0x18
#define PAD_I2C2_SDA			0x408
#define PAD_I2C2_SCL			0x40c

#define SEL_I2C2	0
#define SEL_HDMI	1

/* HPD gpio debounce time in microseconds */
#define HPD_DEBOUNCE_TIME	1000

struct panel_drv_data {
	struct omap_dss_device dssdev;
	struct omap_dss_device *in;

	int ct_cp_hpd_gpio;
	int ls_oe_gpio;
	int hpd_gpio;

	bool disable_hpd;

	struct omap_video_timings timings;

	struct completion hpd_completion;
};

static void config_sel_hdmi_i2c2(struct device *dev)
{
	void __iomem *clk_base = ioremap(CLK_BASE, SZ_4K);
	void __iomem *mcasp2_base = ioremap(MCASP2_BASE, SZ_1K);
	void __iomem *pmux_base = ioremap(PINMUX_BASE, SZ_1K);

	if (!clk_base) {
		dev_err(dev, "couldn't ioremap clock domain regs\n");
		return;
	}

	if (!mcasp2_base) {
		dev_err(dev, "couldn't ioremap MCASP2 regs\n");
		goto mcasp_err;
	}

	if (!pmux_base) {
		dev_err(dev, "couldn't ioremap PMUX regs\n");
		goto pmux_err;
	}

	iowrite32(0x40000, pmux_base + 0xfc);

	/* set CM_L4PER2_CLKSTCTRL to sw supervised wkup */
	iowrite32(0x2, clk_base + CM_L4PER2_CLKSTCTRL);

	/*
	 * Enable the MCASP8_AUX_GFCLK[22:23]: 0x0 - use default
	 * CM_L4PER2_MCASP8_CLKCTRL[1:0]: 0x2 - Enable explicitly
	*/
	iowrite32(0x2, clk_base + CM_L4PER2_MCASP2_CLKCTRL);

	dev_dbg(dev, "CM_L4PER2_CLKSTCTRL %08x\n",
		ioread32(clk_base + CM_L4PER2_CLKSTCTRL));

	/* let it propogate */

	udelay(5);
	/*
	 * make mcasp8_axr2 a gpio and set direction to high
	 */
	iowrite32(1 << 29, mcasp2_base + MCASP_PFUNC);
	iowrite32(1 << 29, mcasp2_base + MCASP_PDIR);

	iounmap(pmux_base);
pmux_err:
	iounmap(mcasp2_base);
mcasp_err:
	iounmap(clk_base);
}

/*
 * use I2C2 to configure pcf8575@26 to set/unset LS_OE and CT_HPD, use HDMI to
 * read edid via the HDMI ddc lines, and recieve HPD events
 */
static void config_demux(struct device *dev, int sel)
{
	void __iomem *mcasp2_base = ioremap(MCASP2_BASE, SZ_1K);
	void __iomem *ctrl_base = ioremap(CTRL_BASE, SZ_1K);

	if (!mcasp2_base) {
		dev_err(dev, "couldn't ioremap MCASP8 regs\n");
		return;
	}

	if (!ctrl_base) {
		dev_err(dev, "couldn't ioremap CTRL base\n");
		goto err_ctrl;
	}

	/*
	 * switch to I2C2 or HDMI DDC internal pinmux and drive MCASP8_PDOUT
	 * to low or high to select I2C2 or HDMI path respectively
	 */
	if (sel == SEL_I2C2) {
		iowrite32(0x0, mcasp2_base + MCASP_PDOUT);
		iowrite32(0x60000, ctrl_base + PAD_I2C2_SDA);
		iowrite32(0x60000, ctrl_base + PAD_I2C2_SCL);
	} else {
		iowrite32(1 << 29, mcasp2_base + MCASP_PDOUT);
		iowrite32(0x60001, ctrl_base + PAD_I2C2_SDA);
		iowrite32(0x60001, ctrl_base + PAD_I2C2_SCL);
	}

	/* let it propogate */
	udelay(5);

	dev_dbg(dev, "select %d, PDOUT %08x\n", sel,
		ioread32(mcasp2_base + MCASP_PDOUT));

	iounmap(ctrl_base);
err_ctrl:
	iounmap(mcasp2_base);
}

#define to_panel_data(x) container_of(x, struct panel_drv_data, dssdev)

static irqreturn_t tpd_hpd_irq_handler(int irq, void *data)
{
	struct panel_drv_data *ddata = data;
	bool hpd;

	hpd = gpio_get_value_cansleep(ddata->hpd_gpio);

	dev_dbg(ddata->dssdev.dev, "hpd %d\n", hpd);

	if (gpio_is_valid(ddata->ls_oe_gpio)) {
		if (hpd)
			gpio_set_value_cansleep(ddata->ls_oe_gpio, 1);
		else
			gpio_set_value_cansleep(ddata->ls_oe_gpio, 0);
	}

	complete_all(&ddata->hpd_completion);

	return IRQ_HANDLED;
}

static int tpd_connect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	bool hpd;
	int r;

	r = in->ops.hdmi->connect(in, dssdev);
	if (r)
		return r;

	dst->src = dssdev;
	dssdev->dst = dst;

	INIT_COMPLETION(ddata->hpd_completion);

	gpio_set_value_cansleep(ddata->ct_cp_hpd_gpio, 1);

	/* DC-DC converter needs at max 300us to get to 90% of 5V */
	udelay(300);

	if (!ddata->disable_hpd) {
		/*
		 * if there's a cable connected, wait for the hpd irq to
		 * trigger, which turns on the level shifters.
		 */
		hpd = gpio_get_value_cansleep(ddata->hpd_gpio);

		if (hpd) {
			unsigned long to;
			to = wait_for_completion_timeout(&ddata->hpd_completion,
					msecs_to_jiffies(250));
			WARN_ON_ONCE(to == 0);
		}
	} else {
		/*
		 * if there's a cable connected, the hpd gpio should be up, turn
		 * the level shifters accordingly, we don't wait for a hot plug
		 * event here since we don't have hpd interrupts enabled. We
		 * also need to swith the demux to HDMI mode to read hpd gpio,
		 * and then switch back to I2C2 in order to control the level
		 * shifter
		 */
		config_demux(dssdev->dev, SEL_HDMI);

		hpd = gpio_get_value_cansleep(ddata->hpd_gpio);

		config_demux(dssdev->dev, SEL_I2C2);

		if (gpio_is_valid(ddata->ls_oe_gpio)) {
			if (hpd)
				gpio_set_value_cansleep(ddata->ls_oe_gpio, 1);
			else
				gpio_set_value_cansleep(ddata->ls_oe_gpio, 0);
		}
	}

	return 0;
}

static void tpd_disconnect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	WARN_ON(dst != dssdev->dst);

	if (dst != dssdev->dst)
		return;

	gpio_set_value_cansleep(ddata->ct_cp_hpd_gpio, 0);

	dst->src = NULL;
	dssdev->dst = NULL;

	in->ops.hdmi->disconnect(in, &ddata->dssdev);
}

static int tpd_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE)
		return 0;

	in->ops.hdmi->set_timings(in, &ddata->timings);

	r = in->ops.hdmi->enable(in);
	if (r)
		return r;

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	return r;
}

static void tpd_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE)
		return;

	in->ops.hdmi->disable(in);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static void tpd_set_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	ddata->timings = *timings;
	dssdev->panel.timings = *timings;

	in->ops.hdmi->set_timings(in, timings);
}

static void tpd_get_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	*timings = ddata->timings;
}

static int tpd_check_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	r = in->ops.hdmi->check_timings(in, timings);

	return r;
}

static int tpd_read_edid(struct omap_dss_device *dssdev,
		u8 *edid, int len)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r = 0;

	if (ddata->disable_hpd)
		config_demux(dssdev->dev, SEL_HDMI);

	if (!gpio_get_value_cansleep(ddata->hpd_gpio))
		r = -ENODEV;

	if (ddata->disable_hpd)
		config_demux(dssdev->dev, SEL_I2C2);

	if (r)
		return r;

	config_demux(dssdev->dev, SEL_HDMI);

	r = in->ops.hdmi->read_edid(in, edid, len);

	config_demux(dssdev->dev, SEL_I2C2);

	return r;
}

static bool tpd_detect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	bool hpd;

	if (ddata->disable_hpd)
		config_demux(dssdev->dev, SEL_HDMI);

	hpd = gpio_get_value_cansleep(ddata->hpd_gpio);

	if (ddata->disable_hpd)
		config_demux(dssdev->dev, SEL_I2C2);

	return hpd;
}

static int tpd_audio_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	return in->ops.hdmi->audio_enable(in);
}

static void tpd_audio_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	in->ops.hdmi->audio_disable(in);
}

static int tpd_audio_start(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	return in->ops.hdmi->audio_start(in);
}

static void tpd_audio_stop(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	in->ops.hdmi->audio_stop(in);
}

static bool tpd_audio_supported(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	return in->ops.hdmi->audio_supported(in);
}

static int tpd_audio_config(struct omap_dss_device *dssdev,
		struct omap_dss_audio *audio)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	return in->ops.hdmi->audio_config(in, audio);
}

static const struct omapdss_hdmi_ops tpd_hdmi_ops = {
	.connect		= tpd_connect,
	.disconnect		= tpd_disconnect,

	.enable			= tpd_enable,
	.disable		= tpd_disable,

	.check_timings		= tpd_check_timings,
	.set_timings		= tpd_set_timings,
	.get_timings		= tpd_get_timings,

	.read_edid		= tpd_read_edid,
	.detect			= tpd_detect,

	.audio_enable		= tpd_audio_enable,
	.audio_disable		= tpd_audio_disable,
	.audio_start		= tpd_audio_start,
	.audio_stop		= tpd_audio_stop,
	.audio_supported	= tpd_audio_supported,
	.audio_config		= tpd_audio_config,
};

static int tpd_probe_pdata(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct encoder_tpd12s015_platform_data *pdata;
	struct omap_dss_device *dssdev, *in;

	pdata = dev_get_platdata(&pdev->dev);

	ddata->ct_cp_hpd_gpio = pdata->ct_cp_hpd_gpio;
	ddata->ls_oe_gpio = pdata->ls_oe_gpio;
	ddata->hpd_gpio = pdata->hpd_gpio;

	in = omap_dss_find_output(pdata->source);
	if (in == NULL) {
		dev_err(&pdev->dev, "Failed to find video source\n");
		return -ENODEV;
	}

	ddata->in = in;

	dssdev = &ddata->dssdev;
	dssdev->name = pdata->name;

	return 0;
}

static int tpd_probe_of(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;
	struct omap_dss_device *in;
	struct device_node *src_node;
	int gpio;

	src_node = of_parse_phandle(node, "video-source", 0);
	if (!src_node) {
		dev_err(&pdev->dev, "failed to parse video source\n");
		return -ENODEV;
	}

	in = omap_dss_find_output_by_node(src_node);
	if (in == NULL) {
		dev_err(&pdev->dev, "failed to find video source\n");
		return -EPROBE_DEFER;
	}
	ddata->in = in;

	/* CT CP HPD GPIO */
	gpio = of_get_gpio(node, 0);
	if (!gpio_is_valid(gpio)) {
		dev_err(&pdev->dev, "failed to parse CT CP HPD gpio\n");
		return gpio;
	}
	ddata->ct_cp_hpd_gpio = gpio;

	/* LS OE GPIO */
	gpio = of_get_gpio(node, 1);
	if (gpio_is_valid(gpio) || gpio == -ENOENT) {
		ddata->ls_oe_gpio = gpio;
	} else {
		dev_err(&pdev->dev, "failed to parse LS OE gpio\n");
		return gpio;
	}

	/* HPD GPIO */
	gpio = of_get_gpio(node, 2);
	if (!gpio_is_valid(gpio)) {
		dev_err(&pdev->dev, "failed to parse HPD gpio\n");
		return gpio;
	}
	ddata->hpd_gpio = gpio;

	if (of_find_property(node, "disable-hpd", NULL))
		ddata->disable_hpd = true;

	return 0;
}

static int tpd_probe(struct platform_device *pdev)
{
	struct omap_dss_device *in, *dssdev;
	struct panel_drv_data *ddata;
	int r;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);

	init_completion(&ddata->hpd_completion);

	if (dev_get_platdata(&pdev->dev)) {
		r = tpd_probe_pdata(pdev);
		if (r)
			return r;
	} else if (pdev->dev.of_node) {
		r = tpd_probe_of(pdev);
		if (r)
			return r;
	} else {
		return -ENODEV;
	}

	/* configure the SEL_HDMI_I2C2 line going to the demux */
	config_sel_hdmi_i2c2(&pdev->dev);

	config_demux(&pdev->dev, SEL_I2C2);

	r = devm_gpio_request_one(&pdev->dev, ddata->ct_cp_hpd_gpio,
			GPIOF_OUT_INIT_LOW, "hdmi_ct_cp_hpd");
	if (r)
		goto err_gpio;

	if (gpio_is_valid(ddata->ls_oe_gpio)) {
		r = devm_gpio_request_one(&pdev->dev, ddata->ls_oe_gpio,
				GPIOF_OUT_INIT_LOW, "hdmi_ls_oe");
		if (r)
			goto err_gpio;
	}

	if (ddata->disable_hpd)
		config_demux(&pdev->dev, SEL_HDMI);

	r = devm_gpio_request_one(&pdev->dev, ddata->hpd_gpio,
			GPIOF_DIR_IN, "hdmi_hpd");

	if (ddata->disable_hpd)
		config_demux(&pdev->dev, SEL_I2C2);
	if (r)
		goto err_gpio;

	/*
	 * we see some low voltage glitches on the HPD_B line before it
	 * stabalizes to around 5V. We see the effects of this glitch on the
	 * HPD_A side, and hence on the gpio on DRA7x. The glitch is quite short
	 * in duration, but it takes a while for the voltage to go down back to
	 * 0 volts, we set a debounce value of 1 millisecond to prevent this,
	 * the reason for the glitch not being taken care of by the TPD chip
	 * needs to be investigated
	 */
	if (!ddata->disable_hpd) {
		r = gpio_set_debounce(ddata->hpd_gpio, HPD_DEBOUNCE_TIME);
		if (r)
			goto err_debounce;

		r = devm_request_threaded_irq(&pdev->dev,
				gpio_to_irq(ddata->hpd_gpio), NULL,
				tpd_hpd_irq_handler, IRQF_TRIGGER_RISING |
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "hpd",
				ddata);
		if (r)
			goto err_irq;
	}

	dssdev = &ddata->dssdev;
	dssdev->ops.hdmi = &tpd_hdmi_ops;
	dssdev->dev = &pdev->dev;
	dssdev->type = OMAP_DISPLAY_TYPE_HDMI;
	dssdev->output_type = OMAP_DISPLAY_TYPE_HDMI;
	dssdev->owner = THIS_MODULE;

	in = ddata->in;

	r = omapdss_register_output(dssdev);
	if (r) {
		dev_err(&pdev->dev, "Failed to register output\n");
		goto err_reg;
	}

	return 0;
err_reg:
err_debounce:
err_irq:
err_gpio:
	omap_dss_put_device(ddata->in);
	return r;
}

static int __exit tpd_remove(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct omap_dss_device *dssdev = &ddata->dssdev;
	struct omap_dss_device *in = ddata->in;

	omapdss_unregister_output(&ddata->dssdev);

	WARN_ON(omapdss_device_is_enabled(dssdev));
	if (omapdss_device_is_enabled(dssdev))
		tpd_disable(dssdev);

	WARN_ON(omapdss_device_is_connected(dssdev));
	if (omapdss_device_is_connected(dssdev))
		tpd_disconnect(dssdev, dssdev->dst);

	omap_dss_put_device(in);

	return 0;
}

static const struct of_device_id tpd_of_match[] = {
	{ .compatible = "ti,draevm-tpd12s015", },
	{},
};

MODULE_DEVICE_TABLE(of, tpd_of_match);

static struct platform_driver tpd_driver = {
	.probe	= tpd_probe,
	.remove	= __exit_p(tpd_remove),
	.driver	= {
		.name	= "draevm-tpd12s015",
		.owner	= THIS_MODULE,
		.of_match_table = tpd_of_match,
	},
};

module_platform_driver(tpd_driver);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ti.com>");
MODULE_DESCRIPTION("DRAEVM-TPD12S015 driver");
MODULE_LICENSE("GPL");
