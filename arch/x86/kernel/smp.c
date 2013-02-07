/*
 *	Intel SMP support routines.
 *
 *	(c) 1995 Alan Cox, Building #3 <alan@lxorguk.ukuu.org.uk>
 *	(c) 1998-99, 2000, 2009 Ingo Molnar <mingo@redhat.com>
 *      (c) 2002,2003 Andi Kleen, SuSE Labs.
 *
 *	i386 and x86_64 integration by Glauber Costa <gcosta@redhat.com>
 *
 *	This code is released under the GNU General Public License version 2 or
 *	later.
 */

#include <linux/init.h>

#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/kernel_stat.h>
#include <linux/mc146818rtc.h>
#include <linux/cache.h>
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/gfp.h>
#include <linux/hash.h>

#include <asm/mtrr.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include <asm/proto.h>
#include <asm/apic.h>
#include <asm/nmi.h>
/*
 *	Some notes on x86 processor bugs affecting SMP operation:
 *
 *	Pentium, Pentium Pro, II, III (and all CPUs) have bugs.
 *	The Linux implications for SMP are handled as follows:
 *
 *	Pentium III / [Xeon]
 *		None of the E1AP-E3AP errata are visible to the user.
 *
 *	E1AP.	see PII A1AP
 *	E2AP.	see PII A2AP
 *	E3AP.	see PII A3AP
 *
 *	Pentium II / [Xeon]
 *		None of the A1AP-A3AP errata are visible to the user.
 *
 *	A1AP.	see PPro 1AP
 *	A2AP.	see PPro 2AP
 *	A3AP.	see PPro 7AP
 *
 *	Pentium Pro
 *		None of 1AP-9AP errata are visible to the normal user,
 *	except occasional delivery of 'spurious interrupt' as trap #15.
 *	This is very rare and a non-problem.
 *
 *	1AP.	Linux maps APIC as non-cacheable
 *	2AP.	worked around in hardware
 *	3AP.	fixed in C0 and above steppings microcode update.
 *		Linux does not use excessive STARTUP_IPIs.
 *	4AP.	worked around in hardware
 *	5AP.	symmetric IO mode (normal Linux operation) not affected.
 *		'noapic' mode has vector 0xf filled out properly.
 *	6AP.	'noapic' mode might be affected - fixed in later steppings
 *	7AP.	We do not assume writes to the LVT deassering IRQs
 *	8AP.	We do not enable low power mode (deep sleep) during MP bootup
 *	9AP.	We do not use mixed mode
 *
 *	Pentium
 *		There is a marginal case where REP MOVS on 100MHz SMP
 *	machines with B stepping processors can fail. XXX should provide
 *	an L1cache=Writethrough or L1cache=off option.
 *
 *		B stepping CPUs may hang. There are hardware work arounds
 *	for this. We warn about it in case your board doesn't have the work
 *	arounds. Basically that's so I can tell anyone with a B stepping
 *	CPU and SMP problems "tough".
 *
 *	Specific items [From Pentium Processor Specification Update]
 *
 *	1AP.	Linux doesn't use remote read
 *	2AP.	Linux doesn't trust APIC errors
 *	3AP.	We work around this
 *	4AP.	Linux never generated 3 interrupts of the same priority
 *		to cause a lost local interrupt.
 *	5AP.	Remote read is never used
 *	6AP.	not affected - worked around in hardware
 *	7AP.	not affected - worked around in hardware
 *	8AP.	worked around in hardware - we get explicit CS errors if not
 *	9AP.	only 'noapic' mode affected. Might generate spurious
 *		interrupts, we log only the first one and count the
 *		rest silently.
 *	10AP.	not affected - worked around in hardware
 *	11AP.	Linux reads the APIC between writes to avoid this, as per
 *		the documentation. Make sure you preserve this as it affects
 *		the C stepping chips too.
 *	12AP.	not affected - worked around in hardware
 *	13AP.	not affected - worked around in hardware
 *	14AP.	we always deassert INIT during bootup
 *	15AP.	not affected - worked around in hardware
 *	16AP.	not affected - worked around in hardware
 *	17AP.	not affected - worked around in hardware
 *	18AP.	not affected - worked around in hardware
 *	19AP.	not affected - worked around in BIOS
 *
 *	If this sounds worrying believe me these bugs are either ___RARE___,
 *	or are signal timing bugs worked around in hardware and there's
 *	about nothing of note with C stepping upwards.
 */

static atomic_t stopping_cpu = ATOMIC_INIT(-1);
static bool smp_no_nmi_ipi = false;

#define DELAY_SHIFT			8
#define DELAY_FIXED_1			(1<<DELAY_SHIFT)
#define MIN_SPINLOCK_DELAY		(1 * DELAY_FIXED_1)
#define MAX_SPINLOCK_DELAY_NATIVE	(16000 * DELAY_FIXED_1)
#define MAX_SPINLOCK_DELAY_GUEST	(16 * DELAY_FIXED_1)
#define DELAY_HASH_SHIFT		6

