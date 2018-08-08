// SPDX-License-Identifier: GPL-2.0
/*
 * pci-dra8 - PCIe controller driver for TI DRA8 SoCs
 *
 * Copyright (C) 2018-2019 Texas Instruments Incorporated - http://www.ti.com
 *
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

#include <dt-bindings/pci/pci.h>
#include <linux/io.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>

#define EOI_REG			0x10

#define ENABLE_REG_SYS_0	0x100
#define STATUS_REG_SYS_0	0x500
#define INTx_EN(num)		(1 << (num))

struct dra8_pcie {
	struct device		*dev;
	struct device_node	*node;
	void __iomem		*intd_cfg_base;
	void __iomem		*user_cfg_base;
	struct irq_domain	*legacy_irq_domain;
};

static inline u32 dra8_pcie_intd_readl(struct dra8_pcie *pcie, u32 offset)
{
	return readl(pcie->intd_cfg_base + offset);
}

static inline void dra8_pcie_intd_writel(struct dra8_pcie *pcie, u32 offset,
					 u32 value)
{
	writel(value, pcie->intd_cfg_base + offset);
}

static inline u32 dra8_pcie_user_readl(struct dra8_pcie *pcie, u32 offset)
{
	return readl(pcie->user_cfg_base + offset);
}

static inline void dra8_pcie_user_writel(struct dra8_pcie *pcie, u32 offset,
					 u32 value)
{
	writel(value, pcie->user_cfg_base + offset);
}

static void dra8_pcie_legacy_irq_handler(struct irq_desc *desc)
{
	int i;
	u32 reg;
	int virq;
	struct dra8_pcie *pcie = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	chained_irq_enter(chip, desc);

	for (i = 0; i < PCI_NUM_INTX; i++) {
		reg = dra8_pcie_intd_readl(pcie, STATUS_REG_SYS_0);
		if (!(reg & INTx_EN(i)))
			continue;

		virq = irq_linear_revmap(pcie->legacy_irq_domain, i);
		generic_handle_irq(virq);
		dra8_pcie_intd_writel(pcie, STATUS_REG_SYS_0, INTx_EN(i));
		dra8_pcie_intd_writel(pcie, EOI_REG, i);
	}

	chained_irq_exit(chip, desc);
}

static int dra8_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops dra8_pcie_intx_domain_ops = {
	.map = dra8_pcie_intx_map,
};

static int dra8_pcie_config_legacy_irq(struct dra8_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct irq_domain *legacy_irq_domain;
	struct device_node *node = pcie->node;
	struct device_node *intc_node;
	int irq;
	u32 reg;
	int i;

	intc_node = of_get_child_by_name(node, "legacy-interrupt-controller");
	if (!intc_node) {
		dev_WARN(dev, "legacy-interrupt-controller node is absent\n");
		return -EINVAL;
	}

	irq = irq_of_parse_and_map(intc_node, 0);
	if (!irq) {
		dev_err(dev, "Failed to parse and map legacy irq\n");
		return -EINVAL;
	}
	irq_set_chained_handler_and_data(irq, dra8_pcie_legacy_irq_handler,
					 pcie);

	legacy_irq_domain = irq_domain_add_linear(intc_node,  PCI_NUM_INTX,
						  &dra8_pcie_intx_domain_ops,
						  NULL);
	if (!legacy_irq_domain) {
		dev_err(dev, "Failed to add irq domain for legacy irqs\n");
		return -EINVAL;
	}
	pcie->legacy_irq_domain = legacy_irq_domain;

	for (i = 0; i < PCI_NUM_INTX; i++) {
		reg = dra8_pcie_intd_readl(pcie, ENABLE_REG_SYS_0);
		reg |= INTx_EN(i);
		dra8_pcie_intd_writel(pcie, ENABLE_REG_SYS_0, reg);
	}

	return 0;
}

static const struct of_device_id of_dra8_pcie_match[] = {
	{
		.compatible = "ti,k3-dra8-pcie",
	},
	{},
};

static int __init dra8_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct platform_device *platform_dev;
	struct device_node *child_node;
	struct dra8_pcie *pcie;
	struct resource *res;
	void __iomem *base;
	u32 mode;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->dev = dev;
	pcie->node = node;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "intd_cfg");
	base = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;
	pcie->intd_cfg_base = base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "user_cfg");
	base = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;
	pcie->user_cfg_base = base;

	ret = of_property_read_u32(node, "pci-mode", &mode);
	if (ret < 0) {
		dev_err(dev, "Failed to get pci-mode binding\n");
		return ret;
	}

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync failed\n");
		goto err_get_sync;
	}

	switch (mode) {
	case PCI_MODE_RC:
		if (!IS_ENABLED(CONFIG_PCIE_CADENCE_HOST)) {
			ret = -ENODEV;
			goto err_get_sync;
		}

		ret = dra8_pcie_config_legacy_irq(pcie);
		if (ret < 0)
			goto err_get_sync;

		child_node = of_get_child_by_name(node, "pcie");
		if (!child_node) {
			dev_WARN(dev, "pcie-rc node is absent\n");
			goto err_get_sync;
		}

		platform_dev = of_platform_device_create(child_node, NULL, dev);
		if (!platform_dev) {
			ret = -ENODEV;
			dev_err(dev, "Failed to create Cadence RC device\n");
			goto err_get_sync;
		}

		break;
	case PCI_MODE_EP:
		if (!IS_ENABLED(CONFIG_PCIE_CADENCE_EP)) {
			ret = -ENODEV;
			goto err_get_sync;
		}

		child_node = of_get_child_by_name(node, "pcie-ep");
		if (!child_node) {
			dev_WARN(dev, "pcie-ep node is absent\n");
			goto err_get_sync;
		}

		platform_dev = of_platform_device_create(child_node, NULL, dev);
		if (!platform_dev) {
			ret = -ENODEV;
			dev_err(dev, "Failed to create Cadence EP device\n");
			goto err_get_sync;
		}

		break;
	default:
		dev_err(dev, "INVALID device type %d\n", mode);
	}

	return 0;

err_get_sync:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return ret;
}

static struct platform_driver dra8_pcie_driver = {
	.driver = {
		.name	= "dra8-pcie",
		.of_match_table = of_dra8_pcie_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver_probe(dra8_pcie_driver, dra8_pcie_probe);
