#ifdef __USE_LIBLOCKDEP

#include <liblockdep/mutex.h>

#else

#define LIBLOCKDEP_PTHREAD_MUTEX_INITIALIZER(mtx) PTHREAD_MUTEX_INITIALIZER
#define liblockdep_init()
#define liblockdep_set_thread()

#endif
