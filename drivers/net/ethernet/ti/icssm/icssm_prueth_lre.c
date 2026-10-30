// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments PRUETH hsr/prp Link Redundancy Entity (LRE) Driver.
 *
 * Copyright (C) 2020 Texas Instruments Incorporated - http://www.ti.com
 */

#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/spinlock_types.h>
#include <linux/hrtimer.h>

#include "icssm_lre_firmware.h"
#include "icssm_prueth.h"
#include "icssm_prueth_lre.h"
#include "icssm_prueth_switch.h"

void icssm_prueth_lre_config_check_flags(struct prueth *prueth)
{
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	/* HSR/PRP: initialize check table when first port is up */
	if (prueth->emac_configured)
		return;

	prueth->tbl_check_mask = ICSS_LRE_HOST_TIMER_HOST_TABLE_CHECK_BIT;
	if (PRUETH_IS_HSR(prueth))
		prueth->tbl_check_mask |=
			ICSS_LRE_HOST_TIMER_PORT_TABLE_CHECK_BITS;
	writel(prueth->tbl_check_mask, dram1 + ICSS_LRE_HOST_TIMER_CHECK_FLAGS);
}

/* A group of PCPs are mapped to a Queue. This is the size of firmware
 * array in shared memory
 */
#define PCP_GROUP_TO_QUEUE_MAP_SIZE	4

/* PRU firmware default PCP to priority Queue map for ingress & egress
 *
 * At ingress to Host
 * ==================
 * byte 0 => PRU 1, PCP 0-3 => Q3
 * byte 1 => PRU 1, PCP 4-7 => Q2
 * byte 2 => PRU 0, PCP 0-3 => Q1
 * byte 3 => PRU 0, PCP 4-7 => Q0
 *
 * At egress to wire/network on PRU-0 and PRU-1
 * ============================================
 * byte 0 => Host, PCP 0-3 => Q3
 * byte 1 => Host, PCP 4-7 => Q2
 *
 * PRU-0
 * -----
 * byte 2 => PRU-1, PCP 0-3 => Q1
 * byte 3 => PRU-1, PCP 4-7 => Q0
 *
 * PRU-1
 * -----
 * byte 2 => PRU-0, PCP 0-3 => Q1
 * byte 3 => PRU-0, PCP 4-7 => Q0
 *
 * queue names below are named 1 based. i.e PRUETH_QUEUE1 is Q0,
 * PRUETH_QUEUE2 is Q1 and so forth. Firmware convention is that
 * a lower queue number has higher priority than a higher queue
 * number.
 */
static u8 fw_pcp_default_priority_queue_map[PCP_GROUP_TO_QUEUE_MAP_SIZE] = {
	/* port 2 or PRU 1 */
	PRUETH_QUEUE4, PRUETH_QUEUE3,
	/* port 1 or PRU 0 */
	PRUETH_QUEUE2, PRUETH_QUEUE1,
};

static void icssm_prueth_lre_pcp_queue_map_config(struct prueth *prueth)
{
	void __iomem *sram  = prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	memcpy_toio(sram + ICSS_LRE_QUEUE_2_PCP_MAP_OFFSET,
		    &fw_pcp_default_priority_queue_map[0],
		    PCP_GROUP_TO_QUEUE_MAP_SIZE);
}

static void icssm_prueth_lre_host_table_init(struct prueth *prueth)
{
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	memset_io(dram0 + ICSS_LRE_DUPLICATE_HOST_TABLE, 0,
		  ICSS_LRE_DUPLICATE_HOST_TABLE_DMEM_SIZE);

	writel(ICSS_LRE_DUPLICATE_HOST_TABLE_SIZE_INIT,
	       dram1 + ICSS_LRE_DUPLICATE_HOST_TABLE_SIZE);

	writel(ICSS_LRE_TABLE_CHECK_RESOLUTION_10_MS,
	       dram1 + ICSS_LRE_DUPLI_HOST_CHECK_RESO);

	writel(ICSS_LRE_MASTER_SLAVE_BUSY_BITS_CLEAR,
	       dram1 + ICSS_LRE_HOST_DUPLICATE_ARBITRATION);
}

