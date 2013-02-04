#include <liblockdep/mutex.h>
#include "common.h"

void main(void)
{
	liblockdep_pthread_mutex_t a, b, c, d;

	liblockdep_init();
	liblockdep_set_thread();

	liblockdep_pthread_mutex_init(&a, NULL);
	liblockdep_pthread_mutex_init(&b, NULL);
	liblockdep_pthread_mutex_init(&c, NULL);
	liblockdep_pthread_mutex_init(&d, NULL);

	LOCK_UNLOCK_2(a, b);
	LOCK_UNLOCK_2(c, d);
	LOCK_UNLOCK_2(b, c);
	LOCK_UNLOCK_2(d, a);
}
