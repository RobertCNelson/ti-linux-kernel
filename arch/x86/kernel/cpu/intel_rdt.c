/*
 * Resource Director Technology(RDT)
 * - Cache Allocation code.
 *
 * Copyright (C) 2014 Intel Corporation
 *
 * 2015-05-25 Written by
 *    Vikas Shivappa <vikas.shivappa@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * More information about RDT be found in the Intel (R) x86 Architecture
 * Software Developer Manual June 2015, volume 3, section 17.15.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <asm/pqr_common.h>
#include <asm/intel_rdt.h>

/*
 * cctable maintains 1:1 mapping between CLOSid and cache bitmask.
 */
static struct clos_cbm_table *cctable;
/*
 * closid availability bit map.
 */
unsigned long *closmap;
/*
 * Minimum bits required in Cache bitmask.
 */
static unsigned int min_bitmask_len = 1;
/*
 * Mask of CPUs for writing CBM values. We only need one CPU per-socket.
 */
static cpumask_t rdt_cpumask;
/*
 * Temporary cpumask used during hot cpu notificaiton handling. The usage
 * is serialized by hot cpu locks.
 */
static cpumask_t tmp_cpumask;
static DEFINE_MUTEX(rdt_group_mutex);
struct static_key __read_mostly rdt_enable_key = STATIC_KEY_INIT_FALSE;

static struct intel_rdt rdt_root_group;
#define rdt_for_each_child(pos_css, parent_ir)		\
	css_for_each_child((pos_css), &(parent_ir)->css)

struct rdt_remote_data {
	int msr;
	u64 val;
};

static DEFINE_SPINLOCK(closid_lock);

/*
 * cache_alloc_hsw_probe() - Have to probe for Intel haswell server CPUs
 * as it does not have CPUID enumeration support for Cache allocation.
 *
 * Probes by writing to the high 32 bits(CLOSid) of the IA32_PQR_MSR and
 * testing if the bits stick. Max CLOSids is always 4 and max cbm length
 * is always 20 on hsw server parts. The minimum cache bitmask length
 * allowed for HSW server is always 2 bits. Hardcode all of them.
 */
static inline bool cache_alloc_hsw_probe(void)
{
	u32 l, h_old, h_new, h_tmp;

	if (rdmsr_safe(MSR_IA32_PQR_ASSOC, &l, &h_old))
		return false;

	/*
	 * Default value is always 0 if feature is present.
	 */
	h_tmp = h_old ^ 0x1U;
	if (wrmsr_safe(MSR_IA32_PQR_ASSOC, l, h_tmp) ||
	    rdmsr_safe(MSR_IA32_PQR_ASSOC, &l, &h_new))
		return false;

	if (h_tmp != h_new)
		return false;

	wrmsr_safe(MSR_IA32_PQR_ASSOC, l, h_old);

	boot_cpu_data.x86_cache_max_closid = 4;
	boot_cpu_data.x86_cache_max_cbm_len = 20;
	min_bitmask_len = 2;

	return true;
}

static inline bool cache_alloc_supported(struct cpuinfo_x86 *c)
{
	if (cpu_has(c, X86_FEATURE_CAT_L3))
		return true;

	/*
	 * Probe for Haswell server CPUs.
	 */
	if (c->x86 == 0x6 && c->x86_model == 0x3f)
		return cache_alloc_hsw_probe();

	return false;
}

void __intel_rdt_sched_in(void *dummy)
{
	struct intel_pqr_state *state = this_cpu_ptr(&pqr_state);
	struct intel_rdt *ir = task_rdt(current);

	if (ir->closid == state->closid)
		return;

	spin_lock(&closid_lock);
	wrmsr(MSR_IA32_PQR_ASSOC, state->rmid, ir->closid);
	spin_unlock(&closid_lock);
	state->closid = ir->closid;
}

/*
 * Synchronize the IA32_PQR_ASSOC MSR of all currently running tasks.
 */
static inline void closid_tasks_sync(void)
{
	on_each_cpu_mask(cpu_online_mask, __intel_rdt_sched_in, NULL, 1);
}

static inline void closid_get(u32 closid)
{
	struct clos_cbm_table *cct = &cctable[closid];

	lockdep_assert_held(&rdt_group_mutex);

	cct->clos_refcnt++;
}

