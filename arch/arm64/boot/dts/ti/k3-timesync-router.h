/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * This header provides headers for Timesync Router
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef DTS_ARM64_TI_K3_TIMESYNC_ROUTER
#define DTS_ARM64_TI_K3_TIMESYNC_ROUTER

/*
 * The value of the input to be mapped to an output has to be written in
 * the register corresponding to the output.
 */
#define K3_TS_OFFSET(output_index, mask, input_index)   ((0x4 + ((output_index) * 4))) (mask) (input_index)

#endif /* DTS_ARM64_TI_K3_TIMESYNC_ROUTER */
