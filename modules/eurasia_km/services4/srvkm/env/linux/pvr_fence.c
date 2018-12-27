/*************************************************************************/ /*!
@File
@Title          PowerVR Linux fence interface
@Codingstyle    LinuxKernel
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Strictly Confidential.
*/ /**************************************************************************/

#include "pvr_sync_common.h"
#include "pvr_fence.h"
#include "pvr_counting_timeline.h"

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/anon_inodes.h>
#include <linux/seq_file.h>
#include <linux/sync_file.h>

#include "services_headers.h"
#include "sgxutils.h"
#include "ttrace.h"
#include "mutex.h"
#include "lock.h"

//#define DEBUG_PRINT

#if defined(DEBUG_PRINT)
#define DPF(fmt, ...) PVR_DPF((PVR_DBG_BUFFERED, fmt, __VA_ARGS__))
#else
#define DPF(fmt, ...) do {} while(0)
#endif

struct sw_sync_create_fence_data {
	__u32 value;
	char name[32];
	__s32 fence;
};
#define SW_SYNC_IOC_MAGIC 'W'
#define SW_SYNC_IOC_CREATE_FENCE \
	(_IOWR(SW_SYNC_IOC_MAGIC, 0, struct sw_sync_create_fence_data))
#define SW_SYNC_IOC_INC _IOW(SW_SYNC_IOC_MAGIC, 1, __u32)

/* Gobal WQ for scheduling work */
static struct workqueue_struct *gpsWorkQueue;

/* Linux work struct for workqueue. */
static struct work_struct gsWork;

static const struct file_operations pvr_sync_fops;

/* The "defer-free" object list. Driver global. */
static LIST_HEAD(gSyncInfoFreeList);
static DEFINE_SPINLOCK(gSyncInfoFreeListLock);

/* List of timelines, used by MISR callback to find signaled fences
 * and also to kick the hardware if signalling may allow progress to be
 * made.
 */
static LIST_HEAD(gFenceCtxList);
static DEFINE_MUTEX(gFenceCtxListLock);

/* Forward declare due to cyclic dependency on gsSyncFenceAllocFOps */
struct PVR_ALLOC_SYNC_DATA *PVRSyncAllocFDGet(int fd);

/* Global data for the sync driver */
static struct {
	/* Process that initialized the sync driver. House-keep this so
	 * the correct per-proc data is used during shutdown. This PID is
	 * conventionally whatever `pvrsrvctl' was when it was alive.
	 */
	IMG_UINT32	ui32Pid;

	/* Device cookie for services allocation functions. The device would
	 * ordinarily be SGX, and the first/only device in the system.
	 */
	IMG_HANDLE	hDevCookie;

	/* Device memory context that all SYNC_INFOs allocated by this driver
	 * will be created in. Because SYNC_INFOs are placed in a shared heap,
	 * it does not matter from which process the create ioctl originates.
	 */
	IMG_HANDLE	hDevMemContext;
	struct PVR_FENCE_CONTEXT *psForeignFenceCtx;
}
gsSyncServicesConnection;


/* NOTE: Must only be called with services bridge mutex held */
static void PVRSyncSWTakeOp(PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo)
{
	psKernelSyncInfo->psSyncData->ui32WriteOpsPending = 1;
}

static void PVRSyncSWCompleteOp(PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo)
{
	psKernelSyncInfo->psSyncData->ui32WriteOpsComplete = 1;
}

#define PVR_DUMPDEBUG_LOG(fmt, ...) \
	do {                                                             \
		PVR_DPF((PVR_DBG_ERROR, fmt "\n", ## __VA_ARGS__));      \
	} while (0)

static IMG_BOOL PVRSyncIsSyncInfoInUse(PVRSRV_KERNEL_SYNC_INFO *psSyncInfo)
{

	return !(psSyncInfo->psSyncData->ui32WriteOpsPending ==
			 psSyncInfo->psSyncData->ui32WriteOpsComplete &&
			 psSyncInfo->psSyncData->ui32ReadOpsPending ==
			 psSyncInfo->psSyncData->ui32ReadOpsComplete &&
			 psSyncInfo->psSyncData->ui32ReadOps2Pending ==
			 psSyncInfo->psSyncData->ui32ReadOps2Complete);
}

static inline bool
pvr_fence_sync_value_met(struct PVR_FENCE *psPVRFence)
{
	return !PVRSyncIsSyncInfoInUse(psPVRFence->psSyncData->psSyncInfo->psBase);
}

static void PVRSyncReleaseSyncInfo(struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo)
{
	unsigned long flags;

	spin_lock_irqsave(&gSyncInfoFreeListLock, flags);
	list_add_tail(&psSyncInfo->sHead, &gSyncInfoFreeList);
	spin_unlock_irqrestore(&gSyncInfoFreeListLock, flags);

	queue_work(gpsWorkQueue, &gsWork);
}

static void PVRSyncFreeSyncData(struct PVR_SYNC_DATA *psSyncData)
{
	PVRSyncReleaseSyncInfo(psSyncData->psSyncInfo);
	psSyncData->psSyncInfo = NULL;
	kfree(psSyncData);
}

static void
pvr_fence_context_fences_dump(struct PVR_FENCE_CONTEXT *psFenceCtx)
{
	struct PVR_FENCE *psPVRFence;
	unsigned long flags;

	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_for_each_entry(psPVRFence, &psFenceCtx->sFenceList, sFenceHead)
	{
		PVR_DUMPDEBUG_LOG(
			"f %llu: WOCVA=0x%.8X WriteOps P %d C %d ReadOps P %d C %d ReadOps2 P %d C %d, %s %s",
			(u64) psPVRFence->psFenceCtx->ui64FenceCtx,
			psPVRFence->psSyncData->psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
			psPVRFence->psSyncData->psSyncInfo->psBase->psSyncData->ui32WriteOpsPending,
			psPVRFence->psSyncData->psSyncInfo->psBase->psSyncData->ui32WriteOpsComplete,
			psPVRFence->psSyncData->psSyncInfo->psBase->psSyncData->ui32ReadOpsPending,
			psPVRFence->psSyncData->psSyncInfo->psBase->psSyncData->ui32ReadOpsComplete,
			psPVRFence->psSyncData->psSyncInfo->psBase->psSyncData->ui32ReadOps2Pending,
			psPVRFence->psSyncData->psSyncInfo->psBase->psSyncData->ui32ReadOps2Complete,
			psPVRFence->pName,
			(&psPVRFence->sBase != psPVRFence->psFence) ? "(foreign)" : "");
	}
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);
}

static inline
IMG_UINT32 pvr_fence_context_seqno_next(struct PVR_FENCE_CONTEXT *psFenceCtx)
{
	return atomic_inc_return(&psFenceCtx->sSeqno) - 1;
}

static inline void
pvr_fence_context_free_deferred(struct PVR_FENCE_CONTEXT *psFenceCtx)
{
	struct PVR_FENCE *psPVRFence, *psPVRFenceTmp;
	LIST_HEAD(deferred_free_list);
	unsigned long flags;
#if defined(DEBUG_PRINT)
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
#endif

	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_for_each_entry_safe(psPVRFence, psPVRFenceTmp, &psFenceCtx->sDeferredFreeList, sFenceHead)
	{
		list_move(&psPVRFence->sFenceHead, &deferred_free_list);
	}
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);

	list_for_each_entry_safe(psPVRFence, psPVRFenceTmp, &deferred_free_list, sFenceHead)
	{
#if defined(DEBUG_PRINT)
		PVRSRV_KERNEL_SYNC_INFO *psSyncInfo = psPVRFence->psSyncData->psSyncInfo->psBase;
#endif
		list_del(&psPVRFence->sFenceHead);
		DPF("R( ): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X "
			"WOP/C=0x%x/0x%x ROP/C=0x%x/0x%x RO2P/C=0x%x/0x%x "
			"S=0x%x, Name=%s",
			psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr,
			psSyncInfo->sReadOpsCompleteDevVAddr.uiAddr,
			psSyncInfo->sReadOps2CompleteDevVAddr.uiAddr,
			psSyncInfo->psSyncData->ui32WriteOpsPending,
			psSyncInfo->psSyncData->ui32WriteOpsComplete,
			psSyncInfo->psSyncData->ui32ReadOpsPending,
			psSyncInfo->psSyncData->ui32ReadOpsComplete,
			psSyncInfo->psSyncData->ui32ReadOps2Pending,
			psSyncInfo->psSyncData->ui32ReadOps2Complete,
			psPVRFence->psSyncData->ui32WOPSnapshot,
			psPVRFence->pName);
		PVRSyncFreeSyncData(psPVRFence->psSyncData);
		dma_fence_free(&psPVRFence->sBase);
	}
}

