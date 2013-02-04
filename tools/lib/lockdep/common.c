#include <stddef.h>
#include <stdbool.h>
#include <linux/compiler.h>
#include <linux/lockdep.h>
#include <unistd.h>
#include <sys/syscall.h>

__thread struct task_struct current_obj;

bool debug_locks = true;
bool debug_locks_silent;

void liblockdep_init(void)
{
	lockdep_init();
}

void liblockdep_set_thread(void)
{
	prctl(PR_GET_NAME, current->comm);
	current->pid = syscall(__NR_gettid);
}
