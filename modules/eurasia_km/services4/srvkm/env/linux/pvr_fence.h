/*************************************************************************/ /*!
@File
@Title          PowerVR Linux fence interface
@Codingstyle    LinuxKernel
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Strictly Confidential.
*/ /**************************************************************************/

#if !defined(__PVR_FENCE_H__)
#define __PVR_FENCE_H__

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0))
#include <linux/fence.h>
#else
#include <linux/dma-fence.h>
#endif

//#define PVR_FENCE_DEBUG 1

struct PVR_SYNC_KERNEL_SYNC_INFO
{
	/* Base services sync info structure */
	PVRSRV_KERNEL_SYNC_INFO	*psBase;

	/* Sync points can go away when there are deferred hardware
	 * operations still outstanding. We must not free the SYNC_INFO
	 * until the hardware is finished, so we add it to a defer list
	 * which is processed periodically ("defer-free").
	 *
	 * This is also used for "defer-free" of a timeline -- the process
	 * may destroy its timeline or terminate abnormally but the HW could
	 * still be using the sync object hanging off of the timeline.
	 *
	 * Note that the defer-free list is global, not per-timeline.
	 */
	struct list_head		sHead;
};

struct PVR_SYNC_DATA
{
	/* Every sync fence has a services sync object. This object is used
	 * by the hardware to enforce ordering -- it is attached as a source
	 * dependency to various commands.
	 */
	struct PVR_SYNC_KERNEL_SYNC_INFO	*psSyncInfo;

	/* This is purely a debug feature. Record the WOP snapshot from the
	 * timeline synchronization object when a new fence is created.
	 */
	IMG_UINT32				ui32WOPSnapshot;
};

/* A PVR_ALLOC_SYNC_DATA is used to back an allocated, but not yet created
 * and inserted into a timeline, sync data. This is required as we must
 * allocate the syncinfo to be passed down with the transfer task used to
 * implement fences in the hardware.
 */
struct PVR_ALLOC_SYNC_DATA
{
	struct PVR_SYNC_KERNEL_SYNC_INFO	*psSyncInfo;

	/* A link to the timeline is required to add a per-timeline sync
	 * to the fence transfer task.
	 */
	struct PVR_SYNC_TIMELINE			*psTimeline;
	struct file					*psFile;
};

/**
 * PVR_FENCE_CONTEXT - PVR fence context used to create and manage PVR fences
 * @sLock: protects the context and fences created on the context
 * @pcName: fence context name (used for debugging)
 * @ui64FenceCtx: fence context with which to associate fences
 * @sSeqno: sequence number to use for the next fence
 * @psFenceWq: work queue for signalled fence work
 * @sSignalWork: work item used to signal fences when fence syncs are met
 * @sListLock: protects the active and active foreign lists
 * @sSignalList: list of fences waiting to be signalled
 * @sFenceList: list of fences (used for debugging)
 * @sDeferredFreeList: list of fences that we will free when we are no longer
 * @sFenceCtxList: list of all fence context
 * holding spinlocks.  The frees get implemented when an update fence is
 * signalled or the context is freed.
 */
struct PVR_FENCE_CONTEXT
{
	spinlock_t sLock;
	const char *pName;

	/* True if a sync fence on the fence context has signaled */
	IMG_BOOL bSyncHasSignaled;

	IMG_UINT64 ui64FenceCtx;
	atomic_t sSeqno;

	struct workqueue_struct *psFenceWq;
	struct work_struct sSignalWork;

	spinlock_t sListLock;
	struct list_head sSignalList;
	struct list_head sFenceList;
	struct list_head sDeferredFreeList;
	struct list_head sFenceCtxList;

	struct kref sRef;
	struct workqueue_struct *psDestroyWq;
	struct work_struct sDestroyWork;
};

/**
 * PVR_FENCE - PVR fence that represents both native and foreign fences
 * @sBase: fence structure
 * @psFenceCtx: fence context on which this fence was created
 * @pcName: fence name (used for debugging)
 * @psFencefence: pointer to base fence structure or foreign fence
 * @psSyncData: services sync data used by hardware
 * @sFenceHead: entry on the context fence and deferred free list
 * @sSignalHead: entry on the context signal list
 * @sFenceCb: foreign fence callback to set the sync to signalled
 */
struct PVR_FENCE {
	struct dma_fence sBase;
	struct PVR_FENCE_CONTEXT *psFenceCtx;
	const char *pName;

	struct dma_fence *psFence;
	struct PVR_SYNC_DATA *psSyncData;

	struct list_head sFenceHead;
	struct list_head sSignalHead;
	struct dma_fence_cb sFenceCb;
};

/* This is the actual timeline metadata. We might keep this around after the
 * base sync driver has destroyed the pvr_sync_timeline_wrapper object.
 */
