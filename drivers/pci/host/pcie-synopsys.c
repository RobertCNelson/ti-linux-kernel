/*
 * PCIe RC driver for Synopsys Designware Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Manjunath Bettegowda <manjumb@synopsys.com>
 *	    Jie Deng <jiedeng@synopsys.com>
 *	    Joao Pinto <jpinto@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/types.h>

#include "pcie-designware.h"

#define to_synopsys_pcie(x)	container_of(x, struct synopsys_pcie, pp)

struct synopsys_pcie {
	void __iomem		*mem_base; /* Memory Base to access Core's [RC]
					    * Config Space Layout
					    */
	struct pcie_port	pp;        /* RC Root Port specific structure -
					    * DWC_PCIE_RC stuff
					    */
};

#define PCI_EQUAL_CONTROL_PHY		0x00000707
#define PCIE_PHY_DEBUG_R1_LINK_UP	0x00000010

/* PCIe Port Logic registers (memory-mapped) */
#define PLR_OFFSET 0x700
#define PCIE_PHY_DEBUG_R0 (PLR_OFFSET + 0x28) /* 0x728 */
#define PCIE_PHY_DEBUG_R1 (PLR_OFFSET + 0x2c) /* 0x72c */

/* This handler was created for future work */
static irqreturn_t synopsys_pcie_irq_handler(int irq, void *arg)
{
	return IRQ_NONE;
}

static irqreturn_t synopsys_pcie_msi_irq_handler(int irq, void *arg)
{
	struct pcie_port *pp = arg;

	return dw_handle_msi_irq(pp);
}

static void synopsys_pcie_init_phy(struct pcie_port *pp)
{
	/* write Lane 0 Equalization Control fields register */
	writel(PCI_EQUAL_CONTROL_PHY, pp->dbi_base + 0x154);
}

static int synopsys_pcie_deassert_core_reset(struct pcie_port *pp)
{
	return 0;
}

static void synopsys_pcie_establish_link(struct pcie_port *pp)
{
	int retries;

	/* wait for link to come up */
	for (retries = 0; retries < 10; retries++) {
		if (dw_pcie_link_up(pp)) {
			dev_info(pp->dev, "Link up\n");
			return;
		}
		mdelay(100);
	}

	dev_err(pp->dev, "Link fail\n");
}

/*
 * synopsys_pcie_host_init()
 * Platform specific host/RC initialization
 *	a. Assert the core reset
 *	b. Assert and deassert phy reset and initialize the phy
 *	c. De-Assert the core reset
 *	d. Initializet the Root Port (BARs/Memory Or IO/ Interrupt/ Commnad Reg)
 *	e. Initiate Link startup procedure
 *
 */
static void synopsys_pcie_host_init(struct pcie_port *pp)
{
	/* Initialize Phy (Reset/poweron/control-inputs ) */
	synopsys_pcie_init_phy(pp);

	synopsys_pcie_deassert_core_reset(pp);

	dw_pcie_setup_rc(pp);

	synopsys_pcie_establish_link(pp);

	if (IS_ENABLED(CONFIG_PCI_MSI))
		dw_pcie_msi_init(pp);
}

static int synopsys_pcie_link_up(struct pcie_port *pp)
{
	u32 val;

	val = readl(pp->dbi_base + PCIE_PHY_DEBUG_R1);
	return val & PCIE_PHY_DEBUG_R1_LINK_UP;
}

/**
 * This is RC operation structure
 * synopsys_pcie_link_up: the function which initiates the phy link up procedure
 * synopsys_pcie_host_init: the function which does the host/RC Root port
 * initialization.
 */
static struct pcie_host_ops synopsys_pcie_host_ops = {
	.link_up = synopsys_pcie_link_up,
	.host_init = synopsys_pcie_host_init,
};

/**
 * synopsys_add_pcie_port
 * This function
 * a. installs the interrupt handler
 * b. registers host operations in the pcie_port structure
 */
static int synopsys_add_pcie_port(struct pcie_port *pp,
				 struct platform_device *pdev)
{
	int ret;

	pp->irq = platform_get_irq(pdev, 1);
	if (pp->irq < 0)
		return pp->irq;

	ret = devm_request_irq(&pdev->dev, pp->irq, synopsys_pcie_irq_handler,
				IRQF_SHARED, "synopsys-pcie", pp);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ %d\n", pp->irq);
		return ret;
	}

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq(pdev, 0);
		if (pp->msi_irq < 0)
			return pp->msi_irq;

		ret = devm_request_irq(&pdev->dev, pp->msi_irq,
					synopsys_pcie_msi_irq_handler,
					IRQF_SHARED, "synopsys-pcie-msi", pp);
		if (ret) {
			dev_err(&pdev->dev, "failed to request MSI IRQ %d\n",
				pp->msi_irq);
			return ret;
		}
	}

	pp->root_bus_nr = -1;
	pp->ops = &synopsys_pcie_host_ops;

	/* Below function:
	 * Checks for range property from DT
	 * Gets the IO and MEMORY and CONFIG-Space ranges from DT
	 * Does IOREMAPS on the physical addresses
	 * Gets the num-lanes from DT
	 * Gets MSI capability from DT
	 * Calls the platform specific host initialization
	 * Program the correct class, BAR0, Link width, in Config space
	 * Then it calls pci common init routine
	 * Then it calls function to assign "unassigned resources"
	 */
	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(&pdev->dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

/**
 * synopsys_pcie_probe()
 * This function gets called as part of PCIe registration. If the ID matches
 * the platform driver framework will call this function.
 *
 * @pdev: Pointer to the platform_device structure
 *
 * Returns zero on success; Negative errno on failure
 */
static int synopsys_pcie_probe(struct platform_device *pdev)
{
	struct synopsys_pcie *synopsys_pcie;
	struct pcie_port *pp;
	struct resource *res;
	int ret;

	synopsys_pcie = devm_kzalloc(&pdev->dev, sizeof(*synopsys_pcie),
					GFP_KERNEL);
	if (!synopsys_pcie)
		return -ENOMEM;

	pp = &synopsys_pcie->pp;
	pp->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	synopsys_pcie->mem_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(synopsys_pcie->mem_base))
		return PTR_ERR(synopsys_pcie->mem_base);

	pp->dbi_base = synopsys_pcie->mem_base;

	ret = synopsys_add_pcie_port(pp, pdev);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, synopsys_pcie);

	return 0;
}

static const struct of_device_id synopsys_pcie_of_match[] = {
	{ .compatible = "snps,pcie-synopsys", },
	{},
};
MODULE_DEVICE_TABLE(of, synopsys_pcie_of_match);

static struct platform_driver synopsys_pcie_driver = {
	.driver = {
		.name	= "pcie-synopsys",
		.of_match_table = synopsys_pcie_of_match,
	},
	.probe = synopsys_pcie_probe,
};

module_platform_driver(synopsys_pcie_driver);

MODULE_AUTHOR("Manjunath Bettegowda <manjumb@synopsys.com>");
MODULE_DESCRIPTION("Synopsys PCIe host controller driver");
MODULE_LICENSE("GPL v2");
