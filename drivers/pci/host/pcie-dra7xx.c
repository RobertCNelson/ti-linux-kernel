/*
 * pcie-dra7xx - PCIe controller driver for TI DRA7xx SoCs
 *
 * Copyright (C) 2013-2014 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Kishon Vijay Abraham I <kishon@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/resource.h>
#include <linux/types.h>

#include "pcie-designware.h"

/* PCIe controller wrapper TI configuration registers */

#define	PCIECTRL_TI_CONF_IRQSTATUS_MAIN		0x0024
#define	PCIECTRL_TI_CONF_IRQENABLE_SET_MAIN	0x0028
#define	 ERR_SYS				(1 << 0)
#define	 ERR_FATAL				(1 << 1)
#define	 ERR_NONFATAL				(1 << 2)
#define	 ERR_COR				(1 << 3)
#define	 ERR_AXI				(1 << 4)
#define	 ERR_ECRC				(1 << 5)
#define	 PME_TURN_OFF				(1 << 8)
#define	 PME_TO_ACK				(1 << 9)
#define	 PM_PME					(1 << 10)
#define	 LINK_REQ_RST				(1 << 11)
#define	 LINK_UP_EVT				(1 << 12)
#define	 CFG_BME_EVT				(1 << 13)
#define	 CFG_MSE_EVT				(1 << 14)
#define	 INTERRUPTS (ERR_SYS | ERR_FATAL | ERR_NONFATAL | ERR_COR | ERR_AXI | \
			ERR_ECRC | PME_TURN_OFF | PME_TO_ACK | PM_PME | \
			LINK_REQ_RST | LINK_UP_EVT | CFG_BME_EVT | CFG_MSE_EVT)

#define	PCIECTRL_TI_CONF_IRQSTATUS_MSI		0x0034
#define	PCIECTRL_TI_CONF_IRQENABLE_SET_MSI	0x0038
#define	 INTA					(1 << 0)
#define	 INTB					(1 << 1)
#define	 INTC					(1 << 2)
#define	 INTD					(1 << 3)
#define	 MSI					(1 << 4)
#define	 LEG_EP_INTERRUPTS (INTA | INTB | INTC | INTD)

#define	PCIECTRL_TI_CONF_DEVICE_TYPE		0x0100
#define	 DEVICE_TYPE_EP				0x0
#define	 DEVICE_TYPE_LEG_EP			0x1
#define	 DEVICE_TYPE_RC				0x4

#define	PCIECTRL_TI_CONF_DEVICE_CMD		0x0104
#define	 LTSSM_EN				0x1

#define	PCIECTRL_TI_CONF_PHY_CS			0x010C
#define	  LINK_UP				(1 << 16)

struct dra7xx_pcie {
	void __iomem		*base;
	void __iomem		*ctrl;
	struct phy		*phy;
	struct device		*dev;
	int			irq;
	struct pcie_port	pp;
};

#define to_dra7xx_pcie(x)	container_of((x), struct dra7xx_pcie, pp)

enum dra7xx_pcie_device_type {
	DRA7XX_PCIE_UNKNOWN_TYPE,
	DRA7XX_PCIE_EP_TYPE,
	DRA7XX_PCIE_LEG_EP_TYPE,
	DRA7XX_PCIE_RC_TYPE,
};

static inline u32 dra7xx_pcie_readl(void __iomem *base, u32 offset)
{
	return readl(base + offset);
}

static inline void dra7xx_pcie_writel(void __iomem *base, u32 offset, u32 value)
{
	writel(value, base + offset);
}

static int dra7xx_pcie_link_up(struct pcie_port *pp)
{
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pp);
	u32 reg = dra7xx_pcie_readl(dra7xx->base, PCIECTRL_TI_CONF_PHY_CS);

	return (reg == LINK_UP);
}