static void
pvr_fence_context_destroy_work(struct work_struct *psData)
{
	struct PVR_FENCE_CONTEXT *psFenceCtx =
		container_of(psData, struct PVR_FENCE_CONTEXT, sDestroyWork);

	pvr_fence_context_free_deferred(psFenceCtx);

	if (WARN_ON(!list_empty_careful(&psFenceCtx->sFenceList)))
	{
		PVR_DPF((PVR_DBG_ERROR, "List is not empty in pvr_fence_context_destroy_kref"));
		pvr_fence_context_fences_dump(psFenceCtx);
	}

	destroy_workqueue(psFenceCtx->psFenceWq);

	kfree(psFenceCtx);
}

static void
pvr_fence_context_destroy_kref(struct kref *pKref)
{
	struct PVR_FENCE_CONTEXT *psFenceCtx =
		container_of(pKref, struct PVR_FENCE_CONTEXT, sRef);

	schedule_work(&psFenceCtx->sDestroyWork);
}

/**
 * pvr_fence_context_destroy - destroys a context
 * @fctx: PVR fence context to destroy
 *
 * Destroys a PVR fence context with the expectation that all fences have been
 * destroyed.
 */
void
pvr_fence_context_destroy(struct PVR_FENCE_CONTEXT *psFenceCtx)
{
	mutex_lock(&gFenceCtxListLock);
	list_del(&psFenceCtx->sFenceCtxList);
	mutex_unlock(&gFenceCtxListLock);

	kref_put(&psFenceCtx->sRef, pvr_fence_context_destroy_kref);
}

static void
pvr_fence_context_signal_fences(struct work_struct *psData)
{
	struct PVR_FENCE_CONTEXT *psFenceCtx =
		container_of(psData, struct PVR_FENCE_CONTEXT, sSignalWork);
	struct PVR_FENCE *psPVRFence, *psPVRTmp;
	unsigned long flags;
	LIST_HEAD(signal_list);

	/*
	 * We can't call fence_signal while holding the lock as we can end up
	 * in a situation whereby pvr_fence_foreign_signal_sync, which also
	 * takes the list lock, ends up being called as a result of the
	 * fence_signal below, i.e. fence_signal(fence) -> fence->callback()
	 *  -> fence_signal(foreign_fence) -> foreign_fence->callback() where
	 * the foreign_fence callback is pvr_fence_foreign_signal_sync.
	 *
	 * So extract the items we intend to signal and add them to their own
	 * queue.
	 */
	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_for_each_entry_safe(psPVRFence, psPVRTmp, &psFenceCtx->sSignalList, sSignalHead)
	{
		if (pvr_fence_sync_value_met(psPVRFence))
		{
			list_move(&psPVRFence->sSignalHead, &signal_list);
		}
	}
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);

	list_for_each_entry_safe(psPVRFence, psPVRTmp, &signal_list, sSignalHead)
	{

		PVR_FENCE_TRACE(&psPVRFence->sBase, "signalled fence (%s) %p\n", psPVRFence->pName, psPVRFence);
		list_del(&psPVRFence->sSignalHead);
		dma_fence_signal(psPVRFence->psFence);
		dma_fence_put(psPVRFence->psFence);
	}

	/*
	 * Take this opportunity to free up any fence objects we
	 * have deferred freeing.
	 */
	pvr_fence_context_free_deferred(psFenceCtx);

	/* Put back ref taken duing queing of fence context work */
	kref_put(&psFenceCtx->sRef, pvr_fence_context_destroy_kref);
}

IMG_INTERNAL
void PVRSyncUpdateAllSyncs(void)
{
	IMG_BOOL bNeedToProcessQueues = IMG_FALSE;
	struct list_head *psEntry;

	/* Check to see if any syncs have signalled. If they have, it may unblock
	 * the GPU. Decide what is needed and optionally schedule queue
	 * processing.
	 */
	mutex_lock(&gFenceCtxListLock);
	list_for_each(psEntry, &gFenceCtxList)
	{
		struct PVR_FENCE_CONTEXT *psFenceCtx = container_of(psEntry, struct PVR_FENCE_CONTEXT, sFenceCtxList);

		if(psFenceCtx->bSyncHasSignaled)
		{
			psFenceCtx->bSyncHasSignaled = IMG_FALSE;
			bNeedToProcessQueues = IMG_TRUE;
		}
		/*
		 * We need to take a reference on fence context as this
		 * function and fence context destruction call can come
		 * in any order. And release it in after serving work.
		*/
		kref_get(&psFenceCtx->sRef);
		queue_work(psFenceCtx->psFenceWq, &psFenceCtx->sSignalWork);
	}
	mutex_unlock(&gFenceCtxListLock);

	if(bNeedToProcessQueues)
	{
		queue_work(gpsWorkQueue, &gsWork);
	}
}

/**
 * pvr_fence_context_create - creates a PVR fence context
 * @name: context name (used for debugging)
 *
 * Creates a PVR fence context that can be used to create PVR fences or to
 * create PVR fences from an existing fence.
 *
 * pvr_fence_context_destroy should be called to clean up the fence context.
 *
 * Returns NULL if a context cannot be created.
 */
struct PVR_FENCE_CONTEXT *
pvr_fence_context_create(const char *pName)
{
	struct PVR_FENCE_CONTEXT *psFenceCtx;

	psFenceCtx = kzalloc(sizeof(*psFenceCtx), GFP_KERNEL);
	if (!psFenceCtx)
		return NULL;

	spin_lock_init(&psFenceCtx->sLock);
	atomic_set(&psFenceCtx->sSeqno, 0);
	INIT_WORK(&psFenceCtx->sSignalWork, pvr_fence_context_signal_fences);
	INIT_WORK(&psFenceCtx->sDestroyWork, pvr_fence_context_destroy_work);
	spin_lock_init(&psFenceCtx->sListLock);
	INIT_LIST_HEAD(&psFenceCtx->sSignalList);
	INIT_LIST_HEAD(&psFenceCtx->sFenceList);
	INIT_LIST_HEAD(&psFenceCtx->sDeferredFreeList);

	psFenceCtx->ui64FenceCtx = dma_fence_context_alloc(1);
	psFenceCtx->pName = pName;
	psFenceCtx->bSyncHasSignaled = IMG_FALSE;

	psFenceCtx->psFenceWq = create_freezable_workqueue("pvr_fence_sync_workqueue");
	if (!psFenceCtx->psFenceWq)
	{
		PVR_DPF((PVR_DBG_ERROR,"%s: failed to create fence workqueue\n", __func__));
		goto err_destroy_workqueue;
	}

	kref_init(&psFenceCtx->sRef);

	mutex_lock(&gFenceCtxListLock);
	list_add_tail(&psFenceCtx->sFenceCtxList, &gFenceCtxList);
	mutex_unlock(&gFenceCtxListLock);

	PVR_FENCE_CTX_TRACE(psFenceCtx, "created fence context (%s)\n", pName);

	return psFenceCtx;

err_destroy_workqueue:
	destroy_workqueue(psFenceCtx->psFenceWq);
	kfree(psFenceCtx);
	return NULL;
}

