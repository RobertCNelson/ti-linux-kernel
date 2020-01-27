// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments ICSSG Ethernet Driver
 *
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/
 *
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/etherdevice.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/pruss.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/dma/ti-cppi5.h>
#include <linux/soc/ti/k3-navss-desc-pool.h>

#include "icssg_prueth.h"
#include "icss_mii_rt.h"

#define PRUETH_MODULE_VERSION "0.1"
#define PRUETH_MODULE_DESCRIPTION "PRUSS ICSSG Ethernet driver"

/* Port queue size in MSMC from firmware
 * PORTQSZ_HP .set (0x1800)
 * PORTQSZ_HP2 .set (PORTQSZ_HP+128) ;include barrier area
 * 0x1880 x 8 bytes per slice  (port)
 */

#define MSMC_RAM_SIZE	(SZ_64K + SZ_32K + SZ_2K)	/* 0x1880 x 8 x 2 */

#define PRUETH_PKT_TYPE_CMD	0x10
#define PRUETH_NAV_PS_DATA_SIZE	16	/* Protocol specific data size */
#define PRUETH_NAV_SW_DATA_SIZE	16	/* SW related data size */
#define PRUETH_MAX_TX_DESC	512
#define PRUETH_MAX_RX_DESC	512
#define PRUETH_MAX_RX_MGM_DESC	8
#define PRUETH_MAX_RX_FLOWS	4	/* excluding default flow */
#define PRUETH_MAX_RX_MGM_FLOWS	3	/* excluding default flow */
#define PRUETH_RX_MGM_FLOW_RESPONSE	0
#define PRUETH_RX_MGM_FLOW_TIMESTAMP	1
#define PRUETH_RX_MGM_FLOW_OTHER	2

#define PRUETH_NUM_BUF_POOLS		16
#define PRUETH_EMAC_BUF_POOL_START	8
#define PRUETH_EMAC_BUF_POOL_SIZE	0x1800

#define PRUETH_MIN_PKT_SIZE	(VLAN_ETH_ZLEN)
#define PRUETH_MAX_PKT_SIZE	(VLAN_ETH_FRAME_LEN + ETH_FCS_LEN)

/* Netif debug messages possible */
#define PRUETH_EMAC_DEBUG	(NETIF_MSG_DRV | \
				 NETIF_MSG_PROBE | \
				 NETIF_MSG_LINK | \
				 NETIF_MSG_TIMER | \
				 NETIF_MSG_IFDOWN | \
				 NETIF_MSG_IFUP | \
				 NETIF_MSG_RX_ERR | \
				 NETIF_MSG_TX_ERR | \
				 NETIF_MSG_TX_QUEUED | \
				 NETIF_MSG_INTR | \
				 NETIF_MSG_TX_DONE | \
				 NETIF_MSG_RX_STATUS | \
				 NETIF_MSG_PKTDATA | \
				 NETIF_MSG_HW | \
				 NETIF_MSG_WOL)

#define prueth_napi_to_emac(napi) container_of(napi, struct prueth_emac, napi)

/* CTRLMMR_ICSSG_RGMII_CTRL register bits */
#define ICSSG_CTRL_RGMII_ID_MODE		BIT(24)

static int debug_level = -1;
module_param(debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "PRUETH debug level (NETIF_MSG bits)");

static void prueth_cleanup_rx_chns(struct prueth_emac *emac,
				   struct prueth_rx_chn *rx_chn,
				   int max_rflows)
{
	int i;

	if (rx_chn->rx_chn) {
		for (i = 0; i < max_rflows; i++)
			k3_nav_udmax_rx_put_irq(rx_chn->rx_chn, i);

		k3_nav_udmax_release_rx_chn(rx_chn->rx_chn);
	}

	if (rx_chn->desc_pool)
		k3_knav_pool_destroy(rx_chn->desc_pool);
}

static void prueth_cleanup_tx_chns(struct prueth_emac *emac)
{
	struct prueth_tx_chn *tx_chn = &emac->tx_chns;

	if (tx_chn->irq)
		k3_nav_udmax_tx_put_irq(tx_chn->tx_chn);

	if (tx_chn->tx_chn)
		k3_nav_udmax_release_tx_chn(tx_chn->tx_chn);

	if (tx_chn->desc_pool)
		k3_knav_pool_destroy(tx_chn->desc_pool);
}

static int prueth_init_tx_chns(struct prueth_emac *emac)
{
	struct net_device *ndev = emac->ndev;
	struct device *dev = emac->prueth->dev;
	struct k3_nav_udmax_tx_channel_cfg tx_cfg;
	static const struct k3_ring_cfg ring_cfg = {
		.elm_size = K3_RINGACC_RING_ELSIZE_8,
		.mode = K3_RINGACC_RING_MODE_RING,
		.flags = 0,
		.size = PRUETH_MAX_TX_DESC,
	};
	u32 hdesc_size;
	int ret, slice;
	struct prueth_tx_chn *tx_chn = &emac->tx_chns;
	char tx_chn_name[16];

	slice = prueth_emac_slice(emac);
	if (slice < 0)
		return slice;

	init_completion(&emac->tdown_complete);

	hdesc_size = cppi5_hdesc_calc_size(true, PRUETH_NAV_PS_DATA_SIZE,
					   PRUETH_NAV_SW_DATA_SIZE);
	memset(&tx_cfg, 0, sizeof(tx_cfg));
	tx_cfg.swdata_size = PRUETH_NAV_SW_DATA_SIZE;
	tx_cfg.tx_cfg = ring_cfg;
	tx_cfg.txcq_cfg = ring_cfg;

	/* To differentiate channels for SLICE0 vs SLICE1 */
	snprintf(tx_chn_name, sizeof(tx_chn_name), "tx%d-0", slice);

	tx_chn->descs_num = PRUETH_MAX_TX_DESC;
	spin_lock_init(&tx_chn->lock);
	tx_chn->desc_pool = k3_knav_pool_create_name(dev, tx_chn->descs_num,
						     hdesc_size, tx_chn_name);
	if (IS_ERR(tx_chn->desc_pool)) {
		ret = PTR_ERR(tx_chn->desc_pool);
		tx_chn->desc_pool = NULL;
		netdev_err(ndev, "Failed to create tx pool: %d\n", ret);
		goto fail;
	}

	tx_chn->tx_chn = k3_nav_udmax_request_tx_chn(dev, tx_chn_name, &tx_cfg);
	if (IS_ERR(tx_chn->tx_chn)) {
		ret = PTR_ERR(tx_chn->tx_chn);
		tx_chn->tx_chn = NULL;
		netdev_err(ndev, "Failed to request tx dma ch: %d\n", ret);
		goto fail;
	}

	ret = k3_nav_udmax_tx_get_irq(tx_chn->tx_chn, &tx_chn->irq,
				      IRQF_TRIGGER_HIGH, false, NULL);
	if (ret) {
		tx_chn->irq = 0;
		netdev_err(ndev, "failed to get tx irq\n");
		goto fail;
	}

	return 0;

fail:
	prueth_cleanup_tx_chns(emac);
	return ret;
}

