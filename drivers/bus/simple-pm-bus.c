/*
 * Simple Power-Managed Bus Driver
 *
 * Copyright (C) 2014 Glider bvba
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>


static int simple_pm_bus_probe(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "%s\n", __func__);

	pm_runtime_enable(&pdev->dev);
	return 0;
}

static int simple_pm_bus_remove(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "%s\n", __func__);

	pm_runtime_disable(&pdev->dev);
	return 0;
}

static const struct of_device_id simple_pm_bus_of_match[] = {
	{ .compatible = "renesas,bsc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, simple_pm_bus_of_match);

static struct platform_driver simple_pm_bus_driver = {
	.probe = simple_pm_bus_probe,
	.remove = simple_pm_bus_remove,
	.driver = {
		.name = "simple-pm-bus",
		.of_match_table = simple_pm_bus_of_match,
	},
};

module_platform_driver(simple_pm_bus_driver);

MODULE_DESCRIPTION("Simple Power-Managed Bus Driver");
MODULE_AUTHOR("Geert Uytterhoeven <geert+renesas@glider.be>");
MODULE_LICENSE("GPL v2");