static const char *
pvr_fence_get_driver_name(struct dma_fence *psFence)
{
	return PVR_LDM_DRIVER_REGISTRATION_NAME;
}

static const char *
pvr_fence_get_timeline_name(struct dma_fence *psFence)
{
	struct PVR_FENCE *psPVRFence = to_pvr_fence(psFence);

	return psPVRFence->psFenceCtx->pName;
}

static bool
pvr_fence_enable_signaling(struct dma_fence *psFence)
{
	struct PVR_FENCE *psPVRFence = to_pvr_fence(psFence);
	struct PVR_FENCE_CONTEXT *psFenceCtx = psPVRFence->psFenceCtx;
	unsigned long flags;

	WARN_ON_SMP(!spin_is_locked(&psFenceCtx->sLock));

	if (pvr_fence_sync_value_met(psPVRFence))
	{
		return false;
	}

	dma_fence_get(&psPVRFence->sBase);

	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_add_tail(&psPVRFence->sSignalHead, &psFenceCtx->sSignalList);
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);

	PVR_FENCE_TRACE(&psPVRFence->sBase, "signalling enabled (%p)\n", psPVRFence);

	return true;
}

static bool
pvr_fence_is_signaled(struct dma_fence *psFence)
{
	struct PVR_FENCE *psPVRFence = to_pvr_fence(psFence);

	if(pvr_fence_sync_value_met(psPVRFence))
	{
		psPVRFence->psFenceCtx->bSyncHasSignaled = IMG_TRUE;
		return true;
	}
	else
	{
		return false;
	}
}

static void
pvr_fence_release(struct dma_fence *psFence)
{
	struct PVR_FENCE *psPVRFence = to_pvr_fence(psFence);
	struct PVR_FENCE_CONTEXT *psFenceCtx = psPVRFence->psFenceCtx;
	unsigned long flags;

	PVR_FENCE_TRACE(&psPVRFence->sBase, "released fence (%s) %p\n", psPVRFence->pName, psPVRFence);

	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_move(&psPVRFence->sFenceHead, &psFenceCtx->sDeferredFreeList);
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);

	kref_put(&psFenceCtx->sRef, pvr_fence_context_destroy_kref);
}

const struct dma_fence_ops pvr_fence_ops = {
	.get_driver_name = pvr_fence_get_driver_name,
	.get_timeline_name = pvr_fence_get_timeline_name,
	.enable_signaling = pvr_fence_enable_signaling,
	.signaled = pvr_fence_is_signaled,
	.wait = dma_fence_default_wait,
	.release = pvr_fence_release,
};

/**
 * pvr_fence_create - creates a PVR fence
 * @fctx: PVR fence context on which the PVR fence should be created
 * @name: PVR fence name (used for debugging)
 *
 * Creates a PVR fence.
 *
 * Once the fence is finished with pvr_fence_destroy should be called.
 *
 * Returns NULL if a PVR fence cannot be created.
 */
struct PVR_FENCE *
pvr_fence_create(struct PVR_FENCE_CONTEXT *psFenceCtx, const char *name, struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo)
{
	struct PVR_FENCE *psPVRFence;
	unsigned int seqno;
	unsigned long flags;

	psPVRFence = kzalloc(sizeof(*psPVRFence), GFP_KERNEL);
	if (!psPVRFence)
		return NULL;

	psPVRFence->psSyncData = kmalloc(sizeof(struct PVR_SYNC_DATA), GFP_KERNEL);
	if(!psPVRFence->psSyncData)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate PVR_SYNC_DATA", __func__));
		goto err_free_fence;
	}
	psPVRFence->psSyncData->psSyncInfo = psSyncInfo;

	INIT_LIST_HEAD(&psPVRFence->sFenceHead);
	INIT_LIST_HEAD(&psPVRFence->sSignalHead);
	psPVRFence->psFenceCtx = psFenceCtx;
	psPVRFence->pName = name;
	psPVRFence->psFence = &psPVRFence->sBase;

	seqno = pvr_fence_context_seqno_next(psFenceCtx);
	dma_fence_init(&psPVRFence->sBase, &pvr_fence_ops, &psFenceCtx->sLock,
		       psFenceCtx->ui64FenceCtx, seqno);

	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_add_tail(&psPVRFence->sFenceHead, &psFenceCtx->sFenceList);
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);

	kref_get(&psFenceCtx->sRef);

	PVR_FENCE_TRACE(&psPVRFence->sBase, "created fence (%s) %p\n", name, psPVRFence);

	return psPVRFence;

err_free_fence:
	kfree(psPVRFence);
	return NULL;
}

static const char *
pvr_fence_foreign_get_driver_name(struct dma_fence *psFence)
{
	return "unknown";
}

static const char *
pvr_fence_foreign_get_timeline_name(struct dma_fence *psFence)
{
	return "unknown";
}

static bool
pvr_fence_foreign_enable_signaling(struct dma_fence *psFence)
{
	WARN_ON("cannot enable signalling on foreign fence");
	return false;
}

static signed long
pvr_fence_foreign_wait(struct dma_fence *psFence, bool intr, signed long timeout)
{
	WARN_ON("cannot wait on foreign fence");
	return 0;
}

static void
pvr_fence_foreign_release(struct dma_fence *psFence)
{
	struct PVR_FENCE *psPVRFence = to_pvr_fence(psFence);
	struct PVR_FENCE_CONTEXT *psFenceCtx = psPVRFence->psFenceCtx;
	unsigned long flags;

	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_move(&psPVRFence->sFenceHead, &psFenceCtx->sDeferredFreeList);
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);

	kref_put(&psFenceCtx->sRef, pvr_fence_context_destroy_kref);
}

const struct dma_fence_ops pvr_fence_foreign_ops = {
	.get_driver_name = pvr_fence_foreign_get_driver_name,
	.get_timeline_name = pvr_fence_foreign_get_timeline_name,
	.enable_signaling = pvr_fence_foreign_enable_signaling,
	.wait = pvr_fence_foreign_wait,
	.release = pvr_fence_foreign_release,
};

static void
pvr_fence_foreign_signal_sync(struct dma_fence *psFence, struct dma_fence_cb *psCb)
{
	struct PVR_FENCE *psPVRFence = container_of(psCb, struct PVR_FENCE, sFenceCb);

	if (WARN_ON_ONCE(is_pvr_fence(psFence)))
		return;

	PVRSyncSWCompleteOp(psPVRFence->psSyncData->psSyncInfo->psBase);

	PVR_FENCE_TRACE(&psPVRFence->sBase,
			"foreign fence %llu#%d signalled (%s)\n",
			psPVRFence->psFenceCtx->ui64FenceCtx,
			psPVRFence->psFenceCtx->sSeqno, psPVRFence->pName);

	psPVRFence->psFenceCtx->bSyncHasSignaled = IMG_TRUE;

	/* Drop the reference on the base fence */
	dma_fence_put(&psPVRFence->sBase);
}

