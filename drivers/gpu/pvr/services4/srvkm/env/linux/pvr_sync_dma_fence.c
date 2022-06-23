/*************************************************************************/ /*!
@File           pvr_sync2_dma_fence.c
@Title          Kernel driver for Android's sync mechanism
@Codingstyle    LinuxKernel
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Strictly Confidential.
*/ /**************************************************************************/

#include "pvr_sync2.h"
#include "pvr_sync_common.h"
#include "pvr_sync_user.h"

#include "pvr_counting_timeline.h"

/* FIXME: Proper interface file? */
#include <linux/types.h>
struct sw_sync_create_fence_data {
	__u32 value;
	char name[32];
	__s32 fence;
};
#define SW_SYNC_IOC_MAGIC 'W'
#define SW_SYNC_IOC_CREATE_FENCE \
	(_IOWR(SW_SYNC_IOC_MAGIC, 0, struct sw_sync_create_fence_data))
#define SW_SYNC_IOC_INC _IOW(SW_SYNC_IOC_MAGIC, 1, __u32)

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sync_file.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

#define DEBUG_OUTPUT 1
#ifdef DEBUG_OUTPUT
#define DPF(fmt, ...) PVR_DPF((PVR_DBG_ERROR, "pvr_sync_dma_fence: " fmt "\n", __VA_ARGS__)
#else
#define DPF(fmt, ...) do {} while (0)
#endif

static struct workqueue_struct *gpsWorkQueue;

/* Linux work struct for workqueue. */
static struct work_struct gsWork;

static const struct file_operations pvr_sync_fops;

static bool is_pvr_timeline(struct file *file)
{
	return file->f_op == &pvr_sync_fops;
}

static struct pvr_sync_timeline *pvr_sync_timeline_fget(int fd)
{
	struct file *file = fget(fd);

	if (!file)
		return NULL;

	if (!is_pvr_timeline(file)) {
		fput(file);
		return NULL;
	}

	return file->private_data;
}

struct PVR_ALLOC_SYNC_DATA *PVRSyncAllocFDGet(int fd)
{
	struct file *file = fget(fd);
	if (!file)
		return NULL;

	if (!is_pvr_timeline(file))
	{
		fput(file);
		return NULL;
	}

	return file->private_data;
}

/* ioctl and fops handling */

static int pvr_sync_open(struct inode *inode, struct file *file)
{
	struct pvr_fence_context *fence_context;
	struct pvr_sync_timeline *psTimeline;
	char task_comm[TASK_COMM_LEN];
	int err = -ENOMEM;

	get_task_comm(task_comm, current);

	timeline = kzalloc(sizeof(*timeline), GFP_KERNEL);
	if (!timeline)
		goto err_out;

	strlcpy(timeline->name, task_comm, sizeof(timeline->name));

	PVR_DPF((PVR_DBG_ERROR, "BG: %s: pvr_sync_open ", timeline->name));
	fence_context =  pvr_fence_context_create(timeline->name);
	if (!fence_context) {
		pr_err("pvr_sync2: %s: pvr_fence_context_create failed\n",
			__func__);
		goto err_free_timeline;
	}

	timeline->psSyncInfo = kmalloc(sizeof(struct PVR_SYNC_KERNEL_SYNC_INFO), GFP_KERNEL);
	if(!timeline->psSyncInfo)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate "
								"PVR_SYNC_KERNEL_SYNC_INFO", __func__));
		goto err_free_fence;
	}

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	eError = PVRSRVAllocSyncInfoKM(gsSyncServicesConnection.hDevCookie,
								   gsSyncServicesConnection.hDevMemContext,
								   &timeline->psSyncInfo->psBase);
	LinuxUnLockMutex(&gPVRSRVLock);

	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate timeline syncinfo",
								__func__));
		goto err_free_syncinfo;
	}

	timeline->fence_context = fence_context;
	timeline->file = file;

	file->private_data = timeline;
	err = 0;
err_out:
	return err;
err_free_syncinfo:
	kfree(timeline->psSyncInfo);