static int closid_alloc(u32 *closid)
{
	u32 maxid;
	u32 id;

	lockdep_assert_held(&rdt_group_mutex);

	maxid = boot_cpu_data.x86_cache_max_closid;
	id = find_first_zero_bit(closmap, maxid);
	if (id == maxid)
		return -ENOSPC;

	set_bit(id, closmap);
	closid_get(id);
	*closid = id;

	return 0;
}

static inline void closid_free(u32 closid)
{
	clear_bit(closid, closmap);
	cctable[closid].l3_cbm = 0;
}

static void closid_put(u32 closid)
{
	struct clos_cbm_table *cct = &cctable[closid];

	lockdep_assert_held(&rdt_group_mutex);
	if (WARN_ON(!cct->clos_refcnt))
		return;

	if (!--cct->clos_refcnt)
		closid_free(closid);
}

static bool cbm_validate(unsigned long var)
{
	u32 max_cbm_len = boot_cpu_data.x86_cache_max_cbm_len;
	unsigned long first_bit, zero_bit;
	u64 max_cbm;

	if (bitmap_weight(&var, max_cbm_len) < min_bitmask_len)
		return false;

	max_cbm = (1ULL << max_cbm_len) - 1;
	if (var & ~max_cbm)
		return false;

	first_bit = find_first_bit(&var, max_cbm_len);
	zero_bit = find_next_zero_bit(&var, max_cbm_len, first_bit);

	if (find_next_bit(&var, max_cbm_len, zero_bit) < max_cbm_len)
		return false;

	return true;
}

static int clos_cbm_table_read(u32 closid, unsigned long *l3_cbm)
{
	u32 maxid = boot_cpu_data.x86_cache_max_closid;

	lockdep_assert_held(&rdt_group_mutex);

	if (closid >= maxid)
		return -EINVAL;

	*l3_cbm = cctable[closid].l3_cbm;

	return 0;
}

/*
 * clos_cbm_table_update() - Update a clos cbm table entry.
 * @closid: the closid whose cbm needs to be updated
 * @cbm: the new cbm value that has to be updated
 *
 * This assumes the cbm is validated as per the interface requirements
 * and the cache allocation requirements(through the cbm_validate).
 */
static int clos_cbm_table_update(u32 closid, unsigned long cbm)
{
	u32 maxid = boot_cpu_data.x86_cache_max_closid;

	lockdep_assert_held(&rdt_group_mutex);

	if (closid >= maxid)
		return -EINVAL;

	cctable[closid].l3_cbm = cbm;

	return 0;
}

static bool cbm_search(unsigned long cbm, u32 *closid)
{
	u32 maxid = boot_cpu_data.x86_cache_max_closid;
	u32 i;

	for (i = 0; i < maxid; i++) {
		if (cctable[i].clos_refcnt &&
		    bitmap_equal(&cbm, &cctable[i].l3_cbm, MAX_CBM_LENGTH)) {
			*closid = i;
			return true;
		}
	}

	return false;
}

static void closcbm_map_dump(void)
{
	u32 i;

	pr_debug("CBMMAP\n");
	for (i = 0; i < boot_cpu_data.x86_cache_max_closid; i++) {
		pr_debug("l3_cbm: 0x%x,clos_refcnt: %u\n",
		 (unsigned int)cctable[i].l3_cbm, cctable[i].clos_refcnt);
	}
}

static void msr_cpu_update(void *arg)
{
	struct rdt_remote_data *info = arg;

	wrmsrl(info->msr, info->val);
}

/*
 * msr_update_all() - Update the msr for all packages.
 */
static inline void msr_update_all(int msr, u64 val)
{
	struct rdt_remote_data info;

	info.msr = msr;
	info.val = val;
	on_each_cpu_mask(&rdt_cpumask, msr_cpu_update, &info, 1);
}

static inline bool rdt_cpumask_update(int cpu)
{
	cpumask_and(&tmp_cpumask, &rdt_cpumask, topology_core_cpumask(cpu));
	if (cpumask_empty(&tmp_cpumask)) {
		cpumask_set_cpu(cpu, &rdt_cpumask);
		return true;
	}

	return false;
}

/*
 * cbm_update_msrs() - Updates all the existing IA32_L3_MASK_n MSRs
 * which are one per CLOSid on the current package.
 */
