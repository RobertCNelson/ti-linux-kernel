/*************************************************************************/ /*!
@File
@Title          PowerVR Linux software "counting" timeline fence implementation
@Codingstyle    LinuxKernel
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Strictly Confidential.
*/ /**************************************************************************/

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/sync_file.h>

#include "img_types.h"
#include "services_headers.h"
#include "servicesext.h"
#include "pvr_counting_timeline.h"
#include "pvr_sw_fence.h"

struct PVR_COUNTING_FENCE_TIMELINE {
	char name[32];
	struct PVR_SW_FENCE_CONTEXT *psSwFenceCtx;

	spinlock_t sActive_fences_lock;
	IMG_UINT64 ui64Current_value; /* guarded by active_fences_lock */
	struct list_head sActive_fences;

	struct kref sRef;
};

struct PVR_COUNTING_FENCE {
	IMG_UINT64 ui64Value;
	struct dma_fence *psFence;
	struct list_head sActive_list_entry;
};

struct PVR_COUNTING_FENCE_TIMELINE *pvr_counting_fence_timeline_create(const char *name)
{
	struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline = kmalloc(sizeof(*psFenceTimeline), GFP_KERNEL);

	if (!psFenceTimeline)
		goto err_out;

	strlcpy(psFenceTimeline->name, name, sizeof(psFenceTimeline->name));

	psFenceTimeline->psSwFenceCtx = pvr_sw_fence_context_create(psFenceTimeline->name, "pvr_sw_sync");
	if (!psFenceTimeline->psSwFenceCtx)
		goto err_free_timeline;

	psFenceTimeline->ui64Current_value = 0;
	kref_init(&psFenceTimeline->sRef);
	spin_lock_init(&psFenceTimeline->sActive_fences_lock);
	INIT_LIST_HEAD(&psFenceTimeline->sActive_fences);

err_out:
	return psFenceTimeline;

err_free_timeline:
	kfree(psFenceTimeline);
	psFenceTimeline = NULL;
	goto err_out;
}

void pvr_counting_fence_timeline_force_complete(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline)
{
	struct list_head *entry, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&psFenceTimeline->sActive_fences_lock, flags);

	list_for_each_safe(entry, tmp, &psFenceTimeline->sActive_fences)
	{
		struct PVR_COUNTING_FENCE *psPvrCountingFence = list_entry(entry, struct PVR_COUNTING_FENCE, sActive_list_entry);
		dma_fence_signal(psPvrCountingFence->psFence);
		dma_fence_put(psPvrCountingFence->psFence);
		psPvrCountingFence->psFence = NULL;
		list_del(&psPvrCountingFence->sActive_list_entry);
		kfree(psPvrCountingFence);
	}
	spin_unlock_irqrestore(&psFenceTimeline->sActive_fences_lock, flags);
}

static void pvr_counting_fence_timeline_destroy(struct kref *kref)
{
	struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline = container_of(kref, struct PVR_COUNTING_FENCE_TIMELINE, sRef);

	WARN_ON(!list_empty(&psFenceTimeline->sActive_fences));

	pvr_sw_fence_context_destroy(psFenceTimeline->psSwFenceCtx);
	kfree(psFenceTimeline);
}

void pvr_counting_fence_timeline_put(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline)
{
	kref_put(&psFenceTimeline->sRef, pvr_counting_fence_timeline_destroy);
}

struct PVR_COUNTING_FENCE_TIMELINE *pvr_counting_fence_timeline_get(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline)
{
	if (!psFenceTimeline)
		return NULL;
	kref_get(&psFenceTimeline->sRef);
	return psFenceTimeline;
}

struct dma_fence *pvr_counting_fence_create(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline, IMG_UINT64 ui64Value)
{
	unsigned long flags;
	struct dma_fence *psSwFence;
	struct PVR_COUNTING_FENCE *psCountFence = kmalloc(sizeof(*psCountFence), GFP_KERNEL);

	if (!psCountFence)
		return NULL;

	psSwFence = pvr_sw_fence_create(psFenceTimeline->psSwFenceCtx);
	if (!psSwFence)
		goto err_free_fence;

	psCountFence->psFence = dma_fence_get(psSwFence);
	psCountFence->ui64Value = ui64Value;

	spin_lock_irqsave(&psFenceTimeline->sActive_fences_lock, flags);

	list_add_tail(&psCountFence->sActive_list_entry, &psFenceTimeline->sActive_fences);

	spin_unlock_irqrestore(&psFenceTimeline->sActive_fences_lock, flags);

	return psSwFence;

err_free_fence:
	kfree(psCountFence);
	return NULL;
}

void pvr_counting_fence_timeline_inc(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline, IMG_UINT64 ui64Value)
{
	struct list_head *entry, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&psFenceTimeline->sActive_fences_lock, flags);

	psFenceTimeline->ui64Current_value += ui64Value;

	list_for_each_safe(entry, tmp, &psFenceTimeline->sActive_fences)
	{
		struct PVR_COUNTING_FENCE *psCountFence = list_entry(entry, struct PVR_COUNTING_FENCE, sActive_list_entry);
		if (psCountFence->ui64Value <= psFenceTimeline->ui64Current_value)
		{
			dma_fence_signal(psCountFence->psFence);
			dma_fence_put(psCountFence->psFence);
			psCountFence->psFence = NULL;
			list_del(&psCountFence->sActive_list_entry);
			kfree(psCountFence);
		}
	}

	spin_unlock_irqrestore(&psFenceTimeline->sActive_fences_lock, flags);
}
