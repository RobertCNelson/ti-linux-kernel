// SPDX-License-Identifier: GPL-2.0
/*
 * MMIO register bit-field controlled multiplexer driver
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com
 *
 * Based on drivers/mux/mmio.c by Philipp Zabel <kernel@pengutronix.de>
 * Modified to support 3-field format: reg-offset, mask & value
 *
 * Author: Rahul Sharma <r-sharma3@ti.com>
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define MUX_ENABLE_INTR BIT(16)

struct mux_ti_k3_event {
	struct regmap *regmap;
	u32 reg;
	u32 mask;
	u32 value;
};

struct mux_ti_k3_event_chip {
	struct mux_chip *mux_chip;
	struct mux_ti_k3_event *fields;
	int num_fields;
	u32 *saved_states;
};

static int mux_ti_k3_event_suspend(struct device *dev)
{
	struct mux_ti_k3_event_chip *chip = dev_get_drvdata(dev);
	int i, ret;

	if (!chip->saved_states) {
		chip->saved_states = devm_kcalloc(dev, chip->num_fields,
						  sizeof(u32), GFP_KERNEL);
		if (!chip->saved_states)
			return -ENOMEM;
	}

	for (i = 0; i < chip->num_fields; i++) {
		struct mux_ti_k3_event *field = &chip->fields[i];

		ret = regmap_read(field->regmap, field->reg,
				  &chip->saved_states[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int mux_ti_k3_event_resume(struct device *dev)
{
	struct mux_ti_k3_event_chip *chip = dev_get_drvdata(dev);
	int i, ret;

	if (!chip->saved_states)
		return 0;

	for (i = 0; i < chip->num_fields; i++) {
		struct mux_ti_k3_event *field = &chip->fields[i];

		ret = regmap_write(field->regmap, field->reg,
				   chip->saved_states[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mux_ti_k3_event_pm_ops,
				mux_ti_k3_event_suspend,
				mux_ti_k3_event_resume);

/*
 * State behavior:
 * - state 0: Clears the mask bits in the target register (inactive state)
 * - state 1: Sets both the value bits and enable bit (bit 16) in the register
 */
static int mux_ti_k3_event_set(struct mux_control *mux, int state)
{
	struct mux_ti_k3_event *fields = mux_chip_priv(mux->chip);
	struct mux_ti_k3_event *field = &fields[mux_control_get_index(mux)];

	if (!state)
		return regmap_update_bits(field->regmap, field->reg, field->mask, 0);

	return regmap_update_bits(field->regmap, field->reg, field->mask | MUX_ENABLE_INTR,
		field->value | MUX_ENABLE_INTR);
}

static const struct mux_control_ops mux_ti_k3_event_ops = {
	.set = mux_ti_k3_event_set,
};

static const struct regmap_config mux_ti_k3_event_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int mux_ti_k3_event_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct mux_ti_k3_event_chip *chip;
	struct mux_ti_k3_event *fields;
	struct mux_chip *mux_chip;
	struct regmap *regmap;
	void __iomem *base;
	int num_fields;
	int ret;
	int i;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		return dev_err_probe(dev, -ENODEV,
				     "failed to get base address\n");
	} else {
		regmap = devm_regmap_init_mmio(dev, base, &mux_ti_k3_event_regmap_cfg);
	}
	if (IS_ERR(regmap)) {
		iounmap(base);
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get regmap\n");
	}

	ret = of_property_count_u32_elems(np, "mux-reg-mask-val");
	if (!ret || ret % 3) {
		ret = -EINVAL;
		dev_err(dev, "mux-reg-mask-val property missing or invalid: %d\n",
			ret);
		return ret;
	}

	num_fields = ret / 3;
	mux_chip = devm_mux_chip_alloc(dev, num_fields, num_fields *
				       sizeof(*fields));
	if (IS_ERR(mux_chip))
		return PTR_ERR(mux_chip);

	fields = mux_chip_priv(mux_chip);
	chip->mux_chip = mux_chip;
	chip->fields = fields;
	chip->num_fields = num_fields;

	platform_set_drvdata(pdev, chip);

	for (i = 0; i < num_fields; i++) {
		struct mux_control *mux = &mux_chip->mux[i];
		s32 idle_state = MUX_IDLE_AS_IS;
		u32 reg, mask, value;

		ret = of_property_read_u32_index(np, "mux-reg-mask-val",
						 3 * i, &reg);
		if (!ret)
			ret = of_property_read_u32_index(np, "mux-reg-mask-val",
							 3 * i + 1, &mask);
		if (!ret)
			ret = of_property_read_u32_index(np, "mux-reg-mask-val",
							 3 * i + 2, &value);
		if (ret < 0) {
			dev_err(dev, "field %d: failed to read mux-reg-mask-val property: %d\n",
				i, ret);
			return ret;
		}

		/* Validate that value bits are within mask */
		if (value & ~mask) {
			dev_err(dev, "field %d: value 0x%x has bits outside mask 0x%x\n",
				i, value, mask);
			return -EINVAL;
		}

		fields[i].regmap = regmap;
		fields[i].reg = reg;
		fields[i].mask = mask;
		fields[i].value = value;

		/* This driver supports binary mux (2 states: 0 and active) */
		mux->states = 2;

		of_property_read_u32_index(np, "idle-states", i,
					   (u32 *)&idle_state);
		if (idle_state != MUX_IDLE_AS_IS) {
			if (idle_state < 0 || idle_state >= mux->states) {
				dev_err(dev, "field: %d: out of range idle state %d\n",
					i, idle_state);
				return -EINVAL;
			}

			mux->idle_state = idle_state;
		}
	}

	mux_chip->ops = &mux_ti_k3_event_ops;

	return devm_mux_chip_register(dev, mux_chip);
}

static const struct of_device_id mux_ti_k3_event_dt_ids[] = {
	{ .compatible = "ti,am62l-event-mux-router", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mux_ti_k3_event_dt_ids);

static struct platform_driver mux_ti_k3_event_driver = {
	.driver = {
		.name = "ti-k3-event-mux",
		.of_match_table	= mux_ti_k3_event_dt_ids,
		.pm = &mux_ti_k3_event_pm_ops,
	},
	.probe = mux_ti_k3_event_probe,
};
module_platform_driver(mux_ti_k3_event_driver);

MODULE_DESCRIPTION("TI K3 Bit-field Controlled Event Multiplexer driver");
MODULE_AUTHOR("Rahul Sharma <r-sharma3@ti.com>");
MODULE_LICENSE("GPL");
