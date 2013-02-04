#include <liblockdep/mutex.h>

void main(void)
{
	pthread_mutex_t a, b;

	liblockdep_init();
	liblockdep_set_thread();

	pthread_mutex_init(&a, NULL);
	pthread_mutex_init(&b, NULL);

	pthread_mutex_lock(&a);
	pthread_mutex_unlock(&b);
	pthread_mutex_lock(&a);
}
