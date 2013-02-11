#include <stddef.h>
#include <stdbool.h>
#include <linux/compiler.h>
#include <linux/lockdep.h>
#include <unistd.h>
#include <sys/syscall.h>

__thread struct task_struct current_obj;

bool debug_locks = true;
bool debug_locks_silent;

__attribute__((constructor)) static void liblockdep_init(void)
{
	lockdep_init();
}

struct task_struct *__curr(void)
{
	prctl(PR_GET_NAME, current_obj.comm);
	current_obj.pid = syscall(__NR_gettid);

	return &current_obj;
}
