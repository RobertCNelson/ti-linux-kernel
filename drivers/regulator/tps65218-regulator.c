/*
 * tps65218-regulator.c
 *
 * Regulator driver for TPS65218 PMIC
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether expressed or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/mfd/tps65218.h>

static unsigned int tps65218_ramp_delay = 4000;

#define TPS65218_REGULATOR(_name, _id, _ops, _n, _vr, _vm, _er, _em, _t) \
	{						\
		.name		= _name,		\
		.id		= _id,			\
		.ops		= &_ops,		\
		.n_voltages	= _n,			\
		.type		= REGULATOR_VOLTAGE,	\
		.owner		= THIS_MODULE,		\
		.vsel_reg	= _vr,			\
		.vsel_mask	= _vm,			\
		.enable_reg	= _er,	\
		.enable_mask	= _em,			\
		.volt_table	= _t,			\
	}						\

#define TPS65218_INFO(_id, _nm, _min, _max, _f1, _f2)	\
	{						\
		.id		= _id,			\
		.name		= _nm,			\
		.min_uV		= _min,			\
		.max_uV		= _max,			\
		.vsel_to_uv	= _f1,			\
		.uv_to_vsel	= _f2,			\
	}

static int tps65218_ldo1_dcdc3_vsel_to_uv(unsigned int vsel)
{
	int uV = 0;

	if (vsel <= 26)
		uV = vsel * 25000 + 900000;
	else
		uV = (vsel - 26) * 50000 + 1550000;

	return uV;
}

static int tps65218_ldo1_dcdc3_uv_to_vsel(int uV, unsigned int *vsel)
{
	if (uV <= 1550000)
		*vsel = DIV_ROUND_UP(uV - 900000, 25000);
	else
		*vsel = 26 + DIV_ROUND_UP(uV - 1550000, 50000);

	return 0;
}

static int tps65218_dcdc1_2_vsel_to_uv(unsigned int vsel)
{
	int uV = 0;

	if (vsel <= 50)
		uV = vsel * 10000 + 850000;
	else
		uV = (vsel - 50) * 25000 + 1350000;

	return uV;
}

static int tps65218_dcdc1_2_uv_to_vsel(int uV, unsigned int *vsel)
{
	if (uV <= 1350000)
		*vsel = DIV_ROUND_UP(uV - 850000, 10000);
	else
		*vsel = 50 + DIV_ROUND_UP(uV - 1350000, 25000);

	return 0;
}

static int tps65218_dcd4_vsel_to_uv(unsigned int vsel)
{
	int uV = 0;

	if (vsel <= 15)
		uV = vsel * 25000 + 1175000;
	else
		uV = (vsel - 15) * 50000 + 1550000;

	return uV;
}

static int tps65218_dcdc4_uv_to_vsel(int uV, unsigned int *vsel)
{
	if (uV <= 1550000)
		*vsel = DIV_ROUND_UP(uV - 1175000, 25000);
	else
		*vsel = 15 + DIV_ROUND_UP(uV - 1550000, 50000);

	return 0;
}

static struct tps_info tps65218_pmic_regs[] = {
	TPS65218_INFO(0, "DCDC1", 850000, 1675000, tps65218_dcdc1_2_vsel_to_uv,
		      tps65218_dcdc1_2_uv_to_vsel),
	TPS65218_INFO(1, "DCDC2", 850000, 1675000, tps65218_dcdc1_2_vsel_to_uv,
		      tps65218_dcdc1_2_uv_to_vsel),
	TPS65218_INFO(2, "DCDC3", 900000, 3400000,
		      tps65218_ldo1_dcdc3_vsel_to_uv,
		      tps65218_ldo1_dcdc3_uv_to_vsel),
	TPS65218_INFO(3, "DCDC4", 1175000, 3400000, tps65218_dcd4_vsel_to_uv,
		      tps65218_dcdc4_uv_to_vsel),
	TPS65218_INFO(4, "DCDC5", 1000000, 1000000, NULL, NULL),
	TPS65218_INFO(5, "DCDC6", 1800000, 1800000, NULL, NULL),
	TPS65218_INFO(6, "LDO1", 900000, 3400000,
		      tps65218_ldo1_dcdc3_vsel_to_uv,
		      tps65218_ldo1_dcdc3_uv_to_vsel),
};

#define TPS65218_OF_MATCH(comp, label) \
	{ \
		.compatible = comp, \
		.data = &label, \
	}

static const struct of_device_id tps65218_of_match[] = {
	TPS65218_OF_MATCH("ti,tps65218-dcdc1", tps65218_pmic_regs[0]),
	TPS65218_OF_MATCH("ti,tps65218-dcdc2", tps65218_pmic_regs[1]),
	TPS65218_OF_MATCH("ti,tps65218-dcdc3", tps65218_pmic_regs[2]),
	TPS65218_OF_MATCH("ti,tps65218-dcdc4", tps65218_pmic_regs[3]),
	TPS65218_OF_MATCH("ti,tps65218-dcdc5", tps65218_pmic_regs[4]),
	TPS65218_OF_MATCH("ti,tps65218-dcdc6", tps65218_pmic_regs[5]),
	TPS65218_OF_MATCH("ti,tps65218-ldo1", tps65218_pmic_regs[6]),
};
MODULE_DEVICE_TABLE(of, tps65218_of_match);

static int tps65218_pmic_set_voltage_sel(struct regulator_dev *dev,
					 unsigned selector)
{
	int ret;
	struct tps65218 *tps = rdev_get_drvdata(dev);
	unsigned int rid = rdev_get_id(dev);

	/* Set the voltage based on vsel value and write protect level is 2 */
	ret = tps65218_set_bits(tps, dev->desc->vsel_reg, dev->desc->vsel_mask,
				selector, TPS65218_PROTECT_L1);

	/* Set GO bit for DCDC1/2 to initiate voltage transistion */
	switch (rid) {
	case TPS65218_DCDC_1:
	case TPS65218_DCDC_2:
		ret = tps65218_set_bits(tps, TPS65218_REG_CONTRL_SLEW_RATE,
					TPS65218_SLEW_RATE_GO,
					TPS65218_SLEW_RATE_GO,
					TPS65218_PROTECT_L1);
		break;
	}

	return ret;
}

