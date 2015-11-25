/*
 * led driver for RT5033
 *
 * Copyright (C) 2015 Samsung Electronics, Co., Ltd.
 * Ingi Kim <ingi2.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/led-class-flash.h>
#include <linux/mfd/rt5033.h>
#include <linux/mfd/rt5033-private.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#define RT5033_LED_FLASH_TIMEOUT_MIN		64000
#define RT5033_LED_FLASH_TIMEOUT_STEP		32000
#define RT5033_LED_FLASH_BRIGHTNESS_MIN		50000
#define RT5033_LED_FLASH_BRIGHTNESS_MAX_1CH	600000
#define RT5033_LED_FLASH_BRIGHTNESS_MAX_2CH	800000
#define RT5033_LED_FLASH_BRIGHTNESS_STEP	25000
#define RT5033_LED_TORCH_BRIGHTNESS_MIN		12500
#define RT5033_LED_TORCH_BRIGHTNESS_STEP	12500

#define FLED1_IOUT		(BIT(0))
#define FLED2_IOUT		(BIT(1))

enum rt5033_fled {
	FLED1,
	FLED2,
};

struct rt5033_sub_led {
	enum rt5033_fled fled_id;
	struct led_classdev_flash fled_cdev;

	u32 flash_brightness;
	u32 flash_timeout;
};

/* RT5033 Flash led platform data */
struct rt5033_led {
	struct device *dev;
	struct mutex lock;
	struct regmap *regmap;
	struct rt5033_sub_led sub_leds[2];

	u32 current_flash_timeout;
	u32 current_flash_brightness;

	bool iout_joint;
	u8 fled_mask;
	u8 current_iout;
};

struct rt5033_led_config_data {
	const char *label[2];
	u32 flash_max_microamp[2];
	u32 flash_max_timeout[2];
	u32 torch_max_microamp[2];
	u32 num_leds;
};

static u8 rt5033_torch_brightness_to_reg(u32 ua)
{
	return (ua - RT5033_LED_TORCH_BRIGHTNESS_MIN) /
		RT5033_LED_TORCH_BRIGHTNESS_STEP;
}

static u8 rt5033_flash_brightness_to_reg(u32 ua)
{
	return (ua - RT5033_LED_FLASH_BRIGHTNESS_MIN) /
		RT5033_LED_FLASH_BRIGHTNESS_STEP;
}

static u8 rt5033_flash_timeout_to_reg(u32 us)
{
	return (us - RT5033_LED_FLASH_TIMEOUT_MIN) /
		RT5033_LED_FLASH_TIMEOUT_STEP;
}

static struct rt5033_sub_led *flcdev_to_sub_led(
					struct led_classdev_flash *fled_cdev)
{
	return container_of(fled_cdev, struct rt5033_sub_led, fled_cdev);
}

static struct rt5033_led *sub_led_to_led(struct rt5033_sub_led *sub_led)
{
	return container_of(sub_led, struct rt5033_led,
			    sub_leds[sub_led->fled_id]);
}

static bool rt5033_fled_used(struct rt5033_led *led, enum rt5033_fled fled_id)
{
	u8 fled_bit = (fled_id == FLED1) ? FLED1_IOUT : FLED2_IOUT;

	return led->fled_mask & fled_bit;
}

static u8 rt5033_get_iout_to_set(struct rt5033_led *led,
				 enum rt5033_fled fled_id)
{
	u8 fled_bit;

	if (led->iout_joint)
		fled_bit = FLED1_IOUT | FLED2_IOUT;
	else
		fled_bit = (fled_id == FLED1) ? FLED1_IOUT : FLED2_IOUT;

	return fled_bit;
}

static int rt5033_led_iout_disable(struct rt5033_led *led,
				   enum rt5033_fled fled_id)
{
	int ret;
	u8 fled_bit;

	fled_bit = rt5033_get_iout_to_set(led, fled_id);
	led->current_iout &= ~fled_bit;

	ret = regmap_update_bits(led->regmap, RT5033_REG_FLED_FUNCTION1,
				 RT5033_FLED_FUNC1_MASK,
				 RT5033_FLED_PINCTRL | led->current_iout);

	return ret;
}

static int rt5033_set_flash_current(struct rt5033_led *led, u32 micro_amp)
{
	u8 v;
	int ret;

	v = rt5033_flash_brightness_to_reg(micro_amp);

	ret = regmap_write(led->regmap, RT5033_REG_FLED_STROBE_CTRL1, v);
	if (ret < 0)
		return ret;

	led->current_flash_brightness = micro_amp;

	return 0;
}

