#include <liblockdep/rwlock.h>

void main(void)
{
	pthread_rwlock_t a, b;

	liblockdep_init();
	liblockdep_set_thread();

	pthread_rwlock_init(&a, NULL);
	pthread_rwlock_init(&b, NULL);

	pthread_rwlock_wrlock(&a);
	pthread_rwlock_rdlock(&b);
	pthread_rwlock_wrlock(&a);
}