static int dra7xx_pcie_establish_link(struct pcie_port *pp)
{
	u32 reg;
	int retries = 10000;
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pp);

	if (dw_pcie_link_up(pp)) {
		dev_err(pp->dev, "link is already up\n");
		return 0;
	}

	reg = dra7xx_pcie_readl(dra7xx->base, PCIECTRL_TI_CONF_DEVICE_CMD);
	reg |= LTSSM_EN;
	dra7xx_pcie_writel(dra7xx->base, PCIECTRL_TI_CONF_DEVICE_CMD, reg);

	while (--retries) {
		reg = dra7xx_pcie_readl(dra7xx->base, PCIECTRL_TI_CONF_PHY_CS);
		if (reg & LINK_UP)
			break;
		else
			usleep_range(5, 10);
	}

	if (retries <= 0) {
		dev_err(pp->dev, "link is not up\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static void dra7xx_pcie_enable_interrupts(struct pcie_port *pp)
{
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pp);

	dra7xx_pcie_writel(dra7xx->base, PCIECTRL_TI_CONF_IRQSTATUS_MAIN,
		~INTERRUPTS);
	dra7xx_pcie_writel(dra7xx->base, PCIECTRL_TI_CONF_IRQENABLE_SET_MAIN,
		INTERRUPTS);
	dra7xx_pcie_writel(dra7xx->base, PCIECTRL_TI_CONF_IRQSTATUS_MSI,
		~LEG_EP_INTERRUPTS & ~MSI);

	if (IS_ENABLED(CONFIG_PCI_MSI))
		dra7xx_pcie_writel(dra7xx->base,
			PCIECTRL_TI_CONF_IRQENABLE_SET_MSI, MSI);
	else
		dra7xx_pcie_writel(dra7xx->base,
			PCIECTRL_TI_CONF_IRQENABLE_SET_MSI, LEG_EP_INTERRUPTS);
}

static void dra7xx_pcie_host_init(struct pcie_port *pp)
{
	dw_pcie_setup_rc(pp);
	dra7xx_pcie_establish_link(pp);
	if (IS_ENABLED(CONFIG_PCI_MSI))
		dw_pcie_msi_init(pp);
	dra7xx_pcie_enable_interrupts(pp);
}

static struct pcie_host_ops dra7xx_pcie_host_ops = {
	.link_up = dra7xx_pcie_link_up,
	.host_init = dra7xx_pcie_host_init,
};

static irqreturn_t dra7xx_pcie_msi_irq_handler(int irq, void *arg)
{
	struct pcie_port *pp = arg;
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pp);
	u32 reg;

	reg = dra7xx_pcie_readl(dra7xx->base, PCIECTRL_TI_CONF_IRQSTATUS_MSI);
	dw_handle_msi_irq(pp);
	dra7xx_pcie_writel(dra7xx->base, PCIECTRL_TI_CONF_IRQSTATUS_MSI, reg);

	return IRQ_HANDLED;
}


static irqreturn_t dra7xx_pcie_irq_handler(int irq, void *arg)
{
	struct dra7xx_pcie *dra7xx = arg;
	u32 reg;

	reg = dra7xx_pcie_readl(dra7xx->base, PCIECTRL_TI_CONF_IRQSTATUS_MAIN);

	if (reg & ERR_SYS)
		dev_dbg(dra7xx->dev, "System Error\n");

	if (reg & ERR_FATAL)
		dev_dbg(dra7xx->dev, "Fatal Error\n");

	if (reg & ERR_NONFATAL)
		dev_dbg(dra7xx->dev, "Non Fatal Error\n");

	if (reg & ERR_COR)
		dev_dbg(dra7xx->dev, "Correctable Error\n");

	if (reg & ERR_AXI)
		dev_dbg(dra7xx->dev, "AXI tag lookup fatal Error\n");

	if (reg & ERR_ECRC)
		dev_dbg(dra7xx->dev, "ECRC Error\n");

	if (reg & PME_TURN_OFF)
		dev_dbg(dra7xx->dev,
			"Power Management Event Turn-Off message received\n");

	if (reg & PME_TO_ACK)
		dev_dbg(dra7xx->dev,
		"Power Management Event Turn-Off Ack message received\n");

	if (reg & PM_PME)
		dev_dbg(dra7xx->dev,
			"PM Power Management Event message received\n");

	if (reg & LINK_REQ_RST)
		dev_dbg(dra7xx->dev, "Link Request Reset\n");

	if (reg & LINK_UP_EVT)
		dev_dbg(dra7xx->dev, "Link-up state change\n");

	if (reg & CFG_BME_EVT)
		dev_dbg(dra7xx->dev, "CFG 'Bus Master Enable' change\n");

	if (reg & CFG_MSE_EVT)
		dev_dbg(dra7xx->dev, "CFG 'Memory Space Enable' change\n");

	dra7xx_pcie_writel(dra7xx->base, PCIECTRL_TI_CONF_IRQSTATUS_MAIN, reg);

	return IRQ_HANDLED;
}

static int add_pcie_port(struct dra7xx_pcie *dra7xx,
	struct platform_device *pdev)
{
	int ret;
	struct pcie_port *pp;
	struct resource *res;
	struct device *dev = &pdev->dev;

	pp = &dra7xx->pp;
	pp->dev = dev;
	pp->ops = &dra7xx_pcie_host_ops;

	spin_lock_init(&pp->conf_lock);

