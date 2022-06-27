/*************************************************************************/ /*!
@File           pvr_sync.c
@Title          Kernel driver for Android's sync mechanism
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

#include "pvr_sync_common.h"
#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
#include "pvr_sync.h"
#else
#include "pvr_fence.h"
#endif

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

#include "services_headers.h"
#include "sgxutils.h"
#include "ttrace.h"
#include "mutex.h"
#include "lock.h"

static void
CopyKernelSyncInfoToDeviceSyncObject(PVRSRV_KERNEL_SYNC_INFO *psSyncInfo,
				     PVRSRV_DEVICE_SYNC_OBJECT *psSyncObject)
{
	psSyncObject->sReadOpsCompleteDevVAddr  = psSyncInfo->sReadOpsCompleteDevVAddr;
	psSyncObject->sWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr;
	psSyncObject->sReadOps2CompleteDevVAddr = psSyncInfo->sReadOps2CompleteDevVAddr;
	psSyncObject->ui32WriteOpsPendingVal = psSyncInfo->psSyncData->ui32WriteOpsPending;
	psSyncObject->ui32ReadOpsPendingVal  = psSyncInfo->psSyncData->ui32ReadOpsPending;
	psSyncObject->ui32ReadOps2PendingVal = psSyncInfo->psSyncData->ui32ReadOps2Pending;
}

IMG_BOOL
AddSyncInfoToArray(PVRSRV_KERNEL_SYNC_INFO *psSyncInfo,
				   IMG_UINT32 ui32SyncPointLimit,
				   IMG_UINT32 *pui32NumRealSyncs,
				   PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[])
{
	/* Ran out of syncs. Not much userspace can do about this, since it
	 * could have been passed multiple merged syncs and doesn't know they
	 * were merged. Allow this through, but print a warning and stop
	 * synchronizing.
	 */
	if(*pui32NumRealSyncs == ui32SyncPointLimit)
	{
		PVR_DPF((PVR_DBG_WARNING, "%s: Ran out of source syncs %d == %d",
								  __func__, *pui32NumRealSyncs,
								  ui32SyncPointLimit));
		return IMG_FALSE;
	}

	apsSyncInfo[*pui32NumRealSyncs] = psSyncInfo;
	(*pui32NumRealSyncs)++;
	return IMG_TRUE;
}

IMG_INTERNAL PVRSRV_ERROR
PVRSyncPatchCCBKickSyncInfos(IMG_HANDLE    ahSyncs[SGX_MAX_SRC_SYNCS_TA],
		      PVRSRV_DEVICE_SYNC_OBJECT asDevSyncs[SGX_MAX_SRC_SYNCS_TA],
							 IMG_UINT32 *pui32NumSrcSyncs)
{
	PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[SGX_MAX_SRC_SYNCS_TA];
#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	struct sync_fence *apsFence[SGX_MAX_SRC_SYNCS_TA] = {};
#else  /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_FENCE) */
	struct dma_fence *apsFence[SGX_MAX_SRC_SYNCS_TA] = {};
#endif
	IMG_UINT32 i, ui32NumRealSrcSyncs;
	PVRSRV_ERROR eError = PVRSRV_OK;

	if(!ExpandAndDeDuplicateFenceSyncs(*pui32NumSrcSyncs,
									   (IMG_HANDLE *)ahSyncs,
									   SGX_MAX_SRC_SYNCS_TA,
									   apsFence,
									   &ui32NumRealSrcSyncs,
									   apsSyncInfo))
	{
		eError = PVRSRV_ERROR_HANDLE_NOT_FOUND;
		goto err_put_fence;
	}

	/* There should only be one destination sync for a transfer.
	 * Ultimately this will be patched to two (the sync_pt SYNCINFO,
	 * and the timeline's SYNCINFO for debugging).
	 */
	for(i = 0; i < ui32NumRealSrcSyncs; i++)
	{
		PVRSRV_KERNEL_SYNC_INFO *psSyncInfo = apsSyncInfo[i];

		/* The following code is mostly the same as the texture dependencies
		 * handling in SGXDoKickKM, but we have to copy it here because it
		 * must be run while the fence is 'locked' by sync_fence_fdget.
		 */

		PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_KICK, KICK_TOKEN_SRC_SYNC,
				       psSyncInfo, PVRSRV_SYNCOP_SAMPLE);

		CopyKernelSyncInfoToDeviceSyncObject(psSyncInfo, &asDevSyncs[i]);

		/* Texture dependencies are read operations */
		psSyncInfo->psSyncData->ui32ReadOpsPending++;

		/* Finally, patch the sync back into the input array.
		 * NOTE: The syncs are protected here by the defer-free worker.
		 */
		ahSyncs[i] = psSyncInfo;
	}

	/* Updating this allows the PDUMP handling and ROP rollbacks to work
	 * correctly in SGXDoKickKM.
	 */
	*pui32NumSrcSyncs = ui32NumRealSrcSyncs;

err_put_fence:
	for(i = 0; i < SGX_MAX_SRC_SYNCS_TA && apsFence[i]; i++)
#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
		sync_fence_put(apsFence[i]);
