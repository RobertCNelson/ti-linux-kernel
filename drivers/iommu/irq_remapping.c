#include <linux/seq_file.h>
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/msi.h>
#include <linux/irq.h>
#include <linux/pci.h>
#include <linux/irqdomain.h>

#include <asm/hw_irq.h>
#include <asm/irq_remapping.h>
#include <asm/processor.h>
#include <asm/x86_init.h>
#include <asm/apic.h>
#include <asm/hpet.h>

#include "irq_remapping.h"

int irq_remapping_enabled;

int disable_irq_remap;
int irq_remap_broken;
int disable_sourceid_checking;
int no_x2apic_optout;

static struct irq_remap_ops *remap_ops;

static void irq_remapping_disable_io_apic(void)
{
	/*
	 * With interrupt-remapping, for now we will use virtual wire A
	 * mode, as virtual wire B is little complex (need to configure
	 * both IOAPIC RTE as well as interrupt-remapping table entry).
	 * As this gets called during crash dump, keep this simple for
	 * now.
	 */
	if (cpu_has_apic || apic_from_smp_config())
		disconnect_bsp_APIC(0);
}

static void __init irq_remapping_modify_x86_ops(void)
{
	x86_io_apic_ops.disable		= irq_remapping_disable_io_apic;
}

static __init int setup_nointremap(char *str)
{
	disable_irq_remap = 1;
	return 0;
}
early_param("nointremap", setup_nointremap);

static __init int setup_irqremap(char *str)
{
	if (!str)
		return -EINVAL;

	while (*str) {
		if (!strncmp(str, "on", 2))
			disable_irq_remap = 0;
		else if (!strncmp(str, "off", 3))
			disable_irq_remap = 1;
		else if (!strncmp(str, "nosid", 5))
			disable_sourceid_checking = 1;
		else if (!strncmp(str, "no_x2apic_optout", 16))
			no_x2apic_optout = 1;

		str += strcspn(str, ",");
		while (*str == ',')
			str++;
	}

	return 0;
}
early_param("intremap", setup_irqremap);

void set_irq_remapping_broken(void)
{
	irq_remap_broken = 1;
}

int irq_remapping_supported(void)
{
	if (disable_irq_remap)
		return 0;

	if (!remap_ops || !remap_ops->supported)
		return 0;

	return remap_ops->supported();
}

int __init irq_remapping_prepare(void)
{
	remap_ops = &intel_irq_remap_ops;

#ifdef CONFIG_AMD_IOMMU
	if (amd_iommu_irq_ops.prepare() == 0) {
		remap_ops = &amd_iommu_irq_ops;
		return 0;
	}
#endif
	return remap_ops->prepare();
}

int __init irq_remapping_enable(void)
{
	int ret;

	if (!remap_ops || !remap_ops->enable)
		return -ENODEV;

	ret = remap_ops->enable();

	if (irq_remapping_enabled)
		irq_remapping_modify_x86_ops();

	return ret;
}

void irq_remapping_disable(void)
{
	if (!irq_remapping_enabled ||
	    !remap_ops ||
	    !remap_ops->disable)
		return;

	remap_ops->disable();
}

int irq_remapping_reenable(int mode)
{
	if (!irq_remapping_enabled ||
	    !remap_ops ||
	    !remap_ops->reenable)
		return 0;

	return remap_ops->reenable(mode);
}

int __init irq_remap_enable_fault_handling(void)
{
	if (!irq_remapping_enabled)
		return 0;

	if (!remap_ops || !remap_ops->enable_faulting)
		return -ENODEV;

	return remap_ops->enable_faulting();
}

void panic_if_irq_remap(const char *msg)
{
	if (irq_remapping_enabled)
		panic(msg);
}

void ir_ack_apic_edge(struct irq_data *data)
{
	ack_APIC_irq();
}

void irq_remapping_print_chip(struct irq_data *data, struct seq_file *p)
{
	/*
	 * Assume interrupt is remapped if the parent irqdomain isn't the
	 * vector domain, which is true for MSI, HPET and IOAPIC on x86
	 * platforms.
	 */
	if (data->domain && data->domain->parent != arch_get_ir_parent_domain())
		seq_printf(p, " IR-%s", data->chip->name);
	else
		seq_printf(p, " %s", data->chip->name);
}

/**
 * irq_remapping_get_ir_irq_domain - Get the irqdomain associated with the IOMMU
 *				     device serving request @info
 * @info: interrupt allocation information, used to identify the IOMMU device
 *
 * It's used to get parent irqdomain for HPET and IOAPIC irqdomains.
 * Returns pointer to IRQ domain, or NULL on failure.
 */
struct irq_domain *
irq_remapping_get_ir_irq_domain(struct irq_alloc_info *info)
{
	if (!remap_ops || !remap_ops->get_ir_irq_domain)
		return NULL;

	return remap_ops->get_ir_irq_domain(info);
}

/**
 * irq_remapping_get_irq_domain - Get the irqdomain serving the request @info
 * @info: interrupt allocation information, used to identify the IOMMU device
 *
 * There will be one PCI MSI/MSIX irqdomain associated with each interrupt
 * remapping device, so this interface is used to retrieve the PCI MSI/MSIX
 * irqdomain serving request @info.
 * Returns pointer to IRQ domain, or NULL on failure.
 */
struct irq_domain *
irq_remapping_get_irq_domain(struct irq_alloc_info *info)
{
	if (!remap_ops || !remap_ops->get_irq_domain)
		return NULL;

	return remap_ops->get_irq_domain(info);
}
