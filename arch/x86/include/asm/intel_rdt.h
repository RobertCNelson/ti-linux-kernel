#ifndef _RDT_H_
#define _RDT_H_

#ifdef CONFIG_INTEL_RDT

#include <linux/cgroup.h>
#include <linux/jump_label.h>

#define MAX_CBM_LENGTH			32
#define IA32_L3_CBM_BASE		0xc90
#define CBM_FROM_INDEX(x)		(IA32_L3_CBM_BASE + x)

extern struct static_key rdt_enable_key;
void __intel_rdt_sched_in(void *dummy);

struct intel_rdt {
	struct cgroup_subsys_state css;
	u32 closid;
};

struct clos_cbm_table {
	unsigned long l3_cbm;
	unsigned int clos_refcnt;
};

/*
 * Return rdt group corresponding to this container.
 */
static inline struct intel_rdt *css_rdt(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct intel_rdt, css) : NULL;
}

static inline struct intel_rdt *parent_rdt(struct intel_rdt *ir)
{
	return css_rdt(ir->css.parent);
}

/*
 * Return rdt group to which this task belongs.
 */
static inline struct intel_rdt *task_rdt(struct task_struct *task)
{
	return css_rdt(task_css(task, intel_rdt_cgrp_id));
}

/*
 * intel_rdt_sched_in() - Writes the task's CLOSid to IA32_PQR_MSR
 *
 * Following considerations are made so that this has minimal impact
 * on scheduler hot path:
 * - This will stay as no-op unless we are running on an Intel SKU
 * which supports L3 cache allocation.
 * - When support is present and enabled, does not do any
 * IA32_PQR_MSR writes until the user starts really using the feature
 * ie creates a rdt cgroup directory and assigns a cache_mask thats
 * different from the root cgroup's cache_mask.
 * - Caches the per cpu CLOSid values and does the MSR write only
 * when a task with a different CLOSid is scheduled in. That
 * means the task belongs to a different cgroup.
 * - Closids are allocated so that different cgroup directories
 * with same cache_mask gets the same CLOSid. This minimizes CLOSids
 * used and reduces MSR write frequency.
 */
static inline void intel_rdt_sched_in(void)
{
	/*
	 * Call the schedule in code only when RDT is enabled.
	 */
	if (static_key_false(&rdt_enable_key))
		__intel_rdt_sched_in(NULL);
}

#else

static inline void intel_rdt_sched_in(void) {}

#endif
#endif