#else
		dma_fence_put(apsFence[i]);
#endif
	return eError;
}

/* Patching for TQ fence in queueBuffer() */
IMG_INTERNAL PVRSRV_ERROR
PVRSyncPatchTransferSyncInfos(IMG_HANDLE    ahSyncs[SGX_MAX_SRC_SYNCS_TA],
			      PVRSRV_DEVICE_SYNC_OBJECT asDevSyncs[SGX_MAX_SRC_SYNCS_TA],
							     IMG_UINT32 *pui32NumSrcSyncs)
{
	struct PVR_ALLOC_SYNC_DATA *psTransferSyncData;
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (*pui32NumSrcSyncs != 1)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Invalid number of syncs (%d), clamping "
								"to 1", __func__, *pui32NumSrcSyncs));
	}

	psTransferSyncData = PVRSyncAllocFDGet((int)ahSyncs[0]);

	if (!psTransferSyncData)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to get PVR_SYNC_DATA from "
								"supplied fd", __func__));
		eError = PVRSRV_ERROR_HANDLE_NOT_FOUND;
		goto err_out;
	}

	/* There should only be one destination sync for a transfer.
	 * Ultimately this will be patched to two (the sync_pt SYNCINFO,
	 * and the timeline's SYNCINFO for debugging).
	 */
	psSyncInfo = psTransferSyncData->psSyncInfo->psBase;

	/* The following code is mostly the same as the texture dependencies
	 * handling in SGXDoKickKM, but we have to copy it here because it
	 * must be run while the fence is 'locked' by sync_fence_fdget.
	 */

	PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_TRANSFER, TRANSFER_TOKEN_SRC_SYNC,
			       psSyncInfo, PVRSRV_SYNCOP_SAMPLE);

	CopyKernelSyncInfoToDeviceSyncObject(psSyncInfo, &asDevSyncs[0]);
	CopyKernelSyncInfoToDeviceSyncObject(psTransferSyncData->psTimeline->psSyncInfo->psBase,
					     &asDevSyncs[1]);

	/* Treat fence TQs as write operations */
	psSyncInfo->psSyncData->ui32WriteOpsPending++;
	psTransferSyncData->psTimeline->psSyncInfo->psBase->psSyncData->ui32WriteOpsPending++;

	/* Finally, patch the sync back into the input array.
	 * NOTE: The syncs are protected here by the defer-free worker.
	 */
	ahSyncs[0] = psSyncInfo;
	ahSyncs[1] = psTransferSyncData->psTimeline->psSyncInfo->psBase;

	/* Updating this allows the PDUMP handling and ROP rollbacks to work
	 * correctly in SGXDoKickKM.
	 */
	*pui32NumSrcSyncs = 2;

	fput(psTransferSyncData->psFile);
err_out:
	return eError;
}


/* NOTE: This returns an array of sync_fences which need to be 'put'
 *       or they will leak.
 */
/* Display side patching */
IMG_INTERNAL PVRSRV_ERROR
PVRSyncFencesToSyncInfos(PVRSRV_KERNEL_SYNC_INFO *apsSyncs[],
						 IMG_UINT32 *pui32NumSyncs,
#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
						 struct sync_fence *apsFence[SGX_MAX_SRC_SYNCS_TA]
#else /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_FENCE) */
						 struct dma_fence *apsFence[SGX_MAX_SRC_SYNCS_TA]
#endif
						 )
{
	PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[SGX_MAX_SRC_SYNCS_TA];
	IMG_UINT32 i, ui32NumRealSrcSyncs;
	PVRSRV_ERROR eError = PVRSRV_OK;

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	memset(apsFence, 0, sizeof(struct sync_fence *) * SGX_MAX_SRC_SYNCS_TA);
#else /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_FENCE) */
	memset(apsFence, 0, sizeof(struct dma_fence *) * SGX_MAX_SRC_SYNCS_TA);
#endif

	if(!ExpandAndDeDuplicateFenceSyncs(*pui32NumSyncs,
									   (IMG_HANDLE *)apsSyncs,
									   *pui32NumSyncs,
									   apsFence,
									   &ui32NumRealSrcSyncs,
									   apsSyncInfo))
	{
		for(i = 0; i < SGX_MAX_SRC_SYNCS_TA && apsFence[i]; i++)
#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
			sync_fence_put(apsFence[i]);
#else /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_FENCE) */
			dma_fence_put(apsFence[i]);
#endif

		return PVRSRV_ERROR_HANDLE_NOT_FOUND;
	}

	/* We don't expect to see merged syncs here. Abort if that happens.
	 * Allow through cases where the same fence was specified more than
	 * once -- we can handle that without reallocation of memory.
	 */
	PVR_ASSERT(ui32NumRealSrcSyncs <= *pui32NumSyncs);

	for(i = 0; i < ui32NumRealSrcSyncs; i++)
		apsSyncs[i] = apsSyncInfo[i];

	*pui32NumSyncs = ui32NumRealSrcSyncs;
	//PVR_DPF((PVR_DBG_ERROR, "%s END HERE", __func__));
	return eError;
}

/*PVRSRV_ERROR PVRSyncInitServices(void)
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
}*/
