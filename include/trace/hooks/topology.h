/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM topology

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_TOPOLOGY_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_TOPOLOGY_H

#include <trace/hooks/vendor_hooks.h>

struct cpumask;

#if defined(CONFIG_TRACEPOINTS) && defined(CONFIG_ANDROID_VENDOR_HOOKS)

DECLARE_HOOK(android_vh_arch_set_freq_scale,
	TP_PROTO(const struct cpumask *cpus, unsigned long freq, unsigned long max,
		unsigned long *scale),
	TP_ARGS(cpus, freq, max, scale));

#else

#define trace_android_vh_arch_set_freq_scale(cpus, freq, max, scale)

#endif

DECLARE_HOOK(android_vh_use_amu_fie,
	TP_PROTO(bool *use_amu_fie),
	TP_ARGS(use_amu_fie));

#endif /* _TRACE_HOOK_TOPOLOGY_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
