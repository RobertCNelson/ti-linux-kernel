/*
 * Si-En SN3218 18 Channel LED Driver
 *
 * Copyright (C) 2016 Stefan Wahren <stefan.wahren@i2se.com>
 *
 * Based on leds-pca963x.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * Datasheet: http://www.si-en.com/uploadpdf/s2011517171720.pdf
 *
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define SN3218_MODE		0x00
#define SN3218_PWM_1		0x01
#define SN3218_PWM_2		0x02
#define SN3218_PWM_3		0x03
#define SN3218_PWM_4		0x04
#define SN3218_PWM_5		0x05
#define SN3218_PWM_6		0x06
#define SN3218_PWM_7		0x07
#define SN3218_PWM_8		0x08
#define SN3218_PWM_9		0x09
#define SN3218_PWM_10		0x0a
#define SN3218_PWM_11		0x0b
#define SN3218_PWM_12		0x0c
#define SN3218_PWM_13		0x0d
#define SN3218_PWM_14		0x0e
#define SN3218_PWM_15		0x0f
#define SN3218_PWM_16		0x10
#define SN3218_PWM_17		0x11
#define SN3218_PWM_18		0x12
#define SN3218_LED_1_6		0x13
#define SN3218_LED_7_12		0x14
#define SN3218_LED_13_18	0x15
#define SN3218_UPDATE		0x16	/* applies to reg 0x01 .. 0x15 */
#define SN3218_RESET		0x17

#define SN3218_LED_MASK		0x3f
#define SN3218_LED_ON		0x01
#define SN3218_LED_OFF		0x00

#define SN3218_MODE_SHUTDOWN	0x00
#define SN3218_MODE_NORMAL	0x01

#define NUM_LEDS		18

struct sn3218_led;

/**
 * struct sn3218 -
 * @client - Pointer to the I2C client
 * @leds - Pointer to the individual LEDs
 * @num_leds - Actual number of LEDs
**/
struct sn3218 {
	struct i2c_client *client;
	struct regmap *regmap;
	struct sn3218_led *leds;
	int num_leds;
};

/**
 * struct sn3218_led -
 * @chip - Pointer to the container
 * @led_cdev - led class device pointer
 * @led_num - LED index ( 0 .. 17 )
**/
struct sn3218_led {
	struct sn3218 *chip;
	struct led_classdev led_cdev;
	int led_num;
};

static int sn3218_led_set(struct led_classdev *led_cdev,
			  enum led_brightness brightness)
{
	struct sn3218_led *led =
			container_of(led_cdev, struct sn3218_led, led_cdev);
	struct regmap *regmap = led->chip->regmap;
	u8 bank = led->led_num / 6;
	u8 mask = 0x1 << (led->led_num % 6);
	u8 val;
	int ret;

	if (brightness == LED_OFF)
		val = 0;
	else
		val = mask;

	ret = regmap_update_bits(regmap, SN3218_LED_1_6 + bank, mask, val);
	if (ret < 0)
		return ret;

	if (brightness > LED_OFF) {
		ret = regmap_write(regmap, SN3218_PWM_1 + led->led_num,
				   brightness);
		if (ret < 0)
			return ret;
	}

	ret = regmap_write(regmap, SN3218_UPDATE, 0xff);

	return ret;
}

static void sn3218_led_init(struct sn3218 *sn3218, struct device_node *node,
			    u32 reg)
{
	struct sn3218_led *leds = sn3218->leds;
	struct led_classdev *cdev = &leds[reg].led_cdev;

	leds[reg].led_num = reg;
	leds[reg].chip = sn3218;

	if (of_property_read_string(node, "label", &cdev->name))
		cdev->name = node->name;

	of_property_read_string(node, "linux,default-trigger",
				&cdev->default_trigger);

	cdev->brightness_set_blocking = sn3218_led_set;
}

static const struct reg_default sn3218_reg_defs[] = {
	{ SN3218_MODE, 0x00},
	{ SN3218_PWM_1, 0x00},
	{ SN3218_PWM_2, 0x00},
	{ SN3218_PWM_3, 0x00},
	{ SN3218_PWM_4, 0x00},
	{ SN3218_PWM_5, 0x00},
	{ SN3218_PWM_6, 0x00},
	{ SN3218_PWM_7, 0x00},
	{ SN3218_PWM_8, 0x00},
	{ SN3218_PWM_9, 0x00},
	{ SN3218_PWM_10, 0x00},
	{ SN3218_PWM_11, 0x00},
	{ SN3218_PWM_12, 0x00},
	{ SN3218_PWM_13, 0x00},
	{ SN3218_PWM_14, 0x00},
	{ SN3218_PWM_15, 0x00},
	{ SN3218_PWM_16, 0x00},
	{ SN3218_PWM_17, 0x00},
	{ SN3218_PWM_18, 0x00},
	{ SN3218_LED_1_6, 0x00},
	{ SN3218_LED_7_12, 0x00},
	{ SN3218_LED_13_18, 0x00},
	{ SN3218_UPDATE, 0x00},
	{ SN3218_RESET, 0x00},
};

