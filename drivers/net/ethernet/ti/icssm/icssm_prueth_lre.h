/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2020 Texas Instruments Incorporated - http://www.ti.com
 */

#ifndef __NET_TI_PRUETH_LRE_H
#define __NET_TI_PRUETH_LRE_H

#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/if_vlan.h>

#include "icssm_prueth.h"
#include "icssm_lre_firmware.h"

#define PRUETH_MAX_PKTLEN_LRE		(VLAN_ETH_FRAME_LEN +  ETH_FCS_LEN + \
					 ICSS_LRE_TAG_RCT_SIZE)
#define LRE_SV_FRAME_OFFSET			20

/* Link Redundancy Entity stats counters */
struct lre_statistics {
	u32 lre_tx_a;
	u32 lre_tx_b;
	u32 lre_tx_c;

	u32 lre_errwronglan_a;
	u32 lre_errwronglan_b;
	u32 lre_errwronglan_c;

	u32 lre_rx_a;
	u32 lre_rx_b;
	u32 lre_rx_c;

	u32 lre_errors_a;
	u32 lre_errors_b;
	u32 lre_errors_c;

	u32 lre_nodes;
	u32 lre_proxy_nodes;

	u32 lre_unique_rx_a;
	u32 lre_unique_rx_b;
	u32 lre_unique_rx_c;

	u32 lre_duplicate_rx_a;
	u32 lre_duplicate_rx_b;
	u32 lre_duplicate_rx_c;

	u32 lre_multiple_rx_a;
	u32 lre_multiple_rx_b;
	u32 lre_multiple_rx_c;

	u32 lre_own_rx_a;
	u32 lre_own_rx_b;

	u32 duplicate_discard;
	u32 transparent_reception;

	u32 lre_node_table_full;
	u32 lre_multicast_dropped;
	u32 lre_vlan_dropped;
	u32 lre_intr_tmr_exp;

	/* additional debug counters */
	u32 lre_total_rx_a; /* count of all frames received at port-A */
	u32 lre_total_rx_b; /* count of all frames received at port-B */
	u32 lre_overflow_pru0; /* count of overflow frames to host on PRU 0 */
	u32 lre_overflow_pru1; /* count of overflow frames to host on PRU 1 */
	u32 lre_cnt_dd_pru0; /* count of DD frames to host on PRU 0 */
	u32 lre_cnt_dd_pru1; /* count of DD frames to host on PRU 1 */
	u32 lre_cnt_sup_pru0; /* count of supervisor frames to host on PRU 0 */
	u32 lre_cnt_sup_pru1; /* count of supervisor frames to host on PRU 1 */

	/* count of frames with no RCT tag to host on PRU 0 */
	u32 lre_no_prp_tag_rx_a;
	/* count of frames with no RCT tag to host on PRU 1 */
	u32 lre_no_prp_tag_rx_b;
	/* count of port overflow frames to port on PRU 0 */
	u32 lre_fwd_overflow_pru0;
	/* count of port overflow frames to port on PRU 1 */
	u32 lre_fwd_overflow_pru1;
} __packed;

/* separate counters in shared memory for PRUs */
struct lre_statistics_pru1 {
	u32 lre_tx_cb;
	u32 lre_unique_rx_cb;
	u32 lre_duplicate_rx_cb;
	u32 lre_multicast_dropped_b;
	u32 lre_vlan_dropped_b;
};


void icssm_prueth_lre_config(struct prueth *prueth);
void icssm_prueth_lre_cleanup(struct prueth *prueth);
int icssm_prueth_lre_get_sset_count(struct prueth *prueth);
void icssm_prueth_lre_get_strings(struct prueth *prueth, u8 *data);
void icssm_prueth_lre_update_stats(struct prueth_emac *emac, u64 *data);
void icssm_prueth_lre_set_stats(struct prueth *prueth,
				struct lre_statistics *pstats);
void icssm_prueth_lre_get_stats(struct prueth *prueth,
				struct lre_statistics *pstats);
void icssm_lre_update_hardware_stats(struct prueth_emac *emac);
void icssm_prueth_lre_config_check_flags(struct prueth *prueth);
void icssm_prueth_lre_free_memory(struct prueth *prueth);


extern const struct lredev_ops icssm_prueth_lredev_ops;

#endif /* __NET_TI_PRUETH_LRE_H */
