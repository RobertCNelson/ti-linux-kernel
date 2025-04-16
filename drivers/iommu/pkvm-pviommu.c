// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <linux/of_platform.h>
#include <linux/arm-smccc.h>
#include <linux/iommu.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

struct pviommu_domain {
	struct iommu_domain		domain;
	unsigned long			id; /* pKVM domain ID. */
};

struct pviommu {
	struct iommu_device		iommu;
	u32				id;
};

struct pviommu_master {
	struct device			*dev;
	struct pviommu			*iommu;
	u32				ssid_bits;
	struct pviommu_domain		*domain;
};

static int smccc_to_linux_ret(u64 smccc_ret)
{
	switch (smccc_ret) {
	case SMCCC_RET_SUCCESS:
		return 0;
	case SMCCC_RET_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case SMCCC_RET_NOT_REQUIRED:
		return -ENOENT;
	case SMCCC_RET_INVALID_PARAMETER:
		return -EINVAL;
	};

	return -ENODEV;
}

static int pviommu_map_pages(struct iommu_domain *domain, unsigned long iova,
			     phys_addr_t paddr, size_t pgsize, size_t pgcount,
			     int prot, gfp_t gfp, size_t *mapped)
{
	return 0;
}

static size_t pviommu_unmap_pages(struct iommu_domain *domain, unsigned long iova,
				  size_t pgsize, size_t pgcount,
				  struct iommu_iotlb_gather *gather)
{
	return 0;
}

static phys_addr_t pviommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	return 0;
}

static void pviommu_domain_free(struct iommu_domain *domain)
{
	struct pviommu_domain *pv_domain = container_of(domain, struct pviommu_domain, domain);
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(ARM_SMCCC_VENDOR_HYP_KVM_PVIOMMU_OP_FUNC_ID,
			  KVM_PVIOMMU_OP_FREE_DOMAIN, pv_domain->id, 0, 0, 0, 0, &res);
	if (res.a0 != SMCCC_RET_SUCCESS)
		pr_err("Failed to free domain %ld\n", res.a0);
	kfree(pv_domain);
}

static void pviommu_detach_dev(struct pviommu_master *master)
{
	struct device *dev = master->dev;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct pviommu *pv = master->iommu;
	struct pviommu_domain *pv_domain = master->domain;
	struct arm_smccc_res res;
	u32 sid;
	int i;

	if (!fwspec || !pv_domain)
		return;

	for (i = 0; i < fwspec->num_ids; i++) {
		sid = fwspec->ids[i];
		arm_smccc_1_1_hvc(ARM_SMCCC_VENDOR_HYP_KVM_PVIOMMU_OP_FUNC_ID,
				  KVM_PVIOMMU_OP_DETACH_DEV,
				  pv->id, sid, 0, pv_domain->id, 0, &res);
		if (res.a0 != SMCCC_RET_SUCCESS)
			dev_err(dev, "Failed to detach_dev sid %d, err %ld\n", sid, res.a0);
	}

	master->domain = NULL;
}

static int pviommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	int ret = 0, i;
	struct arm_smccc_res res;
	u32 sid;
	struct pviommu_master *master = dev_iommu_priv_get(dev);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct pviommu_domain *pv_domain = container_of(domain, struct pviommu_domain, domain);
	struct pviommu *pv = master->iommu;

	if (!fwspec)
		return -ENOENT;

	if (master->domain)
		pviommu_detach_dev(master);

	for (i = 0; i < fwspec->num_ids; i++) {
		sid = fwspec->ids[i];
		arm_smccc_1_1_hvc(ARM_SMCCC_VENDOR_HYP_KVM_PVIOMMU_OP_FUNC_ID,
				  KVM_PVIOMMU_OP_ATTACH_DEV,
				  pv->id, sid, 0 /* PASID */,
				  pv_domain->id, master->ssid_bits, &res);
		if (res.a0) {
			ret = smccc_to_linux_ret(res.a0);
			break;
		}
	}

	if (ret) {
		while (i--) {
			arm_smccc_1_1_hvc(ARM_SMCCC_VENDOR_HYP_KVM_PVIOMMU_OP_FUNC_ID,
					  KVM_PVIOMMU_OP_DETACH_DEV,
					  pv->id, sid, 0 /* PASID */,
					  pv_domain->id, 0, &res);
		}
	} else {
		master->domain = pv_domain;
	}

	return ret;
}