/**
 * pvr_fence_create_from_fence - creates a PVR fence from a fence
 * @fctx: PVR fence context on which the PVR fence should be created
 * @fence: fence from which the PVR fence should be created
 * @name: PVR fence name (used for debugging)
 *
 * Creates a PVR fence from an existing fence. If the fence is a foreign fence,
 * i.e. one that doesn't originate from a PVR fence context, then a new PVR
 * fence will be created. Otherwise, a reference will be taken on the underlying
 * fence and the PVR fence will be returned.
 *
 * Once the fence is finished with pvr_fence_destroy should be called.
 *
 * Returns NULL if a PVR fence cannot be created.
 */
struct PVR_FENCE *
pvr_fence_create_from_fence(struct PVR_FENCE_CONTEXT *psFenceCtx,
			    struct dma_fence *psFence,
			    const char *name)
{
	struct PVR_FENCE *psPVRFence;
	unsigned int seqno;
	unsigned long flags;
	struct PVR_SYNC_KERNEL_SYNC_INFO *psKernelSyncInfo;
	int err;

	psPVRFence = kzalloc(sizeof(*psPVRFence), GFP_KERNEL);
	if (!psPVRFence)
		return NULL;

	psPVRFence->psSyncData = kmalloc(sizeof(struct PVR_SYNC_DATA), GFP_KERNEL);
	if (!psPVRFence->psSyncData)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate PVR_SYNC_DATA",
								__func__));
		err = -ENOMEM;
		goto err_free_pvr_fence;
	}

	psKernelSyncInfo = kmalloc(sizeof(struct PVR_SYNC_KERNEL_SYNC_INFO), GFP_KERNEL);
	if (!psKernelSyncInfo)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate "
								"PVR_SYNC_KERNEL_SYNC_INFO", __func__));
		err = -ENOMEM;
		goto err_free_pvr_fence;
	}

	/* Allocate a "shadow" SYNCINFO for this foreign fence and set it up to be
	 * completed by the callback.
	 */
	err = PVRSRVAllocSyncInfoKM(gsSyncServicesConnection.hDevCookie,
								   gsSyncServicesConnection.hDevMemContext,
								   &psKernelSyncInfo->psBase);
	if(err != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate syncinfo", __func__));
		goto err_free_sync_data;
	}

	PVRSyncSWTakeOp(psKernelSyncInfo->psBase);

	INIT_LIST_HEAD(&psPVRFence->sFenceHead);
	INIT_LIST_HEAD(&psPVRFence->sSignalHead);
	psPVRFence->psFenceCtx = psFenceCtx;
	psPVRFence->pName = name;
	psPVRFence->psFence = psFence;
	psPVRFence->psSyncData->psSyncInfo = psKernelSyncInfo;
	/*
	 * We use the base fence to refcount the PVR fence and to do the
	 * necessary clean up once the refcount drops to 0.
	 */
	seqno = pvr_fence_context_seqno_next(psFenceCtx);
	dma_fence_init(&psPVRFence->sBase, &pvr_fence_foreign_ops, &psFenceCtx->sLock,
		       psFenceCtx->ui64FenceCtx, seqno);

	/*
	 * Take an extra reference on the base fence that gets dropped when the
	 * foreign fence is signalled.
	 */
	dma_fence_get(&psPVRFence->sBase);

	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_add_tail(&psPVRFence->sFenceHead, &psFenceCtx->sFenceList);
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);
	kref_get(&psFenceCtx->sRef);

	PVR_FENCE_TRACE(&psPVRFence->sBase,
			"created fence from foreign fence %llu#%d (%s)\n",
			(u64) psPVRFence->psFenceCtx->ui64FenceCtx,
			psPVRFence->psFenceCtx->sSeqno, name);

	err = dma_fence_add_callback(psFence, &psPVRFence->sFenceCb,
				     pvr_fence_foreign_signal_sync);
	if (err) {
		if (err != -ENOENT)
			goto err_put_ref;

		PVRSyncSWCompleteOp(psKernelSyncInfo->psBase);
		PVR_FENCE_TRACE(&psPVRFence->sBase,
				"foreign fence %llu#%d already signaled (%s)\n",
				(u64) psPVRFence->psFenceCtx->ui64FenceCtx,
				psPVRFence->psFenceCtx->sSeqno,
				name);
		dma_fence_put(&psPVRFence->sBase);
	}


	return psPVRFence;

err_put_ref:
	kref_put(&psFenceCtx->sRef, pvr_fence_context_destroy_kref);
	spin_lock_irqsave(&psFenceCtx->sListLock, flags);
	list_del(&psPVRFence->sFenceHead);
	spin_unlock_irqrestore(&psFenceCtx->sListLock, flags);
	PVRSyncSWCompleteOp(psKernelSyncInfo->psBase);
	PVRSRVReleaseSyncInfoKM(psKernelSyncInfo->psBase);
err_free_sync_data:
	kfree(psPVRFence->psSyncData);
err_free_pvr_fence:
	kfree(psPVRFence);
	return NULL;
}

/**
 * pvr_fence_destroy - destroys a PVR fence
 * @pvr_fence: PVR fence to destroy
 *
 * Destroys a PVR fence. Upon return, the PVR fence may still exist if something
 * else still references the underlying fence, e.g. a reservation object, or if
 * software signalling has been enabled and the fence hasn't yet been signalled.
 */
void
pvr_fence_destroy(struct PVR_FENCE *psPVRFence)
{
	PVR_FENCE_TRACE(&psPVRFence->sBase, "destroyed fence (%s)\n", psPVRFence->pName);

	dma_fence_put(&psPVRFence->sBase);
}

static bool is_pvr_timeline(struct file *psFile)
{
	return psFile->f_op == &pvr_sync_fops;
}

static struct PVR_SYNC_TIMELINE *pvr_sync_timeline_fget(int fd)
{
	struct file *psFile = fget(fd);

	if (!psFile)
		return NULL;

	if (!is_pvr_timeline(psFile)) {
		fput(psFile);
		return NULL;
	}

	return psFile->private_data;
}

static void pvr_sync_timeline_fput(struct PVR_SYNC_TIMELINE *psTimeLine)
{
	fput(psTimeLine->psFile);
}

static int PVRSyncOpen(struct inode *inode, struct file *psFile)
{
	struct PVR_FENCE_CONTEXT *psFenceCtx;
	struct PVR_SYNC_TIMELINE *psTimeline;
	char task_comm[TASK_COMM_LEN];
	int err = -ENOMEM;

	get_task_comm(task_comm, current);

	psTimeline = kzalloc(sizeof(*psTimeline), GFP_KERNEL);
	if (!psTimeline)
		goto err_out;

	strlcpy(psTimeline->name, task_comm, sizeof(psTimeline->name));

	psFenceCtx =  pvr_fence_context_create(psTimeline->name);
	if (!psFenceCtx) {
		PVR_DPF((PVR_DBG_ERROR, "pvr_fence: %s: pvr_fence_context_create failed\n", __func__));
		goto err_free_timeline;
	}

	psTimeline->psSyncInfo = kmalloc(sizeof(struct PVR_SYNC_KERNEL_SYNC_INFO), GFP_KERNEL);
	if(!psTimeline->psSyncInfo)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate PVR_SYNC_KERNEL_SYNC_INFO", __func__));
		goto err_free_fence;
	}

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	err = PVRSRVAllocSyncInfoKM(gsSyncServicesConnection.hDevCookie,
								   gsSyncServicesConnection.hDevMemContext,
								   &psTimeline->psSyncInfo->psBase);
	LinuxUnLockMutex(&gPVRSRVLock);

	if (err != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate timeline syncinfo",
								__func__));
		goto err_free_syncinfo;
	}

	psTimeline->psFenceCtx = psFenceCtx;
	psTimeline->psFile = psFile;

	psFile->private_data = psTimeline;
	err = 0;
