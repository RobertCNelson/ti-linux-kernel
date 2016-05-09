/*
 * Arizona clock control
 *
 * Copyright 2016 Cirrus Logic, Inc.
 *
 * Author: Charles Keepax <ckeepax@opensource.wolfsonmicro.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <linux/mfd/arizona/core.h>
#include <linux/mfd/arizona/pdata.h>
#include <linux/mfd/arizona/registers.h>

#define CLK32K_RATE 32768

struct arizona_clk {
	struct arizona *arizona;

	struct clk_hw clk32k_hw;
	struct clk *clk32k;
};

static inline struct arizona_clk *clk32k_to_arizona_clk(struct clk_hw *hw)
{
	return container_of(hw, struct arizona_clk, clk32k_hw);
}

static int arizona_32k_enable(struct clk_hw *hw)
{
	struct arizona_clk *clkdata = clk32k_to_arizona_clk(hw);
	struct arizona *arizona = clkdata->arizona;
	int ret;

	switch (arizona->pdata.clk32k_src) {
	case ARIZONA_32KZ_MCLK1:
		ret = pm_runtime_get_sync(arizona->dev);
		if (ret)
			goto out;
		break;
	}

	ret = regmap_update_bits_async(arizona->regmap, ARIZONA_CLOCK_32K_1,
				       ARIZONA_CLK_32K_ENA,
				       ARIZONA_CLK_32K_ENA);

out:
	return ret;
}

static void arizona_32k_disable(struct clk_hw *hw)
{
	struct arizona_clk *clkdata = clk32k_to_arizona_clk(hw);
	struct arizona *arizona = clkdata->arizona;

	regmap_update_bits_async(arizona->regmap, ARIZONA_CLOCK_32K_1,
				 ARIZONA_CLK_32K_ENA, 0);

	switch (arizona->pdata.clk32k_src) {
	case ARIZONA_32KZ_MCLK1:
		pm_runtime_put_sync(arizona->dev);
		break;
	}
}

static const struct clk_ops arizona_32k_ops = {
	.prepare = arizona_32k_enable,
	.unprepare = arizona_32k_disable,
};

static int arizona_clk_of_get_pdata(struct arizona *arizona)
{
	const char * const pins[] = { "mclk1", "mclk2" };
	struct clk *mclk;
	int i;

	if (!of_property_read_bool(arizona->dev->of_node, "clocks"))
		return 0;

	for (i = 0; i < ARRAY_SIZE(pins); ++i) {
		mclk = of_clk_get_by_name(arizona->dev->of_node, pins[i]);
		if (IS_ERR(mclk))
			return PTR_ERR(mclk);

		if (clk_get_rate(mclk) == CLK32K_RATE) {
			arizona->pdata.clk32k_src = ARIZONA_32KZ_MCLK1 + i;
			arizona->pdata.clk32k_parent = __clk_get_name(mclk);
		}

		clk_put(mclk);
	}

	return 0;
}

static int arizona_clk_probe(struct platform_device *pdev)
{
	struct arizona *arizona = dev_get_drvdata(pdev->dev.parent);
	struct arizona_clk *clkdata;
	int ret;

	struct clk_init_data clk32k_init = {
		.name = "arizona-32k",
		.ops = &arizona_32k_ops,
	};

	if (IS_ENABLED(CONFIG_OF) && !dev_get_platdata(arizona->dev)) {
		ret = arizona_clk_of_get_pdata(arizona);
		if (ret) {
			dev_err(arizona->dev, "Failed parsing clock DT: %d\n",
				ret);
			return ret;
		}
	}

	clkdata = devm_kzalloc(&pdev->dev, sizeof(*clkdata), GFP_KERNEL);
	if (!clkdata)
		return -ENOMEM;

	clkdata->arizona = arizona;

	switch (arizona->pdata.clk32k_src) {
	case 0:
		arizona->pdata.clk32k_src = ARIZONA_32KZ_MCLK2;
		/* Fall through */
	case ARIZONA_32KZ_MCLK1:
	case ARIZONA_32KZ_MCLK2:
	case ARIZONA_32KZ_NONE:
		regmap_update_bits(arizona->regmap, ARIZONA_CLOCK_32K_1,
				   ARIZONA_CLK_32K_SRC_MASK,
				   arizona->pdata.clk32k_src - 1);
		break;
	default:
		dev_err(arizona->dev, "Invalid 32kHz clock source: %d\n",
			arizona->pdata.clk32k_src);
		return -EINVAL;
	}

	if (arizona->pdata.clk32k_parent) {
		clk32k_init.num_parents = 1;
		clk32k_init.parent_names = &arizona->pdata.clk32k_parent;
	}

	clkdata->clk32k_hw.init = &clk32k_init;
	clkdata->clk32k = devm_clk_register(&pdev->dev, &clkdata->clk32k_hw);
	if (IS_ERR(clkdata->clk32k)) {
		ret = PTR_ERR(clkdata->clk32k);
		dev_err(arizona->dev, "Failed to register 32k clock: %d\n",
			ret);
		return ret;
	}

	ret = clk_register_clkdev(clkdata->clk32k, "arizona-32k",
				  dev_name(arizona->dev));
	if (ret) {
		dev_err(arizona->dev, "Failed to register 32k clock dev: %d\n",
			ret);
		return ret;
	}

	platform_set_drvdata(pdev, clkdata);

	return 0;
}

static struct platform_driver arizona_clk_driver = {
	.probe = arizona_clk_probe,
	.driver		= {
		.name	= "arizona-clk",
	},
};

module_platform_driver(arizona_clk_driver);

/* Module information */
MODULE_AUTHOR("Charles Keepax <ckeepax@opensource.wolfsonmicro.com>");
MODULE_DESCRIPTION("Clock driver for Arizona devices");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:arizona-clk");