	pp->irq = platform_get_irq(pdev, 1);
	if (pp->irq < 0) {
		dev_err(dev, "missing IRQ resource\n");
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		ret = devm_request_irq(&pdev->dev, pp->irq,
			dra7xx_pcie_msi_irq_handler, IRQF_SHARED, "dra7-pcie",
			pp);
		if (ret) {
			dev_err(&pdev->dev, "failed to request irq\n");
			return ret;
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_dbics");
	if (!res) {
		dev_err(dev, "missing dbics base resource\n");
		return -EINVAL;
	}

	pp->dbi_base = devm_ioremap_nocache(dev, res->start,
		resource_size(res));
	if (!pp->dbi_base) {
		dev_err(dev, "ioremap failed\n");
		return -ENOMEM;
	}

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dra7xx->dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static void dra7xx_unlock_memory(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_writel(dra7xx->ctrl, 0x0, 0x2FF1AC2B);
	dra7xx_pcie_writel(dra7xx->ctrl, 0x4, 0xF757FDC0);
	dra7xx_pcie_writel(dra7xx->ctrl, 0x8, 0xE2BC3A6D);
	dra7xx_pcie_writel(dra7xx->ctrl, 0xc, 0x1eBF131D);
	dra7xx_pcie_writel(dra7xx->ctrl, 0x10, 0x6F361E05);
}

static int __init dra7xx_pcie_probe(struct platform_device *pdev)
{
	u32 reg;
	int ret;
	int irq;
	struct phy *phy;
	void __iomem *base;
	void __iomem *ctrl;
	struct resource *res;
	struct dra7xx_pcie *dra7xx;
	int device_type = 0;
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	struct reset_control *rstc;
	int retries = 10000;

	dra7xx = devm_kzalloc(&pdev->dev, sizeof(*dra7xx), GFP_KERNEL);
	if (!dra7xx)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "missing IRQ resource\n");
		return -EINVAL;
	}

	ret = devm_request_irq(&pdev->dev, irq, dra7xx_pcie_irq_handler,
				IRQF_SHARED, "dra7xx-pcie1", dra7xx);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		return ret;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ti_conf");
	if (!res) {
		dev_err(dev, "missing PCIe TI conf resource\n");
		return -EINVAL;
	}

	base = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!base) {
		dev_err(dev, "ioremap failed\n");
		return -ENOMEM;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mmr_unlock");
	if (!res) {
		dev_err(dev, "missing mmr unlock base resource\n");
		return -EINVAL;
	}

	ctrl = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!ctrl) {
		dev_err(dev, "ioremap failed\n");
		return -ENOMEM;
	}

	phy = devm_phy_get(dev, "pcie-phy1");
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	dra7xx->base = base;
	dra7xx->ctrl = ctrl;
	dra7xx->phy = phy;
	dra7xx->irq = irq;
	dra7xx->dev = dev;

	dra7xx_unlock_memory(dra7xx);

	rstc = devm_reset_control_get(dev, "reset");
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	ret = reset_control_deassert(rstc);
	if (ret)
		return ret;

	while (--retries) {
		if (reset_control_is_reset(rstc))
			break;
		else
			usleep_range(5, 10);
	}

	if (retries <= 0) {
		dev_err(dev, "reset failed\n");
		return -ETIMEDOUT;
	}

	phy_init(phy);
	phy_power_on(phy);

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (IS_ERR_VALUE(ret)) {
		dev_err(dev, "pm_runtime_get_sync failed\n");
		return ret;
	}

	of_property_read_u32(node, "ti,device-type", &device_type);
	switch (device_type) {
	case DRA7XX_PCIE_RC_TYPE:
		dra7xx_pcie_writel(dra7xx->base,
			PCIECTRL_TI_CONF_DEVICE_TYPE, DEVICE_TYPE_RC);
		break;
	case DRA7XX_PCIE_EP_TYPE:
		dra7xx_pcie_writel(dra7xx->base,
			PCIECTRL_TI_CONF_DEVICE_TYPE, DEVICE_TYPE_EP);
		break;
	case DRA7XX_PCIE_LEG_EP_TYPE:
		dra7xx_pcie_writel(dra7xx->base,
			PCIECTRL_TI_CONF_DEVICE_TYPE, DEVICE_TYPE_LEG_EP);
		break;
	default:
		dev_dbg(dev, "UNKNOWN device type %d\n", device_type);
	}

	reg = dra7xx_pcie_readl(dra7xx->base, PCIECTRL_TI_CONF_DEVICE_CMD);
	reg &= ~LTSSM_EN;
	dra7xx_pcie_writel(dra7xx->base, PCIECTRL_TI_CONF_DEVICE_CMD, reg);

	ret = add_pcie_port(dra7xx, pdev);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, dra7xx);
	return 0;
}

static int __exit dra7xx_pcie_remove(struct platform_device *pdev)
{
	struct dra7xx_pcie *dra7xx = platform_get_drvdata(pdev);

	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	phy_power_off(dra7xx->phy);
	phy_exit(dra7xx->phy);

	return 0;
}

static const struct of_device_id of_dra7xx_pcie_match[] = {
	{ .compatible = "ti,dra7xx-pcie", },
	{},
};
MODULE_DEVICE_TABLE(of, of_dra7xx_pcie_match);

static struct platform_driver dra7xx_pcie_driver = {
	.remove		= __exit_p(dra7xx_pcie_remove),
	.driver = {
		.name	= "dra7xx-pcie",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(of_dra7xx_pcie_match),
	},
};

module_platform_driver_probe(dra7xx_pcie_driver, dra7xx_pcie_probe);

MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_DESCRIPTION("TI PCIe controller driver");
MODULE_LICENSE("GPL v2");
