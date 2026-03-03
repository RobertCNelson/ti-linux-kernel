// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments ICSSG PRUETH QoS submodule
 * Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com/
 */

#include <linux/printk.h>
#include "icssg_prueth.h"
#include "icssg_switch_map.h"

static void tas_update_fw_list_pointers(struct prueth_emac *emac)
{
	struct tas_config *tas = &emac->qos.tas.config;

	if ((readb(tas->active_list)) == TAS_LIST0) {
		tas->fw_active_list = emac->dram.va + TAS_GATE_MASK_LIST0;
		tas->fw_shadow_list = emac->dram.va + TAS_GATE_MASK_LIST1;
	} else {
		tas->fw_active_list = emac->dram.va + TAS_GATE_MASK_LIST1;
		tas->fw_shadow_list = emac->dram.va + TAS_GATE_MASK_LIST0;
	}
}

static void tas_update_maxsdu_table(struct prueth_emac *emac)
{
	struct tas_config *tas = &emac->qos.tas.config;
	u16 __iomem *max_sdu_tbl_ptr;
	u8 gate_idx;

	/* update the maxsdu table */
	max_sdu_tbl_ptr = emac->dram.va + TAS_QUEUE_MAX_SDU_LIST;

	for (gate_idx = 0; gate_idx < TAS_MAX_NUM_QUEUES; gate_idx++) {
		if (!tas->max_sdu_table.max_sdu[gate_idx])
			tas->max_sdu_table.max_sdu[gate_idx] = PRUETH_MAX_MTU;
		writew(tas->max_sdu_table.max_sdu[gate_idx], &max_sdu_tbl_ptr[gate_idx]);
	}
}

static void tas_reset(struct prueth_emac *emac)
{
	struct tas_config *tas = &emac->qos.tas.config;
	int i;

	for (i = 0; i < TAS_MAX_NUM_QUEUES; i++)
		tas->max_sdu_table.max_sdu[i] = PRUETH_MAX_MTU;

	tas_update_maxsdu_table(emac);

	memset_io(tas->fw_active_list, 0, sizeof(*tas->fw_active_list));
	memset_io(tas->fw_shadow_list, 0, sizeof(*tas->fw_shadow_list));
}

static int tas_set_state(struct prueth_emac *emac, enum tas_state state)
{
	struct tas_config *tas = &emac->qos.tas.config;
	int ret;

	if (tas->state == state)
		return 0;

	switch (state) {
	case TAS_STATE_RESET:
		tas_reset(emac);
		ret = icssg_set_port_state(emac, ICSSG_EMAC_PORT_TAS_RESET);
		break;
	case TAS_STATE_ENABLE:
		ret = icssg_set_port_state(emac, ICSSG_EMAC_PORT_TAS_ENABLE);
		break;
	case TAS_STATE_DISABLE:
		ret = icssg_set_port_state(emac, ICSSG_EMAC_PORT_TAS_DISABLE);
		break;
	}

	if (!ret)
		tas->state = state;

	return ret;
}

static int tas_set_trigger_list_change(struct prueth_emac *emac)
{
	struct tc_taprio_qopt_offload *admin_list = emac->qos.tas.taprio_admin;
	struct tas_config *tas = &emac->qos.tas.config;
	u32 change_cycle_count;
	u32 extension_time;
	u32 cycle_time;
	u64 base_time;

	/* IEP clock has a hardware errata due to which it wraps around exactly
	 * once every taprio cycle. To compensate for that, adjust cycle time
	 * by the wrap around time which is stored in emac->iep->def_inc
	 */
	cycle_time = admin_list->cycle_time - emac->iep->def_inc;
	base_time = admin_list->base_time;

	change_cycle_count = base_time / cycle_time;
	extension_time = base_time % cycle_time;

	writel(cycle_time, emac->dram.va + TAS_ADMIN_CYCLE_TIME);
	writel(change_cycle_count, emac->dram.va + TAS_CONFIG_CHANGE_CYCLE_COUNT);
	writeb(admin_list->num_entries, emac->dram.va + TAS_ADMIN_LIST_LENGTH);
	writel(extension_time, emac->dram.va + TAS_CONFIG_EXTN_TIME);

	/* config_change cleared by f/w to ack reception of new shadow list */
	writeb(1, &tas->config_list->config_change);
	/* config_pending cleared by f/w when new shadow list is copied to active list */
	writeb(1, &tas->config_list->config_pending);

	return icssg_set_port_state(emac, ICSSG_EMAC_PORT_TAS_TRIGGER);
}

