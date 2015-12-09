#define _GNU_SOURCE
#include <time.h>
#include <getopt.h>
#include <string.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdbool.h>
#include <linux/virtio.h>
#include <linux/virtio_ring.h>

/* Unused */
void *__kmalloc_fake, *__kfree_ignore_start, *__kfree_ignore_end;

static struct vring vring;
static uint16_t last_avail_idx;
static unsigned int returned;

static bool vq_notify(struct virtqueue *vq)
{
	/* "Use" them all. */
	if (vq->vdev->features & (1ULL << VIRTIO_RING_F_POLL)) {
		for (;;) {
			unsigned int i, head;

			i = last_avail_idx & (vring.num - 1);
			head = vring.avail->ring[i];
			if ((head ^ last_avail_idx ^ 0x8000) & ~(vring.num - 1))
				break;

			last_avail_idx++;
			i = vring.used->idx & (vring.num - 1);
			vring.used->ring[i].id = head;
			vring.used->ring[i].len = 0;
			vring.used->idx++;
		}
	} else {
		while (vring.avail->idx != last_avail_idx) {
			unsigned int i, head;

			i = last_avail_idx++ & (vring.num - 1);
			head = vring.avail->ring[i];
			assert(head < vring.num);

			i = vring.used->idx & (vring.num - 1);
			vring.used->ring[i].id = head;
			vring.used->ring[i].len = 0;
			vring.used->idx++;
		}
	}


	return true;
}

static void vq_callback(struct virtqueue *vq)
{
	unsigned int len;
	void *p;

	while ((p = virtqueue_get_buf(vq, &len)) != NULL)
		returned++;
}

/* Ring size 128, just like qemu uses */
#define VRING_NUM 128
#define SG_SIZE 16

static inline struct timespec time_sub(struct timespec recent,
				       struct timespec old)
{
	struct timespec diff;

	diff.tv_sec = recent.tv_sec - old.tv_sec;
	if (old.tv_nsec > recent.tv_nsec) {
		diff.tv_sec--;
		diff.tv_nsec = 1000000000 + recent.tv_nsec - old.tv_nsec;
	} else
		diff.tv_nsec = recent.tv_nsec - old.tv_nsec;

	return diff;
}

static struct timespec time_now(void)
{
	struct timespec ret;
	clock_gettime(CLOCK_REALTIME, &ret);
	return ret;
}

static inline uint64_t time_to_nsec(struct timespec t)
{
	uint64_t nsec;

	nsec = t.tv_nsec + (uint64_t)t.tv_sec * 1000000000;
	return nsec;
}

int main(int argc, char *argv[])
{
	struct virtqueue *vq;
	struct virtio_device vdev;
	void *ring;
	unsigned int i, num;
	int e;
	struct scatterlist sg[SG_SIZE];
	struct timespec start;

	sg_init_table(sg, SG_SIZE);

	ring = NULL; /* shut up compiler used uninititialized warning */
	e = posix_memalign(&ring, 4096, vring_size(VRING_NUM, 4096));
	assert(e >= 0);
	assert(ring);

	vdev.features = (1ULL << VIRTIO_RING_F_INDIRECT_DESC) |
		(1ULL << VIRTIO_RING_F_EVENT_IDX) |
		(1ULL << VIRTIO_RING_F_POLL);

	vq = vring_new_virtqueue(0, VRING_NUM, 4096, &vdev, true, ring,
				 vq_notify, vq_callback, "benchmark");
	assert(vq);
	vring_init(&vring, VRING_NUM, ring, 4096);

	num = atoi(argv[1] ?: "10000000");

	start = time_now();
	for (i = 0; i < num; i++) {
	again:
		e = virtqueue_add_outbuf(vq, sg, SG_SIZE, sg, GFP_ATOMIC);
		if (e < 0) {
			virtqueue_kick(vq);
			vring_interrupt(0, vq);
			goto again;
		}
	}
	printf("%lluns\n",
	       (long long)time_to_nsec(time_sub(time_now(), start)));
	printf("%u returned\n", returned);
	return 0;
}