static int rt5033_set_timeout(struct rt5033_led *led, u32 microsec)
{
	u8 v;
	int ret;

	v = rt5033_flash_timeout_to_reg(microsec);

	ret = regmap_write(led->regmap, RT5033_REG_FLED_STROBE_CTRL2, v);
	if (ret < 0)
		return ret;

	led->current_flash_timeout = microsec;

	return 0;
}

static int rt5033_led_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness brightness)
{
	struct led_classdev_flash *fled_cdev = lcdev_to_flcdev(led_cdev);
	struct rt5033_sub_led *sub_led = flcdev_to_sub_led(fled_cdev);
	struct rt5033_led *led = sub_led_to_led(sub_led);
	int fled_id = sub_led->fled_id, ret;
	u8 fled_bit;

	mutex_lock(&led->lock);

	if (!brightness) {
		ret = rt5033_led_iout_disable(led, fled_id);
		goto torch_unlock;
	}

	fled_bit = rt5033_get_iout_to_set(led, fled_id);

	ret = regmap_update_bits(led->regmap, RT5033_REG_FLED_CTRL1,
				 RT5033_FLED_CTRL1_MASK, (brightness - 1) << 4);
	if (ret < 0)
		goto torch_unlock;

	if (led->current_iout != fled_bit) {
		ret = regmap_update_bits(led->regmap, RT5033_REG_FLED_FUNCTION1,
					 RT5033_FLED_FUNC1_MASK,
					 RT5033_FLED_PINCTRL | fled_bit);
		if (ret < 0)
			goto torch_unlock;
		led->current_iout = fled_bit;
	}
	ret = regmap_update_bits(led->regmap, RT5033_REG_FLED_FUNCTION2,
				 RT5033_FLED_FUNC2_MASK, RT5033_FLED_ENFLED);

torch_unlock:
	mutex_unlock(&led->lock);
	return ret;
}

static int rt5033_led_flash_brightness_set(struct led_classdev_flash *fled_cdev,
					   u32 brightness)
{
	struct rt5033_sub_led *sub_led = flcdev_to_sub_led(fled_cdev);
	struct rt5033_led *led = sub_led_to_led(sub_led);

	mutex_lock(&led->lock);
	sub_led->flash_brightness = brightness;
	mutex_unlock(&led->lock);

	return 0;
}

static int rt5033_led_flash_timeout_set(struct led_classdev_flash *fled_cdev,
					u32 timeout)
{
	struct rt5033_sub_led *sub_led = flcdev_to_sub_led(fled_cdev);
	struct rt5033_led *led = sub_led_to_led(sub_led);

	mutex_lock(&led->lock);
	sub_led->flash_timeout = timeout;
	mutex_unlock(&led->lock);

	return 0;
}

static int rt5033_led_flash_strobe_set(struct led_classdev_flash *fled_cdev,
				       bool state)
{
	struct rt5033_sub_led *sub_led = flcdev_to_sub_led(fled_cdev);
	struct rt5033_led *led = sub_led_to_led(sub_led);
	enum rt5033_fled fled_id = sub_led->fled_id;
	int ret;
	u8 fled_bit;

	mutex_lock(&led->lock);

	fled_bit = rt5033_get_iout_to_set(led, fled_id);
	led->current_iout = fled_bit;

	if (state == 0) {
		ret = rt5033_led_iout_disable(led, fled_id);
		if (ret < 0)
			goto strobe_unlock;
		ret = regmap_update_bits(led->regmap,
					 RT5033_REG_FLED_FUNCTION2,
					 RT5033_FLED_FUNC2_MASK, 0);
		goto strobe_unlock;
	}

	if (sub_led->flash_brightness != led->current_flash_brightness) {
		ret = rt5033_set_flash_current(led, sub_led->flash_brightness);
		if (ret < 0)
			goto strobe_unlock;
	}

	if (sub_led->flash_timeout != led->current_flash_timeout) {
		ret = rt5033_set_timeout(led, sub_led->flash_timeout);
		if (ret < 0)
			goto strobe_unlock;
	}

	ret = regmap_update_bits(led->regmap, RT5033_REG_FLED_FUNCTION1,
				 RT5033_FLED_FUNC1_MASK, RT5033_FLED_PINCTRL |
				 RT5033_FLED_STRB_SEL | led->current_iout);
	if (ret < 0)
		goto strobe_unlock;
	ret = regmap_update_bits(led->regmap, RT5033_REG_FLED_FUNCTION2,
				 RT5033_FLED_FUNC2_MASK, RT5033_FLED_ENFLED |
				 RT5033_FLED_SREG_STRB);

	led->current_iout = 0;
strobe_unlock:
	mutex_unlock(&led->lock);
	return ret;
}

