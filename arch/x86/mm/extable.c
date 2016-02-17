
#include <linux/module.h>

#include <asm/uaccess.h>

static inline unsigned long
ex_fixup_addr(const struct exception_table_entry *x)
{
	return (unsigned long)&x->fixup + x->fixup;
}

int fixup_exception(struct pt_regs *regs)
{
	const struct exception_table_entry *fixup;
	unsigned long new_ip;

#ifdef CONFIG_PNPBIOS
	if (unlikely(SEGMENT_IS_PNP_CODE(regs->cs))) {
		extern u32 pnp_bios_fault_eip, pnp_bios_fault_esp;
		extern u32 pnp_bios_is_utter_crap;
		pnp_bios_is_utter_crap = 1;
		printk(KERN_CRIT "PNPBIOS fault.. attempting recovery.\n");
		__asm__ volatile(
			"movl %0, %%esp\n\t"
			"jmp *%1\n\t"
			: : "g" (pnp_bios_fault_esp), "g" (pnp_bios_fault_eip));
		panic("do_trap: can't hit this");
	}
#endif

	fixup = search_exception_tables(regs->ip);
	if (fixup) {
		new_ip = ex_fixup_addr(fixup);

		if (fixup->fixup - fixup->insn >= 0x7ffffff0 - 4) {
			/* Special hack for uaccess_err */
			current_thread_info()->uaccess_err = 1;
			new_ip -= 0x7ffffff0;
		}
		regs->ip = new_ip;
		return 1;
	}

	return 0;
}

/* Restricted version used during very early boot */
int __init early_fixup_exception(unsigned long *ip)
{
	const struct exception_table_entry *fixup;
	unsigned long new_ip;

	fixup = search_exception_tables(*ip);
	if (fixup) {
		new_ip = ex_fixup_addr(fixup);

		if (fixup->fixup - fixup->insn >= 0x7ffffff0 - 4) {
			/* uaccess handling not supported during early boot */
			return 0;
		}

		*ip = new_ip;
		return 1;
	}

	return 0;
}