static void icssm_prueth_lre_port_table_init(struct prueth *prueth)
{
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	if (PRUETH_IS_HSR(prueth)) {
		memset_io(dram1 + ICSS_LRE_DUPLICATE_PORT_TABLE_PRU0, 0,
			  ICSS_LRE_DUPLICATE_PORT_TABLE_DMEM_SIZE);
		memset_io(dram1 + ICSS_LRE_DUPLICATE_PORT_TABLE_PRU1, 0,
			  ICSS_LRE_DUPLICATE_PORT_TABLE_DMEM_SIZE);

		writel(ICSS_LRE_DUPLICATE_PORT_TABLE_SIZE_INIT,
		       dram1 + ICSS_LRE_DUPLICATE_PORT_TABLE_SIZE);
	} else {
		writel(0, dram1 + ICSS_LRE_DUPLICATE_PORT_TABLE_SIZE);
	}

	writel(ICSS_LRE_TABLE_CHECK_RESOLUTION_10_MS,
	       dram1 + ICSS_LRE_DUPLI_PORT_CHECK_RESO);
}

static void icssm_prueth_lre_init(struct prueth *prueth)
{
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	memset_io(sram + ICSS_LRE_START, 0, ICSS_LRE_STATS_DMEM_SIZE);

	writel(ICSS_LRE_IEC62439_CONST_DUPLICATE_DISCARD,
	       sram + ICSS_LRE_DUPLICATE_DISCARD);
	writel(ICSS_LRE_IEC62439_CONST_TRANSP_RECEPTION_REMOVE_RCT,
	       sram + ICSS_LRE_TRANSPARENT_RECEPTION);
	prueth->prp_tr_mode = IEC62439_3_TR_REMOVE_RCT;
}

static void icssm_prueth_lre_dbg_init(struct prueth *prueth)
{
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;

	memset_io(dram0 + ICSS_LRE_DBG_START, 0,
		  ICSS_LRE_DEBUG_COUNTER_DMEM_SIZE);
}

static void icssm_prueth_lre_protocol_init(struct prueth *prueth)
{
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	if (PRUETH_IS_HSR(prueth))
		writew(prueth->hsr_mode, dram0 + ICSS_LRE_HSR_MODE);

	writel(ICSS_LRE_DUPLICATE_FORGET_TIME_400_MS,
	       dram1 + ICSS_LRE_DUPLI_FORGET_TIME);
	writel(ICSS_LRE_SUP_ADDRESS_INIT_OCTETS_HIGH,
	       dram1 + ICSS_LRE_SUP_ADDR);
	writel(ICSS_LRE_SUP_ADDRESS_INIT_OCTETS_LOW,
	       dram1 + ICSS_LRE_SUP_ADDR_LOW);
}

static void icssm_prueth_lre_config_packet_timestamping(struct prueth *prueth)
{
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	writeb(1, sram + ICSS_LRE_PRIORITY_INTRS_STATUS_OFFSET);
	writeb(1, sram + ICSS_LRE_TIMESTAMP_PKTS_STATUS_OFFSET);
}


static enum hrtimer_restart icssm_prueth_lre_timer(struct hrtimer *timer)
{
	struct prueth *prueth = container_of(timer, struct prueth,
					     tbl_check_timer);
	unsigned int timeout = PRUETH_TIMER_MS;
	void __iomem *dram =  prueth->mem[PRUETH_MEM_DRAM1].va;

	hrtimer_forward_now(timer, ms_to_ktime(timeout));
	if (prueth->emac_configured !=
	    (BIT(PRUETH_PORT_MII0) | BIT(PRUETH_PORT_MII1)))
		return HRTIMER_RESTART;

	writel(prueth->tbl_check_mask, dram + ICSS_LRE_HOST_TIMER_CHECK_FLAGS);

	return HRTIMER_RESTART;
}