static int prueth_init_rx_chns(struct prueth_emac *emac,
			       struct prueth_rx_chn *rx_chn,
			       char *name, u32 max_rflows,
			       u32 max_desc_num)
{
	struct net_device *ndev = emac->ndev;
	struct device *dev = emac->prueth->dev;
	struct k3_nav_udmax_rx_channel_cfg rx_cfg;
	u32 fdqring_id;
	u32 hdesc_size;
	int i, ret = 0, slice;
	char rx_chn_name[16];

	slice = prueth_emac_slice(emac);
	if (slice < 0)
		return slice;

	/* To differentiate channels for SLICE0 vs SLICE1 */
	snprintf(rx_chn_name, sizeof(rx_chn_name), "%s%d", name, slice);

	hdesc_size = cppi5_hdesc_calc_size(true, PRUETH_NAV_PS_DATA_SIZE,
					   PRUETH_NAV_SW_DATA_SIZE);
	memset(&rx_cfg, 0, sizeof(rx_cfg));
	rx_cfg.swdata_size = PRUETH_NAV_SW_DATA_SIZE;
	rx_cfg.flow_id_num = max_rflows;
	rx_cfg.flow_id_base = -1; /* udmax will auto select flow id base */

	/* init all flows */
	rx_chn->dev = dev;
	rx_chn->descs_num = max_desc_num;
	spin_lock_init(&rx_chn->lock);
	rx_chn->desc_pool = k3_knav_pool_create_name(dev, rx_chn->descs_num,
						     hdesc_size, rx_chn_name);
	if (IS_ERR(rx_chn->desc_pool)) {
		ret = PTR_ERR(rx_chn->desc_pool);
		rx_chn->desc_pool = NULL;
		netdev_err(ndev, "Failed to create rx pool: %d\n", ret);
		goto fail;
	}

	rx_chn->rx_chn = k3_nav_udmax_request_rx_chn(dev, rx_chn_name, &rx_cfg);
	if (IS_ERR(rx_chn->rx_chn)) {
		ret = PTR_ERR(rx_chn->rx_chn);
		rx_chn->rx_chn = NULL;
		netdev_err(ndev, "Failed to request rx dma ch: %d\n", ret);
		goto fail;
	}

	if (!strncmp(name, "rxmgm", 5)) {
		emac->rx_mgm_flow_id_base = k3_nav_udmax_rx_get_flow_id_base(rx_chn->rx_chn);
		netdev_dbg(ndev, "mgm flow id base = %d\n",
			   emac->rx_mgm_flow_id_base);
	} else {
		emac->rx_flow_id_base = k3_nav_udmax_rx_get_flow_id_base(rx_chn->rx_chn);
		netdev_dbg(ndev, "flow id base = %d\n",
			   emac->rx_flow_id_base);
	}

	fdqring_id = K3_RINGACC_RING_ID_ANY;
	for (i = 0; i < rx_cfg.flow_id_num; i++) {
		struct k3_ring_cfg rxring_cfg = {
			.elm_size = K3_RINGACC_RING_ELSIZE_8,
			.mode = K3_RINGACC_RING_MODE_MESSAGE,
			.flags = 0,
		};
		struct k3_ring_cfg fdqring_cfg = {
			.elm_size = K3_RINGACC_RING_ELSIZE_8,
			.mode = K3_RINGACC_RING_MODE_MESSAGE,
			.flags = K3_RINGACC_RING_SHARED,
		};
		struct k3_nav_udmax_rx_flow_cfg rx_flow_cfg = {
			.rx_cfg = rxring_cfg,
			.rxfdq_cfg = fdqring_cfg,
			.ring_rxq_id = K3_RINGACC_RING_ID_ANY,
			.src_tag_lo_sel =
				K3_NAV_UDMAX_SRC_TAG_LO_USE_REMOTE_SRC_TAG,
		};

		rx_flow_cfg.ring_rxfdq0_id = fdqring_id;
		rx_flow_cfg.rx_cfg.size = max_desc_num;
		rx_flow_cfg.rxfdq_cfg.size = max_desc_num;

		ret = k3_nav_udmax_rx_flow_init(rx_chn->rx_chn,
						i, &rx_flow_cfg);
		if (ret) {
			dev_err(dev, "Failed to init rx flow%d %d\n", i, ret);
			goto fail;
		}
		if (!i)
			fdqring_id = k3_nav_udmax_rx_flow_get_fdq_id(rx_chn->rx_chn,
								     i);
		ret = k3_nav_udmax_rx_get_irq(rx_chn->rx_chn, i, &rx_chn->irq,
					      IRQF_TRIGGER_HIGH,
					      true, i ? 0 : -1);
		if (ret) {
			dev_err(dev, "Failed to get rx dma irq %d\n", ret);
			goto fail;
		}
	}

	return 0;

fail:
	prueth_cleanup_rx_chns(emac, rx_chn, max_rflows);
	return ret;
}

static int prueth_dma_rx_push(struct prueth_emac *emac,
			      struct sk_buff *skb,
			      struct prueth_rx_chn *rx_chn)
{
	struct cppi5_host_desc_t *desc_rx;
	struct device *dev = emac->prueth->dev;
	struct net_device *ndev = emac->ndev;
	dma_addr_t desc_dma;
	dma_addr_t buf_dma;
	u32 pkt_len = skb_tailroom(skb);
	void **swdata;

	desc_rx = k3_knav_pool_alloc(rx_chn->desc_pool);
	if (!desc_rx) {
		netdev_err(ndev, "rx push: failed to allocate descriptor\n");
		return -ENOMEM;
	}
	desc_dma = k3_knav_pool_virt2dma(rx_chn->desc_pool, desc_rx);

	buf_dma = dma_map_single(dev, skb->data, pkt_len, DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(dev, buf_dma))) {
		k3_knav_pool_free(rx_chn->desc_pool, desc_rx);
		netdev_err(ndev, "rx push: failed to map rx pkt buffer\n");
		return -EINVAL;
	}

	cppi5_hdesc_init(desc_rx, CPPI5_INFO0_HDESC_EPIB_PRESENT,
			 PRUETH_NAV_PS_DATA_SIZE);
	cppi5_hdesc_attach_buf(desc_rx, 0, 0, buf_dma, skb_tailroom(skb));

	swdata = cppi5_hdesc_get_swdata(desc_rx);
	*swdata = skb;

	return k3_nav_udmax_push_rx_chn(rx_chn->rx_chn, 0,
					desc_rx, desc_dma);
}

static void emac_rx_timestamp(struct sk_buff *skb, u32 *psdata)
{
	struct skb_shared_hwtstamps *ssh;
	u64 ns;

	ns = (u64)psdata[1] << 32 | psdata[0];

	ssh = skb_hwtstamps(skb);
	memset(ssh, 0, sizeof(*ssh));
	ssh->hwtstamp = ns_to_ktime(ns);
}

/**
 * emac_rx_packet - Get one packet from RX ring and push to netdev.
 * Returns 0 on success, else error code.
 */
static int emac_rx_packet(struct prueth_emac *emac, u32 flow_id)
{
	struct prueth_rx_chn *rx_chn = &emac->rx_chns;
	struct device *dev = emac->prueth->dev;
	struct net_device *ndev = emac->ndev;
	struct cppi5_host_desc_t *desc_rx;
	dma_addr_t desc_dma, buf_dma;
	u32 buf_dma_len, pkt_len, port_id = 0;
	int ret;
	void **swdata;
	struct sk_buff *skb, *new_skb;
	u32 *psdata;

	ret = k3_nav_udmax_pop_rx_chn(rx_chn->rx_chn, flow_id, &desc_dma);
	if (ret) {
		if (ret != -ENODATA)
			netdev_err(ndev, "rx pop: failed: %d\n", ret);
		return ret;
	}

	if (desc_dma & 0x1) /* Teardown ? */
		return 0;

	desc_rx = k3_knav_pool_dma2virt(rx_chn->desc_pool, desc_dma);

	swdata = cppi5_hdesc_get_swdata(desc_rx);
	skb = *swdata;

	psdata = cppi5_hdesc_get_psdata32(desc_rx);
	/* RX HW timestamp */
	if (emac->rx_ts_enabled)
		emac_rx_timestamp(skb, psdata);

	cppi5_hdesc_get_obuf(desc_rx, &buf_dma, &buf_dma_len);
	pkt_len = cppi5_hdesc_get_pktlen(desc_rx);
	/* firmware adds 4 CRC bytes, strip them */
	pkt_len -= 4;
	cppi5_desc_get_tags_ids(&desc_rx->hdr, &port_id, NULL);

	dma_unmap_single(dev, buf_dma, buf_dma_len, DMA_FROM_DEVICE);
	k3_knav_pool_free(rx_chn->desc_pool, desc_rx);

	skb->dev = ndev;
	if (!netif_running(skb->dev)) {
		dev_kfree_skb_any(skb);
		return 0;
	}

	new_skb = netdev_alloc_skb_ip_align(ndev, PRUETH_MAX_PKT_SIZE);
	/* if allocation fails we drop the packet but push the
	 * descriptor back to the ring with old skb to prevent a stall
	 */
	if (!new_skb) {
		ndev->stats.rx_dropped++;
		new_skb = skb;
	} else {
		/* send the filled skb up the n/w stack */
		skb_put(skb, pkt_len);
		skb->protocol = eth_type_trans(skb, ndev);
		netif_receive_skb(skb);
		ndev->stats.rx_bytes += pkt_len;
		ndev->stats.rx_packets++;
	}

	/* queue another RX DMA */
	ret = prueth_dma_rx_push(emac, new_skb, &emac->rx_chns);
	if (WARN_ON(ret < 0)) {
		dev_kfree_skb_any(new_skb);
		ndev->stats.rx_errors++;
		ndev->stats.rx_dropped++;
	}

	return ret;
}

static void prueth_rx_cleanup(void *data, dma_addr_t desc_dma)
{
	struct prueth_rx_chn *rx_chn = data;
	struct cppi5_host_desc_t *desc_rx;
	struct sk_buff *skb;
	dma_addr_t buf_dma;
	u32 buf_dma_len;
	void **swdata;

	desc_rx = k3_knav_pool_dma2virt(rx_chn->desc_pool, desc_dma);
	swdata = cppi5_hdesc_get_swdata(desc_rx);
	skb = *swdata;
	cppi5_hdesc_get_obuf(desc_rx, &buf_dma, &buf_dma_len);

	dma_unmap_single(rx_chn->dev, buf_dma, buf_dma_len,
			 DMA_FROM_DEVICE);
	k3_knav_pool_free(rx_chn->desc_pool, desc_rx);

	dev_kfree_skb_any(skb);
}

static void prueth_xmit_free(struct prueth_tx_chn *tx_chn,
			     struct device *dev,
			     struct cppi5_host_desc_t *desc)
{
	struct cppi5_host_desc_t *first_desc, *next_desc;
	dma_addr_t buf_dma, next_desc_dma;
	u32 buf_dma_len;

	first_desc = desc;
	next_desc = first_desc;

	cppi5_hdesc_get_obuf(first_desc, &buf_dma, &buf_dma_len);

	dma_unmap_single(dev, buf_dma, buf_dma_len,
			 DMA_TO_DEVICE);

