#ifndef _LINUX_KCOV_H
#define _LINUX_KCOV_H

#include <uapi/linux/kcov.h>

struct task_struct;

#ifdef CONFIG_KCOV

void kcov_task_init(struct task_struct *t);
void kcov_task_exit(struct task_struct *t);

#else

static inline void kcov_task_init(struct task_struct *t) {}
static inline void kcov_task_exit(struct task_struct *t) {}

#endif /* CONFIG_KCOV */
#endif /* _LINUX_KCOV_H */