static const struct regmap_config sn3218_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = SN3218_RESET,
	.reg_defaults = sn3218_reg_defs,
	.num_reg_defaults = ARRAY_SIZE(sn3218_reg_defs),
	.cache_type = REGCACHE_RBTREE,
};

static int sn3218_init(struct i2c_client *client, struct sn3218 *sn3218)
{
	struct device_node *np = client->dev.of_node, *child;
	struct sn3218_led *leds;
	int ret, count;

	count = of_get_child_count(np);
	if (!count)
		return -ENODEV;

	if (count > NUM_LEDS) {
		dev_err(&client->dev, "Invalid LED count %d\n", count);
		return -EINVAL;
	}

	leds = devm_kzalloc(&client->dev, count * sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	sn3218->leds = leds;
	sn3218->num_leds = count;
	sn3218->client = client;

	sn3218->regmap = devm_regmap_init_i2c(client, &sn3218_regmap_config);
	if (IS_ERR(sn3218->regmap)) {
		ret = PTR_ERR(sn3218->regmap);
		dev_err(&client->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	for_each_child_of_node(np, child) {
		u32 reg;

		ret = of_property_read_u32(child, "reg", &reg);
		if (ret)
			goto fail;

		if (reg >= count) {
			dev_err(&client->dev, "Invalid LED (%u >= %d)\n", reg,
				count);
			ret = -EINVAL;
			goto fail;
		}

		sn3218_led_init(sn3218, child, reg);
	}

	return 0;

fail:
	of_node_put(child);
	return ret;
}

static int sn3218_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct sn3218 *sn3218;
	struct sn3218_led *leds;
	struct device *dev = &client->dev;
	int i, ret;

	sn3218 = devm_kzalloc(dev, sizeof(*sn3218), GFP_KERNEL);
	if (!sn3218)
		return -ENOMEM;

	ret = sn3218_init(client, sn3218);
	if (ret)
		return ret;

	i2c_set_clientdata(client, sn3218);
	leds = sn3218->leds;

	/*
	 * Since the chip is write-only we need to reset him into
	 * a defined state (all LEDs off).
	 */
	ret = regmap_write(sn3218->regmap, SN3218_RESET, 0xff);
	if (ret)
		return ret;

	for (i = 0; i < sn3218->num_leds; i++) {
		ret = devm_led_classdev_register(dev, &leds[i].led_cdev);
		if (ret < 0)
			return ret;
	}

	return regmap_write(sn3218->regmap, SN3218_MODE, SN3218_MODE_NORMAL);
}

static int sn3218_remove(struct i2c_client *client)
{
	struct sn3218 *sn3218 = i2c_get_clientdata(client);

	regmap_write(sn3218->regmap, SN3218_MODE, SN3218_MODE_SHUTDOWN);

	return 0;
}

static void sn3218_shutdown(struct i2c_client *client)
{
	struct sn3218 *sn3218 = i2c_get_clientdata(client);

	regmap_write(sn3218->regmap, SN3218_MODE, SN3218_MODE_SHUTDOWN);
}

static const struct i2c_device_id sn3218_id[] = {
	{ "sn3218", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sn3218_id);

static const struct of_device_id of_sn3218_match[] = {
	{ .compatible = "si-en,sn3218", },
	{},
};
MODULE_DEVICE_TABLE(of, of_sn3218_match);

static struct i2c_driver sn3218_driver = {
	.driver = {
		.name	= "leds-sn3218",
		.of_match_table = of_match_ptr(of_sn3218_match),
	},
	.probe	= sn3218_probe,
	.remove	= sn3218_remove,
	.shutdown = sn3218_shutdown,
	.id_table = sn3218_id,
};

module_i2c_driver(sn3218_driver);

MODULE_DESCRIPTION("Si-En SN3218 LED Driver");
MODULE_AUTHOR("Stefan Wahren <stefan.wahren@i2se.com>");
MODULE_LICENSE("GPL v2");
