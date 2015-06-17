#ifndef PERF_LINUX_RCUPDATE_H_
#define PERF_LINUX_RCUPDATE_H_

/* Simple trivial wrappers for now, we don't use RCU in perf user-space (yet): */
#define WRITE_ONCE(var, val)			((var) = (val))
#define rcu_assign_pointer(ptr, val)		WRITE_ONCE(ptr, val)

#endif