/*
 * Modern Intel and AMD CPUs tell the hypervisor when a guest is
 * spinning excessively on a spinlock. The hypervisor will then
 * schedule something else, effectively taking care of the backoff
 * for us. Doing our own backoff on top of the hypervisor's pause
 * loop exit handling can lead to excessively long delays, and
 * performance degradations. Limit the spinlock delay in virtual
 * machines to a smaller value. Called from init_hypervisor_platform
 */
static int __read_mostly max_spinlock_delay = MAX_SPINLOCK_DELAY_NATIVE;
void __init init_guest_spinlock_delay(void)
{
	max_spinlock_delay = MAX_SPINLOCK_DELAY_GUEST;
}

struct delay_entry {
	u32 hash;
	u32 delay;
};
static DEFINE_PER_CPU(struct delay_entry [1 << DELAY_HASH_SHIFT], spinlock_delay) = {
	[0 ... (1 << DELAY_HASH_SHIFT) - 1] = {
		.hash = 0,
		.delay = MIN_SPINLOCK_DELAY,
	},
};

/*
 * Wait on a congested ticket spinlock. Many spinlocks are embedded in
 * data structures; having many CPUs pounce on the cache line with the
 * spinlock simultaneously can slow down the lock holder, and the system
 * as a whole.
 *
 * To prevent total performance collapse in case of bad spinlock contention,
 * perform proportional backoff. The per-cpu value of delay is automatically
 * tuned to limit the number of times spinning CPUs poll the lock before
 * obtaining it. This limits the amount of cross-CPU traffic required to obtain
 * a spinlock, and keeps system performance from dropping off a cliff.
 *
 * There is a tradeoff. If we poll too often, the whole system is slowed
 * down. If we sleep too long, the lock will go unused for a period of
 * time. The solution is to go for a fast spin if we are at the head of
 * the queue, to slowly increase the delay if we sleep for too short a
 * time, and to decrease the delay if we slept for too long.
 */
void ticket_spin_lock_wait(arch_spinlock_t *lock, struct __raw_tickets inc)
{
	__ticket_t head = inc.head, ticket = inc.tail;
	__ticket_t waiters_ahead;
	u32 hash = hash32_ptr(lock);
	u32 slot = hash_32(hash, DELAY_HASH_SHIFT);
	struct delay_entry *ent = &__get_cpu_var(spinlock_delay[slot]);
	u32 delay = (ent->hash == hash) ? ent->delay : MIN_SPINLOCK_DELAY;
	unsigned loops;

	for (;;) {
		waiters_ahead = ticket - head - 1;
		/*
		 * We are next after the current lock holder. Check often
		 * to avoid wasting time when the lock is released.
		 */
		if (!waiters_ahead) {
			do {
				cpu_relax();
			} while (ACCESS_ONCE(lock->tickets.head) != ticket);
			break;
		}

		/* Aggressively increase delay, to minimize lock accesses. */
		if (delay < max_spinlock_delay)
			delay += DELAY_FIXED_1 / 7;

		loops = (delay * waiters_ahead) >> DELAY_SHIFT;
		while (loops--)
			cpu_relax();

		head = ACCESS_ONCE(lock->tickets.head);
		if (head == ticket) {
			/*
			 * We overslept, and do not know by how much.
			 * Exponentially decay the value of delay,
			 * to get it back to a good value quickly.
			 */
			if (delay >= 2 * DELAY_FIXED_1)
				delay -= max_t(u32, delay/32, DELAY_FIXED_1);
			break;
		}
	}
	ent->hash = hash;
	ent->delay = delay;
}

/*
 * this function sends a 'reschedule' IPI to another CPU.
 * it goes straight through and wastes no time serializing
 * anything. Worst case is that we lose a reschedule ...
 */
static void native_smp_send_reschedule(int cpu)
{
	if (unlikely(cpu_is_offline(cpu))) {
		WARN_ON(1);
		return;
	}
	apic->send_IPI_mask(cpumask_of(cpu), RESCHEDULE_VECTOR);
}

void native_send_call_func_single_ipi(int cpu)
{
	apic->send_IPI_mask(cpumask_of(cpu), CALL_FUNCTION_SINGLE_VECTOR);
}

void native_send_call_func_ipi(const struct cpumask *mask)
{
	cpumask_var_t allbutself;

	if (!alloc_cpumask_var(&allbutself, GFP_ATOMIC)) {
		apic->send_IPI_mask(mask, CALL_FUNCTION_VECTOR);
		return;
	}

	cpumask_copy(allbutself, cpu_online_mask);
	cpumask_clear_cpu(smp_processor_id(), allbutself);

	if (cpumask_equal(mask, allbutself) &&
	    cpumask_equal(cpu_online_mask, cpu_callout_mask))
		apic->send_IPI_allbutself(CALL_FUNCTION_VECTOR);
	else
		apic->send_IPI_mask(mask, CALL_FUNCTION_VECTOR);

	free_cpumask_var(allbutself);
}