struct PVR_SYNC_TIMELINE {
	struct PVR_FENCE_CONTEXT *psFenceCtx;
	struct file *psFile;
	char name[32];
	struct PVR_COUNTING_FENCE_TIMELINE *pSWTimeline;

	/* Every timeline has a services sync object. This object must not
	 * be used by the hardware to enforce ordering -- that's what the
	 * per sync-point objects are for. This object is attached to every
	 * TQ scheduled on the timeline and is primarily useful for debugging.
	 */
	struct PVR_SYNC_KERNEL_SYNC_INFO	*psSyncInfo;
};

extern const struct dma_fence_ops pvr_fence_ops;
extern const struct dma_fence_ops pvr_fence_foreign_ops;

static inline bool is_our_fence(struct PVR_FENCE_CONTEXT *psFenceCtx,
				struct dma_fence *psFence)
{
	return (psFence->context == psFenceCtx->ui64FenceCtx);
}

static inline bool is_pvr_fence(struct dma_fence *psFence)
{
	return ((psFence->ops == &pvr_fence_ops) ||
		(psFence->ops == &pvr_fence_foreign_ops));
}

static inline struct PVR_FENCE *to_pvr_fence(struct dma_fence *psFence)
{
	if (is_pvr_fence(psFence))
		return container_of(psFence, struct PVR_FENCE, sBase);

	return NULL;
}

struct PVR_FENCE_CONTEXT *pvr_fence_context_create(const char *pcName);
void pvr_fence_context_destroy(struct PVR_FENCE_CONTEXT *psFenceCtx);

struct PVR_FENCE *pvr_fence_create(struct PVR_FENCE_CONTEXT *fctx,
				   const char *name, struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo);
struct PVR_FENCE *pvr_fence_create_from_fence(struct PVR_FENCE_CONTEXT *psFenceCtx,
					      struct dma_fence *psFence,
					      const char *pcName);
void pvr_fence_destroy(struct PVR_FENCE *psPvrFence);

PVRSRV_ERROR PVRSyncInitServices(void);
void PVRSyncCloseServices(void);

IMG_BOOL ExpandAndDeDuplicateFenceSyncs(IMG_UINT32 ui32NumSyncs,
							   IMG_HANDLE aiFenceFds[],
							   IMG_UINT32 ui32SyncPointLimit,
							   struct dma_fence *apsFence[],
							   IMG_UINT32 *pui32NumRealSyncs,
							   PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[]);

struct PVR_ALLOC_SYNC_DATA *PVRSyncAllocFDGet(int fd);

struct PVR_COUNTING_FENCE_TIMELINE *pvr_sync_get_sw_timeline(int fd);

static inline void pvr_fence_cleanup(void)
{
	/*
	 * Ensure all PVR fence contexts have been destroyed, by flushing
	 * the global workqueue.
	 * For those versions of the DDK don't use PVR fences, this isn't
	 * necessary, but should be harmless.
	 */
	flush_scheduled_work();
}

#if defined(PVR_FENCE_DEBUG)
#define PVR_FENCE_CTX_TRACE(c, fmt, ...)                                   \
	do {                                                               \
		struct PVR_FENCE_CONTEXT *__fctx = (c);                    \
		pr_err("c %llu: (PVR) " fmt, (u64) __fctx->ui64FenceCtx,  \
		       ## __VA_ARGS__);                                    \
	} while (0)
#else
#define PVR_FENCE_CTX_TRACE(c, fmt, ...)
#endif

#define PVR_FENCE_CTX_WARN(c, fmt, ...)                                    \
	do {                                                               \
		struct PVR_FENCE_CONTEXT *__fctx = (c);                    \
		pr_warn("c %llu: (PVR) " fmt, (u64) __fctx->ui64FenceCtx, \
			## __VA_ARGS__);                                   \
	} while (0)

#define PVR_FENCE_CTX_ERR(c, fmt, ...)                                     \
	do {                                                               \
		struct PVR_FENCE_CONTEXT *__fctx = (c);                    \
		pr_err("c %llu: (PVR) " fmt, (u64) __fctx->ui64FenceCtx,  \
		       ## __VA_ARGS__);                                    \
	} while (0)

#if defined(PVR_FENCE_DEBUG)
#define PVR_FENCE_TRACE(f, fmt, ...)                                       \
	FENCE_ERR(f, "(PVR) " fmt, ## __VA_ARGS__)
#else
#define PVR_FENCE_TRACE(f, fmt, ...)
#endif

#define PVR_FENCE_WARN(f, fmt, ...)                                        \
	FENCE_WARN(f, "(PVR) " fmt, ## __VA_ARGS__)

#define PVR_FENCE_ERR(f, fmt, ...)                                         \
	FENCE_ERR(f, "(PVR) " fmt, ## __VA_ARGS__)

#endif /* !defined(__PVR_FENCE_H__) */
