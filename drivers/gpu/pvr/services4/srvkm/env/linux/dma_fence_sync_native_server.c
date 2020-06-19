/*************************************************************************/ /*!
@File           sync_native_server.c
@Title          Native implementation of server fence sync interface.
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    The server implementation of software native synchronisation.
@License        Strictly Confidential.
*/ /**************************************************************************/

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/sync_file.h>
#include <linux/version.h>

#include "img_types.h"
#include "services_headers.h"
#include "servicesext.h"
#include "pvrsrv_sync_server.h"
#include "pvr_fence.h"
#include "pvr_counting_timeline.h"

struct dma_fence* SyncSWTimelineFenceCreateKM(IMG_INT32 iSWTimeline,
					 IMG_UINT32 ui32NextSyncPtValue,
					 const IMG_CHAR *pszFenceName)
{
	PVRSRV_ERROR eError;
	struct PVR_COUNTING_FENCE_TIMELINE *psSWTimeline;
	struct dma_fence *psFence = NULL;

	psSWTimeline = pvr_sync_get_sw_timeline(iSWTimeline);
	if (!psSWTimeline)
	{
		/* unrecognised timeline */
		PVR_DPF((PVR_DBG_ERROR, "%s: unrecognised timeline", __func__));
		goto ErrorOut;
	}

	psFence = pvr_counting_fence_create(psSWTimeline, ui32NextSyncPtValue);
	pvr_counting_fence_timeline_put(psSWTimeline);
	if(!psFence)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorPutFence;
	}

	return psFence;

ErrorPutFence:
	dma_fence_put(psFence);
ErrorOut:
	return IMG_NULL;
}

PVRSRV_ERROR SyncSWTimelineAdvanceKM(IMG_PVOID pvSWTimeline)
{
	pvr_counting_fence_timeline_inc(pvSWTimeline, 1);
	return PVRSRV_OK;
}

PVRSRV_ERROR SyncSWTimelineReleaseKM(IMG_PVOID pvSWTimeline)
{
	pvr_counting_fence_timeline_put(pvSWTimeline);
	return PVRSRV_OK;
}

PVRSRV_ERROR SyncSWTimelineFenceReleaseKM(IMG_PVOID pvSWFenceObj)
{
	dma_fence_put(pvSWFenceObj);
	return PVRSRV_OK;
}

PVRSRV_ERROR SyncSWGetTimelineObj(IMG_INT32 i32SWTimeline, IMG_PVOID *ppvSWTimelineObj)
{
	struct PVR_COUNTING_FENCE_TIMELINE *psSwTimeline = pvr_sync_get_sw_timeline(i32SWTimeline);

	if (psSwTimeline == NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	*ppvSWTimelineObj = psSwTimeline;
	return PVRSRV_OK;
}

PVRSRV_ERROR SyncSWGetFenceObj(IMG_INT32 i32SWFence, IMG_PVOID *ppvSWFenceObj)
{
	struct dma_fence *psFence = sync_file_get_fence(i32SWFence);

	if(psFence == NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	*ppvSWFenceObj = (IMG_PVOID*)psFence;
	return PVRSRV_OK;
}
