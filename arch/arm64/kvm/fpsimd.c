// SPDX-License-Identifier: GPL-2.0
/*
 * arch/arm64/kvm/fpsimd.c: Guest/host FPSIMD context coordination helpers
 *
 * Copyright 2018 Arm Limited
 * Author: Dave Martin <Dave.Martin@arm.com>
 */
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/kvm_host.h>
#include <asm/fpsimd.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/sysreg.h>

void kvm_vcpu_unshare_task_fp(struct kvm_vcpu *vcpu)
{
	struct task_struct *p = vcpu->arch.parent_task;
	struct user_fpsimd_state *fpsimd;

	if (!is_protected_kvm_enabled() || !p)
		return;

	fpsimd = &p->thread.uw.fpsimd_state;
	kvm_unshare_hyp(fpsimd, fpsimd + 1);
	put_task_struct(p);
}

/*
 * Called on entry to KVM_RUN unless this vcpu previously ran at least
 * once and the most recent prior KVM_RUN for this vcpu was called from
 * the same task as current (highly likely).
 *
 * This is guaranteed to execute before kvm_arch_vcpu_load_fp(vcpu),
 * such that on entering hyp the relevant parts of current are already
 * mapped.
 */
int kvm_arch_vcpu_run_map_fp(struct kvm_vcpu *vcpu)
{
	int ret;

	struct user_fpsimd_state *fpsimd = &current->thread.uw.fpsimd_state;

	kvm_vcpu_unshare_task_fp(vcpu);

	/* Make sure the host task fpsimd state is visible to hyp: */
	ret = kvm_share_hyp(fpsimd, fpsimd + 1);
	if (ret)
		return ret;

	/*
	 * We need to keep current's task_struct pinned until its data has been
	 * unshared with the hypervisor to make sure it is not re-used by the
	 * kernel and donated to someone else while already shared -- see
	 * kvm_vcpu_unshare_task_fp() for the matching put_task_struct().
	 */
	if (is_protected_kvm_enabled()) {
		get_task_struct(current);
		vcpu->arch.parent_task = current;
	}

	return 0;
}

/*
 * Prepare vcpu for saving the host's FPSIMD state and loading the guest's.
 * The actual loading is done by the FPSIMD access trap taken to hyp.
 *
 * Here, we just set the correct metadata to indicate that the FPSIMD
 * state in the cpu regs (if any) belongs to current on the host.
 */
void kvm_arch_vcpu_load_fp(struct kvm_vcpu *vcpu)
{
	BUG_ON(!current->mm);

	if (!system_supports_fpsimd())
		return;

	/*
	 * Ensure that any host FPSIMD/SVE/SME state is saved and unbound such
	 * that the host kernel is responsible for restoring this state upon
	 * return to userspace, and the hyp code doesn't need to save anything.
	 *
	 * When the host may use SME, fpsimd_save_and_flush_cpu_state() ensures
	 * that PSTATE.{SM,ZA} == {0,0}.
	 */
	fpsimd_save_and_flush_cpu_state();
	vcpu->arch.fp_state = FP_STATE_FREE;
}

/*
 * Called just before entering the guest once we are no longer preemptable
 * and interrupts are disabled. If we have managed to run anything using
 * FP while we were preemptible (such as off the back of an interrupt),
 * then neither the host nor the guest own the FP hardware (and it was the
 * responsibility of the code that used FP to save the existing state).
 */
void kvm_arch_vcpu_ctxflush_fp(struct kvm_vcpu *vcpu)
{
	if (test_thread_flag(TIF_FOREIGN_FPSTATE))
		vcpu->arch.fp_state = FP_STATE_FREE;
}

/*
 * Called just after exiting the guest. If the guest FPSIMD state
 * was loaded, update the host's context tracking data mark the CPU
 * FPSIMD regs as dirty and belonging to vcpu so that they will be
 * written back if the kernel clobbers them due to kernel-mode NEON
 * before re-entry into the guest.
 */
void kvm_arch_vcpu_ctxsync_fp(struct kvm_vcpu *vcpu)
{
	struct cpu_fp_state fp_state;

	WARN_ON_ONCE(!irqs_disabled());

	if (vcpu->arch.fp_state == FP_STATE_GUEST_OWNED) {

		/*
		 * Currently we do not support SME guests so SVCR is
		 * always 0 and we just need a variable to point to.
		 */
		fp_state.st = &vcpu->arch.ctxt.fp_regs;
		fp_state.sve_state = vcpu->arch.sve_state;
		fp_state.sve_vl = vcpu->arch.sve_max_vl;
		fp_state.sme_state = NULL;
		fp_state.svcr = &vcpu->arch.svcr;
		fp_state.fp_type = &vcpu->arch.fp_type;

		if (vcpu_has_sve(vcpu))
			fp_state.to_save = FP_STATE_SVE;
		else
			fp_state.to_save = FP_STATE_FPSIMD;

		fpsimd_bind_state_to_cpu(&fp_state);

		clear_thread_flag(TIF_FOREIGN_FPSTATE);
	}
}

/*
 * Write back the vcpu FPSIMD regs if they are dirty, and invalidate the
 * cpu FPSIMD regs so that they can't be spuriously reused if this vcpu
 * disappears and another task or vcpu appears that recycles the same
 * struct fpsimd_state.
 */
void kvm_arch_vcpu_put_fp(struct kvm_vcpu *vcpu)
{
	unsigned long flags;

	local_irq_save(flags);

	if (vcpu->arch.fp_state == FP_STATE_GUEST_OWNED) {
		/*
		 * Flush (save and invalidate) the fpsimd/sve state so that if
		 * the host tries to use fpsimd/sve, it's not using stale data
		 * from the guest.
		 *
		 * Flushing the state sets the TIF_FOREIGN_FPSTATE bit for the
		 * context unconditionally, in both nVHE and VHE. This allows
		 * the kernel to restore the fpsimd/sve state, including ZCR_EL1
		 * when needed.
		 */
		fpsimd_save_and_flush_cpu_state();
	}

	local_irq_restore(flags);
}