static struct iommu_domain *pviommu_domain_alloc(unsigned int type)
{
	struct pviommu_domain *pv_domain;
	struct arm_smccc_res res;

	if (type != IOMMU_DOMAIN_UNMANAGED &&
	    type != IOMMU_DOMAIN_DMA)
		return ERR_PTR(-EOPNOTSUPP);

	pv_domain = kzalloc(sizeof(*pv_domain), GFP_KERNEL);
	if (!pv_domain)
		return ERR_PTR(-ENOMEM);

	arm_smccc_1_1_hvc(ARM_SMCCC_VENDOR_HYP_KVM_PVIOMMU_OP_FUNC_ID,
			  KVM_PVIOMMU_OP_ALLOC_DOMAIN, 0, 0, 0, 0, 0, &res);
	if (res.a0 != SMCCC_RET_SUCCESS) {
		kfree(pv_domain);
		return ERR_PTR(smccc_to_linux_ret(res.a0));
	}

	pv_domain->id = res.a1;

	return &pv_domain->domain;
}

static struct platform_driver pkvm_pviommu_driver;

static struct pviommu *pviommu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev = driver_find_device_by_fwnode(&pkvm_pviommu_driver.driver, fwnode);

	put_device(dev);
	return dev ? dev_get_drvdata(dev) : NULL;
}

static struct iommu_ops pviommu_ops;

static struct iommu_device *pviommu_probe_device(struct device *dev)
{
	struct pviommu_master *master;
	struct pviommu *pv = NULL;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (!fwspec)
		return ERR_PTR(-ENODEV);

	pv = pviommu_get_by_fwnode(fwspec->iommu_fwnode);
	if (!pv)
		return ERR_PTR(-ENODEV);

	master = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);

	master->dev = dev;
	master->iommu = pv;
	device_property_read_u32(dev, "pasid-num-bits", &master->ssid_bits);
	dev_iommu_priv_set(dev, master);

	return &pv->iommu;
}

static void pviommu_release_device(struct device *dev)
{
	struct pviommu_master *master = dev_iommu_priv_get(dev);

	pviommu_detach_dev(master);
}

static int pviommu_of_xlate(struct device *dev, const struct of_phandle_args *args)
{
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

static struct iommu_group *pviommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	else
		return generic_device_group(dev);
}

static struct iommu_ops pviommu_ops = {
	.device_group		= pviommu_device_group,
	.of_xlate		= pviommu_of_xlate,
	.probe_device		= pviommu_probe_device,
	.release_device		= pviommu_release_device,
	.domain_alloc		= pviommu_domain_alloc,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= pviommu_attach_dev,
		.map_pages	= pviommu_map_pages,
		.unmap_pages	= pviommu_unmap_pages,
		.iova_to_phys	= pviommu_iova_to_phys,
		.free		= pviommu_domain_free,
	}
};

static int pviommu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pviommu *pv = devm_kmalloc(dev, sizeof(*pv), GFP_KERNEL);
	struct device_node *np = pdev->dev.of_node;
	int ret;
	struct arm_smccc_res res;

	ret = of_property_read_u32_index(np, "id", 0, &pv->id);
	if (ret) {
		dev_err(dev, "Failed to read id from device tree node %d\n", ret);
		return ret;
	}

	arm_smccc_1_1_hvc(ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID, 0, 0, 0, &res);
	if (res.a0 < 0)
		return -ENODEV;

	pviommu_ops.pgsize_bitmap = res.a0;

	ret = iommu_device_sysfs_add(&pv->iommu, dev, NULL,
				     "pviommu.%pa", &pv->id);

	ret = iommu_device_register(&pv->iommu, &pviommu_ops, dev);
	if (ret) {
		dev_err(dev, "Couldn't register %d\n", ret);
		iommu_device_sysfs_remove(&pv->iommu);
	}

	platform_set_drvdata(pdev, pv);

	return ret;
}

static const struct of_device_id pviommu_of_match[] = {
	{ .compatible = "pkvm,pviommu", },
	{ },
};

static struct platform_driver pkvm_pviommu_driver = {
	.probe = pviommu_probe,
	.driver = {
		.name = "pkvm-pviommu",
		.of_match_table = pviommu_of_match,
	},
};

module_platform_driver(pkvm_pviommu_driver);

MODULE_DESCRIPTION("IOMMU API for pKVM paravirtualized IOMMU");
MODULE_AUTHOR("Mostafa Saleh <smostafa@google.com>");
MODULE_LICENSE("GPL");