static int tps65218_pmic_map_voltage(struct regulator_dev *dev,
				     int min_uV, int max_uV)
{
	struct tps65218 *tps = rdev_get_drvdata(dev);
	unsigned int sel, rid = rdev_get_id(dev);
	int ret;

	if (rid < TPS65218_DCDC_1 || rid > TPS65218_LDO_1)
		return -EINVAL;

	if (min_uV < tps->info[rid]->min_uV)
		min_uV = tps->info[rid]->min_uV;

	if (max_uV < tps->info[rid]->min_uV || min_uV > tps->info[rid]->max_uV)
		return -EINVAL;

	ret = tps->info[rid]->uv_to_vsel(min_uV, &sel);
	if (ret)
		return ret;

	return sel;
}

static int tps65218_pmic_list_voltage(struct regulator_dev *dev,
					unsigned selector)
{
	struct tps65218 *tps = rdev_get_drvdata(dev);
	unsigned int rid = rdev_get_id(dev);

	if (rid < TPS65218_DCDC_1 || rid > TPS65218_LDO_1)
		return -EINVAL;

	if (selector >= dev->desc->n_voltages)
		return -EINVAL;

	return tps->info[rid]->vsel_to_uv(selector);
}

static int tps65218_pmic_enable(struct regulator_dev *dev)
{
	struct tps65218 *tps = rdev_get_drvdata(dev);
	unsigned int rid = rdev_get_id(dev);

	if (rid < TPS65218_DCDC_1 || rid > TPS65218_LDO_1)
		return -EINVAL;

	/* Enable the regulator and password protection is level 1 */
	return tps65218_set_bits(tps, dev->desc->enable_reg,
				 dev->desc->enable_mask, dev->desc->enable_mask,
				 TPS65218_PROTECT_L1);
}

static int tps65218_pmic_disable(struct regulator_dev *dev)
{
	struct tps65218 *tps = rdev_get_drvdata(dev);
	unsigned int rid = rdev_get_id(dev);

	if (rid < TPS65218_DCDC_1 || rid > TPS65218_LDO_1)
		return -EINVAL;

	/* Disable the regulator and password protection is level 1 */
	return tps65218_clear_bits(tps, dev->desc->enable_reg,
				   dev->desc->enable_mask, TPS65218_PROTECT_L1);
}

static int tps65218_set_voltage_time_sel(struct regulator_dev *rdev,
	unsigned int old_selector, unsigned int new_selector)
{
	int old_uv, new_uv;

	old_uv = tps65218_pmic_list_voltage(rdev, old_selector);
	if (old_uv < 0)
		return old_uv;

	new_uv = tps65218_pmic_list_voltage(rdev, new_selector);
	if (new_uv < 0)
		return new_uv;

	return DIV_ROUND_UP(abs(old_uv - new_uv), tps65218_ramp_delay);
}

/* Operations permitted on DCDC1, DCDC2 */
static struct regulator_ops tps65218_dcdc12_ops = {
	.is_enabled		= regulator_is_enabled_regmap,
	.enable			= tps65218_pmic_enable,
	.disable		= tps65218_pmic_disable,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= tps65218_pmic_set_voltage_sel,
	.list_voltage		= tps65218_pmic_list_voltage,
	.map_voltage		= tps65218_pmic_map_voltage,
	.set_voltage_time_sel	= tps65218_set_voltage_time_sel,
};

/* Operations permitted on DCDC3, DCDC4 and LDO1 */
static struct regulator_ops tps65218_ldo1_dcdc34_ops = {
	.is_enabled		= regulator_is_enabled_regmap,
	.enable			= tps65218_pmic_enable,
	.disable		= tps65218_pmic_disable,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= tps65218_pmic_set_voltage_sel,
	.list_voltage		= tps65218_pmic_list_voltage,
	.map_voltage		= tps65218_pmic_map_voltage,
};

