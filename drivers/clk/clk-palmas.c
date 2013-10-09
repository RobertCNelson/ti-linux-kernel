/*
 * Clock driver for Palmas device.
 *
 * Copyright (c) 2013, NVIDIA Corporation.
 *
 * Author: Laxman Dewangan <ldewangan@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/mfd/palmas.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define PALMAS_CLOCK_MAX			2

#define PALMAS_CLOCK_DT_EXT_CONTROL_ENABLE1	1
#define PALMAS_CLOCK_DT_EXT_CONTROL_ENABLE2	2
#define PALMAS_CLOCK_DT_EXT_CONTROL_NSLEEP	3

struct palmas_clks;

struct palmas_clk32k_desc {
	const char *clk_name;
	unsigned int control_reg;
	unsigned int enable_mask;
	unsigned int sleep_mask;
	unsigned int sleep_reqstr_id;
};

struct palmas_clock_info {
	struct clk *clk;
	struct clk_hw hw;
	struct palmas_clk32k_desc *clk_desc;
	struct palmas_clks *palmas_clk;
	int ext_control_pin;
};

struct palmas_clks {
	struct device *dev;
	struct palmas *palmas;
	struct clk_onecell_data clk_data;
	struct palmas_clock_info clk_info[PALMAS_CLOCK_MAX];
};

static struct palmas_clk32k_desc palmas_clk32k_descs[] = {
	{
		.clk_name = "clk32k_kg",
		.control_reg = PALMAS_CLK32KG_CTRL,
		.enable_mask = PALMAS_CLK32KG_CTRL_MODE_ACTIVE,
		.sleep_mask = PALMAS_CLK32KG_CTRL_MODE_SLEEP,
		.sleep_reqstr_id = PALMAS_EXTERNAL_REQSTR_ID_CLK32KG,
	}, {
		.clk_name = "clk32k_kg_audio",
		.control_reg = PALMAS_CLK32KGAUDIO_CTRL,
		.enable_mask = PALMAS_CLK32KG_CTRL_MODE_ACTIVE,
		.sleep_mask = PALMAS_CLK32KG_CTRL_MODE_SLEEP,
		.sleep_reqstr_id = PALMAS_EXTERNAL_REQSTR_ID_CLK32KGAUDIO,
	},
};

static inline struct palmas_clock_info *to_palmas_clks_info(struct clk_hw *hw)
{
	return container_of(hw, struct palmas_clock_info, hw);
}

static unsigned long palmas_clks_recalc_rate(struct clk_hw *hw,
	unsigned long parent_rate)
{
	return 32768;
}

static int palmas_clks_prepare(struct clk_hw *hw)
{
	struct palmas_clock_info *cinfo = to_palmas_clks_info(hw);
	struct palmas_clks *palmas_clks = cinfo->palmas_clk;
	int ret;

	ret = palmas_update_bits(palmas_clks->palmas, PALMAS_RESOURCE_BASE,
			cinfo->clk_desc->control_reg,
			cinfo->clk_desc->enable_mask,
			cinfo->clk_desc->enable_mask);
	if (ret < 0)
		dev_err(palmas_clks->dev, "Reg 0x%02x update failed, %d\n",
			cinfo->clk_desc->control_reg, ret);

	return ret;
}

static void palmas_clks_unprepare(struct clk_hw *hw)
{
	struct palmas_clock_info *cinfo = to_palmas_clks_info(hw);
	struct palmas_clks *palmas_clks = cinfo->palmas_clk;
	int ret;

	/*
	 * Clock can be disabled through external pin if it is externally
	 * controlled.
	 */
	if (cinfo->ext_control_pin)
		return;

	ret = palmas_update_bits(palmas_clks->palmas, PALMAS_RESOURCE_BASE,
			cinfo->clk_desc->control_reg,
			cinfo->clk_desc->enable_mask, 0);
	if (ret < 0)
		dev_err(palmas_clks->dev, "Reg 0x%02x update failed, %d\n",
			cinfo->clk_desc->control_reg, ret);

}

static int palmas_clks_is_prepared(struct clk_hw *hw)
{
	struct palmas_clock_info *cinfo = to_palmas_clks_info(hw);
	struct palmas_clks *palmas_clks = cinfo->palmas_clk;
	int ret;
	u32 val;

	if (cinfo->ext_control_pin)
		return 1;

	ret = palmas_read(palmas_clks->palmas, PALMAS_RESOURCE_BASE,
			cinfo->clk_desc->control_reg, &val);
	if (ret < 0) {
		dev_err(palmas_clks->dev, "Reg 0x%02x read failed, %d\n",
				cinfo->clk_desc->control_reg, ret);
		return ret;
	}
	return !!(val & cinfo->clk_desc->enable_mask);
}

static struct clk_ops palmas_clks_ops = {
	.prepare	= palmas_clks_prepare,
	.unprepare	= palmas_clks_unprepare,
	.is_prepared	= palmas_clks_is_prepared,
	.recalc_rate	= palmas_clks_recalc_rate,
};

static struct clk_init_data palmas_clks_hw_init[PALMAS_CLOCK_MAX] = {
	{
		.name = "clk32k_kg",
		.ops = &palmas_clks_ops,
		.flags = CLK_IS_ROOT | CLK_IGNORE_UNUSED,
	}, {
		.name = "clk32k_kg_audio",
		.ops = &palmas_clks_ops,
		.flags = CLK_IS_ROOT | CLK_IGNORE_UNUSED,
	},
};