static int smp_stop_nmi_callback(unsigned int val, struct pt_regs *regs)
{
	/* We are registered on stopping cpu too, avoid spurious NMI */
	if (raw_smp_processor_id() == atomic_read(&stopping_cpu))
		return NMI_HANDLED;

	stop_this_cpu(NULL);

	return NMI_HANDLED;
}

/*
 * this function calls the 'stop' function on all other CPUs in the system.
 */

asmlinkage void smp_reboot_interrupt(void)
{
	ack_APIC_irq();
	irq_enter();
	stop_this_cpu(NULL);
	irq_exit();
}

static void native_stop_other_cpus(int wait)
{
	unsigned long flags;
	unsigned long timeout;

	if (reboot_force)
		return;

	/*
	 * Use an own vector here because smp_call_function
	 * does lots of things not suitable in a panic situation.
	 */

	/*
	 * We start by using the REBOOT_VECTOR irq.
	 * The irq is treated as a sync point to allow critical
	 * regions of code on other cpus to release their spin locks
	 * and re-enable irqs.  Jumping straight to an NMI might
	 * accidentally cause deadlocks with further shutdown/panic
	 * code.  By syncing, we give the cpus up to one second to
	 * finish their work before we force them off with the NMI.
	 */
	if (num_online_cpus() > 1) {
		/* did someone beat us here? */
		if (atomic_cmpxchg(&stopping_cpu, -1, safe_smp_processor_id()) != -1)
			return;

		/* sync above data before sending IRQ */
		wmb();

		apic->send_IPI_allbutself(REBOOT_VECTOR);

		/*
		 * Don't wait longer than a second if the caller
		 * didn't ask us to wait.
		 */
		timeout = USEC_PER_SEC;
		while (num_online_cpus() > 1 && (wait || timeout--))
			udelay(1);
	}
	
	/* if the REBOOT_VECTOR didn't work, try with the NMI */
	if ((num_online_cpus() > 1) && (!smp_no_nmi_ipi))  {
		if (register_nmi_handler(NMI_LOCAL, smp_stop_nmi_callback,
					 NMI_FLAG_FIRST, "smp_stop"))
			/* Note: we ignore failures here */
			/* Hope the REBOOT_IRQ is good enough */
			goto finish;

		/* sync above data before sending IRQ */
		wmb();

		pr_emerg("Shutting down cpus with NMI\n");

		apic->send_IPI_allbutself(NMI_VECTOR);

		/*
		 * Don't wait longer than a 10 ms if the caller
		 * didn't ask us to wait.
		 */
		timeout = USEC_PER_MSEC * 10;
		while (num_online_cpus() > 1 && (wait || timeout--))
			udelay(1);
	}

finish:
	local_irq_save(flags);
	disable_local_APIC();
	local_irq_restore(flags);
}

/*
 * Reschedule call back.
 */
void smp_reschedule_interrupt(struct pt_regs *regs)
{
	ack_APIC_irq();
	inc_irq_stat(irq_resched_count);
	scheduler_ipi();
	/*
	 * KVM uses this interrupt to force a cpu out of guest mode
	 */
}

void smp_call_function_interrupt(struct pt_regs *regs)
{
	ack_APIC_irq();
	irq_enter();
	generic_smp_call_function_interrupt();
	inc_irq_stat(irq_call_count);
	irq_exit();
}

void smp_call_function_single_interrupt(struct pt_regs *regs)
{
	ack_APIC_irq();
	irq_enter();
	generic_smp_call_function_single_interrupt();
	inc_irq_stat(irq_call_count);
	irq_exit();
}

static int __init nonmi_ipi_setup(char *str)
{
	smp_no_nmi_ipi = true;
	return 1;
}

__setup("nonmi_ipi", nonmi_ipi_setup);

struct smp_ops smp_ops = {
	.smp_prepare_boot_cpu	= native_smp_prepare_boot_cpu,
	.smp_prepare_cpus	= native_smp_prepare_cpus,
	.smp_cpus_done		= native_smp_cpus_done,

	.stop_other_cpus	= native_stop_other_cpus,
	.smp_send_reschedule	= native_smp_send_reschedule,

	.cpu_up			= native_cpu_up,
	.cpu_die		= native_cpu_die,
	.cpu_disable		= native_cpu_disable,
	.play_dead		= native_play_dead,

	.send_call_func_ipi	= native_send_call_func_ipi,
	.send_call_func_single_ipi = native_send_call_func_single_ipi,
};
EXPORT_SYMBOL_GPL(smp_ops);
