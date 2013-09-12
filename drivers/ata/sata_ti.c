/**
 * sata-ti.c - Texas Instruments Specific SATA Glue layer
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Roger Quadros <rogerq@ti.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_platform.h>

/*
 * All these registers belong to OMAP's Wrapper around the
 * DesignWare SATA Core.
 */

#define SATA_SYSCONFIG				0x0000
#define SATA_CDRLOCK				0x0004

struct ti_sata {
	struct device *dev;
	void __iomem *base;
};

static int ti_sata_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct ti_sata *sata;
	struct resource *res;
	void __iomem *base;
	int ret;

	if (!np) {
		dev_err(dev, "device node not found\n");
		return -EINVAL;
	}

	sata = devm_kzalloc(dev, sizeof(*sata), GFP_KERNEL);
	if (!sata)
		return -ENOMEM;

	platform_set_drvdata(pdev, sata);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "missing memory base resource\n");
		return -EINVAL;
	}

	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	sata->dev = dev;
	sata->base = base;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync failed with err %d\n",
									ret);
		goto runtime_disable;
	}

	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to create TI SATA children\n");
		goto runtime_put;
	}

	return 0;

runtime_put:
	pm_runtime_put_sync(dev);

runtime_disable:
	pm_runtime_disable(dev);

	return ret;
}

static int ti_sata_remove_child(struct device *dev, void *c)
{
	struct platform_device *pdev = to_platform_device(dev);

	platform_device_unregister(pdev);

	return 0;
}

static int ti_sata_remove(struct platform_device *pdev)
{
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	device_for_each_child(&pdev->dev, NULL, ti_sata_remove_child);

	return 0;
}

static const struct of_device_id of_ti_sata_match[] = {
	{
		.compatible =	"ti,sata"
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_ti_sata_match);

#ifdef CONFIG_PM

static int ti_sata_resume(struct device *dev)
{
	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	return 0;
}

static const struct dev_pm_ops ti_sata_dev_pm_ops = {
	.resume = ti_sata_resume,
};

#define DEV_PM_OPS	(&ti_sata_dev_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif /* CONFIG_PM */

static struct platform_driver ti_sata_driver = {
	.probe		= ti_sata_probe,
	.remove		= ti_sata_remove,
	.driver		= {
		.name	= "ti-sata",
		.of_match_table	= of_ti_sata_match,
		.pm	= DEV_PM_OPS,
	},
};

module_platform_driver(ti_sata_driver);

MODULE_ALIAS("platform:ti-sata");
MODULE_AUTHOR("Roger Quadros <rogerq@ti.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TI SATA Glue Layer");