	next_desc_dma = cppi5_hdesc_get_next_hbdesc(first_desc);
	while (next_desc_dma) {
		next_desc = k3_knav_pool_dma2virt(tx_chn->desc_pool,
						  next_desc_dma);
		cppi5_hdesc_get_obuf(next_desc, &buf_dma, &buf_dma_len);

		dma_unmap_page(dev, buf_dma, buf_dma_len,
			       DMA_TO_DEVICE);

		next_desc_dma = cppi5_hdesc_get_next_hbdesc(next_desc);

		k3_knav_pool_free(tx_chn->desc_pool, next_desc);
	}

	k3_knav_pool_free(tx_chn->desc_pool, first_desc);
}

/* TODO: Convert this to use worker/workqueue mechanism to serialize the
 * request to firmware
 */
static int emac_send_command(struct prueth_emac *emac, u32 cmd)
{
	struct device *dev = emac->prueth->dev;
	dma_addr_t desc_dma, buf_dma;
	struct prueth_tx_chn *tx_chn;
	struct cppi5_host_desc_t *first_desc;
	int ret = 0;
	u32 *epib;
	u32 *data = emac->cmd_data;
	u32 pkt_len = sizeof(emac->cmd_data);
	void **swdata;

	netdev_dbg(emac->ndev, "Sending cmd %x\n", cmd);

	/* only one command at a time allowed to firmware */
	mutex_lock(&emac->cmd_lock);
	data[0] = cpu_to_le32(cmd);

	/* Map the linear buffer */
	buf_dma = dma_map_single(dev, data, pkt_len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, buf_dma)) {
		netdev_err(emac->ndev, "cmd %x: failed to map cmd buffer\n",
			   cmd);
		ret = -EINVAL;
		goto err_unlock;
	}

	tx_chn = &emac->tx_chns;

	first_desc = k3_knav_pool_alloc(tx_chn->desc_pool);
	if (!first_desc) {
		netdev_err(emac->ndev,
			   "cmd %x: failed to allocate descriptor\n", cmd);
		dma_unmap_single(dev, buf_dma, pkt_len, DMA_TO_DEVICE);
		ret = -ENOMEM;
		goto err_unlock;
	}

	cppi5_hdesc_init(first_desc, CPPI5_INFO0_HDESC_EPIB_PRESENT,
			 PRUETH_NAV_PS_DATA_SIZE);
	cppi5_hdesc_set_pkttype(first_desc, PRUETH_PKT_TYPE_CMD);
	epib = first_desc->epib;
	epib[0] = 0;
	epib[1] = 0;

	cppi5_hdesc_attach_buf(first_desc, buf_dma, pkt_len, buf_dma, pkt_len);
	swdata = cppi5_hdesc_get_swdata(first_desc);
	*swdata = data;

	cppi5_hdesc_set_pktlen(first_desc, pkt_len);
	desc_dma = k3_knav_pool_virt2dma(tx_chn->desc_pool, first_desc);

	/* send command */
	reinit_completion(&emac->cmd_complete);
	ret = k3_nav_udmax_push_tx_chn(tx_chn->tx_chn, first_desc, desc_dma);
	if (ret) {
		netdev_err(emac->ndev, "cmd %x: push failed: %d\n", cmd, ret);
		goto free_desc;
	}
	ret = wait_for_completion_timeout(&emac->cmd_complete,
					  msecs_to_jiffies(100));
	if (!ret)
		netdev_err(emac->ndev, "cmd %x: completion timeout\n", cmd);

	mutex_unlock(&emac->cmd_lock);

	return ret;
free_desc:
	prueth_xmit_free(tx_chn, dev, first_desc);
err_unlock:
	mutex_unlock(&emac->cmd_lock);

	return ret;
}

static void emac_change_port_speed_duplex(struct prueth_emac *emac,
					  bool full_duplex, int speed)
{
	u32 cmd = ICSSG_PSTATE_SPEED_DUPLEX_CMD, val;
	struct prueth *prueth = emac->prueth;
	int slice = prueth_emac_slice(emac);

	/* only 100M and 1G and full duplex supported for now */
	if (!(full_duplex && (speed == SPEED_1000 || speed == SPEED_100)))
		return;

	val = icssg_rgmii_get_speed(prueth->miig_rt, slice);
	/* firmware expects full duplex settings in bit 2-1 */
	val <<= 1;
	cmd |= val;

	val = icssg_rgmii_get_fullduplex(prueth->miig_rt, slice);
	/* firmware expects full duplex settings in bit 3 */
	val <<= 3;
	cmd |= val;
	emac_send_command(emac, cmd);
}

static int emac_shutdown(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);

	return emac_send_command(emac, ICSSG_SHUTDOWN_CMD);
}

/**
 * emac_ndo_start_xmit - EMAC Transmit function
 * @skb: SKB pointer
 * @ndev: EMAC network adapter
 *
 * Called by the system to transmit a packet  - we queue the packet in
 * EMAC hardware transmit queue
 * Doesn't wait for completion we'll check for TX completion in
 * emac_tx_complete_packets().
 *
 * Returns success(NETDEV_TX_OK) or error code (typically out of descs)
 */
static int emac_ndo_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	int ret = 0;
	struct device *dev = emac->prueth->dev;
	struct cppi5_host_desc_t *first_desc, *next_desc, *cur_desc;
	struct prueth_tx_chn *tx_chn;
	dma_addr_t desc_dma, buf_dma;
	u32 pkt_len;
	int i;
	void **swdata;
	u32 *epib;
	bool in_tx_ts = 0;

	/* frag list based linkage is not supported for now. */
	if (skb_shinfo(skb)->frag_list) {
		dev_err_ratelimited(dev, "NETIF_F_FRAGLIST not supported\n");
		ret = -EINVAL;
		goto drop_free_skb;
	}

	pkt_len = skb_headlen(skb);
	tx_chn = &emac->tx_chns;

	/* Map the linear buffer */
	buf_dma = dma_map_single(dev, skb->data, pkt_len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, buf_dma)) {
		netdev_err(ndev, "tx: failed to map skb buffer\n");
		ret = -EINVAL;
		goto drop_stop_q;
	}

	first_desc = k3_knav_pool_alloc(tx_chn->desc_pool);
	if (!first_desc) {
		netdev_dbg(ndev, "tx: failed to allocate descriptor\n");
		dma_unmap_single(dev, buf_dma, pkt_len, DMA_TO_DEVICE);
		ret = -ENOMEM;
		goto drop_stop_q_busy;
	}

	cppi5_hdesc_init(first_desc, CPPI5_INFO0_HDESC_EPIB_PRESENT,
			 PRUETH_NAV_PS_DATA_SIZE);
	cppi5_hdesc_set_pkttype(first_desc, 0);
	epib = first_desc->epib;
	epib[0] = 0;
	epib[1] = 0;
	if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP &&
	    emac->tx_ts_enabled) {
		/* We currently support only one TX HW timestamp at a time */
		if (!test_and_set_bit_lock(__STATE_TX_TS_IN_PROGRESS,
					   &emac->state)) {
			skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
			/* Request TX timestamp */
			epib[0] = emac->tx_ts_cookie;
			epib[1] = 0x80000000;	/* TX TS request */
			emac->tx_ts_skb = skb_get(skb);
			in_tx_ts = 1;
			/* TODO: note time and check for timeout if HW
			 * doesn't come back with TS
			 */
		}
	}

	cppi5_hdesc_attach_buf(first_desc, buf_dma, pkt_len, buf_dma, pkt_len);
	swdata = cppi5_hdesc_get_swdata(first_desc);
	*swdata = skb;

	if (!skb_is_nonlinear(skb))
		goto tx_push;

	/* Handle the case where skb is fragmented in pages */
	cur_desc = first_desc;
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		u32 frag_size = skb_frag_size(frag);

		next_desc = k3_knav_pool_alloc(tx_chn->desc_pool);
		if (!next_desc) {
			netdev_err(ndev,
				   "tx: failed to allocate frag. descriptor\n");
			ret = -ENOMEM;
			goto cleanup_tx_ts;
		}

		buf_dma = skb_frag_dma_map(dev, frag, 0, frag_size,
					   DMA_TO_DEVICE);
		if (dma_mapping_error(dev, buf_dma)) {
			netdev_err(ndev, "tx: Failed to map skb page\n");
			k3_knav_pool_free(tx_chn->desc_pool, next_desc);
			ret = -EINVAL;
			goto cleanup_tx_ts;
		}

		cppi5_hdesc_reset_hbdesc(next_desc);
		cppi5_hdesc_attach_buf(next_desc,
				       buf_dma, frag_size, buf_dma, frag_size);

		desc_dma = k3_knav_pool_virt2dma(tx_chn->desc_pool, next_desc);
		cppi5_hdesc_link_hbdesc(cur_desc, desc_dma);

		pkt_len += frag_size;
		cur_desc = next_desc;
	}
	WARN_ON(pkt_len != skb->len);