static int palmas_clks_get_clk_data(struct platform_device *pdev,
	struct palmas_clks *palmas_clks)
{
	struct device_node *node = pdev->dev.of_node;
	struct device_node *child;
	struct palmas_clock_info *cinfo;
	unsigned int prop;
	int ret;
	int i;

	for (i = 0; i < PALMAS_CLOCK_MAX; ++i) {
		child = of_get_child_by_name(node,
				palmas_clk32k_descs[i].clk_name);
		if (!child)
			continue;

		cinfo = &palmas_clks->clk_info[i];

		ret = of_property_read_u32(child, "ti,external-sleep-control",
					&prop);
		if (!ret) {
			switch (prop) {
			case PALMAS_CLOCK_DT_EXT_CONTROL_ENABLE1:
				prop = PALMAS_EXT_CONTROL_ENABLE1;
				break;
			case PALMAS_CLOCK_DT_EXT_CONTROL_ENABLE2:
				prop = PALMAS_EXT_CONTROL_ENABLE2;
				break;
			case PALMAS_CLOCK_DT_EXT_CONTROL_NSLEEP:
				prop = PALMAS_EXT_CONTROL_NSLEEP;
				break;
			default:
				WARN_ON(1);
				dev_warn(&pdev->dev,
					"%s: Invalid ext control option: %u\n",
					child->name, prop);
				prop = 0;
				break;
			}
			cinfo->ext_control_pin = prop;
		}
	}

	return 0;
}

static int palmas_clks_init_configure(struct palmas_clock_info *cinfo)
{
	struct palmas_clks *palmas_clks = cinfo->palmas_clk;
	int ret;

	ret = palmas_update_bits(palmas_clks->palmas, PALMAS_RESOURCE_BASE,
			cinfo->clk_desc->control_reg,
			cinfo->clk_desc->sleep_mask, 0);
	if (ret < 0) {
		dev_err(palmas_clks->dev, "Reg 0x%02x update failed, %d\n",
			cinfo->clk_desc->control_reg, ret);
		return ret;
	}

	if (cinfo->ext_control_pin) {
		ret = clk_prepare(cinfo->clk);
		if (ret < 0) {
			dev_err(palmas_clks->dev,
				"Clock prep failed, %d\n", ret);
			return ret;
		}

		ret = palmas_ext_control_req_config(palmas_clks->palmas,
				cinfo->clk_desc->sleep_reqstr_id,
				cinfo->ext_control_pin, true);
		if (ret < 0) {
			dev_err(palmas_clks->dev,
				"Ext config for %s failed, %d\n",
				cinfo->clk_desc->clk_name, ret);
			return ret;
		}
	}

	return ret;
}

static int palmas_clks_probe(struct platform_device *pdev)
{
	struct palmas *palmas = dev_get_drvdata(pdev->dev.parent);
	struct palmas_clock_info *cinfo;
	struct palmas_clks *palmas_clks;
	struct clk *clk;
	int i, ret;

	palmas_clks = devm_kzalloc(&pdev->dev, sizeof(*palmas_clks),
				GFP_KERNEL);
	if (!palmas_clks)
		return -ENOMEM;

	palmas_clks->clk_data.clks = devm_kzalloc(&pdev->dev,
			PALMAS_CLOCK_MAX * sizeof(*palmas_clks->clk_data.clks),
			GFP_KERNEL);
	if (!palmas_clks->clk_data.clks)
		return -ENOMEM;

	palmas_clks_get_clk_data(pdev, palmas_clks);
	platform_set_drvdata(pdev, palmas_clks);

	palmas_clks->dev = &pdev->dev;
	palmas_clks->palmas = palmas;

	for (i = 0; i < PALMAS_CLOCK_MAX; i++) {
		cinfo = &palmas_clks->clk_info[i];
		cinfo->clk_desc = &palmas_clk32k_descs[i];
		cinfo->hw.init = &palmas_clks_hw_init[i];
		cinfo->palmas_clk = palmas_clks;
		clk = devm_clk_register(&pdev->dev, &cinfo->hw);
		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			dev_err(&pdev->dev, "Fail to register clock %s, %d\n",
				palmas_clk32k_descs[i].clk_name, ret);
			return ret;
		}

		cinfo->clk = clk;
		palmas_clks->clk_data.clks[i] = clk;
		palmas_clks->clk_data.clk_num++;
		ret = palmas_clks_init_configure(cinfo);
		if (ret < 0) {
			dev_err(&pdev->dev, "Clock config failed, %d\n", ret);
			return ret;
		}
	}

	ret = of_clk_add_provider(pdev->dev.of_node, of_clk_src_simple_get,
			&palmas_clks->clk_data);
	if (ret < 0)
		dev_err(&pdev->dev, "Fail to add clock driver, %d\n", ret);
	return ret;
}

static int palmas_clks_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
	return 0;
}

static struct of_device_id of_palmas_clks_match_tbl[] = {
	{ .compatible = "ti,palmas-clk", },
	{},
};
MODULE_DEVICE_TABLE(of, of_palmas_clks_match_tbl);

static struct platform_driver palmas_clks_driver = {
	.driver = {
		.name = "palmas-clk",
		.owner = THIS_MODULE,
		.of_match_table = of_palmas_clks_match_tbl,
	},
	.probe = palmas_clks_probe,
	.remove = palmas_clks_remove,
};

module_platform_driver(palmas_clks_driver);

MODULE_DESCRIPTION("Clock driver for Palmas Series Devices");
MODULE_ALIAS("platform:palmas-clk");
MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_LICENSE("GPL v2");