err_free_fence:
	pvr_fence_context_destroy(fence_context);
err_free_timeline:
	kfree(timeline);
	goto err_out;
}

static int pvr_sync_close(struct inode *inode, struct file *file)
{
	struct pvr_sync_timeline *timeline = file->private_data;

	if (timeline->sw_timeline)
	{
		/* This makes sure any outstanding SW syncs are marked as
		 * complete at timeline close time. Otherwise it'll leak the
		 * timeline (as outstanding fences hold a ref) and possibly
		 * wedge the system is something is waiting on one of those
		 * fences
		 */
		pvr_counting_fence_timeline_force_complete(timeline->sw_timeline);
		pvr_counting_fence_timeline_put(timeline->sw_timeline);
	}

	pvr_fence_context_destroy(timeline->fence_context);
	kfree(timeline);

	return 0;
}

static long PVRSyncIOCTLCreate(struct pvr_sync_timeline *psTimeline, void __user *pvData)
{
	struct PVR_SYNC_KERNEL_SYNC_INFO *psProvidedSyncInfo = NULL;
	struct PVR_ALLOC_SYNC_DATA *psAllocSyncData;
	struct PVR_SYNC_CREATE_IOCTL_DATA sData;
	int err = -EFAULT, iFd;
	struct sync_fence *psFence;

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
		pr_err("pvr_sync2: %s: Failed to open supplied file fd (%d)\n",
			__func__, new_fence_timeline);
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
	pvr_fence = pvr_fence_create(timeline->fence_context, sData.name, psProvidedSyncInfo);
	if (!pvr_fence) {
		pr_err("pvr_sync2: %s: Failed to create new pvr_fence\n",
			__func__);
		err = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto err_put_fd;
	}

	sync_file = sync_file_create(&pvr_fence->base);
	if (!sync_file) {
		pr_err(FILE_NAME ": %s: Failed to create sync_file\n",
		       __func__);
		err = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto err_destroy_fence;
	}
	fence_put(&pvr_fence->base);

	sData.fence = iFd;

	if (!access_ok(VERIFY_WRITE, pvData, sizeof(sData)))
	{
		goto err_destroy_fence;
	}

	if (copy_to_user(pvData, &sData, sizeof(sData)))
	{
		goto err_destroy_fence;
	}

	DPF("C( ): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X F=%p %s",
		psProvidedSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
		psProvidedSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
		psProvidedSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr,
		psFence, sData.name);

	fd_install(iFd, sync_file->file);
	err = 0;
err_out:
	return err;

err_destroy_fence:
	pvr_fence_destroy(pvr_fence);
err_put_fd:
	put_unused_fd(iFd);
	goto err_out;
}

static long pvr_sync_ioctl_rename(struct pvr_sync_timeline *timeline, void __user *user_data)
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
	strlcpy(timeline->name, data.name, sizeof(timeline->name));

err:
	return err;
}

static long pvr_sync_ioctl_force_sw_only(struct pvr_sync_timeline *timeline,
	void **private_data)
{
	/* Already in SW mode? */
	if (timeline->sw_timeline)
		return 0;
	/* Create a sw_sync timeline with the old GPU timeline's name */
	timeline->sw_timeline = pvr_counting_fence_timeline_create(
		timeline->name);
	if (!timeline->sw_timeline)
		return -ENOMEM;

	return 0;
}