static int tas_update_oper_list(struct prueth_emac *emac)
{
	struct tc_taprio_qopt_offload *admin_list = emac->qos.tas.taprio_admin;
	struct tas_config *tas = &emac->qos.tas.config;
	u32 tas_acc_gate_close_time = 0;
	u8 idx, gate_idx, val;
	int ret;

	tas_update_fw_list_pointers(emac);

	for (idx = 0; idx < admin_list->num_entries; idx++) {
		writeb(admin_list->entries[idx].gate_mask,
		       &tas->fw_shadow_list->gate_mask_list[idx]);
		tas_acc_gate_close_time += admin_list->entries[idx].interval;

		/* extend last entry till end of cycle time */
		if (idx == admin_list->num_entries - 1)
			writel(admin_list->cycle_time,
			       &tas->fw_shadow_list->win_end_time_list[idx]);
		else
			writel(tas_acc_gate_close_time,
			       &tas->fw_shadow_list->win_end_time_list[idx]);
	}

	/* clear remaining entries */
	for (idx = admin_list->num_entries; idx < TAS_MAX_CMD_LISTS; idx++) {
		writeb(0, &tas->fw_shadow_list->gate_mask_list[idx]);
		writel(0, &tas->fw_shadow_list->win_end_time_list[idx]);
	}

	/* update the Array of gate close time for each queue in each window */
	for (idx = 0 ; idx < admin_list->num_entries; idx++) {
		/* On Linux, only PRUETH_MAX_TX_QUEUES are supported per port */
		for (gate_idx = 0; gate_idx < PRUETH_MAX_TX_QUEUES; gate_idx++) {
			u8 gate_mask_list_idx = readb(&tas->fw_shadow_list->gate_mask_list[idx]);
			u32 gate_close_time = 0;

			if (gate_mask_list_idx & BIT(gate_idx))
				gate_close_time = readl(&tas->fw_shadow_list->win_end_time_list[idx]);

			writel(gate_close_time,
			       &tas->fw_shadow_list->gate_close_time_list[idx][gate_idx]);
		}
	}

	/* Update the maxsdu table for firmware */
	tas_update_maxsdu_table(emac);

	/* tell f/w to swap active & shadow list */
	ret = tas_set_trigger_list_change(emac);
	if (ret) {
		netdev_err(emac->ndev, "failed to swap f/w config list: %d\n", ret);
		return ret;
	}

	/* Wait for completion */
	ret = readb_poll_timeout(&tas->config_list->config_change, val, !val,
				 USEC_PER_MSEC, 10 * USEC_PER_MSEC);
	if (ret) {
		netdev_err(emac->ndev, "TAS list change completion time out\n");
		return ret;
	}

	return 0;
}

static int emac_taprio_replace(struct net_device *ndev,
			       struct tc_taprio_qopt_offload *taprio)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	int ret;

	if (taprio->cycle_time_extension) {
		NL_SET_ERR_MSG_MOD(taprio->extack, "Cycle time extension not supported");
		return -EOPNOTSUPP;
	}

	if (taprio->cycle_time > TAS_MAX_CYCLE_TIME) {
		NL_SET_ERR_MSG_FMT_MOD(taprio->extack, "cycle_time %llu is more than max supported cycle_time",
				       taprio->cycle_time);
		return -EINVAL;
	}

	if (taprio->cycle_time < TAS_MIN_CYCLE_TIME) {
		NL_SET_ERR_MSG_FMT_MOD(taprio->extack, "cycle_time %llu is less than min supported cycle_time %d",
				       taprio->cycle_time, TAS_MIN_CYCLE_TIME);
		return -EINVAL;
	}

	if (taprio->num_entries > TAS_MAX_CMD_LISTS) {
		NL_SET_ERR_MSG_FMT_MOD(taprio->extack, "num_entries %lu is more than max supported entries %d",
				       taprio->num_entries, TAS_MAX_CMD_LISTS);
		return -EINVAL;
	}

	if (emac->qos.tas.taprio_admin)
		taprio_offload_free(emac->qos.tas.taprio_admin);

	emac->qos.tas.taprio_admin = taprio_offload_get(taprio);
	ret = tas_update_oper_list(emac);
	if (ret)
		goto clear_taprio;

	ret = tas_set_state(emac, TAS_STATE_ENABLE);
	if (ret)
		goto clear_taprio;

	return 0;

clear_taprio:
	emac->qos.tas.taprio_admin = NULL;
	taprio_offload_free(taprio);

	return ret;
}

static int emac_taprio_destroy(struct net_device *ndev,
			       struct tc_taprio_qopt_offload *taprio)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	int ret;

	ret = tas_set_state(emac, TAS_STATE_DISABLE);
	if (ret)
		return ret;

	return tas_set_state(emac, TAS_STATE_RESET);
}