tx_push:
	/* report bql before sending packet */
	netdev_sent_queue(ndev, pkt_len);

	cppi5_hdesc_set_pktlen(first_desc, pkt_len);
	desc_dma = k3_knav_pool_virt2dma(tx_chn->desc_pool, first_desc);
	/* cppi5_desc_dump(first_desc, 64); */

	skb_tx_timestamp(skb);	/* SW timestamp if SKBTX_IN_PROGRESS not set */
	ret = k3_nav_udmax_push_tx_chn(tx_chn->tx_chn, first_desc, desc_dma);
	if (ret) {
		netdev_err(ndev, "tx: push failed: %d\n", ret);
		goto drop_free_descs;
	}

	if (k3_knav_pool_avail(tx_chn->desc_pool) < MAX_SKB_FRAGS)
		netif_stop_queue(ndev);

	return NETDEV_TX_OK;

cleanup_tx_ts:
	if (in_tx_ts) {
		dev_kfree_skb_any(emac->tx_ts_skb);
		emac->tx_ts_skb = NULL;
		clear_bit_unlock(__STATE_TX_TS_IN_PROGRESS, &emac->state);
	}

drop_free_descs:
	prueth_xmit_free(tx_chn, dev, first_desc);
drop_stop_q:
	netif_stop_queue(ndev);
drop_free_skb:
	dev_kfree_skb_any(skb);

	/* error */
	ndev->stats.tx_dropped++;
	netdev_err(ndev, "tx: error: %d\n", ret);

	return ret;

drop_stop_q_busy:
	netif_stop_queue(ndev);
	return NETDEV_TX_BUSY;
}

/**
 * emac_tx_complete_packets - Check if TX completed packets upto budget.
 * Returns number of completed TX packets.
 */
static int emac_tx_complete_packets(struct prueth_emac *emac, int budget)
{
	struct net_device *ndev = emac->ndev;
	struct cppi5_host_desc_t *desc_tx;
	struct device *dev = emac->prueth->dev;
	struct prueth_tx_chn *tx_chn;
	unsigned int total_bytes = 0;
	struct sk_buff *skb;
	dma_addr_t desc_dma;
	int res, num_tx = 0;
	void **swdata;

	tx_chn = &emac->tx_chns;

	while (budget--) {
		res = k3_nav_udmax_pop_tx_chn(tx_chn->tx_chn, &desc_dma);
		if (res == -ENODATA)
			break;

		/* teardown completion */
		if (desc_dma & 0x1) {
			complete(&emac->tdown_complete);
			break;
		}

		desc_tx = k3_knav_pool_dma2virt(tx_chn->desc_pool, desc_dma);
		swdata = cppi5_hdesc_get_swdata(desc_tx);

		/* was this command's TX complete? */
		if (*(swdata) == emac->cmd_data) {
			prueth_xmit_free(tx_chn, dev, desc_tx);
			budget++;	/* not a data packet */
			continue;
		}

		skb = *(swdata);
		prueth_xmit_free(tx_chn, dev, desc_tx);

		ndev = skb->dev;
		ndev->stats.tx_packets++;
		ndev->stats.tx_bytes += skb->len;
		total_bytes += skb->len;
		napi_consume_skb(skb, budget);
		num_tx++;
	}

	if (!num_tx)
		return 0;

	netdev_completed_queue(ndev, num_tx, total_bytes);

	if (netif_queue_stopped(ndev)) {
		/* If the the TX queue was stopped, wake it now
		 * if we have enough room.
		 */
		netif_tx_lock(ndev);
		if (netif_running(ndev) &&
		    (k3_knav_pool_avail(tx_chn->desc_pool) >= MAX_SKB_FRAGS))
			netif_wake_queue(ndev);
		netif_tx_unlock(ndev);
	}

	return num_tx;
}

static void prueth_tx_cleanup(void *data, dma_addr_t desc_dma)
{
	struct prueth_emac *emac = data;
	struct prueth_tx_chn *tx_chn = &emac->tx_chns;
	struct cppi5_host_desc_t *desc_tx;
	struct sk_buff *skb;
	void **swdata;

	desc_tx = k3_knav_pool_dma2virt(tx_chn->desc_pool, desc_dma);
	swdata = cppi5_hdesc_get_swdata(desc_tx);
	skb = *(swdata);
	prueth_xmit_free(tx_chn, emac->prueth->dev, desc_tx);

	dev_kfree_skb_any(skb);
}

/* get one packet from requested flow_id
 *
 * Returns skb pointer if packet found else NULL
 * Caller must free the returned skb.
 */
static struct sk_buff *prueth_process_rx_mgm(struct prueth_emac *emac,
					     u32 flow_id)
{
	struct prueth_rx_chn *rx_chn = &emac->rx_mgm_chn;
	struct device *dev = emac->prueth->dev;
	struct net_device *ndev = emac->ndev;
	struct cppi5_host_desc_t *desc_rx;
	dma_addr_t desc_dma, buf_dma;
	u32 buf_dma_len, pkt_len;
	int ret;
	void **swdata;
	struct sk_buff *skb, *new_skb;

	ret = k3_nav_udmax_pop_rx_chn(rx_chn->rx_chn, flow_id, &desc_dma);
	if (ret) {
		if (ret != -ENODATA)
			netdev_err(ndev, "rx mgm pop: failed: %d\n", ret);
		return NULL;
	}

	if (desc_dma & 0x1) /* Teardown ? */
		return NULL;

	desc_rx = k3_knav_pool_dma2virt(rx_chn->desc_pool, desc_dma);

	/* Fix FW bug about incorrect PSDATA size */
	if (cppi5_hdesc_get_psdata_size(desc_rx) != PRUETH_NAV_PS_DATA_SIZE) {
		cppi5_hdesc_update_psdata_size(desc_rx,
					       PRUETH_NAV_PS_DATA_SIZE);
	}

	swdata = cppi5_hdesc_get_swdata(desc_rx);
	skb = *swdata;
	cppi5_hdesc_get_obuf(desc_rx, &buf_dma, &buf_dma_len);
	pkt_len = cppi5_hdesc_get_pktlen(desc_rx);

	dma_unmap_single(dev, buf_dma, buf_dma_len, DMA_FROM_DEVICE);
	k3_knav_pool_free(rx_chn->desc_pool, desc_rx);

	new_skb = netdev_alloc_skb_ip_align(ndev, PRUETH_MAX_PKT_SIZE);
	/* if allocation fails we drop the packet but push the
	 * descriptor back to the ring with old skb to prevent a stall
	 */
	if (!new_skb) {
		netdev_err(ndev,
			   "skb alloc failed, dropped mgm pkt from flow %d\n",
			   flow_id);
		new_skb = skb;
		skb = NULL;	/* return NULL */
	} else {
		/* return the filled skb */
		skb_put(skb, pkt_len);
	}

	/* queue another DMA */
	ret = prueth_dma_rx_push(emac, new_skb, &emac->rx_mgm_chn);
	if (WARN_ON(ret < 0))
		dev_kfree_skb_any(new_skb);

	return skb;
}

static void prueth_tx_ts(struct prueth_emac *emac,
			 struct emac_tx_ts_response *tsr)
{
	u64 ns;
	struct skb_shared_hwtstamps ssh;
	struct sk_buff *skb;

	ns = (u64)tsr->hi_ts << 32 | tsr->lo_ts;

	if (!test_bit(__STATE_TX_TS_IN_PROGRESS, &emac->state)) {
		netdev_err(emac->ndev, "unexpected TS response\n");
		return;
	}

	skb = emac->tx_ts_skb;
	if (tsr->cookie != emac->tx_ts_cookie) {
		netdev_err(emac->ndev, "TX TS cookie mismatch 0x%x:0x%x\n",
			   tsr->cookie, emac->tx_ts_cookie);
		goto error;
	}

	emac->tx_ts_cookie++;
	memset(&ssh, 0, sizeof(ssh));
	ssh.hwtstamp = ns_to_ktime(ns);
	clear_bit_unlock(__STATE_TX_TS_IN_PROGRESS, &emac->state);

	skb_tstamp_tx(skb, &ssh);
	dev_consume_skb_any(skb);

	return;

error:
	dev_kfree_skb_any(skb);
	emac->tx_ts_skb = NULL;
	clear_bit_unlock(__STATE_TX_TS_IN_PROGRESS, &emac->state);
}

static irqreturn_t prueth_rx_mgm_irq_thread(int irq, void *dev_id)
{
	struct prueth_emac *emac = dev_id;
	struct sk_buff *skb;
	int flow = PRUETH_MAX_RX_MGM_FLOWS - 1;
	u32 rsp;

	while (flow--) {
		skb = prueth_process_rx_mgm(emac, flow);
		if (!skb)
			continue;

		switch (flow) {
		case PRUETH_RX_MGM_FLOW_RESPONSE:
			/* Process command response */
			rsp = le32_to_cpu(*(u32 *)skb->data);
			if ((rsp & 0xffff0000) == ICSSG_SHUTDOWN_CMD) {
				netdev_dbg(emac->ndev,
					   "f/w Shutdown cmd resp %x\n", rsp);
				complete(&emac->cmd_complete);
			} else if ((rsp & 0xffff0000) ==
				ICSSG_PSTATE_SPEED_DUPLEX_CMD) {
				netdev_dbg(emac->ndev,
					   "f/w Speed/Duplex cmd rsp %x\n",
					    rsp);
				complete(&emac->cmd_complete);
			} else {
				netdev_err(emac->ndev, "Unknown f/w cmd rsp %x\n",
					   rsp);
			}
			break;
		case PRUETH_RX_MGM_FLOW_TIMESTAMP:
			prueth_tx_ts(emac, (void *)skb->data);
			break;
		default:
			continue;
		}

		dev_kfree_skb_any(skb);
	}

	return IRQ_HANDLED;
}