static const struct led_flash_ops flash_ops = {
	.flash_brightness_set = rt5033_led_flash_brightness_set,
	.strobe_set = rt5033_led_flash_strobe_set,
	.timeout_set = rt5033_led_flash_timeout_set,
};

static void rt5033_init_flash_properties(struct rt5033_sub_led *sub_led,
					 struct rt5033_led_config_data *led_cfg)
{
	struct led_classdev_flash *fled_cdev = &sub_led->fled_cdev;
	struct rt5033_led *led = sub_led_to_led(sub_led);
	struct led_flash_setting *tm_set, *fl_set;
	enum rt5033_fled fled_id = sub_led->fled_id;

	tm_set = &fled_cdev->timeout;
	tm_set->min = RT5033_LED_FLASH_TIMEOUT_MIN;
	tm_set->max = led_cfg->flash_max_timeout[fled_id];
	tm_set->step = RT5033_LED_FLASH_TIMEOUT_STEP;
	tm_set->val = tm_set->max;

	fl_set = &fled_cdev->brightness;
	fl_set->min = RT5033_LED_FLASH_BRIGHTNESS_MIN;
	if (led->iout_joint)
		fl_set->max = min(led_cfg->flash_max_microamp[FLED1] +
				  led_cfg->flash_max_microamp[FLED2],
				  (u32)RT5033_LED_FLASH_BRIGHTNESS_MAX_2CH);
	else
		fl_set->max = min(led_cfg->flash_max_microamp[fled_id],
				  (u32)RT5033_LED_FLASH_BRIGHTNESS_MAX_1CH);
	fl_set->step = RT5033_LED_FLASH_BRIGHTNESS_STEP;
	fl_set->val = fl_set->max;
}

static void rt5033_led_init_fled_cdev(struct rt5033_sub_led *sub_led,
				      struct rt5033_led_config_data *led_cfg)
{
	struct led_classdev_flash *fled_cdev;
	struct led_classdev *led_cdev;
	enum rt5033_fled fled_id = sub_led->fled_id;

	/* Initialize LED Flash class device */
	fled_cdev = &sub_led->fled_cdev;
	fled_cdev->ops = &flash_ops;
	led_cdev = &fled_cdev->led_cdev;

	led_cdev->name = led_cfg->label[fled_id];

	led_cdev->brightness_set_blocking = rt5033_led_brightness_set;
	led_cdev->max_brightness = rt5033_torch_brightness_to_reg(
					led_cfg->torch_max_microamp[fled_id]);
	led_cdev->flags |= LED_DEV_CAP_FLASH;

	rt5033_init_flash_properties(sub_led, led_cfg);

	sub_led->flash_timeout = fled_cdev->timeout.val;
	sub_led->flash_brightness = fled_cdev->brightness.val;
}

static int rt5033_led_parse_dt(struct rt5033_led *led, struct device *dev,
			       struct rt5033_led_config_data *cfg,
			       struct device_node **sub_nodes)
{
	struct device_node *np = dev->of_node;
	struct device_node *child_node;
	struct rt5033_sub_led *sub_leds = led->sub_leds;
	struct property *prop;
	u32 led_sources[2];
	enum rt5033_fled fled_id;
	int i, ret;

	for_each_available_child_of_node(np, child_node) {
		prop = of_find_property(child_node, "led-sources", NULL);
		if (prop) {
			const __be32 *srcs = NULL;

			for (i = 0; i < ARRAY_SIZE(led_sources); ++i) {
				srcs = of_prop_next_u32(prop, srcs,
							&led_sources[i]);
				if (!srcs)
					break;
			}
		} else {
			dev_err(dev, "led-sources DT property missing\n");
			ret = -EINVAL;
			goto err_parse_dt;
		}

		if (i == 2) {
			fled_id = FLED1;
			led->fled_mask = FLED1_IOUT | FLED2_IOUT;
		} else if (led_sources[0] == FLED1) {
			fled_id = FLED1;
			led->fled_mask |= FLED1_IOUT;
		} else if (led_sources[0] == FLED2) {
			fled_id = FLED2;
			led->fled_mask |= FLED2_IOUT;
		} else {
			dev_err(dev, "Wrong led-sources DT property value.\n");
			ret = -EINVAL;
			goto err_parse_dt;
		}

		if (sub_nodes[fled_id]) {
			dev_err(dev,
				"Conflicting \"led-sources\" DT properties\n");
			ret = -EINVAL;
			goto err_parse_dt;
		}

		sub_nodes[fled_id] = child_node;
		sub_leds[fled_id].fled_id = fled_id;

		cfg->label[fled_id] =
			of_get_property(child_node, "label", NULL) ? :
					child_node->name;

		ret = of_property_read_u32(child_node, "led-max-microamp",
					   &cfg->torch_max_microamp[fled_id]);
		if (ret < 0) {
			dev_err(dev, "failed to parse led-max-microamp\n");
			goto err_parse_dt;
		}

		ret = of_property_read_u32(child_node, "flash-max-microamp",
					   &cfg->flash_max_microamp[fled_id]);
		if (ret < 0) {
			dev_err(dev, "failed to parse flash-max-microamp\n");
			goto err_parse_dt;
		}

		ret = of_property_read_u32(child_node, "flash-max-timeout-us",
					   &cfg->flash_max_timeout[fled_id]);
		if (ret < 0) {
			dev_err(dev, "failed to parse flash-max-timeout-us\n");
			goto err_parse_dt;
		}

		if (++cfg->num_leds == 2 ||
		    (rt5033_fled_used(led, FLED1) &&
		     rt5033_fled_used(led, FLED2))) {
			of_node_put(child_node);
			break;
		}
	}