err_out:
	return err;
err_free_syncinfo:
	kfree(psTimeline->psSyncInfo);
err_free_fence:
	pvr_fence_context_destroy(psFenceCtx);
err_free_timeline:
	kfree(psTimeline);
	goto err_out;
}

static int PVRSyncRelease(struct inode *inode, struct file *psFile)
{
	struct PVR_SYNC_TIMELINE *psTimeline = psFile->private_data;

	if (psTimeline->pSWTimeline)
	{
		/* This makes sure any outstanding SW syncs are marked as
		 * complete at timeline close time. Otherwise it'll leak the
		 * timeline (as outstanding fences hold a ref) and possibly
		 * wedge the system is something is waiting on one of those
		 * fences
		 */
		pvr_counting_fence_timeline_force_complete(psTimeline->pSWTimeline);
		pvr_counting_fence_timeline_put(psTimeline->pSWTimeline);

		/*
		 * pvr_fence_context_destroy can not be called for sw timeline -
		 * otherwise it leads to double list_del on sFenceCtxList
		 */
		kref_put(&psTimeline->psFenceCtx->sRef, pvr_fence_context_destroy_kref);
	} else {
		pvr_fence_context_destroy(psTimeline->psFenceCtx);
	}

	PVRSyncReleaseSyncInfo(psTimeline->psSyncInfo);
	kfree(psTimeline);

	return 0;
}

static long PVRSyncIOCTLCreate(struct PVR_SYNC_TIMELINE *psTimeline, void __user *pvData)
{
	struct PVR_SYNC_KERNEL_SYNC_INFO *psProvidedSyncInfo = NULL;
	struct PVR_ALLOC_SYNC_DATA *psAllocSyncData;
	struct PVR_SYNC_CREATE_IOCTL_DATA sData;
	int err = -EFAULT, iFd;
	struct PVR_FENCE *psPVRFence;
	struct sync_file *psSyncfile;

#if (LINUX_VERSION_CODE > KERNEL_VERSION(4,2,0))
	iFd = get_unused_fd_flags(0);
#else
	iFd = get_unused_fd();
#endif
	if (iFd < 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to find unused fd (%d)",
								__func__, iFd));
		goto err_out;
	}

	if (!access_ok(VERIFY_READ, pvData, sizeof(sData)))
		goto err_put_fd;

	if (copy_from_user(&sData, pvData, sizeof(sData)))
		goto err_put_fd;

	if (sData.allocdSyncInfo < 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Requested to create a fence from "
								" an invalid alloc'd fd (%d)", __func__,
								sData.allocdSyncInfo));
		goto err_put_fd;
	}

	psAllocSyncData = PVRSyncAllocFDGet(sData.allocdSyncInfo);
	if (!psAllocSyncData) {
		PVR_DPF((PVR_DBG_ERROR, "pvr_fence: %s: Failed to open supplied file fd (%d)\n",
			__func__, sData.allocdSyncInfo));
		err = PVRSRV_ERROR_HANDLE_NOT_FOUND;
		goto err_put_fd;
	}

	/* Move the psSyncInfo to the newly created sync, to avoid attempting
	 * to create multiple syncs from the same allocation.
	 */
	psProvidedSyncInfo = psAllocSyncData->psSyncInfo;
	psAllocSyncData->psSyncInfo = NULL;

	if (psProvidedSyncInfo == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Alloc'd sync info is null - "
								"possibly already CREATEd?", __func__));
		fput(psAllocSyncData->psFile);
		goto err_put_fd;
	}
	fput(psAllocSyncData->psFile);

	sData.name[sizeof(sData.name) - 1] = '\0';

	psPVRFence = pvr_fence_create(psAllocSyncData->psTimeline->psFenceCtx, sData.name, psProvidedSyncInfo);
	if (!psPVRFence)
	{
		PVR_DPF((PVR_DBG_ERROR, "pvr_fence: %s: Failed to create new pvr_fence\n", __func__));
		err = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto err_put_fd;
	}

	psPVRFence->psSyncData->ui32WOPSnapshot = psAllocSyncData->psTimeline->psSyncInfo->psBase->psSyncData->ui32WriteOpsPending;

	psSyncfile = sync_file_create(&psPVRFence->sBase);
	if (!psSyncfile) {
		PVR_DPF((PVR_DBG_ERROR, ": %s: Failed to create sync_file\n", __func__));
		err = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto err_destroy_fence;
	}

	sData.fence = iFd;

	if (!access_ok(VERIFY_WRITE, pvData, sizeof(sData)))
	{
		goto err_destroy_fence;
	}

	if (copy_to_user(pvData, &sData, sizeof(sData)))
	{
		goto err_destroy_fence;
	}

	/* If the fence is a 'real' one, its signal status will be updated by
	 * the MISR calling PVRSyncUpdateAllSyncs(). However, if we created
	 * a 'fake' fence (for power optimization reasons) it has already
	 * completed, and needs to be marked signalled (as the MISR will
	 * never run for 'fake' fences).
	 */
	if(psProvidedSyncInfo->psBase->psSyncData->ui32WriteOpsPending == 0)
	{
		psPVRFence->psFenceCtx->bSyncHasSignaled = IMG_TRUE;
	}

	DPF("Create: WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X F=%p %s",
		psProvidedSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
		psProvidedSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
		psProvidedSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr,
		psPVRFence, sData.name);

	fd_install(iFd, psSyncfile->file);
	err = 0;
err_out:
	return err;

err_destroy_fence:
	pvr_fence_destroy(psPVRFence);
err_put_fd:
	put_unused_fd(iFd);
	goto err_out;
}

static long PVRSyncIOCTLRename(struct PVR_SYNC_TIMELINE *psTimeline, void __user *user_data)
{
	int err = 0;
	struct PVR_SYNC_RENAME_IOCTL_DATA data;

	if (!access_ok(VERIFY_READ, user_data, sizeof(data))) {
		err = -EFAULT;
		goto err;
	}

	if (copy_from_user(&data, user_data, sizeof(data))) {
		err = -EFAULT;
		goto err;
	}

	data.szName[sizeof(data.szName) - 1] = '\0';
	strlcpy(psTimeline->name, data.szName, sizeof(psTimeline->name));

err:
	return err;
}

static long PVRSyncIOCTLForceSw(struct PVR_SYNC_TIMELINE *psTimeline, void **private_data)
{
	/* Already in SW mode? */
	if (psTimeline->pSWTimeline)
		return 0;

	/* Create a sw_sync timeline with the old GPU timeline's name */
	psTimeline->pSWTimeline = pvr_counting_fence_timeline_create(psTimeline->name);

	/* Don't add SW timeline to global timeline list */
	mutex_lock(&gFenceCtxListLock);
	list_del(&psTimeline->psFenceCtx->sFenceCtxList);
	mutex_unlock(&gFenceCtxListLock);

	if (!psTimeline->pSWTimeline)
		return -ENOMEM;

	return 0;
}