static irqreturn_t prueth_rx_irq(int irq, void *dev_id)
{
	struct prueth_emac *emac = dev_id;

	disable_irq_nosync(irq);
	napi_schedule(&emac->napi_rx);

	return IRQ_HANDLED;
}

static irqreturn_t prueth_tx_irq(int irq, void *dev_id)
{
	struct prueth_emac *emac = dev_id;

	disable_irq_nosync(irq);
	napi_schedule(&emac->napi_tx);

	return IRQ_HANDLED;
}

static void icssg_config_set(struct prueth *prueth, int slice)
{
	void __iomem *va;

	va = prueth->shram.va + slice * ICSSG_CONFIG_OFFSET_SLICE1;
	memcpy_toio(va, &prueth->config[slice], sizeof(prueth->config[slice]));
}

static int prueth_emac_start(struct prueth *prueth, struct prueth_emac *emac)
{
	struct device *dev = prueth->dev;
	int slice, ret;
	struct icssg_config *config;
	int i;

	slice = prueth_emac_slice(emac);
	if (slice < 0) {
		netdev_err(emac->ndev, "invalid port\n");
		return -EINVAL;
	}

	/* Set Load time configuration */
	config = &prueth->config[slice];
	memset(config, 0, sizeof(*config));
	config->addr_lo = cpu_to_le32(lower_32_bits(prueth->msmcram.pa));
	config->addr_hi = cpu_to_le32(upper_32_bits(prueth->msmcram.pa));
	config->num_tx_threads = 0;
	config->rx_flow_id = emac->rx_flow_id_base; /* flow id for host port */
	config->rx_mgr_flow_id = emac->rx_mgm_flow_id_base; /* for mgm ch */

	/* set buffer sizes for the pools. 0-7 are not used for dual-emac */
	for (i = PRUETH_EMAC_BUF_POOL_START;
	     i < PRUETH_NUM_BUF_POOLS; i++)
		config->tx_buf_sz[i] = cpu_to_le32(PRUETH_EMAC_BUF_POOL_SIZE);

	icssg_config_set(prueth, slice);

	ret = rproc_boot(prueth->pru[slice]);
	if (ret) {
		dev_err(dev, "failed to boot PRU%d: %d\n", slice, ret);
		return -EINVAL;
	}

	ret = rproc_boot(prueth->rtu[slice]);
	if (ret) {
		dev_err(dev, "failed to boot RTU%d: %d\n", slice, ret);
		goto halt_pru;
	}

	return 0;

halt_pru:
	rproc_shutdown(prueth->pru[slice]);

	return ret;
}

static void prueth_emac_stop(struct prueth_emac *emac)
{
	struct prueth *prueth = emac->prueth;
	int slice;

	switch (emac->port_id) {
	case PRUETH_PORT_MII0:
		slice = ICSS_SLICE0;
		break;
	case PRUETH_PORT_MII1:
		slice = ICSS_SLICE1;
		break;
	default:
		netdev_err(emac->ndev, "invalid port\n");
		return;
	}

	rproc_shutdown(prueth->rtu[slice]);
	rproc_shutdown(prueth->pru[slice]);
}

/* called back by PHY layer if there is change in link state of hw port*/
static void emac_adjust_link(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct phy_device *phydev = emac->phydev;
	bool gig_en = false, full_duplex = false;
	struct prueth *prueth = emac->prueth;
	int slice = prueth_emac_slice(emac);
	bool new_state = false;
	unsigned long flags;

	if (phydev->link) {
		/* check the mode of operation - full/half duplex */
		if (phydev->duplex != emac->duplex) {
			new_state = true;
			emac->duplex = phydev->duplex;
		}
		if (phydev->speed != emac->speed) {
			new_state = true;
			emac->speed = phydev->speed;
		}
		if (!emac->link) {
			new_state = true;
			emac->link = 1;
		}
	} else if (emac->link) {
		new_state = true;
		emac->link = 0;
		/* defaults for no link */

		/* f/w should support 100 & 1000 */
		emac->speed = SPEED_1000;

		/* half duplex may not be supported by f/w */
		emac->duplex = DUPLEX_FULL;
	}

	if (new_state) {
		phy_print_status(phydev);

		/* update RGMII and MII configuration based on PHY negotiated
		 * values
		 */
		spin_lock_irqsave(&emac->lock, flags);
		if (emac->link) {
			if (phydev->speed == SPEED_1000)
				gig_en = true;

			if (phydev->duplex == DUPLEX_FULL)
				full_duplex = true;

			/* Set the RGMII cfg for gig en and full duplex */
			icssg_update_rgmii_cfg(prueth->miig_rt, gig_en,
					       full_duplex, slice);
			/* update the Tx IPG based on 100M/1G speed */
			icssg_update_mii_rt_cfg(prueth->mii_rt, emac->speed,
						slice);
		} else {
			icssg_update_rgmii_cfg(prueth->miig_rt, true, true,
					       slice);
			icssg_update_mii_rt_cfg(prueth->mii_rt, emac->speed,
						slice);
		}
		spin_unlock_irqrestore(&emac->lock, flags);

		/* send command to firmware to change speed and duplex
		 * setting when link is up.
		 */
		if (emac->link)
			emac_change_port_speed_duplex(emac, full_duplex,
						      emac->speed);
	}

	if (emac->link) {
		/* link ON */
		netif_carrier_on(ndev);
		/* reactivate the transmit queue */
		netif_tx_wake_all_queues(ndev);
	} else {
		/* link OFF */
		netif_carrier_off(ndev);
		netif_tx_stop_all_queues(ndev);
	}
}

static int emac_napi_rx_poll(struct napi_struct *napi_rx, int budget)
{
	struct prueth_emac *emac = prueth_napi_to_emac(napi_rx);
	int num_rx = 0;
	int flow = PRUETH_MAX_RX_FLOWS;
	int cur_budget;
	int ret;

	while (flow--) {
		cur_budget = budget - num_rx;

		while (cur_budget--) {
			ret = emac_rx_packet(emac, flow);
			if (ret)
				break;
			num_rx++;
		}

		if (num_rx >= budget)
			break;
	}

	if (num_rx < budget) {
		napi_complete(napi_rx);
		enable_irq(emac->rx_chns.irq);
	}

	return num_rx;
}

static int emac_napi_tx_poll(struct napi_struct *napi_tx, int budget)
{
	struct prueth_emac *emac = prueth_napi_to_emac(napi_tx);
	int num_tx_packets;

	num_tx_packets = emac_tx_complete_packets(emac, budget);

	if (num_tx_packets < budget) {
		napi_complete(napi_tx);
		enable_irq(emac->tx_chns.irq);
	}

	return num_tx_packets;
}

/**
 * emac_ndo_open - EMAC device open
 * @ndev: network adapter device
 *
 * Called when system wants to start the interface.
 *
 * Returns 0 for a successful open, or appropriate error code
 */