	if (cfg->num_leds == 0) {
		dev_err(dev, "No DT child node found for connected LED(s).\n");
		return -EINVAL;
	}

	return 0;

err_parse_dt:
	of_node_put(child_node);
	return ret;
}

static int rt5033_led_probe(struct platform_device *pdev)
{
	struct rt5033_dev *rt5033 = dev_get_drvdata(pdev->dev.parent);
	struct rt5033_led *led;
	struct rt5033_sub_led *sub_leds;
	struct device_node *sub_nodes[2] = {};
	struct rt5033_led_config_data led_cfg = {};
	int init_fled_cdev[2], i, ret;

	led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	platform_set_drvdata(pdev, led);
	led->dev = &pdev->dev;
	led->regmap = rt5033->regmap;
	sub_leds = led->sub_leds;

	ret = rt5033_led_parse_dt(led, &pdev->dev, &led_cfg, sub_nodes);
	if (ret < 0)
		return ret;

	if (led_cfg.num_leds == 1 && rt5033_fled_used(led, FLED1) &&
	    rt5033_fled_used(led, FLED2))
		led->iout_joint = true;

	mutex_init(&led->lock);

	init_fled_cdev[FLED1] =
			led->iout_joint || rt5033_fled_used(led, FLED1);
	init_fled_cdev[FLED2] =
			!led->iout_joint && rt5033_fled_used(led, FLED2);

	for (i = FLED1; i <= FLED2; ++i) {
		if (!init_fled_cdev[i])
			continue;

		rt5033_led_init_fled_cdev(&sub_leds[i], &led_cfg);
		ret = led_classdev_flash_register(led->dev,
						  &sub_leds[i].fled_cdev);
		if (ret < 0) {
			if (i == FLED2)
				goto err_register_led2;
			else
				goto err_register_led1;
		}
	}

	led->current_iout = 0;
	ret = regmap_update_bits(led->regmap, RT5033_REG_FLED_FUNCTION1,
				 RT5033_FLED_FUNC1_MASK, RT5033_FLED_RESET);
	if (ret < 0)
		dev_dbg(led->dev, "Failed to reset flash led (%d)\n", ret);

	return 0;

err_register_led2:
	/* It is possible than only FLED2 was to be registered */
	if (!init_fled_cdev[FLED1])
		goto err_register_led1;
	led_classdev_flash_unregister(&sub_leds[FLED1].fled_cdev);
err_register_led1:
	mutex_destroy(&led->lock);

	return ret;
}

static int rt5033_led_remove(struct platform_device *pdev)
{
	struct rt5033_led *led = platform_get_drvdata(pdev);
	struct rt5033_sub_led *sub_leds = led->sub_leds;

	if (led->iout_joint || rt5033_fled_used(led, FLED1))
		led_classdev_flash_unregister(&sub_leds[FLED1].fled_cdev);

	if (!led->iout_joint && rt5033_fled_used(led, FLED2))
		led_classdev_flash_unregister(&sub_leds[FLED2].fled_cdev);

	mutex_destroy(&led->lock);

	return 0;
}

static const struct of_device_id rt5033_led_match[] = {
	{ .compatible = "richtek,rt5033-led", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rt5033_led_match);

static struct platform_driver rt5033_led_driver = {
	.driver = {
		.name = "rt5033-led",
		.of_match_table = rt5033_led_match,
	},
	.probe		= rt5033_led_probe,
	.remove		= rt5033_led_remove,
};
module_platform_driver(rt5033_led_driver);

MODULE_AUTHOR("Ingi Kim <ingi2.kim@samsung.com>");
MODULE_DESCRIPTION("Richtek RT5033 LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:rt5033-led");
