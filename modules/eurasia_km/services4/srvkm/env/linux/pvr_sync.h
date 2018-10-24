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

/* This is the IMG extension of a sync_timeline */

struct PVR_SYNC_TIMELINE
{
	struct sync_timeline				obj;

	/* Needed to keep a global list of all timelines for MISR checks. */
	struct list_head					sTimelineList;

	/* True if a sync point on the timeline has signaled */
	IMG_BOOL							bSyncHasSignaled;

	/* A mutex, as we want to ensure that the comparison (and possible
	 * reset) of the highest SW fence value is atomic with the takeop,
	 * so both the SW fence value and the WOP snapshot should both have
	 * the same order for all SW syncs.
	 *
	 * This mutex also protects modifications to the fence stamp counter.
	 */
	struct mutex						sTimelineLock;

	/* Every timeline has a services sync object. This object must not
	 * be used by the hardware to enforce ordering -- that's what the
	 * per sync-point objects are for. This object is attached to every
	 * TQ scheduled on the timeline and is primarily useful for debugging.
	 */
	struct PVR_SYNC_KERNEL_SYNC_INFO	*psSyncInfo;
};

/* A PVR_SYNC_DATA is the basic guts of a sync point. It's kept separate
 * because sync points can be dup'ed, and we don't want to duplicate all
 * of the shared metadata.
 *
 * This is also used to back an allocated sync info, which can be passed to
 * the CREATE ioctl to insert the fence and add it to the timeline. This is
 * used as an intermediate step as a PVRSRV_KERNEL_SYNC_INFO is needed to
 * attach to the transfer task used as a fence in the hardware.
 */

struct PVR_SYNC_DATA
{
	/* Every sync point has a services sync object. This object is used
	 * by the hardware to enforce ordering -- it is attached as a source
	 * dependency to various commands.
	 */
	struct PVR_SYNC_KERNEL_SYNC_INFO	*psSyncInfo;

	/* This refcount is incremented at create and dup time, and decremented
	 * at free time. It ensures the object doesn't start the defer-free
	 * process until it is no longer referenced.
	 */
	atomic_t							sRefcount;

	/* This is purely a debug feature. Record the WOP snapshot from the
	 * timeline synchronization object when a new fence is created.
	 */
	IMG_UINT32							ui32WOPSnapshot;

	/* This is a globally unique ID for the sync point. If a sync point is
	 * duplicated, its stamp is copied over (seems counter-intuitive, but in
	 * nearly all cases a sync point is merged with another, the original
	 * is freed).
	 */
	IMG_UINT64							ui64Stamp;
};

/* This is the IMG extension of a sync_pt */

struct PVR_SYNC
{
	struct sync_pt			pt;
	struct PVR_SYNC_DATA	*psSyncData;
};

struct PVR_SYNC_FENCE
{
	/* Base sync_fence structure */
	struct sync_fence	*psBase;

	/* To ensure callbacks are always received for fences / sync_pts, even
	 * after the fence has been 'put' (freed), we must take a reference to
	 * the fence. We still need to 'put' the fence ourselves, but this might
	 * happen in irq context, where fput() is not allowed (in kernels <3.6).
	 * We must add the fence to a list which is processed in WQ context.
	 */
	struct list_head	sHead;
};

/* Any sync point from a foreign (non-PVR) timeline needs to have a "shadow"
 * syncinfo. This is modelled as a software operation. The foreign driver
 * completes the operation by calling a callback we registered with it.
 *
 * Because we are allocating SYNCINFOs for each sync_pt, rather than each
 * fence, we need to extend the waiter struct slightly to include the
 * necessary metadata.
 */
struct PVR_SYNC_FENCE_WAITER
{
	/* Base sync driver waiter structure */
	struct sync_fence_waiter			sWaiter;

	/* "Shadow" syncinfo backing the foreign driver's sync_pt */
	struct PVR_SYNC_KERNEL_SYNC_INFO	*psSyncInfo;

	/* Optimizes lookup of fence for defer-put operation */
	struct PVR_SYNC_FENCE				*psSyncFence;
};

/* Local wrapper around PVRSRV_KERNEL_SYNC_INFO to add a list head */

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
	struct file							*psFile;
};


IMG_BOOL
ExpandAndDeDuplicateFenceSyncs(IMG_UINT32 ui32NumSyncs,
							   IMG_HANDLE aiFenceFds[],
							   IMG_UINT32 ui32SyncPointLimit,
							   struct sync_fence *apsFence[],
							   IMG_UINT32 *pui32NumRealSyncs,
							   PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[]);

PVRSRV_ERROR PVRSyncInitServices(void);
void PVRSyncCloseServices(void);

struct PVR_ALLOC_SYNC_DATA *PVRSyncAllocFDGet(int fd);

