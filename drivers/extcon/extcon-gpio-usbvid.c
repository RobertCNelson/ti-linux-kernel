/*
 * Generic  USB VBUS-ID pin detection driver
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Author: George Cherian <george.cherian@ti.com>
 *
 * Based on extcon-palmas.c
 *
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

struct gpio_usbvid {
	struct device *dev;

	struct extcon_dev edev;

	/* GPIO pin */
	int id_gpio;
	int vbus_gpio;

	int id_irq;
	int vbus_irq;
	int type;
};

static const char *dra7xx_extcon_cable[] = {
	[0] = "USB",
	[1] = "USB-HOST",
	NULL,
};

static const int mutually_exclusive[] = {0x3, 0x0};

/* Two types of support are provided.
 * Systems which has
 *	1) VBUS and ID pin connected via GPIO
 *	2) only ID pin connected via GPIO
 *  For Case 1 both the gpios should be provided via DT
 *  Always the first GPIO in dt is considered ID pin GPIO
 */

enum {
	UNKNOWN = 0,
	ID_DETECT,
	VBUS_ID_DETECT,
};

#define ID_GND		0
#define ID_FLOAT	1
#define VBUS_OFF	0
#define VBUS_ON		1

static irqreturn_t id_irq_handler(int irq, void *data)
{
	struct gpio_usbvid *gpio_usbvid = (struct gpio_usbvid *)data;
	int id_current;

	id_current = gpio_get_value_cansleep(gpio_usbvid->id_gpio);
	if (id_current == ID_GND) {
		if (gpio_usbvid->type == ID_DETECT)
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB", false);
		extcon_set_cable_state(&gpio_usbvid->edev, "USB-HOST", true);
	} else {
		extcon_set_cable_state(&gpio_usbvid->edev, "USB-HOST", false);
		if (gpio_usbvid->type == ID_DETECT)
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB", true);
	}

	return IRQ_HANDLED;
}

static irqreturn_t vbus_irq_handler(int irq, void *data)
{
	struct gpio_usbvid *gpio_usbvid = (struct gpio_usbvid *)data;
	int vbus_current;

	vbus_current = gpio_get_value_cansleep(gpio_usbvid->vbus_gpio);
	if (vbus_current == VBUS_OFF)
		extcon_set_cable_state(&gpio_usbvid->edev, "USB", false);
	else
		extcon_set_cable_state(&gpio_usbvid->edev, "USB", true);

	return IRQ_HANDLED;
}

static void gpio_usbvid_set_initial_state(struct gpio_usbvid *gpio_usbvid)
{
	int id_current, vbus_current;

	switch (gpio_usbvid->type) {
	case ID_DETECT:
		id_current = gpio_get_value_cansleep(gpio_usbvid->id_gpio);
		if (!!id_current == ID_FLOAT) {
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB-HOST", false);
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB", true);
		} else {
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB", false);
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB-HOST", true);
		}
		break;

	case VBUS_ID_DETECT:
		id_current = gpio_get_value_cansleep(gpio_usbvid->id_gpio);
		if (!!id_current == ID_FLOAT)
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB-HOST", false);
		else
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB-HOST", true);

		vbus_current = gpio_get_value_cansleep(gpio_usbvid->vbus_gpio);
		if (!!vbus_current == VBUS_ON)
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB", true);
		else
			extcon_set_cable_state(&gpio_usbvid->edev,
							"USB", false);
		break;

	default:
		dev_err(gpio_usbvid->dev, "Unknown VBUS-ID type\n");
	}
}