static long PVRSyncIOCTLCreateSwFence(struct PVR_SYNC_TIMELINE *psTimeline, void __user *user_data)
{
	struct sw_sync_create_fence_data sData;
	struct sync_file *psSyncFile;
	int fd = get_unused_fd_flags(0);
	struct dma_fence *psFence;
	int err = -EFAULT;

	if (fd < 0)
	{
		PVR_DPF((PVR_DBG_ERROR,"pvr_fence: %s: Failed to find unused fd (%d)", __func__, fd));
		goto err_out;
	}

	if (copy_from_user(&sData, user_data, sizeof(sData)))
	{
		PVR_DPF((PVR_DBG_ERROR,"pvr_fence: %s: Failed copy from user", __func__));
		goto err_put_fd;
	}

	psFence = pvr_counting_fence_create(psTimeline->pSWTimeline, sData.value);
	if (!psFence)
	{
		PVR_DPF((PVR_DBG_ERROR,"pvr_fence: %s: Failed to create a sync point (%d)", __func__, fd));
		err = -ENOMEM;
		goto err_put_fd;
	}

	psSyncFile = sync_file_create(psFence);
	if (!psSyncFile)
	{
		PVR_DPF((PVR_DBG_ERROR,"pvr_fence: %s: Failed to create a sync point (%d)", __func__, fd));
		 err = -ENOMEM;
		goto err_put_fence;
	}

	sData.fence = fd;

	if (copy_to_user(user_data, &sData, sizeof(sData)))
	{
		PVR_DPF((PVR_DBG_ERROR,"pvr_fence: %s: Failed copy to user", __func__));
		goto err_put_fence;
	}

	fd_install(fd, psSyncFile->file);
	err = 0;
err_out:
	return err;
err_put_fence:
	dma_fence_put(psFence);
err_put_fd:
	put_unused_fd(fd);
	goto err_out;
}

static long PVRSyncIOCTLSWInc(struct PVR_SYNC_TIMELINE *psTimeline, void __user *user_data)
{
	u32 value;

	if (copy_from_user(&value, user_data, sizeof(value)))
		return -EFAULT;

	pvr_counting_fence_timeline_inc(psTimeline->pSWTimeline, value);
	return 0;
}

static int PVRSyncFenceAllocRelease(struct inode *inode, struct file *file)
{
	struct PVR_ALLOC_SYNC_DATA *psAllocSyncData = file->private_data;

	if(psAllocSyncData->psSyncInfo)
	{

		DPF("R(a): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X",
			psAllocSyncData->psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
			psAllocSyncData->psSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
			psAllocSyncData->psSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr);

		PVRSyncReleaseSyncInfo(psAllocSyncData->psSyncInfo);
		psAllocSyncData->psSyncInfo = NULL;
	}

	kfree(psAllocSyncData);
	return 0;
}

static const struct file_operations gsSyncFenceAllocFOps =
{
	.release = PVRSyncFenceAllocRelease,
};

struct PVR_ALLOC_SYNC_DATA *PVRSyncAllocFDGet(int fd)
{
	struct file *file = fget(fd);
	if (!file)
		return NULL;
	if (file->f_op != &gsSyncFenceAllocFOps)
		goto err;
	return file->private_data;
err:
	fput(file);
	return NULL;
}

static long
PVRSyncIOCTLAlloc(struct PVR_SYNC_TIMELINE *psTimeline, void __user *pvData)
{
	struct PVR_ALLOC_SYNC_DATA *psAllocSyncData;
	int err = -EFAULT, iFd;
	struct PVR_SYNC_ALLOC_IOCTL_DATA sData;
	PVRSRV_SYNC_DATA *psSyncData;
	struct file *psFile;
	PVRSRV_ERROR eError;

#if (LINUX_VERSION_CODE > KERNEL_VERSION(4,2,0))
	iFd = get_unused_fd_flags(0);
#else
	iFd = get_unused_fd();
#endif
	if (iFd < 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to find unused fd (%d)",
					__func__, iFd));
		goto err_out;
	}

	if (!access_ok(VERIFY_READ, pvData, sizeof(sData)))
		goto err_put_fd;

	if (copy_from_user(&sData, pvData, sizeof(sData)))
		goto err_put_fd;

	psAllocSyncData = kmalloc(sizeof(struct PVR_ALLOC_SYNC_DATA), GFP_KERNEL);
	if (!psAllocSyncData)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate PVR_ALLOC_SYNC_DATA", __func__));
		err = -ENOMEM;
		goto err_put_fd;
	}

	psAllocSyncData->psSyncInfo = kmalloc(sizeof(struct PVR_SYNC_KERNEL_SYNC_INFO), GFP_KERNEL);
	if (!psAllocSyncData->psSyncInfo)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate PVR_SYNC_KERNEL_SYNC_INFO", __func__));
		err = -ENOMEM;
		goto err_free_alloc_sync_data;
	}

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	eError = PVRSRVAllocSyncInfoKM(gsSyncServicesConnection.hDevCookie,
				       gsSyncServicesConnection.hDevMemContext,
								   &psAllocSyncData->psSyncInfo->psBase);
	LinuxUnLockMutex(&gPVRSRVLock);


	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to alloc syncinfo (%d)", __func__, eError));
		err = -ENOMEM;
		goto err_free_sync_info;
	}

	psFile = anon_inode_getfile("pvr_fence_alloc", &gsSyncFenceAllocFOps, psAllocSyncData, 0);
	if (!psFile)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to create anon inode", __func__));
		err = -ENOMEM;
		goto err_release_sync_info;
	}

	sData.fence = iFd;

	/* Check if this timeline looks idle. If there are still TQs running
	 * on it, userspace shouldn't attempt any kind of power optimization
	 * (e.g. it must not dummy-process GPU fences).
	 *
	 * Determining idleness here is safe because the ALLOC and CREATE
	 * pvr_sync ioctls must be called under the gralloc module lock, so
	 * we can't be creating another new fence op while we are still
	 * processing this one.
	 *
	 * Take the bridge lock anyway so we can be sure that we read the
	 * timeline sync's pending value coherently. The complete value may
	 * be modified by the GPU, but worse-case we will decide we can't do
	 * the power optimization and will still be correct.
	 */
	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);

	psSyncData = psTimeline->psSyncInfo->psBase->psSyncData;
	if(psSyncData->ui32WriteOpsPending == psSyncData->ui32WriteOpsComplete)
	{
		sData.bTimelineIdle = IMG_TRUE;
	}
	else
	{
		sData.bTimelineIdle = IMG_FALSE;
	}

	LinuxUnLockMutex(&gPVRSRVLock);

	if (!access_ok(VERIFY_WRITE, pvData, sizeof(sData)))
		goto err_release_file;

	if (copy_to_user(pvData, &sData, sizeof(sData)))
		goto err_release_file;

	psAllocSyncData->psTimeline = psTimeline;
	psAllocSyncData->psFile = psFile;

	DPF("A( ): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X",
		psAllocSyncData->psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
		psAllocSyncData->psSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
		psAllocSyncData->psSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr);

	fd_install(iFd, psFile);
	err = 0;
err_out:
	return err;
err_release_sync_info:
	PVRSRVReleaseSyncInfoKM(psAllocSyncData->psSyncInfo->psBase);
err_free_sync_info:
	kfree(psAllocSyncData->psSyncInfo);
err_free_alloc_sync_data:
	kfree(psAllocSyncData);
err_put_fd:
	put_unused_fd(iFd);
	goto err_out;