static int emac_ndo_open(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;
	struct device *dev = prueth->dev;
	int ret, i;
	struct sk_buff *skb;
	int slice = prueth_emac_slice(emac);

	/* clear SMEM of this slice */
	memset_io(prueth->shram.va + slice * ICSSG_CONFIG_OFFSET_SLICE1,
		  0, ICSSG_CONFIG_OFFSET_SLICE1);
	/* set h/w MAC as user might have re-configured */
	ether_addr_copy(emac->mac_addr, ndev->dev_addr);

	icssg_class_set_mac_addr(prueth->miig_rt, slice, emac->mac_addr);
	icssg_class_default(prueth->miig_rt, slice, 0);

	netif_carrier_off(ndev);

	init_completion(&emac->cmd_complete);
	ret = prueth_init_tx_chns(emac);
	if (ret) {
		dev_err(dev, "failed to init tx channel: %d\n", ret);
		return ret;
	}

	ret = prueth_init_rx_chns(emac, &emac->rx_chns, "rx",
				  PRUETH_MAX_RX_FLOWS, PRUETH_MAX_RX_DESC);
	if (ret) {
		dev_err(dev, "failed to init rx channel: %d\n", ret);
		goto cleanup_tx;
	}

	ret = prueth_init_rx_chns(emac, &emac->rx_mgm_chn, "rxmgm",
				  PRUETH_MAX_RX_MGM_FLOWS,
				  PRUETH_MAX_RX_MGM_DESC);
	if (ret) {
		dev_err(dev, "failed to init rx management channel: %d\n", ret);
		goto cleanup_rx;
	}

	ret = request_irq(emac->tx_chns.irq, prueth_tx_irq, 0,
			  dev_name(dev), emac);
	if (ret) {
		dev_err(dev, "unable to request TX IRQ\n");
		goto cleanup_rx_mgm;
	}

	ret = request_irq(emac->rx_chns.irq, prueth_rx_irq, 0,
			  dev_name(dev), emac);
	if (ret) {
		dev_err(dev, "unable to request RX IRQ\n");
		goto free_tx_irq;
	}

	ret = request_threaded_irq(emac->rx_mgm_chn.irq, NULL,
				   prueth_rx_mgm_irq_thread, IRQF_ONESHOT,
				   dev_name(dev), emac);
	if (ret) {
		dev_err(dev, "unable to request RX Management IRQ\n");
		goto free_rx_irq;
	}

	/* reset and start PRU firmware */
	ret = prueth_emac_start(prueth, emac);
	if (ret)
		goto free_rx_mgm_irq;

	/* Get attached phy details */
	phy_attached_info(emac->phydev);

	/* start PHY */
	phy_start(emac->phydev);

	/* prepare RX & TX */
	for (i = 0; i < emac->rx_chns.descs_num; i++) {
		skb = __netdev_alloc_skb_ip_align(NULL,
						  PRUETH_MAX_PKT_SIZE,
						  GFP_KERNEL);
		if (!skb) {
			netdev_err(ndev, "cannot allocate skb\n");
			ret = -ENOMEM;
			goto err;
		}

		ret = prueth_dma_rx_push(emac, skb, &emac->rx_chns);
		if (ret < 0) {
			netdev_err(ndev, "cannot submit skb for rx: %d\n",
				   ret);
			kfree_skb(skb);
			goto err;
		}
	}

	for (i = 0; i < emac->rx_mgm_chn.descs_num; i++) {
		skb = __netdev_alloc_skb_ip_align(NULL,
						  64,
						  GFP_KERNEL);
		if (!skb) {
			netdev_err(ndev, "cannot allocate skb\n");
			ret = -ENOMEM;
			goto err;
		}

		ret = prueth_dma_rx_push(emac, skb, &emac->rx_mgm_chn);
		if (ret < 0) {
			netdev_err(ndev, "cannot submit skb for rx_mgm: %d\n",
				   ret);
			kfree_skb(skb);
			goto err;
		}
	}

	k3_nav_udmax_enable_rx_chn(emac->rx_mgm_chn.rx_chn);
	k3_nav_udmax_enable_rx_chn(emac->rx_chns.rx_chn);
	k3_nav_udmax_enable_tx_chn(emac->tx_chns.tx_chn);

	napi_enable(&emac->napi_tx);
	napi_enable(&emac->napi_rx);

	if (netif_msg_drv(emac))
		dev_notice(&ndev->dev, "started\n");

	return 0;

err:
	prueth_emac_stop(emac);
free_rx_mgm_irq:
	free_irq(emac->rx_mgm_chn.irq, emac);
free_rx_irq:
	free_irq(emac->rx_chns.irq, emac);
free_tx_irq:
	free_irq(emac->tx_chns.irq, emac);
cleanup_rx_mgm:
	prueth_cleanup_rx_chns(emac, &emac->rx_mgm_chn,
			       PRUETH_MAX_RX_MGM_FLOWS);
cleanup_rx:
	prueth_cleanup_rx_chns(emac, &emac->rx_chns, PRUETH_MAX_RX_FLOWS);
cleanup_tx:
	prueth_cleanup_tx_chns(emac);

	return ret;
}

/**
 * emac_ndo_stop - EMAC device stop
 * @ndev: network adapter device
 *
 * Called when system wants to stop or down the interface.
 */
static int emac_ndo_stop(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;
	int ret, i;

	/* inform the upper layers. */
	netif_stop_queue(ndev);

	/* block packets from wire */
	phy_stop(emac->phydev);
	icssg_class_disable(prueth->miig_rt, prueth_emac_slice(emac));

	/* send shutdown command */
	emac_shutdown(ndev);

	/* tear down and disable UDMA channels */
	reinit_completion(&emac->tdown_complete);
	k3_nav_udmax_tdown_tx_chn(emac->tx_chns.tx_chn, false);
	ret = wait_for_completion_timeout(&emac->tdown_complete,
			msecs_to_jiffies(1000));
	if (!ret)
		netdev_err(ndev, "tx teardown timeout\n");

	k3_nav_udmax_reset_tx_chn(emac->tx_chns.tx_chn,
				  emac,
				  prueth_tx_cleanup);
	k3_nav_udmax_disable_tx_chn(emac->tx_chns.tx_chn);

	k3_nav_udmax_tdown_rx_chn(emac->rx_chns.rx_chn, true);
	for (i = 0; i < PRUETH_MAX_RX_FLOWS; i++)
		k3_nav_udmax_reset_rx_chn(emac->rx_chns.rx_chn, i,
					  &emac->rx_chns,
					  prueth_rx_cleanup, !!i);

	k3_nav_udmax_disable_rx_chn(emac->rx_chns.rx_chn);

	/* Teardown RX MGM channel */
	k3_nav_udmax_tdown_rx_chn(emac->rx_mgm_chn.rx_chn, true);
	for (i = 0; i < PRUETH_MAX_RX_MGM_FLOWS; i++)
		k3_nav_udmax_reset_rx_chn(emac->rx_mgm_chn.rx_chn, i,
					  &emac->rx_mgm_chn,
					  prueth_rx_cleanup, !!i);

	k3_nav_udmax_disable_rx_chn(emac->rx_mgm_chn.rx_chn);

	napi_disable(&emac->napi_tx);
	napi_disable(&emac->napi_rx);

	/* stop PRUs */
	prueth_emac_stop(emac);

	free_irq(emac->rx_mgm_chn.irq, emac);
	free_irq(emac->rx_chns.irq, emac);
	free_irq(emac->tx_chns.irq, emac);

	prueth_cleanup_rx_chns(emac, &emac->rx_mgm_chn,
			       PRUETH_MAX_RX_MGM_FLOWS);
	prueth_cleanup_rx_chns(emac, &emac->rx_chns, PRUETH_MAX_RX_FLOWS);
	prueth_cleanup_tx_chns(emac);

	if (netif_msg_drv(emac))
		dev_notice(&ndev->dev, "stopped\n");

	return 0;
}

/**
 * emac_ndo_tx_timeout - EMAC Transmit timeout function
 * @ndev: The EMAC network adapter
 *
 * Called when system detects that a skb timeout period has expired
 * potentially due to a fault in the adapter in not being able to send
 * it out on the wire.
 */
static void emac_ndo_tx_timeout(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);

	if (netif_msg_tx_err(emac))
		netdev_err(ndev, "xmit timeout");

	ndev->stats.tx_errors++;

	/* TODO: can we recover or need to reboot firmware? */
}

/**
 * emac_ndo_set_rx_mode - EMAC set receive mode function
 * @ndev: The EMAC network adapter
 *
 * Called when system wants to set the receive mode of the device.
 *
 */
static void emac_ndo_set_rx_mode(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;
	int slice = prueth_emac_slice(emac);
	bool promisc = ndev->flags & IFF_PROMISC;
	bool allmulti = ndev->flags & IFF_ALLMULTI;

	if (promisc) {
		icssg_class_promiscuous(prueth->miig_rt, slice);
		return;
	}

	if (allmulti) {
		icssg_class_default(prueth->miig_rt, slice, 1);
		return;
	}

	icssg_class_default(prueth->miig_rt, slice, 0);
	if (!netdev_mc_empty(ndev)) {
		/* program multicast address list into Classifier */
		icssg_class_add_mcast(prueth->miig_rt, slice, ndev);
		return;
	}
}

static int emac_set_timestamp_mode(struct prueth_emac *emac,
				   struct hwtstamp_config *config)
{
	/* reserved for future extensions */
	if (config->flags)
		return -EINVAL;

	switch (config->tx_type) {
	case HWTSTAMP_TX_OFF:
		emac->tx_ts_enabled = 0;
		break;
	case HWTSTAMP_TX_ON:
		emac->tx_ts_enabled = 1;
		break;
	default:
		return -ERANGE;
	}

	switch (config->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		emac->rx_ts_enabled = 0;
		break;
	case HWTSTAMP_FILTER_ALL:
		emac->rx_ts_enabled = 1;
		break;
	default:
		emac->rx_ts_enabled = 1;
	}

	return 0;
}

static int emac_set_ts_config(struct net_device *ndev, struct ifreq *ifr)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct hwtstamp_config config;
	int ret;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	ret = emac_set_timestamp_mode(emac, &config);
	if (ret)
		return ret;

	/* save these settings for future reference */
	memcpy(&emac->tstamp_config, &config,
	       sizeof(emac->tstamp_config));

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}

static int emac_get_ts_config(struct net_device *ndev, struct ifreq *ifr)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct hwtstamp_config *config = &emac->tstamp_config;

	return copy_to_user(ifr->ifr_data, config, sizeof(*config)) ?
			    -EFAULT : 0;
}

static int emac_ndo_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd)
{
	struct prueth_emac *emac = netdev_priv(ndev);

	switch (cmd) {
	case SIOCGHWTSTAMP:
		return emac_get_ts_config(ndev, ifr);
	case SIOCSHWTSTAMP:
		return emac_set_ts_config(ndev, ifr);
	default:
		break;
	}

	return phy_mii_ioctl(emac->phydev, ifr, cmd);
}