static int emac_setup_taprio(struct net_device *ndev, void *type_data)
{
	struct tc_taprio_qopt_offload *taprio = type_data;
	int ret;

	switch (taprio->cmd) {
	case TAPRIO_CMD_REPLACE:
		ret = emac_taprio_replace(ndev, taprio);
		break;
	case TAPRIO_CMD_DESTROY:
		ret = emac_taprio_destroy(ndev, taprio);
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static int emac_tc_query_caps(struct net_device *ndev, void *type_data)
{
	struct tc_query_caps_base *base = type_data;

	switch (base->type) {
	case TC_SETUP_QDISC_TAPRIO: {
		struct tc_taprio_caps *caps = base->caps;

		caps->gate_mask_per_txq = true;

		return 0;
	}
	case TC_SETUP_QDISC_MQPRIO: {
		struct tc_mqprio_caps *caps = base->caps;

		caps->validate_queue_counts = true;
		return 0;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static void icssg_iet_set_preempt_mask(struct prueth_emac *emac, u8 preemptible_tcs)
{
	void __iomem *config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_mqprio *p_mqprio = &emac->qos.mqprio;
	struct tc_mqprio_qopt *qopt = &p_mqprio->mqprio.qopt;
	int prempt_mask = 0, i;
	u8 tc;

	/* Configure the queues based on the preemptible tc map set by the user */
	for (tc = 0; tc < p_mqprio->mqprio.qopt.num_tc; tc++) {
		/* check if the tc is preemptive or not */
		if (preemptible_tcs & BIT(tc)) {
			for (i = qopt->offset[tc]; i < qopt->offset[tc] + qopt->count[tc]; i++) {
				/* Set all the queues in this tc as preemptive queues */
				writeb(BIT(4), config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);
				prempt_mask &= ~BIT(i);
			}
		} else {
			/* Set all the queues in this tc as express queues */
			for (i = qopt->offset[tc]; i < qopt->offset[tc] + qopt->count[tc]; i++) {
				writeb(0, config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);
				prempt_mask |= BIT(i);
			}
		}
		netdev_set_tc_queue(emac->ndev, tc, qopt->count[tc], qopt->offset[tc]);
	}
	writeb(prempt_mask, config + EXPRESS_PRE_EMPTIVE_Q_MASK);
}

static void icssg_config_ietfpe(struct work_struct *work)
{
	struct prueth_qos_iet *iet =
		container_of(work, struct prueth_qos_iet, fpe_config_task);
	void __iomem *config = iet->emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_mqprio *p_mqprio =  &iet->emac->qos.mqprio;
	bool enable = !!atomic_read(&iet->enable_fpe_config);
	int ret;
	u8 val;

	if (!netif_running(iet->emac->ndev))
		return;

	mutex_lock(&iet->fpe_lock);

	/* Update FPE Tx enable bit (PRE_EMPTION_ENABLE_TX) if
	 * fpe_enabled is set to enable MM in Tx direction
	 */
	writeb(enable ? 1 : 0, config + PRE_EMPTION_ENABLE_TX);

	/* If FPE is to be enabled, first configure MAC Verify state
	 * machine in firmware as firmware kicks the Verify process
	 * as soon as ICSSG_EMAC_PORT_PREMPT_TX_ENABLE command is
	 * received.
	 */
	if (enable && iet->mac_verify_configure) {
		writeb(1, config + PRE_EMPTION_ENABLE_VERIFY);
		writew(iet->tx_min_frag_size, config + PRE_EMPTION_ADD_FRAG_SIZE_LOCAL);
		writel(iet->verify_time_ms, config + PRE_EMPTION_VERIFY_TIME);
	}

	/* Send command to enable FPE Tx side. Rx is always enabled */
	ret = icssg_set_port_state(iet->emac,
				   enable ? ICSSG_EMAC_PORT_PREMPT_TX_ENABLE :
					    ICSSG_EMAC_PORT_PREMPT_TX_DISABLE);
	if (ret) {
		netdev_err(iet->emac->ndev, "TX preempt %s command failed\n",
			   str_enable_disable(enable));
		writeb(0, config + PRE_EMPTION_ENABLE_VERIFY);
		iet->verify_status = ICSSG_IETFPE_STATE_DISABLED;
		goto unlock;
	}

	if (enable && iet->mac_verify_configure) {
		ret = readb_poll_timeout(config + PRE_EMPTION_VERIFY_STATUS, iet->verify_status,
					 (iet->verify_status == ICSSG_IETFPE_STATE_SUCCEEDED),
					 USEC_PER_MSEC, 5 * USEC_PER_SEC);
		if (ret) {
			iet->verify_status = ICSSG_IETFPE_STATE_FAILED;
			netdev_err(iet->emac->ndev,
				   "timeout for MAC Verify: status %x\n",
				   iet->verify_status);
			goto unlock;
		}
	} else if (enable) {
		/* Give f/w some time to update PRE_EMPTION_ACTIVE_TX state */
		usleep_range(100, 200);
	}

	if (enable) {
		val = readb(config + PRE_EMPTION_ACTIVE_TX);
		if (val != 1) {
			netdev_err(iet->emac->ndev,
				   "F/w fails to activate IET/FPE\n");
			goto unlock;
		}
		iet->fpe_active = true;
	} else {
		iet->fpe_active = false;
	}

	netdev_info(iet->emac->ndev, "IET FPE %s successfully\n",
		    str_enable_disable(iet->fpe_active));
	icssg_iet_set_preempt_mask(iet->emac, p_mqprio->preemptible_tcs);

unlock:
	mutex_unlock(&iet->fpe_lock);
}

void icssg_qos_init(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	/* Init work queue for IET MAC verify process */
	iet->emac = emac;
	INIT_WORK(&iet->fpe_config_task, icssg_config_ietfpe);
	mutex_init(&iet->fpe_lock);
}

static int emac_tc_setup_mqprio(struct net_device *ndev, void *type_data)
{
	struct tc_mqprio_qopt_offload *mqprio = type_data;
	struct prueth_emac *emac = netdev_priv(ndev);
	struct tc_mqprio_qopt *qopt = &mqprio->qopt;
	struct prueth_qos_mqprio *p_mqprio;
	u8 num_tc = mqprio->qopt.num_tc;
	int tc, offset, count;

	p_mqprio = &emac->qos.mqprio;

	if (!num_tc) {
		netdev_reset_tc(ndev);
		p_mqprio->preemptible_tcs = 0;
		goto reset_tcs;
	}

	memcpy(&p_mqprio->mqprio, mqprio, sizeof(*mqprio));
	p_mqprio->preemptible_tcs = mqprio->preemptible_tcs;
	netdev_set_num_tc(ndev, mqprio->qopt.num_tc);

	for (tc = 0; tc < num_tc; tc++) {
		count = qopt->count[tc];
		offset = qopt->offset[tc];
		netdev_set_tc_queue(ndev, tc, count, offset);
	}

reset_tcs:
	mutex_lock(&emac->qos.iet.fpe_lock);
	icssg_iet_set_preempt_mask(emac, p_mqprio->preemptible_tcs);
	mutex_unlock(&emac->qos.iet.fpe_lock);

	return 0;
}

int icssg_qos_ndo_setup_tc(struct net_device *ndev, enum tc_setup_type type,
			   void *type_data)
{
	switch (type) {
	case TC_SETUP_QDISC_TAPRIO:
		return emac_setup_taprio(ndev, type_data);
	case TC_QUERY_CAPS:
		return emac_tc_query_caps(ndev, type_data);
	case TC_SETUP_QDISC_MQPRIO:
		return emac_tc_setup_mqprio(ndev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(icssg_qos_ndo_setup_tc);

void icssg_qos_tas_init(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct tas_config *tas;

	tas = &emac->qos.tas.config;

	tas->config_list = emac->dram.va + TAS_CONFIG_CHANGE_TIME;
	tas->active_list = emac->dram.va + TAS_ACTIVE_LIST_INDEX;

	tas_update_fw_list_pointers(emac);

	tas_set_state(emac, TAS_STATE_RESET);
}
EXPORT_SYMBOL_GPL(icssg_qos_tas_init);

void icssg_qos_link_up(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	/* Enable FPE if not active but fpe_enabled is true
	 * and disable FPE if active but fpe_enabled is false
	 */
	if (!iet->fpe_active && iet->fpe_enabled) {
		/* Schedule IET FPE enable */
		atomic_set(&iet->enable_fpe_config, 1);
	} else if (iet->fpe_active && !iet->fpe_enabled) {
		/* Schedule IET FPE disable */
		atomic_set(&iet->enable_fpe_config, 0);
	} else {
		return;
	}
	schedule_work(&iet->fpe_config_task);
}

void icssg_qos_link_down(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	/* disable FPE if active during link down */
	if (iet->fpe_active) {
		/* Schedule IET FPE disable */
		atomic_set(&iet->enable_fpe_config, 0);
		schedule_work(&iet->fpe_config_task);
	}
}
