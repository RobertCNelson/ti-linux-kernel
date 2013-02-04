#ifndef _LIBLOCKDEP_LOCKDEP_H_
#define _LIBLOCKDEP_LOCKDEP_H_

#include <sys/prctl.h>
#include <sys/syscall.h>
#include <string.h>
#include <limits.h>
#include <linux/utsname.h>


#define MAX_LOCK_DEPTH 200UL

#include "../../../include/linux/lockdep.h"

struct task_struct {
	u64 curr_chain_key;
	int lockdep_depth;
	unsigned int lockdep_recursion;
	struct held_lock held_locks[MAX_LOCK_DEPTH];
	gfp_t lockdep_reclaim_gfp;
	int pid;
	char comm[17];
};

extern __thread struct task_struct current_obj;
#define current (&current_obj)

void liblockdep_init(void);
void liblockdep_set_thread(void);

#define debug_locks_off() 1
#define task_pid_nr(tsk) ((tsk)->pid)

#define KSYM_NAME_LEN 128
#define printk printf

#define KERN_ERR
#define KERN_CONT

#define list_del_rcu list_del

#define atomic_t unsigned long
#define atomic_inc(x) ((*(x))++)

static struct new_utsname *init_utsname(void)
{
	static struct new_utsname n = (struct new_utsname) {
		.release = "liblockdep",
		.version = LIBLOCKDEP_VERSION,
	};

	return &n;
}

#define print_tainted() ""
#define static_obj(x) 1

#endif
