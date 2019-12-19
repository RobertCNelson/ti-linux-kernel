/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * TI K3 AM65 CPTS driver interface
 *
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com
 */

#ifndef K3_CPTS_H_
#define K3_CPTS_H_

#include <linux/device.h>
#include <linux/of.h>

struct am65_cpts;

#if IS_ENABLED(CONFIG_TI_AM65_CPTS)
struct am65_cpts *am65_cpts_create(struct device *dev, void __iomem *regs,
				   struct device_node *node);
int am65_cpts_phc_index(struct am65_cpts *cpts);
void am65_cpts_tx_timestamp(struct am65_cpts *cpts, struct sk_buff *skb);
void am65_cpts_ask_tx_timestamp(struct am65_cpts *cpts, struct sk_buff *skb);
void am65_cpts_rx_enable(struct am65_cpts *cpts, bool en);
#else
static inline struct am65_cpts *am65_cpts_create(struct device *dev,
						 void __iomem *regs,
						 struct device_node *node)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int am65_cpts_phc_index(struct am65_cpts *cpts)
{
	return -1;
}

static inline void am65_cpts_tx_timestamp(struct am65_cpts *cpts,
					  struct sk_buff *skb)
{
}

static inline void am65_cpts_ask_tx_timestamp(struct am65_cpts *cpts,
					      struct sk_buff *skb)
{
}

static inline void am65_cpts_rx_enable(struct am65_cpts *cpts, bool en)
{
}
#endif

#endif /* K3_CPTS_H_ */