static const struct net_device_ops emac_netdev_ops = {
	.ndo_open = emac_ndo_open,
	.ndo_stop = emac_ndo_stop,
	.ndo_start_xmit = emac_ndo_start_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_change_mtu	= eth_change_mtu,
	.ndo_tx_timeout = emac_ndo_tx_timeout,
	.ndo_set_rx_mode = emac_ndo_set_rx_mode,
	.ndo_do_ioctl = emac_ndo_ioctl,
};

/* get emac_port corresponding to eth_node name */
static int prueth_node_port(struct device_node *eth_node)
{
	if (!strcmp(eth_node->name, "ethernet-mii0"))
		return PRUETH_PORT_MII0;
	else if (!strcmp(eth_node->name, "ethernet-mii1"))
		return PRUETH_PORT_MII1;
	else
		return -EINVAL;
}

/* get MAC instance corresponding to eth_node name */
static int prueth_node_mac(struct device_node *eth_node)
{
	if (!strcmp(eth_node->name, "ethernet-mii0"))
		return PRUETH_MAC0;
	else if (!strcmp(eth_node->name, "ethernet-mii1"))
		return PRUETH_MAC1;
	else
		return -EINVAL;
}

extern const struct ethtool_ops icssg_ethtool_ops;

static int prueth_netdev_init(struct prueth *prueth,
			      struct device_node *eth_node)
{
	enum prueth_port port;
	enum prueth_mac mac;
	struct net_device *ndev;
	struct prueth_emac *emac;
	const u8 *mac_addr;
	int ret;
	u32 refclk_freq;
	struct regmap *iep_map;

	port = prueth_node_port(eth_node);
	if (port < 0)
		return -EINVAL;

	mac = prueth_node_mac(eth_node);
	if (mac < 0)
		return -EINVAL;

	ndev = alloc_etherdev(sizeof(*emac));
	if (!ndev)
		return -ENOMEM;

	emac = netdev_priv(ndev);
	iep_map = syscon_regmap_lookup_by_phandle(eth_node, "iep");
	if (IS_ERR(iep_map)) {
		ret = PTR_ERR(iep_map);
		if (ret != -EPROBE_DEFER)
			dev_err(prueth->dev, "couldn't get iep regmap\n");
		goto free;
	}

	/* Firmware sets IEP clock to Vbus clk (250MHz) using internal mux.
	 * see AM65 TRM "Figure 6-113. PRU_ICSSG CORE Clock Diagram"
	 */
	refclk_freq = 250e6;

	SET_NETDEV_DEV(ndev, prueth->dev);
	prueth->emac[mac] = emac;
	emac->prueth = prueth;
	emac->ndev = ndev;
	emac->port_id = port;
	emac->msg_enable = netif_msg_init(debug_level, PRUETH_EMAC_DEBUG);
	spin_lock_init(&emac->lock);
	mutex_init(&emac->cmd_lock);

	emac->phy_node = of_parse_phandle(eth_node, "phy-handle", 0);
	if (!emac->phy_node) {
		dev_err(prueth->dev, "couldn't find phy-handle\n");
		ret = -ENODEV;
		goto free;
	}

	if (of_phy_is_fixed_link(emac->phy_node)) {
		ret = of_phy_register_fixed_link(emac->phy_node);
		if (ret) {
			if (ret != -EPROBE_DEFER) {
				dev_err(prueth->dev,
					"failed to register fixed-link phy: %d\n",
					ret);
			}

			goto free;
		}
	}

	emac->phy_if = of_get_phy_mode(eth_node);
	if (emac->phy_if < 0) {
		dev_err(prueth->dev, "could not get phy-mode property\n");
		ret = emac->phy_if;
		goto free;
	}

	/* connect PHY */
	emac->phydev = of_phy_connect(ndev, emac->phy_node,
				      &emac_adjust_link, 0, emac->phy_if);
	if (!emac->phydev) {
		dev_dbg(prueth->dev, "couldn't connect to phy %s\n",
			emac->phy_node->full_name);
		ret = -EPROBE_DEFER;
		goto free;
	}

	/* remove unsupported modes */
	emac->phydev->supported &= ~(PHY_10BT_FEATURES |
				     SUPPORTED_100baseT_Half |
				     SUPPORTED_1000baseT_Half |
				     SUPPORTED_Pause |
				     SUPPORTED_Asym_Pause);
	emac->phydev->advertising = emac->phydev->supported;

	/* get mac address from DT and set private and netdev addr */
	mac_addr = of_get_mac_address(eth_node);
	if (mac_addr)
		ether_addr_copy(ndev->dev_addr, mac_addr);
	if (!is_valid_ether_addr(ndev->dev_addr)) {
		eth_hw_addr_random(ndev);
		dev_warn(prueth->dev, "port %d: using random MAC addr: %pM\n",
			 port, ndev->dev_addr);
	}
	ether_addr_copy(emac->mac_addr, ndev->dev_addr);

	ndev->netdev_ops = &emac_netdev_ops;
	ndev->ethtool_ops = &icssg_ethtool_ops;

	ret = icssg_iep_init(&emac->iep, prueth->dev, iep_map, refclk_freq);
	if (ret)
		goto free;

	netif_tx_napi_add(ndev, &emac->napi_tx,
			  emac_napi_tx_poll, NAPI_POLL_WEIGHT);
	netif_napi_add(ndev, &emac->napi_rx,
		       emac_napi_rx_poll, NAPI_POLL_WEIGHT);

	return 0;

free:
	free_netdev(ndev);
	prueth->emac[mac] = NULL;

	return ret;
}

static void prueth_netdev_exit(struct prueth *prueth,
			       struct device_node *eth_node)
{
	struct prueth_emac *emac;
	enum prueth_mac mac;

	mac = prueth_node_mac(eth_node);
	if (mac < 0)
		return;

	emac = prueth->emac[mac];
	if (!emac)
		return;

	phy_disconnect(emac->phydev);

	if (of_phy_is_fixed_link(emac->phy_node))
		of_phy_deregister_fixed_link(emac->phy_node);

	netif_napi_del(&emac->napi_rx);
	netif_napi_del(&emac->napi_tx);
	icssg_iep_exit(&emac->iep);
	free_netdev(emac->ndev);
	prueth->emac[mac] = NULL;
}

static int prueth_get_cores(struct prueth *prueth, int slice)
{
	struct device *dev = prueth->dev;
	struct device_node *np = dev->of_node;
	int pru, rtu, ret;

	switch (slice) {
	case ICSS_SLICE0:
		pru = 0;
		rtu = 1;
		break;
	case ICSS_SLICE1:
		pru = 2;
		rtu = 3;
		break;
	default:
		return -EINVAL;
	}

	prueth->pru[slice] = pru_rproc_get(np, pru);
	if (IS_ERR(prueth->pru[slice])) {
		ret = PTR_ERR(prueth->pru[slice]);
		prueth->pru[slice] = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "unable to get PRU%d: %d\n", slice, ret);
		return ret;
	}

	prueth->rtu[slice] = pru_rproc_get(np, rtu);
	if (IS_ERR(prueth->rtu[slice])) {
		ret = PTR_ERR(prueth->rtu[slice]);
		prueth->rtu[slice] = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "unable to get RTU%d: %d\n", slice, ret);
		return ret;
	}

	return 0;
}

static void prueth_put_cores(struct prueth *prueth, int slice)
{
	if (prueth->rtu[slice])
		pru_rproc_put(prueth->rtu[slice]);

	if (prueth->pru[slice])
		pru_rproc_put(prueth->pru[slice]);
}

static int prueth_config_rgmiidelay(struct prueth *prueth,
				    struct device_node *eth_np)
{
	struct device *dev = prueth->dev;
	struct regmap *ctrl_mmr;
	u32 icssgctrl;
	struct device_node *np = dev->of_node;

	if (!of_device_is_compatible(np, "ti,am654-icssg-prueth"))
		return 0;

	ctrl_mmr = syscon_regmap_lookup_by_phandle(eth_np, "syscon-rgmii-delay");
	if (IS_ERR(ctrl_mmr)) {
		dev_err(dev, "couldn't get syscon-rgmii-delay\n");
		return -ENODEV;
	}

	if (of_property_read_u32_index(eth_np, "syscon-rgmii-delay", 1,
				       &icssgctrl)) {
		dev_err(dev, "couldn't get rgmii-delay reg. offset\n");
		return -ENODEV;
	}

	regmap_update_bits(ctrl_mmr, icssgctrl, ICSSG_CTRL_RGMII_ID_MODE, 0);

	return 0;
}

static const struct of_device_id prueth_dt_match[];