/* Operations permitted on DCDC5, DCDC6 */
static struct regulator_ops tps65218_dcdc56_pmic_ops = {
	.is_enabled		= regulator_is_enabled_regmap,
	.enable			= tps65218_pmic_enable,
	.disable		= tps65218_pmic_disable,
};

static const struct regulator_desc regulators[] = {
	TPS65218_REGULATOR("DCDC1", TPS65218_DCDC_1, tps65218_dcdc12_ops, 64,
			   TPS65218_REG_CONTROL_DCDC1,
			   TPS65218_CONTROL_DCDC1_MASK,
			   TPS65218_REG_ENABLE1, TPS65218_ENABLE1_DC1_EN, NULL),
	TPS65218_REGULATOR("DCDC2", TPS65218_DCDC_2, tps65218_dcdc12_ops, 64,
			   TPS65218_REG_CONTROL_DCDC2,
			   TPS65218_CONTROL_DCDC2_MASK,
			   TPS65218_REG_ENABLE1, TPS65218_ENABLE1_DC2_EN, NULL),
	TPS65218_REGULATOR("DCDC3", TPS65218_DCDC_3, tps65218_ldo1_dcdc34_ops,
			   64, TPS65218_REG_CONTROL_DCDC3,
			   TPS65218_CONTROL_DCDC3_MASK, TPS65218_REG_ENABLE1,
			   TPS65218_ENABLE1_DC3_EN, NULL),
	TPS65218_REGULATOR("DCDC4", TPS65218_DCDC_4, tps65218_ldo1_dcdc34_ops,
			   53, TPS65218_REG_CONTROL_DCDC4,
			   TPS65218_CONTROL_DCDC4_MASK,
			   TPS65218_REG_ENABLE1, TPS65218_ENABLE1_DC4_EN, NULL),
	TPS65218_REGULATOR("DCDC5", TPS65218_DCDC_5, tps65218_dcdc56_pmic_ops,
			   1, -1, -1, TPS65218_REG_ENABLE1,
			   TPS65218_ENABLE1_DC5_EN, NULL),
	TPS65218_REGULATOR("DCDC6", TPS65218_DCDC_6, tps65218_dcdc56_pmic_ops,
			   1, -1, -1, TPS65218_REG_ENABLE1,
			   TPS65218_ENABLE1_DC6_EN, NULL),
	TPS65218_REGULATOR("LDO1", TPS65218_LDO_1, tps65218_ldo1_dcdc34_ops, 64,
			   TPS65218_REG_CONTROL_LDO1,
			   TPS65218_CONTROL_LDO1_MASK, TPS65218_REG_ENABLE2,
			   TPS65218_ENABLE2_LDO1_EN, NULL),
};

static int tps65218_regulator_probe(struct platform_device *pdev)
{
	struct tps65218 *tps = dev_get_drvdata(pdev->dev.parent);
	struct regulator_init_data *init_data;
	const struct tps_info	*template;
	struct regulator_dev *rdev;
	const struct of_device_id	*match;
	struct regulator_config config = { };
	int id;

	match = of_match_device(tps65218_of_match, &pdev->dev);
	if (match) {
		template = match->data;
		id = template->id;
		init_data = of_get_regulator_init_data(&pdev->dev,
						      pdev->dev.of_node);
	} else {
		return -ENODEV;
	}

	platform_set_drvdata(pdev, tps);

	tps->info[id] = &tps65218_pmic_regs[id];
	config.dev = &pdev->dev;
	config.init_data = init_data;
	config.driver_data = tps;
	config.regmap = tps->regmap;
	config.of_node = pdev->dev.of_node;

	rdev = regulator_register(&regulators[id], &config);
	if (IS_ERR(rdev)) {
		dev_err(tps->dev, "failed to register %s regulator\n",
			pdev->name);
		return PTR_ERR(rdev);
	}

	/* Save regulator */
	tps->rdev[id] = rdev;

	return 0;
}

static int tps65218_regulator_remove(struct platform_device *pdev)
{
	struct tps65218 *tps = platform_get_drvdata(pdev);
	const struct of_device_id	*match;
	const struct tps_info		*template;

	match = of_match_device(tps65218_of_match, &pdev->dev);
	template = match->data;
	regulator_unregister(tps->rdev[template->id]);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver tps65218_regulator_driver = {
	.driver = {
		.name = "tps65218-pmic",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(tps65218_of_match),
	},
	.probe = tps65218_regulator_probe,
	.remove = tps65218_regulator_remove,
};

module_platform_driver(tps65218_regulator_driver);

MODULE_AUTHOR("J Keerthy <j-keerthy@ti.com>");
MODULE_DESCRIPTION("TPS65218 voltage regulator driver");
MODULE_ALIAS("platform:tps65218-pmic");
MODULE_LICENSE("GPL v2");
