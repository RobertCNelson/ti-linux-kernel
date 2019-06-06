// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS PCI Glue driver
 *
 * Copyright (C) 2018-2019 Cadence.
 *
 * Author: Pawel Laszczak <pawell@cadence.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

struct cdns3_wrap {
	struct platform_device *plat_dev;
	struct pci_dev *hg_dev;
	struct resource dev_res[4];
};

#define RES_IRQ_ID		0
#define RES_HOST_ID		1
#define RES_DEV_ID		2
#define RES_DRD_ID		3

#define PCI_BAR_HOST		0
#define PCI_BAR_DEV		2
#define PCI_BAR_OTG		4

#define PCI_DEV_FN_HOST_DEVICE	0
#define PCI_DEV_FN_OTG		1

#define PCI_DRIVER_NAME		"cdns3-pci-usbss"
#define PLAT_DRIVER_NAME	"cdns-usb3"

#define CDNS_VENDOR_ID		0x17cd
#define CDNS_DEVICE_ID		0x0100

static int cdns3_pci_probe(struct pci_dev *pdev,
			   const struct pci_device_id *id)
{
	struct platform_device_info plat_info;
	struct cdns3_wrap *wrap;
	struct resource *res;
	int err;

	/*
	 * for GADGET/HOST PCI (devfn) function number is 0,
	 * for OTG PCI (devfn) function number is 1
	 */
	if (!id || pdev->devfn != PCI_DEV_FN_HOST_DEVICE)
		return -EINVAL;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "Enabling PCI device has failed %d\n", err);
		return err;
	}

	pci_set_master(pdev);
	wrap = devm_kzalloc(&pdev->dev, sizeof(*wrap), GFP_KERNEL);
	if (!wrap) {
		pci_disable_device(pdev);
		return -ENOMEM;
	}

	/* function 0: host(BAR_0) + device(BAR_1) + otg(BAR_2)). */
	dev_dbg(&pdev->dev, "Initialize Device resources\n");
	res = wrap->dev_res;

	res[RES_DEV_ID].start = pci_resource_start(pdev, PCI_BAR_DEV);
	res[RES_DEV_ID].end =   pci_resource_end(pdev, PCI_BAR_DEV);
	res[RES_DEV_ID].name = "dev";
	res[RES_DEV_ID].flags = IORESOURCE_MEM;
	dev_dbg(&pdev->dev, "USBSS-DEV physical base addr: %pa\n",
		&res[RES_DEV_ID].start);

	res[RES_HOST_ID].start = pci_resource_start(pdev, PCI_BAR_HOST);
	res[RES_HOST_ID].end = pci_resource_end(pdev, PCI_BAR_HOST);
	res[RES_HOST_ID].name = "xhci";
	res[RES_HOST_ID].flags = IORESOURCE_MEM;
	dev_dbg(&pdev->dev, "USBSS-XHCI physical base addr: %pa\n",
		&res[RES_HOST_ID].start);

	res[RES_DRD_ID].start = pci_resource_start(pdev, PCI_BAR_OTG);
	res[RES_DRD_ID].end =   pci_resource_end(pdev, PCI_BAR_OTG);
	res[RES_DRD_ID].name = "otg";
	res[RES_DRD_ID].flags = IORESOURCE_MEM;
	dev_dbg(&pdev->dev, "USBSS-DRD physical base addr: %pa\n",
		&res[RES_DRD_ID].start);

	/* Interrupt common for both device and XHCI */
	wrap->dev_res[RES_IRQ_ID].start = pdev->irq;
	wrap->dev_res[RES_IRQ_ID].name = "cdns3-irq";
	wrap->dev_res[RES_IRQ_ID].flags = IORESOURCE_IRQ;

	/* set up platform device info */
	memset(&plat_info, 0, sizeof(plat_info));
	plat_info.parent = &pdev->dev;
	plat_info.fwnode = pdev->dev.fwnode;
	plat_info.name = PLAT_DRIVER_NAME;
	plat_info.id = pdev->devfn;
	plat_info.res = wrap->dev_res;
	plat_info.num_res = ARRAY_SIZE(wrap->dev_res);
	plat_info.dma_mask = pdev->dma_mask;

	/* register platform device */
	wrap->plat_dev = platform_device_register_full(&plat_info);
	if (IS_ERR(wrap->plat_dev)) {
		pci_disable_device(pdev);
		return PTR_ERR(wrap->plat_dev);
	}

	pci_set_drvdata(pdev, wrap);

	return err;
}

static void cdns3_pci_remove(struct pci_dev *pdev)
{
	struct cdns3_wrap *wrap = (struct cdns3_wrap *)pci_get_drvdata(pdev);

	platform_device_unregister(wrap->plat_dev);
}

static const struct pci_device_id cdns3_pci_ids[] = {
	{ PCI_DEVICE(CDNS_VENDOR_ID, CDNS_DEVICE_ID), },
	{ 0, }
};

static struct pci_driver cdns3_pci_driver = {
	.name = PCI_DRIVER_NAME,
	.id_table = cdns3_pci_ids,
	.probe = cdns3_pci_probe,
	.remove = cdns3_pci_remove,
};

module_pci_driver(cdns3_pci_driver);
MODULE_DEVICE_TABLE(pci, cdns3_pci_ids);

MODULE_AUTHOR("Pawel Laszczak <pawell@cadence.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cadence USBSS PCI wrapperr");
