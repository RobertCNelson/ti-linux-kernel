#ifndef _PVRSRV_SYNC_SERVER_H_
#define _PVRSRV_SYNC_SERVER_H_

#include "img_types.h"

#define SYNC_SW_TIMELINE_MAX_LENGTH 32
#define SYNC_SW_FENCE_MAX_LENGTH 32

/*****************************************************************************/
/*                                                                           */
/*                      SW TIMELINE SPECIFIC FUNCTIONS                       */
/*                                                                           */
/*****************************************************************************/

struct dma_fence* SyncSWTimelineFenceCreateKM(IMG_INT32 iSWTimeline,
					IMG_UINT32 ui32NextSyncPtVal,
					const IMG_CHAR *pszFenceName);

PVRSRV_ERROR SyncSWTimelineAdvanceKM(IMG_PVOID pvSWTimelineObj);

PVRSRV_ERROR SyncSWTimelineReleaseKM(IMG_PVOID pvSWTimelineObj);

PVRSRV_ERROR SyncSWTimelineFenceReleaseKM(IMG_PVOID i32SWFenceObj);

PVRSRV_ERROR SyncSWGetTimelineObj(IMG_INT32 iSWTimeline, IMG_PVOID *ppvSWTimelineObj);

PVRSRV_ERROR SyncSWGetFenceObj(IMG_INT32 iSWFence, IMG_PVOID *ppvSWFenceObj);

#endif /* _PVRSRV_SYNC_SERVER_H_ */
