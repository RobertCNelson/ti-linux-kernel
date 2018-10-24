/*************************************************************************/ /*!
@File
@Codingstyle    LinuxKernel
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Strictly Confidential.
*/ /**************************************************************************/

#if !defined(__PVR_SW_FENCES_H__)
#define __PVR_SW_FENCES_H__

struct PVR_SW_FENCE_CONTEXT;

struct PVR_SW_FENCE_CONTEXT *pvr_sw_fence_context_create(const char *name, const char *driver_name);
void pvr_sw_fence_context_destroy(struct PVR_SW_FENCE_CONTEXT *psSWFenceCtx);
struct dma_fence *pvr_sw_fence_create(struct PVR_SW_FENCE_CONTEXT *psSWFenceCtx);

#endif /* !defined(__PVR_SW_FENCES_H__) */
