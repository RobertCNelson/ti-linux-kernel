/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DMABUF Carveout heap helpers
 *
 */

#ifndef _DMA_HEAP_CARVEOUT_H
#define _DMA_HEAP_CARVEOUT_H

#include <linux/types.h>
#include <linux/dma-buf.h>

/**
 * carveout_dma_heap_to_paddr - Get underlying physical addr from dma_buf
 * @dmabuf:	dma_buf to get physical addr from
 */
phys_addr_t carveout_dma_heap_to_paddr(struct dma_buf *dmabuf);

#endif /* _DMA_HEAP_CARVEOUT_H */