static long pvr_sync_ioctl_sw_create_fence(struct pvr_sync_timeline *timeline,
	void __user *user_data)
{
	struct sw_sync_create_fence_data data;
	struct sync_file *sync_file;
	int fd = get_unused_fd_flags(0);
	struct fence *fence;
	int err = -EFAULT;

	if (fd < 0) {
		pr_err("pvr_sync2: %s: Failed to find unused fd (%d)\n",
		       __func__, fd);
		goto err_out;
	}

	if (copy_from_user(&data, user_data, sizeof(data))) {
		pr_err("pvr_sync2: %s: Failed copy from user", __func__);
		goto err_put_fd;
	}

	fence = pvr_counting_fence_create(timeline->sw_timeline, data.value);
	if (!fence) {
		pr_err("pvr_sync2: %s: Failed to create a sync point (%d)\n",
		       __func__, fd);
		err = -ENOMEM;
		goto err_put_fd;
	}

	sync_file = sync_file_create(fence);
	if (!sync_file) {
		pr_err("pvr_sync2: %s: Failed to create a sync point (%d)\n",
			__func__, fd);
		 err = -ENOMEM;
		goto err_put_fence;
	}

	data.fence = fd;

	if (copy_to_user(user_data, &data, sizeof(data))) {
		pr_err("pvr_sync2: %s: Failed copy to user", __func__);
		goto err_put_fence;
	}

	fd_install(fd, sync_file->file);
	err = 0;
err_out:
	return err;
err_put_fence:
	fence_put(fence);
err_put_fd:
	put_unused_fd(fd);
	goto err_out;
}

static long pvr_sync_ioctl_sw_inc(struct pvr_sync_timeline *timeline,
	void __user *user_data)
{
	u32 value;

	if (copy_from_user(&value, user_data, sizeof(value)))
		return -EFAULT;

	pvr_counting_fence_timeline_inc(timeline->sw_timeline, value);
	return 0;
}

static long
PVRSyncIOCTLAlloc(struct pvr_sync_timeline *psTimeline, void __user *pvData)
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
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate PVR_ALLOC_SYNC_DATA",
								__func__));
		err = -ENOMEM;
		goto err_put_fd;
	}

	psAllocSyncData->psSyncInfo =
		kmalloc(sizeof(struct PVR_SYNC_KERNEL_SYNC_INFO), GFP_KERNEL);
	if (!psAllocSyncData->psSyncInfo)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate "
								"PVR_SYNC_KERNEL_SYNC_INFO", __func__));
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
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to alloc syncinfo (%d)",
								__func__, eError));
		err = -ENOMEM;
		goto err_free_sync_info;
	}

	psFile = anon_inode_getfile("pvr_fence_alloc",
								&pvr_sync_fops, psAllocSyncData, 0);
	if (!psFile)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to create anon inode",
								__func__));
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
		sData.bTimelineIdle = IMG_TRUE;
	else
		sData.bTimelineIdle = IMG_FALSE;

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
pvr_sync_ioctl(struct file *file, unsigned int cmd, unsigned long __user arg)
{
	void __user *user_data = (void __user *)arg;
	long err = -ENOTTY;
	struct pvr_sync_timeline *pvr = file->private_data;
	bool is_sw_timeline = pvr->sw_timeline != NULL;

	if (!is_sw_timeline) {

		switch (cmd) {
		case PVR_SYNC_IOC_CREATE_FENCE:
			err = PVRSyncIOCTLCreate(pvr, user_data);
			break;
		/*case PVR_SYNC_IOC_DEBUG_FENCE:
			err = PVRSyncIOCTLDebug(pvr, user_data);
			break;*/
		case PVR_SYNC_IOC_ALLOC_FENCE:
			err = PVRSyncIOCTLAlloc(pvr, user_data);
			break;
		case PVR_SYNC_IOC_RENAME:
			err = pvr_sync_ioctl_rename(pvr, user_data);
			break;
		case PVR_SYNC_IOC_FORCE_SW_ONLY:
			err = pvr_sync_ioctl_force_sw_only(pvr, &file->private_data);
			break;
		default:
			err = -ENOTTY;
		}
	} else {

		switch (cmd) {
		case SW_SYNC_IOC_CREATE_FENCE:
			err = pvr_sync_ioctl_sw_create_fence(pvr, user_data);
			break;
		case SW_SYNC_IOC_INC:
			err = pvr_sync_ioctl_sw_inc(pvr, user_data);
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
	.open           = pvr_sync_open,
	.release        = pvr_sync_close,
	.unlocked_ioctl = pvr_sync_ioctl,
	.compat_ioctl   = pvr_sync_ioctl,
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

	DPF("%s", __func__);

	if(PVRSyncInitServices() != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to initialise services",
								__func__));
		goto err_out;
	}

	gsSyncServicesConnection.foreign_fence_context = pvr_fence_context_create("foreign_sync");
	if (!pvr_sync_data.foreign_fence_context) {
		pr_err("pvr_sync2: %s: Failed to create foreign sync context\n",
			__func__);
		error = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto err_out;
	}

	gpsWorkQueue = create_freezable_workqueue("pvr_sync_workqueue");
	if(!gpsWorkQueue)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to create pvr_sync workqueue",
								__func__));
		goto err_deinit_services;
	}

	INIT_WORK(&gsWork, PVRSyncWorkQueueFunction);

	err = misc_register(&pvr_sync_device);
	if(err)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to register pvr_sync misc "
								"device (err=%d)", __func__, err));
		goto err_deinit_services;
	}
	error = PVRSRV_OK;

