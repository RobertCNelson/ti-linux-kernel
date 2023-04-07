// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <kvm/arm_hypercalls.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/pkvm.h>
#include <nvhe/pviommu.h>
#include <nvhe/pviommu-host.h>

static bool pkvm_guest_iommu_map(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

static bool pkvm_guest_iommu_unmap(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

static void pkvm_pviommu_hyp_req(u64 *exit_code)
{
	write_sysreg_el2(read_sysreg_el2(SYS_ELR) - 4, SYS_ELR);
	*exit_code = ARM_EXCEPTION_HYP_REQ;
}

static bool pkvm_guest_iommu_attach_dev(struct pkvm_hyp_vcpu *hyp_vcpu, u64 *exit_code)
{
	int ret;
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 iommu_id = smccc_get_arg2(vcpu);
	u64 sid = smccc_get_arg3(vcpu);
	u64 pasid = smccc_get_arg4(vcpu);
	u64 domain_id = smccc_get_arg5(vcpu);
	u64 pasid_bits = smccc_get_arg6(vcpu);
	struct pviommu_route route;
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	ret = pkvm_pviommu_get_route(vm, iommu_id, sid, &route);
	if (ret)
		goto out_ret;
	iommu_id = route.iommu;
	sid = route.sid;

	ret = kvm_iommu_attach_dev(iommu_id, domain_id, sid, pasid, pasid_bits);
	if (ret == -ENOMEM) {
		/*
		 * The driver will request memory when returning -ENOMEM, so go back to host to
		 * fulfill the request and repeat the HVC.
		 */
		pkvm_pviommu_hyp_req(exit_code);
		return false;
	}

out_ret:
	smccc_set_retval(vcpu, ret ?  SMCCC_RET_INVALID_PARAMETER : SMCCC_RET_SUCCESS,
			 0, 0, 0);
	return true;
}

static bool pkvm_guest_iommu_detach_dev(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	int ret;
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 iommu_id = smccc_get_arg2(vcpu);
	u64 sid = smccc_get_arg3(vcpu);
	u64 pasid = smccc_get_arg4(vcpu);
	u64 domain_id = smccc_get_arg5(vcpu);
	struct pviommu_route route;
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	/* MBZ */
	if (smccc_get_arg6(vcpu)) {
		ret = -EINVAL;
		goto out_ret;
	}

	ret = pkvm_pviommu_get_route(vm, iommu_id, sid, &route);
	if (ret)
		goto out_ret;
	iommu_id = route.iommu;
	sid = route.sid;

	ret = kvm_iommu_detach_dev(iommu_id, domain_id, sid, pasid);

out_ret:
	smccc_set_retval(vcpu, ret ?  SMCCC_RET_INVALID_PARAMETER : SMCCC_RET_SUCCESS,
			 0, 0, 0);
	return true;
}

static bool pkvm_guest_iommu_alloc_domain(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

static bool pkvm_guest_iommu_free_domain(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

bool kvm_handle_pviommu_hvc(struct kvm_vcpu *vcpu, u64 *exit_code)
{
	u64 iommu_op = smccc_get_arg1(vcpu);
	struct pkvm_hyp_vcpu *hyp_vcpu = container_of(vcpu, struct pkvm_hyp_vcpu, vcpu);
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	/*
	 * Eagerly fill the vm iommu pool to avoid deadlocks from donation path while
	 * doing IOMMU operations.
	 */
	refill_hyp_pool(&vm->iommu_pool, &hyp_vcpu->host_vcpu->arch.iommu_mc);
	switch (iommu_op) {
	case KVM_PVIOMMU_OP_ALLOC_DOMAIN:
		return pkvm_guest_iommu_alloc_domain(hyp_vcpu);
	case KVM_PVIOMMU_OP_FREE_DOMAIN:
		return pkvm_guest_iommu_free_domain(hyp_vcpu);
	case KVM_PVIOMMU_OP_ATTACH_DEV:
		return pkvm_guest_iommu_attach_dev(hyp_vcpu, exit_code);
	case KVM_PVIOMMU_OP_DETACH_DEV:
		return pkvm_guest_iommu_detach_dev(hyp_vcpu);
	case KVM_PVIOMMU_OP_MAP_PAGES:
		return pkvm_guest_iommu_map(hyp_vcpu);
	case KVM_PVIOMMU_OP_UNMAP_PAGES:
		return pkvm_guest_iommu_unmap(hyp_vcpu);
	}

	smccc_set_retval(vcpu, SMCCC_RET_NOT_SUPPORTED, 0, 0, 0);
	return true;
}