static void icssm_prueth_lre_init_timer(struct prueth *prueth)
{
	hrtimer_setup(&prueth->tbl_check_timer, icssm_prueth_lre_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
}

static void icssm_prueth_lre_start_timer(struct prueth *prueth)
{
	unsigned int timeout = PRUETH_TIMER_MS;

	if (hrtimer_active(&prueth->tbl_check_timer))
		return;

	hrtimer_start(&prueth->tbl_check_timer, ms_to_ktime(timeout),
		      HRTIMER_MODE_REL);
}

void icssm_prueth_lre_config(struct prueth *prueth)
{
	if (PRUETH_IS_HSR(prueth))
		prueth->hsr_mode = ICSS_LRE_MODEH;

	icssm_prueth_lre_init_timer(prueth);
	icssm_prueth_lre_start_timer(prueth);
	icssm_prueth_lre_pcp_queue_map_config(prueth);
	icssm_prueth_lre_host_table_init(prueth);
	icssm_prueth_lre_port_table_init(prueth);
	icssm_prueth_lre_init(prueth);
	icssm_prueth_lre_dbg_init(prueth);
	icssm_prueth_lre_protocol_init(prueth);
	/* for HSR/PRP LRE driver order the frames based on
	 * packet timestamp.
	 */
	icssm_prueth_lre_config_packet_timestamping(prueth);
}

void icssm_prueth_lre_cleanup(struct prueth *prueth)
{
	hrtimer_cancel(&prueth->tbl_check_timer);
}



#define PRUETH_LRE_STAT_OFFSET(m, stats_type)   \
{                                               \
	#m,                                     \
	offsetof(struct lre_statistics, m),    \
	stats_type                              \
}

struct icssm_prueth_lre_stats {
	char string[ETH_GSTRING_LEN];
	u32 offset;
	bool standard_stats;
};

static const struct icssm_prueth_lre_stats prueth_ethtool_lre_stats[] = {
	PRUETH_LRE_STAT_OFFSET(lre_tx_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_tx_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_tx_c, false),

	PRUETH_LRE_STAT_OFFSET(lre_errwronglan_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_errwronglan_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_errwronglan_c, false),

	PRUETH_LRE_STAT_OFFSET(lre_rx_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_rx_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_rx_c, false),

	PRUETH_LRE_STAT_OFFSET(lre_errors_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_errors_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_errors_c, false),

	PRUETH_LRE_STAT_OFFSET(lre_nodes, false),
	PRUETH_LRE_STAT_OFFSET(lre_proxy_nodes, false),

	PRUETH_LRE_STAT_OFFSET(lre_unique_rx_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_unique_rx_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_unique_rx_c, false),

	PRUETH_LRE_STAT_OFFSET(lre_duplicate_rx_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_duplicate_rx_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_duplicate_rx_c, false),

	PRUETH_LRE_STAT_OFFSET(lre_multiple_rx_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_multiple_rx_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_multiple_rx_c, false),

	PRUETH_LRE_STAT_OFFSET(lre_own_rx_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_own_rx_b, false),

	PRUETH_LRE_STAT_OFFSET(duplicate_discard, false),
	PRUETH_LRE_STAT_OFFSET(transparent_reception, false),

	PRUETH_LRE_STAT_OFFSET(lre_node_table_full, false),
	PRUETH_LRE_STAT_OFFSET(lre_multicast_dropped, false),
	PRUETH_LRE_STAT_OFFSET(lre_vlan_dropped, false),
	PRUETH_LRE_STAT_OFFSET(lre_intr_tmr_exp, false),
	PRUETH_LRE_STAT_OFFSET(lre_total_rx_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_total_rx_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_overflow_pru0, false),
	PRUETH_LRE_STAT_OFFSET(lre_overflow_pru1, false),
	PRUETH_LRE_STAT_OFFSET(lre_cnt_dd_pru0, false),
	PRUETH_LRE_STAT_OFFSET(lre_cnt_dd_pru1, false),
	PRUETH_LRE_STAT_OFFSET(lre_cnt_sup_pru0, false),
	PRUETH_LRE_STAT_OFFSET(lre_cnt_sup_pru1, false),

	PRUETH_LRE_STAT_OFFSET(lre_no_prp_tag_rx_a, false),
	PRUETH_LRE_STAT_OFFSET(lre_no_prp_tag_rx_b, false),
	PRUETH_LRE_STAT_OFFSET(lre_fwd_overflow_pru0, false),
	PRUETH_LRE_STAT_OFFSET(lre_fwd_overflow_pru1, false),
};

void icssm_prueth_lre_set_stats(struct prueth *prueth,
				struct lre_statistics *pstats)
{
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	if (prueth->emac_configured)
		return;

	/* These two are actually not statistics, so keep original */
	pstats->duplicate_discard = readl(sram + ICSS_LRE_DUPLICATE_DISCARD);
	pstats->transparent_reception =
		readl(sram + ICSS_LRE_TRANSPARENT_RECEPTION);
	memcpy_toio(sram + ICSS_LRE_START + 4, pstats, sizeof(*pstats));
}

void icssm_prueth_lre_get_stats(struct prueth *prueth,
				struct lre_statistics *pstats)
{
	struct lre_statistics_pru1 *pstats_pru1;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	memcpy_fromio((void *)pstats, sram + ICSS_LRE_CNT_TX_A,
		      sizeof(*pstats));

	/* maintains separate counters in shared memory for PRUs */
	pstats_pru1 = kmalloc(sizeof(*pstats_pru1), GFP_KERNEL);

	if (pstats_pru1) {
		memcpy_fromio((void *)pstats_pru1,
			      sram + ICSS_LRE_START + ICSS_LRE_STATS_DMEM_SIZE,
			      sizeof(*pstats_pru1));
		pstats->lre_tx_c += pstats_pru1->lre_tx_cb;
		pstats->lre_unique_rx_c += pstats_pru1->lre_unique_rx_cb;
		pstats->lre_duplicate_rx_c +=
			pstats_pru1->lre_duplicate_rx_cb;
		pstats->lre_multicast_dropped +=
			pstats_pru1->lre_multicast_dropped_b;
		pstats->lre_vlan_dropped += pstats_pru1->lre_vlan_dropped_b;
		kfree(pstats_pru1);
	}
}

int icssm_prueth_lre_get_sset_count(struct prueth *prueth)
{
	if (!PRUETH_IS_LRE(prueth))
		return 0;

	return ICSS_LRE_NUM_STANDARD_STATS;
}

void icssm_prueth_lre_get_strings(struct prueth *prueth, u8 *data)
{
	int i;

	if (!PRUETH_IS_LRE(prueth))
		return;

	for (i = 0; i < ARRAY_SIZE(prueth_ethtool_lre_stats); i++) {
		if (!prueth_ethtool_lre_stats[i].standard_stats) {
			ethtool_puts(&data,
				     prueth_ethtool_lre_stats[i].string);
		}
	}
}

void icssm_lre_update_hardware_stats(struct prueth_emac *emac)
{
	void __iomem *sram, *stat_base;
	int i;

	sram = emac->prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	stat_base = sram + ICSS_LRE_CNT_TX_A;

	for (i = 0; i < ARRAY_SIZE(prueth_ethtool_lre_stats); i++) {
		emac->lre_stats[i] = ioread32(stat_base + i * sizeof(u32));
		if (!strcmp(prueth_ethtool_lre_stats[i].string, "lre_tx_c"))
			emac->lre_stats[i] +=
				ioread32(sram + ICSS_LRE_CNT_TX_CB);
		else if (!strcmp(prueth_ethtool_lre_stats[i].string,
				 "lre_unique_rx_c"))
			emac->lre_stats[i] +=
				ioread32(sram + ICSS_LRE_CNT_UNIQUE_RX_CB);
		else if (!strcmp(prueth_ethtool_lre_stats[i].string,
				 "lre_duplicate_rx_c"))
			emac->lre_stats[i] +=
				ioread32(sram + ICSS_LRE_CNT_DUPLICATE_RX_CB);
		else if (!strcmp(prueth_ethtool_lre_stats[i].string,
				 "lre_multicast_dropped"))
			emac->lre_stats[i] +=
				ioread32(sram + ICSS_LRE_MULTICAST_DROPPED_B);
		else if (!strcmp(prueth_ethtool_lre_stats[i].string,
				 "lre_vlan_dropped"))
			emac->lre_stats[i] +=
				ioread32(sram + ICSS_LRE_VLAN_DROPPED_B);
	}
}

void icssm_prueth_lre_update_stats(struct prueth_emac *emac, u64 *data)
{
	int i;

	if (!PRUETH_IS_LRE(emac->prueth))
		return;

	icssm_lre_update_hardware_stats(emac);

	for (i = 0; i < ARRAY_SIZE(prueth_ethtool_lre_stats); i++) {
		if (!prueth_ethtool_lre_stats[i].standard_stats)
			*(data++) = emac->lre_stats[i];
	}
}

static int icssm_prueth_lre_attr_get(struct net_device *ndev,
				     struct lredev_attr *attr)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;
	int ret = 0;

	netdev_dbg(ndev, "%d:%s, id %d\n", __LINE__, __func__, attr->id);

	switch (attr->id) {
	case LREDEV_ATTR_ID_HSR_MODE:
		if (!PRUETH_IS_HSR(prueth))
			return -EPERM;
		attr->mode = readl(dram0 + ICSS_LRE_HSR_MODE);
		break;
	case LREDEV_ATTR_ID_DD_MODE:
		attr->dd_mode = readl(sram + ICSS_LRE_DUPLICATE_DISCARD);
		break;
	case LREDEV_ATTR_ID_PRP_TR:
		if (!PRUETH_IS_PRP(prueth))
			return -EINVAL;
		attr->tr_mode = prueth->prp_tr_mode;
		break;
	case LREDEV_ATTR_ID_DLRMT:
		attr->dl_reside_max_time =
			readl(dram1 + ICSS_LRE_DUPLI_FORGET_TIME) * 10;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int icssm_prueth_lre_attr_set(struct net_device *ndev,
				     struct lredev_attr *attr)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;
	int ret = 0;

	netdev_dbg(ndev, "%d:%s, id = %d\n", __LINE__, __func__, attr->id);

	switch (attr->id) {
	case LREDEV_ATTR_ID_HSR_MODE:
		if (!PRUETH_IS_HSR(prueth))
			return -EPERM;
		prueth->hsr_mode = attr->mode;
		writel(prueth->hsr_mode, dram0 + ICSS_LRE_HSR_MODE);
		break;
	case LREDEV_ATTR_ID_DD_MODE:
		writel(attr->dd_mode, sram + ICSS_LRE_DUPLICATE_DISCARD);
		break;
	case LREDEV_ATTR_ID_PRP_TR:
		if (!PRUETH_IS_PRP(prueth))
			return -EINVAL;
		prueth->prp_tr_mode = attr->tr_mode;
		break;
	case LREDEV_ATTR_ID_DLRMT:
		/* input is in milli seconds. Firmware expects in unit
		 * of 10 msec
		 */
		writel((attr->dl_reside_max_time / 10),
		       dram1 + ICSS_LRE_DUPLI_FORGET_TIME);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int icssm_prueth_lre_get_lre_stats(struct net_device *ndev,
					  struct lre_stats *stats)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	memcpy_fromio(stats, sram + ICSS_LRE_CNT_TX_A, sizeof(*stats));

	return 0;
}

static int icssm_prueth_lre_set_sv_vlan_id(struct net_device *ndev, u16 vid)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;

	if (!PRUETH_IS_LRE(prueth))
		return 0;

	return icssm_emac_add_del_vid(emac, true, htons(ETH_P_8021Q), vid);
}

const struct lredev_ops icssm_prueth_lredev_ops = {
	.lredev_attr_get = icssm_prueth_lre_attr_get,
	.lredev_attr_set = icssm_prueth_lre_attr_set,
	.lredev_get_stats = icssm_prueth_lre_get_lre_stats,
	.lredev_set_sv_vlan_id = icssm_prueth_lre_set_sv_vlan_id,
};