static int prueth_probe(struct platform_device *pdev)
{
	struct prueth *prueth;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *eth0_node, *eth1_node;
	const struct of_device_id *match;
	struct pruss *pruss;
	int i, ret;

	if (!np)
		return -ENODEV;	/* we don't support non DT */

	match = of_match_device(prueth_dt_match, dev);
	if (!match)
		return -ENODEV;

	prueth = devm_kzalloc(dev, sizeof(*prueth), GFP_KERNEL);
	if (!prueth)
		return -ENOMEM;

	platform_set_drvdata(pdev, prueth);

	prueth->dev = dev;
	eth0_node = of_get_child_by_name(np, "ethernet-mii0");
	if (!of_device_is_available(eth0_node)) {
		of_node_put(eth0_node);
		eth0_node = NULL;
	}

	eth1_node = of_get_child_by_name(np, "ethernet-mii1");
	if (!of_device_is_available(eth1_node)) {
		of_node_put(eth1_node);
		eth1_node = NULL;
	}

	/* At least one node must be present and available else we fail */
	if (!eth0_node && !eth1_node) {
		dev_err(dev, "neither ethernet-mii0 nor ethernet-mii1 node available\n");
		return -ENODEV;
	}

	prueth->eth_node[PRUETH_MAC0] = eth0_node;
	prueth->eth_node[PRUETH_MAC1] = eth1_node;

	prueth->miig_rt = syscon_regmap_lookup_by_phandle(np, "mii-g-rt");
	if (IS_ERR(prueth->miig_rt)) {
		dev_err(dev, "couldn't get mii-g-rt syscon regmap\n");
		return -ENODEV;
	}

	prueth->mii_rt = syscon_regmap_lookup_by_phandle(np, "mii-rt");
	if (IS_ERR(prueth->mii_rt)) {
		dev_err(dev, "couldn't get mii-rt syscon regmap\n");
		return -ENODEV;
	}

	if (eth0_node) {
		ret = prueth_config_rgmiidelay(prueth, eth0_node);
		if (ret)
			goto put_cores;

		ret = prueth_get_cores(prueth, ICSS_SLICE0);
		if (ret)
			goto put_cores;
	}

	if (eth1_node) {
		ret = prueth_config_rgmiidelay(prueth, eth1_node);
		if (ret)
			goto put_cores;

		ret = prueth_get_cores(prueth, ICSS_SLICE1);
		if (ret)
			goto put_cores;
	}

	pruss = pruss_get(eth0_node ?
			  prueth->pru[ICSS_SLICE0] : prueth->pru[ICSS_SLICE1]);
	if (IS_ERR(pruss)) {
		ret = PTR_ERR(pruss);
		dev_err(dev, "unable to get pruss handle\n");
		goto put_cores;
	}

	prueth->pruss = pruss;

	ret = pruss_request_mem_region(pruss, PRUSS_MEM_SHRD_RAM2,
				       &prueth->shram);
	if (ret) {
		dev_err(dev, "unable to get PRUSS SHRD RAM2: %d\n", ret);
		goto put_mem;
	}

	prueth->sram_pool = of_gen_pool_get(np, "sram", 0);
	if (!prueth->sram_pool) {
		dev_err(dev, "unable to get SRAM pool\n");
		ret = -ENODEV;

		goto put_mem;
	}
	prueth->msmcram.va =
			(void __iomem *)gen_pool_alloc(prueth->sram_pool,
						       MSMC_RAM_SIZE);
	if (!prueth->msmcram.va) {
		ret = -ENOMEM;
		dev_err(dev, "unable to allocate MSMC resource\n");
		goto put_mem;
	}
	prueth->msmcram.pa = gen_pool_virt_to_phys(prueth->sram_pool,
						   (unsigned long)prueth->msmcram.va);
	prueth->msmcram.size = MSMC_RAM_SIZE;
	dev_dbg(dev, "sram: pa %llx va %p size %zx\n", prueth->msmcram.pa,
		prueth->msmcram.va, prueth->msmcram.size);

	/* setup netdev interfaces */
	if (eth0_node) {
		ret = prueth_netdev_init(prueth, eth0_node);
		if (ret) {
			if (ret != -EPROBE_DEFER) {
				dev_err(dev, "netdev init %s failed: %d\n",
					eth0_node->name, ret);
			}
			goto free_pool;
		}
	}

	if (eth1_node) {
		ret = prueth_netdev_init(prueth, eth1_node);
		if (ret) {
			if (ret != -EPROBE_DEFER) {
				dev_err(dev, "netdev init %s failed: %d\n",
					eth1_node->name, ret);
			}
			goto netdev_exit;
		}
	}

	/* register the network devices */
	if (eth0_node) {
		ret = register_netdev(prueth->emac[PRUETH_MAC0]->ndev);
		if (ret) {
			dev_err(dev, "can't register netdev for port MII0");
			goto netdev_exit;
		}

		prueth->registered_netdevs[PRUETH_MAC0] = prueth->emac[PRUETH_MAC0]->ndev;
	}

	if (eth1_node) {
		ret = register_netdev(prueth->emac[PRUETH_MAC1]->ndev);
		if (ret) {
			dev_err(dev, "can't register netdev for port MII1");
			goto netdev_unregister;
		}

		prueth->registered_netdevs[PRUETH_MAC1] = prueth->emac[PRUETH_MAC1]->ndev;
	}

	dev_info(dev, "TI PRU ethernet driver initialized: %s EMAC mode\n",
		 (!eth0_node || !eth1_node) ? "single" : "dual");

	if (eth1_node)
		of_node_put(eth1_node);
	if (eth0_node)
		of_node_put(eth0_node);

	return 0;

netdev_unregister:
	for (i = 0; i < PRUETH_NUM_MACS; i++) {
		if (!prueth->registered_netdevs[i])
			continue;
		unregister_netdev(prueth->registered_netdevs[i]);
	}

netdev_exit:
	for (i = 0; i < PRUETH_NUM_MACS; i++) {
		struct device_node *eth_node;

		eth_node = prueth->eth_node[i];
		if (!eth_node)
			continue;

		prueth_netdev_exit(prueth, eth_node);
	}

free_pool:
	gen_pool_free(prueth->sram_pool,
		      (unsigned long)prueth->msmcram.va, MSMC_RAM_SIZE);

put_mem:
	pruss_release_mem_region(prueth->pruss, &prueth->shram);
	pruss_put(prueth->pruss);

put_cores:
	if (eth1_node) {
		prueth_put_cores(prueth, ICSS_SLICE1);
		of_node_put(eth1_node);
	}

	if (eth0_node) {
		prueth_put_cores(prueth, ICSS_SLICE0);
		of_node_put(eth0_node);
	}

	return ret;
}

static int prueth_remove(struct platform_device *pdev)
{
	struct device_node *eth_node;
	struct prueth *prueth = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < PRUETH_NUM_MACS; i++) {
		if (!prueth->registered_netdevs[i])
			continue;
		unregister_netdev(prueth->registered_netdevs[i]);
	}

	for (i = 0; i < PRUETH_NUM_MACS; i++) {
		eth_node = prueth->eth_node[i];
		if (!eth_node)
			continue;

		prueth_netdev_exit(prueth, eth_node);
	}

	gen_pool_free(prueth->sram_pool,
		      (unsigned long)prueth->msmcram.va,
		      MSMC_RAM_SIZE);

	pruss_release_mem_region(prueth->pruss, &prueth->shram);

	pruss_put(prueth->pruss);

	if (prueth->eth_node[PRUETH_MAC1])
		prueth_put_cores(prueth, ICSS_SLICE1);

	if (prueth->eth_node[PRUETH_MAC0])
		prueth_put_cores(prueth, ICSS_SLICE0);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int prueth_suspend(struct device *dev)
{
	struct prueth *prueth = dev_get_drvdata(dev);
	struct net_device *ndev;
	int i, ret;

	for (i = 0; i < PRUETH_NUM_MACS; i++) {
		ndev = prueth->registered_netdevs[i];

		if (!ndev)
			continue;

		if (netif_running(ndev)) {
			netif_device_detach(ndev);
			ret = emac_ndo_stop(ndev);
			if (ret < 0) {
				netdev_err(ndev, "failed to stop: %d", ret);
				return ret;
			}
		}
	}

	return 0;
}

static int prueth_resume(struct device *dev)
{
	struct prueth *prueth = dev_get_drvdata(dev);
	struct net_device *ndev;
	int i, ret;

	for (i = 0; i < PRUETH_NUM_MACS; i++) {
		ndev = prueth->registered_netdevs[i];

		if (!ndev)
			continue;

		if (netif_running(ndev)) {
			ret = emac_ndo_open(ndev);
			if (ret < 0) {
				netdev_err(ndev, "failed to start: %d", ret);
				return ret;
			}
			netif_device_attach(ndev);
		}
	}

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops prueth_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(prueth_suspend, prueth_resume)
};

static const struct of_device_id prueth_dt_match[] = {
	{ .compatible = "ti,am654-icssg-prueth", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, prueth_dt_match);

static struct platform_driver prueth_driver = {
	.probe = prueth_probe,
	.remove = prueth_remove,
	.driver = {
		.name = "icssg-prueth",
		.of_match_table = prueth_dt_match,
		.pm = &prueth_dev_pm_ops,
	},
};
module_platform_driver(prueth_driver);

MODULE_AUTHOR("Roger Quadros <rogerq@ti.com>");
MODULE_DESCRIPTION("PRUSS ICSSG Ethernet Driver");
MODULE_LICENSE("GPL v2");