static void cbm_update_msrs(void *dummy)
{
	int maxid = boot_cpu_data.x86_cache_max_closid;
	struct rdt_remote_data info;
	unsigned int i;

	for (i = 0; i < maxid; i++) {
		if (cctable[i].clos_refcnt) {
			info.msr = CBM_FROM_INDEX(i);
			info.val = cctable[i].l3_cbm;
			msr_cpu_update(&info);
		}
	}
}

static inline void intel_rdt_cpu_start(int cpu)
{
	struct intel_pqr_state *state = &per_cpu(pqr_state, cpu);

	state->closid = 0;
	mutex_lock(&rdt_group_mutex);
	if (rdt_cpumask_update(cpu))
		smp_call_function_single(cpu, cbm_update_msrs, NULL, 1);
	mutex_unlock(&rdt_group_mutex);
}

static void intel_rdt_cpu_exit(unsigned int cpu)
{
	int i;

	mutex_lock(&rdt_group_mutex);
	if (!cpumask_test_and_clear_cpu(cpu, &rdt_cpumask)) {
		mutex_unlock(&rdt_group_mutex);
		return;
	}

	cpumask_and(&tmp_cpumask, topology_core_cpumask(cpu), cpu_online_mask);
	cpumask_clear_cpu(cpu, &tmp_cpumask);
	i = cpumask_any(&tmp_cpumask);

	if (i < nr_cpu_ids)
		cpumask_set_cpu(i, &rdt_cpumask);
	mutex_unlock(&rdt_group_mutex);
}

static int intel_rdt_cpu_notifier(struct notifier_block *nb,
				  unsigned long action, void *hcpu)
{
	unsigned int cpu  = (unsigned long)hcpu;

	switch (action) {
	case CPU_DOWN_FAILED:
	case CPU_ONLINE:
		intel_rdt_cpu_start(cpu);
		break;
	case CPU_DOWN_PREPARE:
		intel_rdt_cpu_exit(cpu);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct cgroup_subsys_state *
intel_rdt_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct intel_rdt *parent = css_rdt(parent_css);
	struct intel_rdt *ir;

	/*
	 * cgroup_init cannot handle failures gracefully.
	 * Return rdt_root_group.css instead of failure
	 * always even when Cache allocation is not supported.
	 */
	if (!parent)
		return &rdt_root_group.css;

	ir = kzalloc(sizeof(struct intel_rdt), GFP_KERNEL);
	if (!ir)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&rdt_group_mutex);
	ir->closid = parent->closid;
	closid_get(ir->closid);
	mutex_unlock(&rdt_group_mutex);

	return &ir->css;
}

static void intel_rdt_css_free(struct cgroup_subsys_state *css)
{
	struct intel_rdt *ir = css_rdt(css);

	mutex_lock(&rdt_group_mutex);
	closid_put(ir->closid);
	kfree(ir);
	mutex_unlock(&rdt_group_mutex);
}

static int intel_cache_alloc_cbm_read(struct seq_file *m, void *v)
{
	struct intel_rdt *ir = css_rdt(seq_css(m));
	unsigned long l3_cbm = 0;

	clos_cbm_table_read(ir->closid, &l3_cbm);
	seq_printf(m, "%08lx\n", l3_cbm);

	return 0;
}

static int cbm_validate_rdt_cgroup(struct intel_rdt *ir, unsigned long cbmvalue)
{
	struct cgroup_subsys_state *css;
	struct intel_rdt *par, *c;
	unsigned long cbm_tmp = 0;
	int err = 0;

	if (!cbm_validate(cbmvalue)) {
		err = -EINVAL;
		goto out_err;
	}

	par = parent_rdt(ir);
	clos_cbm_table_read(par->closid, &cbm_tmp);
	if (!bitmap_subset(&cbmvalue, &cbm_tmp, MAX_CBM_LENGTH)) {
		err = -EINVAL;
		goto out_err;
	}

	rcu_read_lock();
	rdt_for_each_child(css, ir) {
		c = css_rdt(css);
		clos_cbm_table_read(par->closid, &cbm_tmp);
		if (!bitmap_subset(&cbm_tmp, &cbmvalue, MAX_CBM_LENGTH)) {
			rcu_read_unlock();
			err = -EINVAL;
			goto out_err;
		}
	}
	rcu_read_unlock();
out_err:

	return err;
}

