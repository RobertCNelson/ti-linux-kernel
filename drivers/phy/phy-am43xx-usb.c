/*
 * phy-am43xx-usb.c - USB PHY, talking to usb controller in AMXXX.
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Author: George Cherian <george.cherian@ti.com>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/usb/otg.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/pm_runtime.h>
#include <linux/phy/omap_control_phy.h>
#include <linux/phy/phy.h>
#include <linux/of_platform.h>

struct am43xx_usb {
	struct usb_phy	phy;
	struct device	*dev;
	struct device	*control_dev;
	struct clk	*wkupclk;
	struct clk	*optclk;
	u32		id;
};

#define phy_to_am43xxphy(x)	container_of((x), struct amxxxx_phy, phy)


static int am43xx_usb_set_host(struct usb_otg *otg, struct usb_bus *host)
{
	struct usb_phy	*phy = otg->phy;

	otg->host = host;
	if (!host)
		phy->state = OTG_STATE_UNDEFINED;

	return 0;
}

static int am43xx_usb_set_peripheral(struct usb_otg *otg,
		struct usb_gadget *gadget)
{
	struct usb_phy	*phy = otg->phy;

	otg->gadget = gadget;
	if (!gadget)
		phy->state = OTG_STATE_UNDEFINED;

	return 0;
}



static int am43xx_usb_power_off(struct phy *x)
{
	struct am43xx_usb *phy = phy_get_drvdata(x);

	omap_control_phy_power(phy->control_dev, 0);

	return 0;
}

static int am43xx_usb_power_on(struct phy *x)
{
	struct am43xx_usb *phy = phy_get_drvdata(x);

	omap_control_phy_power(phy->control_dev, 1);

	return 0;
}

static struct phy_ops ops = {
	.power_on	= am43xx_usb_power_on,
	.power_off	= am43xx_usb_power_off,
	.owner		= THIS_MODULE,
};

static int am43xx_usb2_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct am43xx_usb			*phy;
	struct device_node *control_node;
	struct platform_device *control_pdev;
	struct phy			*generic_phy;
	struct usb_otg			*otg;
	struct phy_provider		*phy_provider;

	phy = devm_kzalloc(&pdev->dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	otg = devm_kzalloc(&pdev->dev, sizeof(*otg), GFP_KERNEL);
	if (!otg)
		return -ENOMEM;

	phy->dev		= &pdev->dev;


	phy->phy.dev		= phy->dev;
	phy->phy.label		= "am43xx-usb2";
	phy->phy.otg		= otg;
	phy->phy.type		= USB_PHY_TYPE_USB2;

	phy_provider = devm_of_phy_provider_register(phy->dev,
			of_phy_simple_xlate);
	if (IS_ERR(phy_provider))
		return PTR_ERR(phy_provider);

	control_node = of_parse_phandle(node, "ctrl-module", 0);
	if (!control_node) {
		dev_err(&pdev->dev, "Failed to get control device phandle\n");
		return -EINVAL;
	}

	control_pdev = of_find_device_by_node(control_node);
	if (!control_pdev) {
		dev_err(&pdev->dev, "Failed to get control device\n");
		return -EINVAL;
	}

	phy->control_dev = &control_pdev->dev;

	omap_control_phy_power(phy->control_dev, 0);

	otg->set_host		= am43xx_usb_set_host;
	otg->set_peripheral	= am43xx_usb_set_peripheral;
	otg->phy		= &phy->phy;

	platform_set_drvdata(pdev, phy);
	pm_runtime_enable(phy->dev);

	generic_phy = devm_phy_create(phy->dev, &ops, NULL);
	if (IS_ERR(generic_phy))
		return PTR_ERR(generic_phy);

	phy_set_drvdata(generic_phy, phy);

	phy->wkupclk = devm_clk_get(phy->dev, "wkupclk");
	if (IS_ERR(phy->wkupclk))
		dev_err(&pdev->dev, "unable to get wkupclk\n");
	else
		clk_prepare(phy->wkupclk);

	phy->optclk = devm_clk_get(phy->dev, "refclk");
	if (IS_ERR(phy->optclk))
		dev_dbg(&pdev->dev, "unable to get refclk\n");
	else
		clk_prepare(phy->optclk);

	device_init_wakeup(&pdev->dev, true);
	usb_add_phy_dev(&phy->phy);

	return 0;
}

static int am43xx_usb2_remove(struct platform_device *pdev)
{
	struct am43xx_usb	*phy = platform_get_drvdata(pdev);

	clk_unprepare(phy->optclk);
	usb_remove_phy(&phy->phy);

	return 0;
}

#ifdef CONFIG_PM_RUNTIME

static int am43xx_usb2_runtime_suspend(struct device *dev)
{
	struct platform_device	*pdev = to_platform_device(dev);
	struct am43xx_usb	*phy = platform_get_drvdata(pdev);

	omap_control_phy_power(phy->control_dev, 0);
#if 0
	if (device_may_wakeup(dev))
		omap_control_phy_wkup(phy->control_dev, 1);
#endif
	clk_disable(phy->optclk);

	return 0;
}

static int am43xx_usb2_runtime_resume(struct device *dev)
{
	struct platform_device	*pdev = to_platform_device(dev);
	struct am43xx_usb	*phy = platform_get_drvdata(pdev);


	omap_control_phy_power(phy->control_dev, 1);
#if 0
	if (device_may_wakeup(dev))
		omap_control_phy_wkup(phy->control_dev, 0);
#endif
	clk_enable(phy->optclk);

	return 0;

}

static const struct dev_pm_ops am43xx_usb2_pm_ops = {
	SET_RUNTIME_PM_OPS(am43xx_usb2_runtime_suspend,
		am43xx_usb2_runtime_resume, NULL)
};

#define DEV_PM_OPS     (&am43xx_usb2_pm_ops)
#else
#define DEV_PM_OPS     NULL
#endif

#ifdef CONFIG_OF
static const struct of_device_id am43xx_usb2_id_table[] = {
	{ .compatible = "ti,am43xx-usb2" },
	{}
};
MODULE_DEVICE_TABLE(of, am43xx_usb2_id_table);
#endif

static struct platform_driver am43xx_usb2_driver = {
	.probe		= am43xx_usb2_probe,
	.remove		= am43xx_usb2_remove,
	.driver		= {
		.name	= "am43xx-usb2",
		.owner	= THIS_MODULE,
		.pm	= DEV_PM_OPS,
		.of_match_table = of_match_ptr(am43xx_usb2_id_table),
	},
};

module_platform_driver(am43xx_usb2_driver);

MODULE_ALIAS("platform: am43xx_usb2");
MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_DESCRIPTION("AMXXXX USB2 phy driver");
MODULE_LICENSE("GPL v2");