err_out:
	return err;
err_deinit_services:
	pvr_fence_context_destroy(pvr_sync_data.foreign_fence_context);
	PVRSyncCloseServices();
	goto err_out;
}

void PVRSyncDeviceDeInit(void)
{
	DPF("%s", __func__);
	misc_deregister(&pvr_sync_device);
	pvr_fence_context_destroy(pvr_sync_data.foreign_fence_context);
	PVRSyncCloseServices();
}

struct pvr_counting_fence_timeline *pvr_sync_get_sw_timeline(int fd)
{
	struct pvr_sync_timeline *timeline;
	struct pvr_counting_fence_timeline *sw_timeline = NULL;

	timeline = pvr_sync_timeline_fget(fd);
	if (!timeline)
		return NULL;

	sw_timeline = pvr_counting_fence_timeline_get(timeline->sw_timeline);

	pvr_sync_timeline_fput(timeline);
	return sw_timeline;
}

IMG_BOOL
ExpandAndDeDuplicateFenceSyncs(IMG_UINT32 ui32NumSyncs,
							   IMG_HANDLE aiFenceFds[],
							   IMG_UINT32 ui32SyncPointLimit,
							   struct sync_fence *apsFence[],
							   IMG_UINT32 *pui32NumRealSyncs,
							   PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[])
{
	IMG_UINT32 i, j, ui32FenceIndex = 0;
	IMG_BOOL bRet = IMG_TRUE;
	struct sync_pt *psPt;

	*pui32NumRealSyncs = 0;

	for(i = 0; i < ui32NumSyncs; i++)
	{
		PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
		struct pvr_fence *fence;

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
		fence = to_pvr_fence(apsFence[ui32FenceIndex]);
		if(!fence)
		{
			fence = pvr_fence_create_from_fence(gsSyncServicesConnection.foreign_fence_context, apsFence[ui32FenceIndex], "foreign");
			if(fence)
			{
				if(!AddSyncInfoToArray(fence->psSyncData->psSyncInfo, ui32SyncPointLimit,
									   pui32NumRealSyncs, apsSyncInfo))
				{
					/* Soft-fail. Stop synchronizing. */
					goto err_out;
				}
			}
		}
		else
		{
			/* Walk the current list of points and make sure this isn't a
			 * duplicate. Duplicates will deadlock.
			 */
			for(j = 0; j < *pui32NumRealSyncs; j++)
			{
				/* The point is from a different timeline so we must use it */
				if(!PVRSyncIsDuplicate(apsSyncInfo[j], fence->psSyncData->psSyncInfo))
					continue;

				/* There's no need to bump the real sync count as we either
				 * ignored the duplicate or replaced an previously counted
				 * entry.
				 */
				break;
			}
			if(j == *pui32NumRealSyncs)
			{
				/* It's not a duplicate; moving on.. */
				if(!AddSyncInfoToArray(psSyncInfo, ui32SyncPointLimit,
									   pui32NumRealSyncs, apsSyncInfo))
					goto err_out;
			}
		}
		ui32FenceIndex++;
	}

err_out:
	return bRet;
}
