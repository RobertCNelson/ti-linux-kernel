/*
 *  Port on Texas Instruments TMS320C6x architecture
 *
 *  Copyright (C) 2004, 2009, 2010, 2011 Texas Instruments Incorporated
 *  Author: Aurelien Jacquiot <aurelien.jacquiot@ti.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 */
#ifndef _ASM_C6X_DMA_MAPPING_H
#define _ASM_C6X_DMA_MAPPING_H

/*
 * DMA errors are defined by all-bits-set in the DMA address.
 */
#define DMA_ERROR_CODE ~0

extern struct dma_map_ops c6x_dma_ops;

static inline struct dma_map_ops *get_dma_ops(struct device *dev)
{
	return &c6x_dma_ops;
}

#include <asm-generic/dma-mapping-common.h>

#endif	/* _ASM_C6X_DMA_MAPPING_H */
