#define _GNU_SOURCE
#include "main.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <malloc.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>

struct sk_buff;
#define SMP_CACHE_BYTES 64
#define cache_line_size() SMP_CACHE_BYTES
#define ____cacheline_aligned_in_smp __attribute__ ((aligned (SMP_CACHE_BYTES)))
#define unlikely(x)    (__builtin_expect(!!(x), 0))
#define ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))
typedef pthread_spinlock_t  spinlock_t;

typedef int gfp_t;
static void *kzalloc(unsigned size, gfp_t gfp)
{
	void *p = memalign(64, size);
	if (!p)
		return p;
	memset(p, 0, size);

	return p;
}

static void kfree(void *p)
{
	if (p)
		free(p);
}

static void spin_lock_init(spinlock_t *lock)
{
	int r = pthread_spin_init(lock, 0);
	assert(!r);
}

static void spin_lock_bh(spinlock_t *lock)
{
	int ret = pthread_spin_lock(lock);
	assert(!ret);
}

static void spin_unlock_bh(spinlock_t *lock)
{
	int ret = pthread_spin_unlock(lock);
	assert(!ret);
}

#include "../../../include/linux/skb_array.h"

static unsigned long long headcnt, tailcnt;
static struct skb_array array ____cacheline_aligned_in_smp;

/* implemented by ring */
void alloc_ring(void)
{
	int ret = skb_array_init(&array, ring_size, 0);
	assert(!ret);
}

/* guest side */
int add_inbuf(unsigned len, void *buf, void *datap)
{
	int ret;
	
	spin_lock_bh(&array.producer_lock);
	assert(headcnt - tailcnt <= ring_size);
	ret = __skb_array_produce(&array, buf);
	if (ret >= 0) {
		ret = 0;
		headcnt++;
	}
	spin_unlock_bh(&array.producer_lock);

	return ret;
}

/*
 * skb_array API provides no way for producer to find out whether a given
 * buffer was consumed.  Our tests merely require that a successful get_buf
 * implies that add_inbuf succeed in the past, and that add_inbuf will succeed,
 * fake it accordingly.
 */
void *get_buf(unsigned *lenp, void **bufp)
{
	void *datap;

	spin_lock_bh(&array.producer_lock);
	if (tailcnt == headcnt || __skb_array_full(&array))
		datap = NULL;
	else {
		datap = "Buffer\n";
		++tailcnt;
	}
	spin_unlock_bh(&array.producer_lock);

	return datap;
}

void poll_used(void)
{
	void *b;

	spin_lock_bh(&array.producer_lock);
	do {
		if (tailcnt == headcnt || __skb_array_full(&array)) {
			b = NULL;
			barrier();
		} else {
			b = "Buffer\n";
		}
	} while (!b);
	spin_unlock_bh(&array.producer_lock);
}

void disable_call()
{
	assert(0);
}

bool enable_call()
{
	assert(0);
}

void kick_available(void)
{
	assert(0);
}

/* host side */
void disable_kick()
{
	assert(0);
}

bool enable_kick()
{
	assert(0);
}

void poll_avail(void)
{
	void *b;

	spin_lock_bh(&array.consumer_lock);
	do {
		b = __skb_array_peek(&array);
		barrier();
	} while (!b);
	spin_unlock_bh(&array.consumer_lock);
}

bool use_buf(unsigned *lenp, void **bufp)
{
	struct sk_buff *skb;

	spin_lock_bh(&array.consumer_lock);
	skb = __skb_array_peek(&array);
	if (skb) {
		__skb_array_consume(&array);
	}
	spin_unlock_bh(&array.consumer_lock);

	return skb;
}

void call_used(void)
{
	assert(0);
}
