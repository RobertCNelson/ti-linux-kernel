/* -*- mode: c; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* vi: set ts=8 sw=8 sts=8: */
/*************************************************************************/ /*!
@File
@Codingstyle    LinuxKernel
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Strictly Confidential.
*/ /**************************************************************************/

#include <linux/kernel.h>
#include <linux/spinlock_types.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <linux/sync_file.h>

#include "img_types.h"
#include "services_headers.h"
#include "servicesext.h"
#include "pvr_sw_fence.h"

struct PVR_SW_FENCE_CONTEXT
{
	struct kref sRef;
	IMG_INT32 i32ContextId;
	const char *psCtxName;
	const char *psDriverName;
	atomic_t sSeqno;
	atomic_t sFenceCnt;
};

struct PVR_SW_FENCE
{
	struct dma_fence sBase;
	struct PVR_SW_FENCE_CONTEXT *psSWFenceCtx;
	spinlock_t sLock;
};

#define to_pvr_sw_fence(fence) container_of(fence, struct PVR_SW_FENCE, sBase)

static inline unsigned
pvr_sw_fence_context_seqno_next(struct PVR_SW_FENCE_CONTEXT *psSWFenceCtx)
{
	return atomic_inc_return(&psSWFenceCtx->sSeqno) - 1;
}

static const char * pvr_sw_fence_get_driver_name(struct dma_fence *psFence)
{
	struct PVR_SW_FENCE *psPVRSwFence = to_pvr_sw_fence(psFence);

	return psPVRSwFence->psSWFenceCtx->psDriverName;
}

static const char * pvr_sw_fence_get_timeline_name(struct dma_fence *psFence)
{
	struct PVR_SW_FENCE *psPVRSwFence = to_pvr_sw_fence(psFence);

	return psPVRSwFence->psSWFenceCtx->psCtxName;
}

static bool pvr_sw_fence_enable_signaling(struct dma_fence *psFence)
{
	return true;
}

static void pvr_sw_fence_context_destroy_kref(struct kref *kref)
{
	struct PVR_SW_FENCE_CONTEXT *psPVRSwFence = container_of(kref, struct PVR_SW_FENCE_CONTEXT, sRef);
	unsigned fence_count;

	fence_count = atomic_read(&psPVRSwFence->sFenceCnt);
	if (WARN_ON(fence_count))
		pr_debug("%s context has %u fence(s) remaining\n", psPVRSwFence->psCtxName, fence_count);

	kfree(psPVRSwFence);
}

static void pvr_sw_fence_release(struct dma_fence *psFence)
{
	struct PVR_SW_FENCE *psPVRSwFence = to_pvr_sw_fence(psFence);

	atomic_dec(&psPVRSwFence->psSWFenceCtx->sFenceCnt);
	kref_put(&psPVRSwFence->psSWFenceCtx->sRef,
		pvr_sw_fence_context_destroy_kref);
	kfree(psPVRSwFence);
}

static struct dma_fence_ops pvr_sw_fence_ops = {
	.get_driver_name = pvr_sw_fence_get_driver_name,
	.get_timeline_name = pvr_sw_fence_get_timeline_name,
	.enable_signaling = pvr_sw_fence_enable_signaling,
	.wait = dma_fence_default_wait,
	.release = pvr_sw_fence_release,
};

struct PVR_SW_FENCE_CONTEXT *
pvr_sw_fence_context_create(const char *context_name, const char *driver_name)
{
	struct PVR_SW_FENCE_CONTEXT *psSWFenceCtx;

	psSWFenceCtx = kmalloc(sizeof(*psSWFenceCtx), GFP_KERNEL);
	if (!psSWFenceCtx)
		return NULL;

	psSWFenceCtx->i32ContextId = dma_fence_context_alloc(1);
	psSWFenceCtx->psCtxName = context_name;
	psSWFenceCtx->psDriverName = driver_name;
	atomic_set(&psSWFenceCtx->sSeqno, 0);
	atomic_set(&psSWFenceCtx->sFenceCnt, 0);
	kref_init(&psSWFenceCtx->sRef);

	return psSWFenceCtx;
}

void pvr_sw_fence_context_destroy(struct PVR_SW_FENCE_CONTEXT *psSWFenceCtx)
{
	kref_put(&psSWFenceCtx->sRef, pvr_sw_fence_context_destroy_kref);
}

struct dma_fence *
pvr_sw_fence_create(struct PVR_SW_FENCE_CONTEXT *psSWFenceCtx)
{
	struct PVR_SW_FENCE *psPVRSwFence;
	unsigned int seqno;

	psPVRSwFence = kmalloc(sizeof(*psPVRSwFence), GFP_KERNEL);
	if (!psPVRSwFence)
		return NULL;

	spin_lock_init(&psPVRSwFence->sLock);
	psPVRSwFence->psSWFenceCtx = psSWFenceCtx;

	seqno = pvr_sw_fence_context_seqno_next(psSWFenceCtx);
	dma_fence_init(&psPVRSwFence->sBase, &pvr_sw_fence_ops, &psPVRSwFence->sLock, psSWFenceCtx->i32ContextId, seqno);

	atomic_inc(&psSWFenceCtx->sFenceCnt);
	kref_get(&psSWFenceCtx->sRef);

	return &psPVRSwFence->sBase;
}
