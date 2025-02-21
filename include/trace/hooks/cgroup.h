/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cgroup
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_CGROUP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_CGROUP_H
#include <trace/hooks/vendor_hooks.h>

struct task_struct;
struct cgroup_taskset;
struct cgroup_subsys;
struct cgroup_subsys_state;
DECLARE_HOOK(android_vh_cgroup_set_task,
	TP_PROTO(int ret, struct cgroup *cgrp, struct task_struct *task, bool threadgroup),
	TP_ARGS(ret, cgrp, task, threadgroup));

DECLARE_HOOK(android_vh_cgroup_attach,
	TP_PROTO(struct cgroup_subsys *ss, struct cgroup_taskset *tset),
	TP_ARGS(ss, tset));

DECLARE_RESTRICTED_HOOK(android_rvh_cpu_cgroup_attach,
	TP_PROTO(struct cgroup_taskset *tset),
	TP_ARGS(tset), 1);

DECLARE_RESTRICTED_HOOK(android_rvh_cpu_cgroup_online,
	TP_PROTO(struct cgroup_subsys_state *css),
	TP_ARGS(css), 1);
#endif

#include <trace/define_trace.h>
