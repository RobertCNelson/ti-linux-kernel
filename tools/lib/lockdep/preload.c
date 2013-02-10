#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include "include/liblockdep/mutex.h"
#include "../../../include/linux/rbtree.h"

struct lock_lookup {
	void *orig;
	struct lockdep_map dep_map;
	struct rb_node node;
};

struct rb_root locks = RB_ROOT;

int (*ll_pthread_mutex_init)(pthread_mutex_t *mutex,
			const pthread_mutexattr_t *attr);
int (*ll_pthread_mutex_lock)(pthread_mutex_t *mutex);
int (*ll_pthread_mutex_trylock)(pthread_mutex_t *mutex);
int (*ll_pthread_mutex_unlock)(pthread_mutex_t *mutex);
int (*ll_pthread_mutex_destroy)(pthread_mutex_t *mutex);

int (*ll_pthread_rwlock_init)(pthread_rwlock_t *rwlock,
			const pthread_rwlockattr_t *attr);
int (*ll_pthread_rwlock_destroy)(pthread_rwlock_t *rwlock);
int (*ll_pthread_rwlock_rdlock)(pthread_rwlock_t *rwlock);
int (*ll_pthread_rwlock_tryrdlock)(pthread_rwlock_t *rwlock);
int (*ll_pthread_rwlock_trywrlock)(pthread_rwlock_t *rwlock);
int (*ll_pthread_rwlock_wrlock)(pthread_rwlock_t *rwlock);
int (*ll_pthread_rwlock_unlock)(pthread_rwlock_t *rwlock);

static void init_preload();

static struct lock_lookup *__get_lock(void *lock)
{
	struct rb_node **node = &locks.rb_node, *parent = NULL;
	struct lock_lookup *l;

	while (*node) {
		l = rb_entry(*node, struct lock_lookup, node);

		parent = *node;
		if (lock < l->orig)
			node = &l->node.rb_left;
		else if (lock > l->orig)
			node = &l->node.rb_right;
		else
			return l;
	}

	l = malloc(sizeof(*l));
	if (l == NULL)
		return NULL;

	*l = (struct lock_lookup) {
		.orig = lock,
		.dep_map = STATIC_LOCKDEP_MAP_INIT("lock", &l->dep_map),
	};

	rb_link_node(&l->node, parent, node);
	rb_insert_color(&l->node, &locks);

	return l;
}

int pthread_mutex_init(pthread_mutex_t *mutex,
			const pthread_mutexattr_t *attr)
{
	init_preload();

	__get_lock(mutex);
	return ll_pthread_mutex_init(mutex, attr);
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	void *ip = _THIS_IP_;

        init_preload();

	lock_acquire(&__get_lock(mutex)->dep_map, 0, 0, 0, 2, NULL, (unsigned long)ip);
	return ll_pthread_mutex_lock(mutex);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	void *ip = _THIS_IP_;

        init_preload();

	lock_acquire(&__get_lock(mutex)->dep_map, 0, 1, 0, 2, NULL, (unsigned long)ip);
	return ll_pthread_mutex_trylock(mutex);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	void *ip = _THIS_IP_;

        init_preload();

	lock_release(&__get_lock(mutex)->dep_map, 0, (unsigned long)ip);
	return ll_pthread_mutex_unlock(mutex);
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	struct lock_lookup *l = __get_lock(mutex);

        init_preload();

	rb_erase(&l->node, &locks);
	free(l);
	return ll_pthread_mutex_destroy(mutex);
}

int pthread_rwlock_init(pthread_rwlock_t *rwlock,
			const pthread_rwlockattr_t *attr)
{
        init_preload();

	__get_lock(rwlock);
	return ll_pthread_rwlock_init(rwlock, attr);
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
	struct lock_lookup *l = __get_lock(rwlock);

        init_preload();

	rb_erase(&l->node, &locks);
	free(l);
	return ll_pthread_rwlock_destroy(rwlock);
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
	void *ip = _THIS_IP_;

        init_preload();

	lock_acquire(&__get_lock(rwlock)->dep_map, 0, 0, 2, 2, NULL, (unsigned long)ip);
	return ll_pthread_rwlock_rdlock(rwlock);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
	void *ip = _THIS_IP_;

        init_preload();

	lock_acquire(&__get_lock(rwlock)->dep_map, 0, 1, 2, 2, NULL, (unsigned long)ip);
	return ll_pthread_rwlock_tryrdlock(rwlock);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
	void *ip = _THIS_IP_;

        init_preload();

	lock_acquire(&__get_lock(rwlock)->dep_map, 0, 1, 0, 2, NULL, (unsigned long)ip);
	return ll_pthread_rwlock_trywrlock(rwlock);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
	void *ip = _THIS_IP_;

        init_preload();

	lock_acquire(&__get_lock(rwlock)->dep_map, 0, 0, 0, 2, NULL, (unsigned long)ip);
	return ll_pthread_rwlock_wrlock(rwlock);
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
	void *ip = _THIS_IP_;

        init_preload();

	lock_release(&__get_lock(rwlock)->dep_map, 0, (unsigned long)ip);
	return ll_pthread_rwlock_unlock(rwlock);
}

__attribute__((constructor)) static void init_preload(void)
{
	static bool preload_done;

	if (preload_done)
		return;

	ll_pthread_mutex_init = dlsym(RTLD_NEXT, "pthread_mutex_init");
	ll_pthread_mutex_lock = dlsym(RTLD_NEXT, "pthread_mutex_lock");
	ll_pthread_mutex_trylock = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
	ll_pthread_mutex_unlock = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
	ll_pthread_mutex_destroy = dlsym(RTLD_NEXT, "pthread_mutex_destroy");

	ll_pthread_rwlock_init = dlsym(RTLD_NEXT, "pthread_rwlock_init");
	ll_pthread_rwlock_destroy = dlsym(RTLD_NEXT, "pthread_rwlock_destroy");
	ll_pthread_rwlock_rdlock = dlsym(RTLD_NEXT, "pthread_rwlock_rdlock");
	ll_pthread_rwlock_tryrdlock = dlsym(RTLD_NEXT, "pthread_rwlock_tryrdlock");
	ll_pthread_rwlock_wrlock = dlsym(RTLD_NEXT, "pthread_rwlock_wrlock");
	ll_pthread_rwlock_trywrlock = dlsym(RTLD_NEXT, "pthread_rwlock_trywrlock");
	ll_pthread_rwlock_unlock = dlsym(RTLD_NEXT, "pthread_rwlock_unlock");

	preload_done = true;
}
