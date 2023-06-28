// SPDX-License-Identifier: GPL-2.0
/*
 * pci-j721e-host - PCIe host controller driver for TI's J721E SoCs
 *
 * Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/of_device.h>

#include "pcie-cadence.h"
#include "pci-j721e.h"

static int cdns_ti_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 *value)
{
	if (pci_is_root_bus(bus))
		return pci_generic_config_read32(bus, devfn, where, size,
						 value);

	return pci_generic_config_read(bus, devfn, where, size, value);
}

static int cdns_ti_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 value)
{
	if (pci_is_root_bus(bus))
		return pci_generic_config_write32(bus, devfn, where, size,
						value);

	return pci_generic_config_write(bus, devfn, where, size, value);
}

static struct pci_ops cdns_ti_pcie_host_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= cdns_ti_pcie_config_read,
	.write		= cdns_ti_pcie_config_write,
};

static const struct j721e_pcie_data j721e_pcie_rc_data = {
	.mode = PCI_MODE_RC,
	.quirk_retrain_flag = true,
	.byte_access_allowed = false,
	.linkdown_irq_regfield = LINK_DOWN,
	.max_lanes = 2,
};

static const struct j721e_pcie_data j7200_pcie_rc_data = {
	.mode = PCI_MODE_RC,
	.quirk_detect_quiet_flag = true,
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	.byte_access_allowed = true,
	.max_lanes = 2,
};

static const struct j721e_pcie_data am64_pcie_rc_data = {
	.mode = PCI_MODE_RC,
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	.byte_access_allowed = true,
	.max_lanes = 1,
};

static const struct j721e_pcie_data j784s4_pcie_rc_data = {
	.mode = PCI_MODE_RC,
	.quirk_retrain_flag = true,
	.byte_access_allowed = false,
	.linkdown_irq_regfield = LINK_DOWN,
	.max_lanes = 4,
};

static const struct of_device_id of_j721e_pcie_host_match[] = {
	{
		.compatible = "ti,j721e-pcie-host",
		.data = &j721e_pcie_rc_data,
	},
	{
		.compatible = "ti,j7200-pcie-host",
		.data = &j7200_pcie_rc_data,
	},
	{
		.compatible = "ti,am64-pcie-host",
		.data = &am64_pcie_rc_data,
	},
	{
		.compatible = "ti,j784s4-pcie-host",
		.data = &j784s4_pcie_rc_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, of_j721e_pcie_host_match);

static int j721e_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	const struct j721e_pcie_data *data;
	struct cdns_pcie *cdns_pcie;
	struct j721e_pcie *pcie;
	struct cdns_pcie_rc *rc = NULL;
	struct gpio_desc *gpiod;
	struct clk *clk;
	int ret;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	if (!bridge)
		return -ENOMEM;

	if (!data->byte_access_allowed)
		bridge->ops = &cdns_ti_pcie_host_ops;
	rc = pci_host_bridge_priv(bridge);
	rc->quirk_retrain_flag = data->quirk_retrain_flag;
	rc->quirk_detect_quiet_flag = data->quirk_detect_quiet_flag;

	cdns_pcie = &rc->pcie;
	cdns_pcie->dev = dev;
	cdns_pcie->ops = &j721e_pcie_ops;
	pcie->cdns_pcie = cdns_pcie;

	pcie->mode = PCI_MODE_RC;
	pcie->linkdown_irq_regfield = data->linkdown_irq_regfield;

	ret = j721e_pcie_common_init(pcie);
	if (ret)
		return ret;

	gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(gpiod)) {
		ret = PTR_ERR(gpiod);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get reset GPIO\n");
		goto err_get_sync;
	}

	ret = cdns_pcie_init_phy(dev, cdns_pcie);
	if (ret) {
		dev_err(dev, "Failed to init phy\n");
		goto err_get_sync;
	}

	clk = devm_clk_get_optional(dev, "pcie_refclk");
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		dev_err(dev, "failed to get pcie_refclk\n");
		goto err_pcie_setup;
	}

	ret = clk_prepare_enable(clk);
	if (ret) {
		dev_err(dev, "failed to enable pcie_refclk\n");
		goto err_pcie_setup;
	}
	pcie->refclk = clk;

	/*
	 * "Power Sequencing and Reset Signal Timings" table in
	 * PCI EXPRESS CARD ELECTROMECHANICAL SPECIFICATION, REV. 3.0
	 * indicates PERST# should be deasserted after minimum of 100us
	 * once REFCLK is stable. The REFCLK to the connector in RC
	 * mode is selected while enabling the PHY. So deassert PERST#
	 * after 100 us.
	 */
	if (gpiod) {
		usleep_range(100, 200);
		gpiod_set_value_cansleep(gpiod, 1);
	}

	ret = cdns_pcie_host_setup(rc);
	if (ret < 0) {
		clk_disable_unprepare(pcie->refclk);
		goto err_pcie_setup;
	}

	return 0;

err_pcie_setup:
	cdns_pcie_disable_phy(cdns_pcie);

err_get_sync:
	j721e_disable_common_init(dev);

	return ret;
}

static int j721e_pcie_remove(struct platform_device *pdev)
{
	struct j721e_pcie *pcie = platform_get_drvdata(pdev);
	struct cdns_pcie *cdns_pcie = pcie->cdns_pcie;
	struct cdns_pcie_rc *rc = container_of(cdns_pcie, struct cdns_pcie_rc, pcie);
	struct device *dev = &pdev->dev;

	cdns_pcie_host_remove_setup(rc);
	j721e_pcie_remove_link_irq(pcie);

	cdns_pcie_stop_link(cdns_pcie);
	clk_disable_unprepare(pcie->refclk);

	gpiod_set_value_cansleep(pcie->gpiod, 0);
	cdns_pcie_deinit_phy(cdns_pcie);
	j721e_disable_common_init(dev);

	return 0;
}

static struct platform_driver j721e_pcie_host_driver = {
	.probe  = j721e_pcie_probe,
	.remove = j721e_pcie_remove,
	.driver = {
		.name	= "j721e-pcie-host",
		.of_match_table = of_j721e_pcie_host_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(j721e_pcie_host_driver);
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_LICENSE("GPL v2");