static int gpio_usbvid_request_irq(struct gpio_usbvid *gpio_usbvid)
{
	int ret;

	ret = devm_request_threaded_irq(gpio_usbvid->dev, gpio_usbvid->id_irq,
					NULL, id_irq_handler,
					IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
					dev_name(gpio_usbvid->dev),
					(void *) gpio_usbvid);
	if (ret) {
		dev_err(gpio_usbvid->dev, "failed to request id irq #%d\n",
					gpio_usbvid->id_irq);
		return ret;
	}
	if (gpio_usbvid->type == VBUS_ID_DETECT) {
		ret = devm_request_threaded_irq(gpio_usbvid->dev,
					gpio_usbvid->vbus_irq, NULL,
					vbus_irq_handler,
					IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
					dev_name(gpio_usbvid->dev),
					(void *) gpio_usbvid);
		if (ret)
			dev_err(gpio_usbvid->dev, "failed to request vbus irq #%d\n",
						gpio_usbvid->vbus_irq);
	}

	return ret;
}

static int gpio_usbvid_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct gpio_usbvid *gpio_usbvid;
	int ret, gpio;

	gpio_usbvid = devm_kzalloc(&pdev->dev, sizeof(*gpio_usbvid),
				GFP_KERNEL);
	if (!gpio_usbvid)
		return -ENOMEM;

	gpio_usbvid->dev = &pdev->dev;

	platform_set_drvdata(pdev, gpio_usbvid);

	//gpio_usbvid->edev.name = dev_name(&pdev->dev);
	gpio_usbvid->edev.supported_cable = dra7xx_extcon_cable;
	gpio_usbvid->edev.mutually_exclusive = mutually_exclusive;

	if (of_device_is_compatible(node, "ti,gpio-usb-id"))
		gpio_usbvid->type = ID_DETECT;

	gpio = of_get_gpio(node, 0);
	if (gpio_is_valid(gpio)) {
		gpio_usbvid->id_gpio = gpio;
		ret = devm_gpio_request(&pdev->dev, gpio_usbvid->id_gpio,
					"id_gpio");
		if (ret)
			return ret;

		gpio_usbvid->id_irq = gpio_to_irq(gpio_usbvid->id_gpio);
	} else {
		dev_err(&pdev->dev, "failed to get id gpio\n");
		return -EPROBE_DEFER;
	}

	if (of_device_is_compatible(node, "ti,gpio-usb-vid")) {
		gpio_usbvid->type = VBUS_ID_DETECT;
		gpio = of_get_gpio(node, 1);
		if (gpio_is_valid(gpio)) {
			gpio_usbvid->vbus_gpio = gpio;
			ret = devm_gpio_request(&pdev->dev,
						gpio_usbvid->vbus_gpio,
						"vbus_gpio");
			if (ret)
				return ret;

			gpio_usbvid->vbus_irq =
					gpio_to_irq(gpio_usbvid->vbus_gpio);
		} else {
			dev_err(&pdev->dev, "failed to get vbus gpio\n");
			return -ENODEV;
		}
	}

	ret = gpio_usbvid_request_irq(gpio_usbvid);
	if (ret)
		return ret;

	ret = extcon_dev_register(&gpio_usbvid->edev, gpio_usbvid->dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register extcon device\n");
		return ret;
	}

	gpio_usbvid_set_initial_state(gpio_usbvid);

	return 0;

}

static int gpio_usbvid_remove(struct platform_device *pdev)
{
	struct gpio_usbvid *gpio_usbvid = platform_get_drvdata(pdev);

	extcon_dev_unregister(&gpio_usbvid->edev);
	return 0;
}

static struct of_device_id of_gpio_usbvid_match_tbl[] = {
	{ .compatible = "ti,gpio-usb-vid", },
	{ .compatible = "ti,gpio-usb-id", },
	{ /* end */ }
};

static struct platform_driver gpio_usbvid_driver = {
	.probe = gpio_usbvid_probe,
	.remove = gpio_usbvid_remove,
	.driver = {
		.name = "gpio-usbvid",
		.of_match_table = of_gpio_usbvid_match_tbl,
		.owner = THIS_MODULE,
	},
};

module_platform_driver(gpio_usbvid_driver);

MODULE_ALIAS("platform:gpio-usbvid");
MODULE_AUTHOR("George Cherian <george.cherian@ti.com>");
MODULE_DESCRIPTION("GPIO based USB Connector driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, of_gpio_usbvid_match_tbl);