err_release_file:
	fput(psFile);
	put_unused_fd(iFd);
	goto err_out;
}

static long
PVRSyncIOCTLDebug(struct PVR_SYNC_TIMELINE *psTimeline, void __user *pvData)
{
	struct PVR_SYNC_DEBUG_IOCTL_DATA sData;
	PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo;
	struct dma_fence *psFence;
	struct PVR_FENCE *psPVRFence;
	int err = -EFAULT;
	PVR_SYNC_DEBUG *psMetaData;

	if(!access_ok(VERIFY_READ, pvData, sizeof(sData)))
		goto err_out;

	if(copy_from_user(&sData, pvData, sizeof(sData)))
		goto err_out;

	psMetaData = &sData.sSync[0].sMetaData;

	psFence = sync_file_get_fence(sData.iFenceFD);
	if(!psFence)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to get fence from fd", __func__));
		goto err_out;
	}

	psPVRFence = to_pvr_fence(psFence);
	/* Don't dump foreign fence */
	if(!psPVRFence)
		return 0;

	psKernelSyncInfo = psPVRFence->psSyncData->psSyncInfo->psBase;
	PVR_ASSERT(psKernelSyncInfo != NULL);

	/* The sync refcount is valid as long as the FenceFD stays open,
	 * so we can access it directly without worrying about it being
	 * freed.
	 */
	sData.sSync[0].sSyncData = *psKernelSyncInfo->psSyncData;

	psMetaData->ui32WriteOpsPendingSnapshot = psPVRFence->psSyncData->ui32WOPSnapshot;

	dma_fence_put(psFence);

	sData.ui32NumPoints = 1;

	if(!access_ok(VERIFY_WRITE, pvData, sizeof(sData)))
		goto err_out;

	if(copy_to_user(pvData, &sData, sizeof(sData)))
		goto err_out;

	err = 0;
err_out:
	return err;
}

static long
PVRSyncIOCTL(struct file *file, unsigned int cmd, unsigned long __user arg)
{
	void __user *user_data = (void __user *)arg;
	long err = -ENOTTY;
	struct PVR_SYNC_TIMELINE *psTimeline = file->private_data;
	bool is_sw_timeline = psTimeline->pSWTimeline != NULL;

	if (!is_sw_timeline) {

		switch (cmd) {
		case PVR_SYNC_IOC_CREATE_FENCE:
			err = PVRSyncIOCTLCreate(psTimeline, user_data);
			break;
		case PVR_SYNC_IOC_DEBUG_FENCE:
			err = PVRSyncIOCTLDebug(psTimeline, user_data);
			break;
		case PVR_SYNC_IOC_ALLOC_FENCE:
			err = PVRSyncIOCTLAlloc(psTimeline, user_data);
			break;
		case PVR_SYNC_IOC_RENAME:
			err = PVRSyncIOCTLRename(psTimeline, user_data);
			break;
		case PVR_SYNC_IOC_FORCE_SW_ONLY:
			err = PVRSyncIOCTLForceSw(psTimeline, &file->private_data);
			break;
		default:
			err = -ENOTTY;
		}
	} else {

		switch (cmd) {
		case SW_SYNC_IOC_CREATE_FENCE:
			err = PVRSyncIOCTLCreateSwFence(psTimeline, user_data);
			break;
		case SW_SYNC_IOC_INC:
			err = PVRSyncIOCTLSWInc(psTimeline, user_data);
			break;
		default:
			err = -ENOTTY;
		}
	}

	return err;
}

static void PVRSyncWorkQueueFunction(struct work_struct *data)
{
	PVRSRV_DEVICE_NODE *psDevNode =
		(PVRSRV_DEVICE_NODE*)gsSyncServicesConnection.hDevCookie;
	struct list_head sFreeList, *psEntry, *n;
	unsigned long flags;

	/* We lock the bridge mutex here for two reasons.
	 *
	 * Firstly, the SGXScheduleProcessQueuesKM and PVRSRVReleaseSyncInfoKM
	 * functions require that they are called under lock. Multiple threads
	 * into services are not allowed.
	 *
	 * Secondly, we need to ensure that when processing the defer-free list,
	 * the PVRSyncIsSyncInfoInUse() function is called *after* any freed
	 * sync was attached as a HW dependency (had ROP/ROP2 taken). This is
	 * because for 'foreign' sync timelines we allocate a new object and
	 * mark it for deletion immediately. If the 'foreign' sync_pt signals
	 * before the kick ioctl has completed, we can block it from being
	 * prematurely freed by holding the bridge mutex.
	 *
	 * NOTE: This code relies on the assumption that we can acquire a
	 * spinlock while a mutex is held and that other users of the spinlock
	 * do not need to hold the bridge mutex.
	 */
	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);

	/* A completed SW operation may un-block the GPU */
	SGXScheduleProcessQueuesKM(psDevNode);

	/* We can't call PVRSRVReleaseSyncInfoKM directly in this loop because
	 * that will take the mmap mutex. We can't take mutexes while we have
	 * this list locked with a spinlock. So move all the items we want to
	 * free to another, local list (no locking required) and process it
	 * in a second loop.
	 */

	INIT_LIST_HEAD(&sFreeList);
	spin_lock_irqsave(&gSyncInfoFreeListLock, flags);
	list_for_each_safe(psEntry, n, &gSyncInfoFreeList)
	{
		struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo =
			container_of(psEntry, struct PVR_SYNC_KERNEL_SYNC_INFO, sHead);

		if(!PVRSyncIsSyncInfoInUse(psSyncInfo->psBase))
			list_move_tail(psEntry, &sFreeList);

	}
	spin_unlock_irqrestore(&gSyncInfoFreeListLock, flags);

	list_for_each_safe(psEntry, n, &sFreeList)
	{
		struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo =
			container_of(psEntry, struct PVR_SYNC_KERNEL_SYNC_INFO, sHead);

		list_del(psEntry);

		DPF("F(d): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X",
			psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
			psSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
			psSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr);

		PVRSRVReleaseSyncInfoKM(psSyncInfo->psBase);
		psSyncInfo->psBase = NULL;

		kfree(psSyncInfo);
	}

	LinuxUnLockMutex(&gPVRSRVLock);
}

static const struct file_operations pvr_sync_fops = {
	.owner          = THIS_MODULE,
	.open           = PVRSyncOpen,
	.release        = PVRSyncRelease,
	.unlocked_ioctl = PVRSyncIOCTL,
	.compat_ioctl   = PVRSyncIOCTL,
};

static struct miscdevice pvr_sync_device = {
	.minor          = MISC_DYNAMIC_MINOR,
	.name           = "pvr_sync",
	.fops           = &pvr_sync_fops,
};

IMG_INTERNAL
int PVRSyncDeviceInit(void)
{
	int err = -1;

	if(PVRSyncInitServices() != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to initialise services",
								__func__));
		goto err_out;
	}

	gsSyncServicesConnection.psForeignFenceCtx  = pvr_fence_context_create("foreign_sync");
	if (!gsSyncServicesConnection.psForeignFenceCtx)
	{
		PVR_DPF((PVR_DBG_ERROR,"pvr_fence: %s: Failed to create foreign sync context\n",
			__func__));
		err = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto err_out;
	}

	gpsWorkQueue = create_freezable_workqueue("pvr_sync_workqueue");
	if(!gpsWorkQueue)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to create pvr_sync workqueue", __func__));
		goto err_deinit_services;
	}

	INIT_WORK(&gsWork, PVRSyncWorkQueueFunction);

	err = misc_register(&pvr_sync_device);
	if(err)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to register pvr_sync misc "
								"device (err=%d)", __func__, err));
		goto err_destory_wq;
	}
	err = PVRSRV_OK;

