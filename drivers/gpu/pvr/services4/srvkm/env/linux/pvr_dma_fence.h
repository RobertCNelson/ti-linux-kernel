#if !defined(__PVR_DMA_FENCE_H__)
#define __PVR_DMA_FENCE_H__

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,17,0))

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0))
#include <linux/fence.h>

/* Structures */
#define dma_fence fence
#define dma_fence_array fence_array
#define dma_fence_cb fence_cb
#define dma_fence_ops fence_ops

/* Defines and Enums */
#define DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT FENCE_FLAG_ENABLE_SIGNAL_BIT
#define DMA_FENCE_FLAG_SIGNALED_BIT FENCE_FLAG_SIGNALED_BIT
#define DMA_FENCE_FLAG_USER_BITS FENCE_FLAG_USER_BITS

#define DMA_FENCE_ERR FENCE_ERR
#define DMA_FENCE_TRACE FENCE_TRACE
#define DMA_FENCE_WARN FENCE_WARN

/* Functions */
#define dma_fence_add_callback fence_add_callback
#define dma_fence_context_alloc fence_context_alloc
#define dma_fence_default_wait fence_default_wait
#define dma_fence_remove_callback fence_remove_callback
#define dma_fence_is_signaled fence_is_signaled
#define dma_fence_enable_sw_signaling fence_enable_sw_signaling
#define dma_fence_free fence_free
#define dma_fence_get fence_get
#define dma_fence_get_rcu fence_get_rcu
#define dma_fence_init fence_init
#define dma_fence_is_array fence_is_array
#define dma_fence_put fence_put
#define dma_fence_signal fence_signal
#define dma_fence_wait fence_wait
#define to_dma_fence_array to_fence_array

static inline signed long
dma_fence_wait_timeout(struct dma_fence *fence, bool intr, signed long timeout)
{
        signed long lret;

        lret = fence_wait_timeout(fence, intr, timeout);
        if (lret || timeout)
                return lret;

        return test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags) ? 1 : 0;
}

#else
#include <linux/dma-fence.h>
#endif /* (LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)) */

#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(3,17,0)) */

#endif /* !defined(__PVR_DMA_FENCE_H__) */
