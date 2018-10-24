/*************************************************************************/ /*!
@File
@Codingstyle    LinuxKernel
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Strictly Confidential.
*/ /**************************************************************************/

#if !defined(__PVR_COUNTING_TIMELINE_H__)
#define __PVR_COUNTING_TIMELINE_H__

struct PVR_COUNTING_FENCE_TIMELINE;

struct PVR_COUNTING_FENCE_TIMELINE *pvr_counting_fence_timeline_create(const char *name);

void pvr_counting_fence_timeline_put(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline);

struct PVR_COUNTING_FENCE_TIMELINE *pvr_counting_fence_timeline_get(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline);

struct dma_fence *pvr_counting_fence_create(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline, u64 value);

void pvr_counting_fence_timeline_inc(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline, u64 value);

void pvr_counting_fence_timeline_force_complete(struct PVR_COUNTING_FENCE_TIMELINE *psFenceTimeline);

#endif /* !defined(__PVR_COUNTING_TIMELINE_H__) */