err_out:
	return err;
err_destory_wq:
	destroy_workqueue(gpsWorkQueue);
err_deinit_services:
	pvr_fence_context_destroy(gsSyncServicesConnection.psForeignFenceCtx);
	PVRSyncCloseServices();
	goto err_out;
}

void PVRSyncDeviceDeInit(void)
{
	pvr_fence_cleanup();
	misc_deregister(&pvr_sync_device);
	pvr_fence_context_destroy(gsSyncServicesConnection.psForeignFenceCtx);
	destroy_workqueue(gpsWorkQueue);
	PVRSyncCloseServices();
}

struct PVR_COUNTING_FENCE_TIMELINE *pvr_sync_get_sw_timeline(int fd)
{
	struct PVR_SYNC_TIMELINE *psTimeline;
	struct PVR_COUNTING_FENCE_TIMELINE *psSwTimeline = NULL;

	psTimeline = pvr_sync_timeline_fget(fd);
	if (!psTimeline)
		return NULL;

	psSwTimeline = pvr_counting_fence_timeline_get(psTimeline->pSWTimeline);

	pvr_sync_timeline_fput(psTimeline);
	return psSwTimeline;
}

IMG_BOOL
ExpandAndDeDuplicateFenceSyncs(IMG_UINT32 ui32NumSyncs,
							   IMG_HANDLE aiFenceFds[],
							   IMG_UINT32 ui32SyncPointLimit,
							   struct dma_fence *apsFence[],
							   IMG_UINT32 *pui32NumRealSyncs,
							   PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[])
{
	IMG_UINT32 i, ui32FenceIndex = 0;
	IMG_BOOL bRet = IMG_TRUE;

	*pui32NumRealSyncs = 0;

	for(i = 0; i < ui32NumSyncs; i++)
	{
		struct PVR_FENCE *psPVRFence;

		/* Skip any invalid fence file descriptors without error */
		if((IMG_INT32)aiFenceFds[i] < 0)
			continue;

		/* By converting a file descriptor to a struct sync_fence, we are
		 * taking a reference on the fence. We don't want the fence to go
		 * away until we have submitted the command, even if it signals
		 * before we dispatch the command, or the timeline(s) are destroyed.
		 *
		 * This reference should be released by the caller of this function
		 * once hardware operations have been scheduled on the GPU sync_pts
		 * participating in this fence. When our MISR is scheduled, the
		 * defer-free list will be processed, cleaning up the SYNCINFO.
		 *
		 * Note that this reference *isn't* enough for non-GPU sync_pts.
		 * We'll take another reference on the fence for those operations
		 * later (the life-cycle requirements there are totally different).
		 *
		 * Fence lookup may fail here if the fd became invalid since it was
		 * patched in userspace. That's really a userspace driver bug, so
		 * just fail here instead of not synchronizing.
		 */
		apsFence[ui32FenceIndex] = sync_file_get_fence((IMG_INT32)aiFenceFds[i]);
		if(!apsFence[ui32FenceIndex])
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Failed to get fence from fd=%d",
									__func__, (IMG_SIZE_T)aiFenceFds[i]));
			bRet = IMG_FALSE;
			goto err_out;
		}

		/* If this fence has any points from foreign timelines, we need to
		 * allocate a 'shadow' SYNCINFO and update it in software ourselves,
		 * so the ukernel can test the readiness of the dependency.
		 *
		 * It's tempting to just handle all fences like this (since most of
		 * the time they *will* be merged with sw_sync) but such 'shadow'
		 * syncs are slower. This is because we need to wait for the MISR to
		 * schedule to update the GPU part of the fence (normally the ukernel
		 * would be able to make the update directly).
		 */
		psPVRFence = to_pvr_fence(apsFence[ui32FenceIndex]);
		if(!psPVRFence)
		{

			psPVRFence = pvr_fence_create_from_fence(gsSyncServicesConnection.psForeignFenceCtx, apsFence[ui32FenceIndex], "foreign");
			if(psPVRFence)
			{
				if(!AddSyncInfoToArray(psPVRFence->psSyncData->psSyncInfo->psBase, ui32SyncPointLimit,
									   pui32NumRealSyncs, apsSyncInfo))
				{
					/* Soft-fail. Stop synchronizing. */
					goto err_out;
				}
			}
		}
		else
		{
			if(!AddSyncInfoToArray(psPVRFence->psSyncData->psSyncInfo->psBase, ui32SyncPointLimit, pui32NumRealSyncs, apsSyncInfo))
					goto err_out;
		}
		ui32FenceIndex++;
	}

err_out:
	return bRet;
}

PVRSRV_ERROR PVRSyncInitServices(void)
{
	IMG_BOOL bCreated, bShared[PVRSRV_MAX_CLIENT_HEAPS];
	PVRSRV_HEAP_INFO sHeapInfo[PVRSRV_MAX_CLIENT_HEAPS];
	IMG_UINT32 ui32ClientHeapCount = 0;
	PVRSRV_PER_PROCESS_DATA	*psPerProc;
	PVRSRV_ERROR eError;

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);

	gsSyncServicesConnection.ui32Pid = OSGetCurrentProcessIDKM();

	eError = PVRSRVProcessConnect(gsSyncServicesConnection.ui32Pid, 0);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: PVRSRVProcessConnect failed",
								__func__));
		goto err_unlock;
	}

	psPerProc = PVRSRVFindPerProcessData();
	if (!psPerProc)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: PVRSRVFindPerProcessData failed",
								__func__));
		goto err_disconnect;
	}

	eError = PVRSRVAcquireDeviceDataKM(0, PVRSRV_DEVICE_TYPE_SGX,
									   &gsSyncServicesConnection.hDevCookie);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: PVRSRVAcquireDeviceDataKM failed",
								__func__));
		goto err_disconnect;
	}

	if (!gsSyncServicesConnection.hDevCookie)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: hDevCookie is NULL", __func__));
		goto err_disconnect;
	}

	eError = PVRSRVCreateDeviceMemContextKM(gsSyncServicesConnection.hDevCookie,
						psPerProc,
											&gsSyncServicesConnection.hDevMemContext,
											&ui32ClientHeapCount,
											&sHeapInfo[0],
											&bCreated,
											&bShared[0]);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: PVRSRVCreateDeviceMemContextKM failed",
								__func__));
		goto err_disconnect;
	}

	if (!gsSyncServicesConnection.hDevMemContext)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: hDevMemContext is NULL", __func__));
		goto err_disconnect;
	}

err_unlock:
	LinuxUnLockMutex(&gPVRSRVLock);
	return eError;

err_disconnect:
	PVRSRVProcessDisconnect(gsSyncServicesConnection.ui32Pid);
	goto err_unlock;
}

void PVRSyncCloseServices(void)
{
	IMG_BOOL bDummy;

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);

	PVRSRVDestroyDeviceMemContextKM(gsSyncServicesConnection.hDevCookie,
									gsSyncServicesConnection.hDevMemContext,
									&bDummy);
	gsSyncServicesConnection.hDevMemContext = NULL;
	gsSyncServicesConnection.hDevCookie = NULL;

	PVRSRVProcessDisconnect(gsSyncServicesConnection.ui32Pid);
	gsSyncServicesConnection.ui32Pid = 0;

	LinuxUnLockMutex(&gPVRSRVLock);
}
