/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IF_HSR_H_
#define _LINUX_IF_HSR_H_

#include <linux/types.h>
#include <linux/skbuff.h>

struct net_device;

/* used to differentiate various protocols */
enum hsr_version {
	HSR_V0 = 0,
	HSR_V1,
	PRP_V1,
};

enum hsr_port_type {
	HSR_PT_NONE = 0,	/* Must be 0, used by framereg */
	HSR_PT_SLAVE_A,
	HSR_PT_SLAVE_B,
	HSR_PT_INTERLINK,
	HSR_PT_MASTER,
	HSR_PT_PORTS,	/* This must be the last item in the enum */
};

struct hsr_ptp_ext {
	u8	port;
	u8	header;
};

/* HSR Tag.
 * As defined in IEC-62439-3:2010, the HSR tag is really { ethertype = 0x88FB,
 * path, LSDU_size, sequence Nr }. But we let eth_header() create { h_dest,
 * h_source, h_proto = 0x88FB }, and add { path, LSDU_size, sequence Nr,
 * encapsulated protocol } instead.
 *
 * Field names as defined in the IEC:2010 standard for HSR.
 */
struct hsr_tag {
	__be16		path_and_LSDU_size;
	__be16		sequence_nr;
	__be16		encap_proto;
} __packed;

#define HSR_HLEN	6

#if IS_ENABLED(CONFIG_HSR)
extern bool is_hsr_master(struct net_device *dev);
extern int hsr_get_version(struct net_device *dev, enum hsr_version *ver);
struct net_device *hsr_get_port_ndev(struct net_device *ndev,
				     enum hsr_port_type pt);
int hsr_get_port_type(struct net_device *hsr_dev, struct net_device *dev,
		      enum hsr_port_type *type);

static inline bool hsr_skb_has_header(struct sk_buff *skb)
{
	struct hsr_ptp_ext *ptp_ext;

	ptp_ext = skb_ext_find(skb, SKB_EXT_HSR);
	if (!ptp_ext)
		return false;
	return ptp_ext->header;
}

static inline unsigned int hsr_skb_has_port(struct sk_buff *skb)
{
	struct hsr_ptp_ext *ptp_ext;

	if (!skb)
		return 0;

	ptp_ext = skb_ext_find(skb, SKB_EXT_HSR);
	if (!ptp_ext)
		return 0;
	return ptp_ext->port;
}

static inline bool hsr_skb_get_header_port(struct sk_buff *skb, bool *header,
					   enum hsr_port_type *port_type)
{
	struct hsr_ptp_ext *ptp_ext;

	*port_type = HSR_PT_NONE;
	*header = false;

	ptp_ext = skb_ext_find(skb, SKB_EXT_HSR);
	if (!ptp_ext)
		return false;

	*port_type = ptp_ext->port;
	*header = ptp_ext->header;
	return true;
}

static inline bool hsr_skb_add_header_port(struct sk_buff *skb, bool header,
					   enum hsr_port_type port)
{
	struct hsr_ptp_ext *ptp_ext;

	ptp_ext = skb_ext_add(skb, SKB_EXT_HSR);
	if (!ptp_ext)
		return false;
	ptp_ext->port = port;
	ptp_ext->header = header;
	return true;
}

#else
static inline bool is_hsr_master(struct net_device *dev)
{
	return false;
}
static inline int hsr_get_version(struct net_device *dev,
				  enum hsr_version *ver)
{
	return -EINVAL;
}

static inline struct net_device *hsr_get_port_ndev(struct net_device *ndev,
						   enum hsr_port_type pt)
{
	return ERR_PTR(-EINVAL);
}

static inline int hsr_get_port_type(struct net_device *hsr_dev,
				    struct net_device *dev,
				    enum hsr_port_type *type)
{
	return -EINVAL;
}

static inline bool hsr_skb_has_header(struct sk_buff *skb)
{
	return false;
}

static inline unsigned int hsr_skb_has_port(struct sk_buff *skb)
{
	return 0;
}

static inline bool hsr_skb_get_header_port(struct sk_buff *skb, bool *header,
					   enum hsr_port_type *port_type)
{
	return false;
}

static inline bool hsr_skb_add_header_port(struct sk_buff *skb, bool header,
					   enum hsr_port_type port)
{
	return true;
}
#endif /* CONFIG_HSR */

#endif /*_LINUX_IF_HSR_H_*/