/*
 * intel_cache_alloc_cbm_write() - Validates and writes the
 * cache bit mask(cbm) to the IA32_L3_MASK_n
 * and also store the same in the cctable.
 *
 * CLOSids are reused for cgroups which have same bitmask.
 * This helps to use the scant CLOSids optimally. This also
 * implies that at context switch write to PQR-MSR is done
 * only when a task with a different bitmask is scheduled in.
 */
static int intel_cache_alloc_cbm_write(struct cgroup_subsys_state *css,
				 struct cftype *cft, u64 cbmvalue)
{
	struct intel_rdt *ir = css_rdt(css);
	unsigned long ccbm = 0;
	int err = 0;
	u32 closid;

	if (ir == &rdt_root_group)
		return -EPERM;

	/*
	 * Need global mutex as cbm write may allocate a closid.
	 */
	mutex_lock(&rdt_group_mutex);

	clos_cbm_table_read(ir->closid, &ccbm);
	if (cbmvalue == ccbm)
		goto out;

	err = cbm_validate_rdt_cgroup(ir, cbmvalue);
	if (err)
		goto out;

	/*
	 * Try to get a reference for a different CLOSid and release the
	 * reference to the current CLOSid.
	 * Need to put down the reference here and get it back in case we
	 * run out of closids. Otherwise we run into a problem when
	 * we could be using the last closid that could have been available.
	 */
	closid_put(ir->closid);
	if (cbm_search(cbmvalue, &closid)) {
		spin_lock(&closid_lock);
		ir->closid = closid;
		spin_unlock(&closid_lock);
		closid_get(closid);
	} else {
		err = closid_alloc(&ir->closid);
		if (err) {
			closid_get(ir->closid);
			goto out;
		}

		clos_cbm_table_update(ir->closid, cbmvalue);
		msr_update_all(CBM_FROM_INDEX(ir->closid), cbmvalue);
	}
	closid_tasks_sync();
	closcbm_map_dump();
out:
	mutex_unlock(&rdt_group_mutex);

	return err;
}

static void rdt_cgroup_init(void)
{
	int max_cbm_len = boot_cpu_data.x86_cache_max_cbm_len;
	u32 closid;

	closid_alloc(&closid);

	WARN_ON(closid != 0);

	rdt_root_group.closid = closid;
	clos_cbm_table_update(closid, (1ULL << max_cbm_len) - 1);
}

static int __init intel_rdt_late_init(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	u32 maxid, max_cbm_len;
	int err = 0, size, i;

	if (!cache_alloc_supported(c)) {
		static_branch_disable(&intel_rdt_cgrp_subsys_enabled_key);
		return -ENODEV;
	}
	maxid = c->x86_cache_max_closid;
	max_cbm_len = c->x86_cache_max_cbm_len;

	size = maxid * sizeof(struct clos_cbm_table);
	cctable = kzalloc(size, GFP_KERNEL);
	if (!cctable) {
		err = -ENOMEM;
		goto out_err;
	}

	size = BITS_TO_LONGS(maxid) * sizeof(long);
	closmap = kzalloc(size, GFP_KERNEL);
	if (!closmap) {
		kfree(cctable);
		err = -ENOMEM;
		goto out_err;
	}

	cpu_notifier_register_begin();

	for_each_online_cpu(i)
		rdt_cpumask_update(i);

	__hotcpu_notifier(intel_rdt_cpu_notifier, 0);

	cpu_notifier_register_done();
	rdt_cgroup_init();

	static_key_slow_inc(&rdt_enable_key);
	pr_info("Intel cache allocation enabled\n");
out_err:

	return err;
}

late_initcall(intel_rdt_late_init);

static struct cftype rdt_files[] = {
	{
		.name		= "l3_cbm",
		.seq_show	= intel_cache_alloc_cbm_read,
		.write_u64	= intel_cache_alloc_cbm_write,
	},
	{ }	/* terminate */
};

struct cgroup_subsys intel_rdt_cgrp_subsys = {
	.css_alloc		= intel_rdt_css_alloc,
	.css_free		= intel_rdt_css_free,
	.legacy_cftypes		= rdt_files,
	.early_init		= 0,
};
