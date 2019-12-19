// SPDX-License-Identifier: GPL-2.0
/*
 * TI K3 NAVSS Ring Accelerator subsystem driver
 *
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/ti/k3-ringacc.h>
#include <linux/soc/ti/ti_sci_protocol.h>

static LIST_HEAD(k3_ringacc_list);
static DEFINE_MUTEX(k3_ringacc_list_lock);

#ifdef CONFIG_TI_K3_RINGACC_DEBUG
#define	k3_nav_dbg(dev, arg...) dev_err(dev, arg)
static	void dbg_writel(u32 v, void __iomem *reg)
{
	pr_err("WRITEL(32): v(%08X)-->reg(%p)\n", v, reg);
	writel(v, reg);
}

static	u32 dbg_readl(void __iomem *reg)
{
	u32 v;

	v = readl(reg);
	pr_err("READL(32): v(%08X)<--reg(%p)\n", v, reg);
	return v;
}
#else
#define	k3_nav_dbg(dev, arg...) dev_dbg(dev, arg)
#define dbg_writel(v, reg) writel(v, reg)

#define dbg_readl(reg) readl(reg)
#endif

#define K3_RINGACC_CFG_RING_SIZE_ELCNT_MASK		GENMASK(19, 0)

/**
 * struct k3_ring_rt_regs -  The RA Control/Status Registers region
 */
struct k3_ring_rt_regs {
	u32	resv_16[4];
	u32	db;		/* RT Ring N Doorbell Register */
	u32	resv_4[1];
	u32	occ;		/* RT Ring N Occupancy Register */
	u32	indx;		/* RT Ring N Current Index Register */
	u32	hwocc;		/* RT Ring N Hardware Occupancy Register */
	u32	hwindx;		/* RT Ring N Current Index Register */
};

#define K3_RINGACC_RT_REGS_STEP	0x1000

/**
 * struct k3_ring_fifo_regs -  The Ring Accelerator Queues Registers region
 */
struct k3_ring_fifo_regs {
	u32	head_data[128];		/* Ring Head Entry Data Registers */
	u32	tail_data[128];		/* Ring Tail Entry Data Registers */
	u32	peek_head_data[128];	/* Ring Peek Head Entry Data Regs */
	u32	peek_tail_data[128];	/* Ring Peek Tail Entry Data Regs */
};

/**
 * struct k3_ringacc_proxy_gcfg_regs - RA Proxy Global Config MMIO Region
 */
struct k3_ringacc_proxy_gcfg_regs {
	u32	revision;	/* Revision Register */
	u32	config;		/* Config Register */
};

#define K3_RINGACC_PROXY_CFG_THREADS_MASK		GENMASK(15, 0)

/**
 * struct k3_ringacc_proxy_target_regs -  Proxy Datapath MMIO Region
 */
struct k3_ringacc_proxy_target_regs {
	u32	control;	/* Proxy Control Register */
	u32	status;		/* Proxy Status Register */
	u8	resv_512[504];
	u32	data[128];	/* Proxy Data Register */
};

#define K3_RINGACC_PROXY_TARGET_STEP	0x1000
#define K3_RINGACC_PROXY_NOT_USED	(-1)

enum k3_ringacc_proxy_access_mode {
	PROXY_ACCESS_MODE_HEAD = 0,
	PROXY_ACCESS_MODE_TAIL = 1,
	PROXY_ACCESS_MODE_PEEK_HEAD = 2,
	PROXY_ACCESS_MODE_PEEK_TAIL = 3,
};

#define K3_RINGACC_FIFO_WINDOW_SIZE_BYTES  (512U)
#define K3_RINGACC_FIFO_REGS_STEP	0x1000
#define K3_RINGACC_MAX_DB_RING_CNT    (127U)

/**
 * struct k3_ring_ops -  Ring operations
 */
struct k3_ring_ops {
	int (*push_tail)(struct k3_ring *ring, void *elm);
	int (*push_head)(struct k3_ring *ring, void *elm);
	int (*pop_tail)(struct k3_ring *ring, void *elm);
	int (*pop_head)(struct k3_ring *ring, void *elm);
};

/**
 * struct k3_ring - RA Ring descriptor
 *
 * @rt - Ring control/status registers
 * @fifos - Ring queues registers
 * @proxy - Ring Proxy Datapath registers
 * @ring_mem_dma - Ring buffer dma address
 * @ring_mem_virt - Ring buffer virt address
 * @ops - Ring operations
 * @size - Ring size in elements
 * @elm_size - Size of the ring element
 * @mode - Ring mode
 * @flags - flags
 * @free - Number of free elements
 * @occ - Ring occupancy
 * @windex - Write index (only for @K3_RINGACC_RING_MODE_RING)
 * @rindex - Read index (only for @K3_RINGACC_RING_MODE_RING)
 * @ring_id - Ring Id
 * @parent - Pointer on struct @k3_ringacc
 * @use_count - Use count for shared rings
 * @proxy_id - RA Ring Proxy Id (only if @K3_RINGACC_RING_USE_PROXY)
 */
struct k3_ring {
	struct k3_ring_rt_regs __iomem *rt;
	struct k3_ring_fifo_regs __iomem *fifos;
	struct k3_ringacc_proxy_target_regs  __iomem *proxy;
	dma_addr_t	ring_mem_dma;
	void		*ring_mem_virt;
	struct k3_ring_ops *ops;
	u32		size;
	enum k3_ring_size elm_size;
	enum k3_ring_mode mode;
	u32		flags;
#define K3_RING_FLAG_BUSY	BIT(1)
#define K3_RING_FLAG_SHARED	BIT(2)
	u32		free;
	u32		occ;
	u32		windex;
	u32		rindex;
	u32		ring_id;
	struct k3_ringacc	*parent;
	u32		use_count;
	int		proxy_id;
};

/**
 * struct k3_ringacc - Rings accelerator descriptor
 *
 * @dev - pointer on RA device
 * @proxy_gcfg - RA proxy global config registers
 * @proxy_target_base - RA proxy datapath region
 * @num_rings - number of ring in RA
 * @rm_gp_range - general purpose rings range from tisci
 * @dma_ring_reset_quirk - DMA reset w/a enable
 * @num_proxies - number of RA proxies
 * @rings - array of rings descriptors (struct @k3_ring)
 * @list - list of RAs in the system
 * @tisci - pointer ti-sci handle
 * @tisci_ring_ops - ti-sci rings ops
 * @tisci_dev_id - ti-sci device id
 */
struct k3_ringacc {
	struct device *dev;
	struct k3_ringacc_proxy_gcfg_regs __iomem *proxy_gcfg;
	void __iomem *proxy_target_base;
	u32 num_rings; /* number of rings in Ringacc module */
	unsigned long *rings_inuse;
	struct ti_sci_resource *rm_gp_range;

	bool dma_ring_reset_quirk;
	u32 num_proxies;
	unsigned long *proxy_inuse;

	struct k3_ring *rings;
	struct list_head list;
	struct mutex req_lock; /* protect rings allocation */

	const struct ti_sci_handle *tisci;
	const struct ti_sci_rm_ringacc_ops *tisci_ring_ops;
	u32  tisci_dev_id;
};

static long k3_ringacc_ring_get_fifo_pos(struct k3_ring *ring)
{
	return K3_RINGACC_FIFO_WINDOW_SIZE_BYTES -
	       (4 << ring->elm_size);
}

static void *k3_ringacc_get_elm_addr(struct k3_ring *ring, u32 idx)
{
	return (idx * (4 << ring->elm_size) + ring->ring_mem_virt);
}

static int k3_ringacc_ring_push_mem(struct k3_ring *ring, void *elem);
static int k3_ringacc_ring_pop_mem(struct k3_ring *ring, void *elem);

static struct k3_ring_ops k3_ring_mode_ring_ops = {
		.push_tail = k3_ringacc_ring_push_mem,
		.pop_head = k3_ringacc_ring_pop_mem,
};

static int k3_ringacc_ring_push_io(struct k3_ring *ring, void *elem);
static int k3_ringacc_ring_pop_io(struct k3_ring *ring, void *elem);
static int k3_ringacc_ring_push_head_io(struct k3_ring *ring, void *elem);
static int k3_ringacc_ring_pop_tail_io(struct k3_ring *ring, void *elem);

static struct k3_ring_ops k3_ring_mode_msg_ops = {
		.push_tail = k3_ringacc_ring_push_io,
		.push_head = k3_ringacc_ring_push_head_io,
		.pop_tail = k3_ringacc_ring_pop_tail_io,
		.pop_head = k3_ringacc_ring_pop_io,
};

static int k3_ringacc_ring_push_head_proxy(struct k3_ring *ring, void *elem);
static int k3_ringacc_ring_push_tail_proxy(struct k3_ring *ring, void *elem);
static int k3_ringacc_ring_pop_head_proxy(struct k3_ring *ring, void *elem);
static int k3_ringacc_ring_pop_tail_proxy(struct k3_ring *ring, void *elem);

static struct k3_ring_ops k3_ring_mode_proxy_ops = {
		.push_tail = k3_ringacc_ring_push_tail_proxy,
		.push_head = k3_ringacc_ring_push_head_proxy,
		.pop_tail = k3_ringacc_ring_pop_tail_proxy,
		.pop_head = k3_ringacc_ring_pop_head_proxy,
};

#ifdef CONFIG_TI_K3_RINGACC_DEBUG
void k3_ringacc_ring_dump(struct k3_ring *ring)
{
	struct device *dev = ring->parent->dev;

	k3_nav_dbg(dev, "dump ring: %d\n", ring->ring_id);
	k3_nav_dbg(dev, "dump mem virt %p, dma %pad\n",
		   ring->ring_mem_virt, &ring->ring_mem_dma);
	k3_nav_dbg(dev, "dump elmsize %d, size %d, mode %d, proxy_id %d\n",
		   ring->elm_size, ring->size, ring->mode, ring->proxy_id);

	k3_nav_dbg(dev, "dump ring_rt_regs: db%08x\n",
		   readl(&ring->rt->db));
	k3_nav_dbg(dev, "dump occ%08x\n",
		   readl(&ring->rt->occ));
	k3_nav_dbg(dev, "dump indx%08x\n",
		   readl(&ring->rt->indx));
	k3_nav_dbg(dev, "dump hwocc%08x\n",
		   readl(&ring->rt->hwocc));
	k3_nav_dbg(dev, "dump hwindx%08x\n",
		   readl(&ring->rt->hwindx));

	if (ring->ring_mem_virt)
		print_hex_dump(KERN_ERR, "dump ring_mem_virt ",
			       DUMP_PREFIX_NONE, 16, 1,
			       ring->ring_mem_virt, 16 * 8, false);
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_dump);
#endif

struct device *k3_ringacc_get_dev(struct k3_ringacc *ringacc)
{
	return ringacc->dev;
}
EXPORT_SYMBOL_GPL(k3_ringacc_get_dev);

struct k3_ring *k3_ringacc_request_ring(struct k3_ringacc *ringacc,
					int id, u32 flags)
{
	int proxy_id = K3_RINGACC_PROXY_NOT_USED;

	mutex_lock(&ringacc->req_lock);

	if (id == K3_RINGACC_RING_ID_ANY) {
		/* Request for any general purpose ring */
		struct ti_sci_resource_desc *gp_rings =
						&ringacc->rm_gp_range->desc[0];
		unsigned long size;

		size = gp_rings->start + gp_rings->num;
		id = find_next_zero_bit(ringacc->rings_inuse, size,
					gp_rings->start);
		if (id == size)
			goto error;
	} else if (id < 0) {
		goto error;
	}

	if (test_bit(id, ringacc->rings_inuse) &&
	    !(ringacc->rings[id].flags & K3_RING_FLAG_SHARED))
		goto error;
	else if (ringacc->rings[id].flags & K3_RING_FLAG_SHARED)
		goto out;

	if (flags & K3_RINGACC_RING_USE_PROXY) {
		proxy_id = find_next_zero_bit(ringacc->proxy_inuse,
					      ringacc->num_proxies, 0);
		if (proxy_id == ringacc->num_proxies)
			goto error;
	}

	if (!try_module_get(ringacc->dev->driver->owner))
		goto error;

	if (proxy_id != K3_RINGACC_PROXY_NOT_USED) {
		set_bit(proxy_id, ringacc->proxy_inuse);
		ringacc->rings[id].proxy_id = proxy_id;
		k3_nav_dbg(ringacc->dev, "Giving ring#%d proxy#%d\n",
			   id, proxy_id);
	} else {
		k3_nav_dbg(ringacc->dev, "Giving ring#%d\n", id);
	}

	set_bit(id, ringacc->rings_inuse);
out:
	ringacc->rings[id].use_count++;
	mutex_unlock(&ringacc->req_lock);
	return &ringacc->rings[id];

error:
	mutex_unlock(&ringacc->req_lock);
	return NULL;
}
EXPORT_SYMBOL_GPL(k3_ringacc_request_ring);

static void k3_ringacc_ring_reset_sci(struct k3_ring *ring)
{
	struct k3_ringacc *ringacc = ring->parent;
	int ret;

	ret = ringacc->tisci_ring_ops->config(ringacc->tisci,
					TI_SCI_MSG_VALUE_RM_RING_COUNT_VALID,
					ringacc->tisci_dev_id,
					ring->ring_id,
					0,
					0,
					ring->size,
					0,
					0,
					0);
	if (ret)
		dev_err(ringacc->dev, "TISCI reset ring fail (%d) ring_idx %d\n",
			ret, ring->ring_id);
}

void k3_ringacc_ring_reset(struct k3_ring *ring)
{
	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return;

	ring->occ = 0;
	ring->free = 0;
	ring->rindex = 0;
	ring->windex = 0;

	k3_ringacc_ring_reset_sci(ring);
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_reset);

static void k3_ringacc_ring_reconfig_qmode_sci(struct k3_ring *ring,
					       enum k3_ring_mode mode)
{
	struct k3_ringacc *ringacc = ring->parent;
	int ret;

	ret = ringacc->tisci_ring_ops->config(ringacc->tisci,
					TI_SCI_MSG_VALUE_RM_RING_MODE_VALID,
					ringacc->tisci_dev_id,
					ring->ring_id,
					0,
					0,
					0,
					mode,
					0,
					0);
	if (ret)
		dev_err(ringacc->dev, "TISCI reconf qmode fail (%d) ring_idx %d\n",
			ret, ring->ring_id);
}

void k3_ringacc_ring_reset_dma(struct k3_ring *ring, u32 occ)
{
	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return;

	if (!ring->parent->dma_ring_reset_quirk) {
		k3_ringacc_ring_reset(ring);
		return;
	}

	if (!occ)
		occ = dbg_readl(&ring->rt->occ);

	if (occ) {
		u32 db_ring_cnt, db_ring_cnt_cur;

		k3_nav_dbg(ring->parent->dev, "%s %u occ: %u\n", __func__,
			   ring->ring_id, occ);
		/* 2. Reset the ring */
		k3_ringacc_ring_reset_sci(ring);

		/*
		 * 3. Setup the ring in ring/doorbell mode
		 * (if not already in this mode)
		 */
		if (ring->mode != K3_RINGACC_RING_MODE_RING)
			k3_ringacc_ring_reconfig_qmode_sci(ring,
						K3_RINGACC_RING_MODE_RING);
		/*
		 * 4. Ring the doorbell 2**22 – ringOcc times.
		 * This will wrap the internal UDMAP ring state occupancy
		 * counter (which is 21-bits wide) to 0.
		 */
		db_ring_cnt = (1U << 22) - occ;

		while (db_ring_cnt != 0) {
			/*
			 * Ring the doorbell with the maximum count each
			 * iteration if possible to minimize the total
			 * of writes
			 */
			if (db_ring_cnt > K3_RINGACC_MAX_DB_RING_CNT)
				db_ring_cnt_cur = K3_RINGACC_MAX_DB_RING_CNT;
			else
				db_ring_cnt_cur = db_ring_cnt;

			writel(db_ring_cnt_cur, &ring->rt->db);
			db_ring_cnt -= db_ring_cnt_cur;
		}

		/* 5. Restore the original ring mode (if not ring mode) */
		if (ring->mode != K3_RINGACC_RING_MODE_RING)
			k3_ringacc_ring_reconfig_qmode_sci(ring, ring->mode);
	}

	/* 2. Reset the ring */
	k3_ringacc_ring_reset(ring);
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_reset_dma);

static void k3_ringacc_ring_free_sci(struct k3_ring *ring)
{
	struct k3_ringacc *ringacc = ring->parent;
	int ret;

	ret = ringacc->tisci_ring_ops->config(ringacc->tisci,
					      TI_SCI_MSG_VALUE_RM_ALL_NO_ORDER,
					      ringacc->tisci_dev_id,
					      ring->ring_id,
					      0,
					      0,
					      0,
					      0,
					      0,
					      0);
	if (ret)
		dev_err(ringacc->dev, "TISCI ring free fail (%d) ring_idx %d\n",
			ret, ring->ring_id);
}

int k3_ringacc_ring_free(struct k3_ring *ring)
{
	struct k3_ringacc *ringacc;

	if (!ring)
		return -EINVAL;

	ringacc = ring->parent;

	k3_nav_dbg(ring->parent->dev, "flags: 0x%08x\n", ring->flags);

	if (!test_bit(ring->ring_id, ringacc->rings_inuse))
		return -EINVAL;

	mutex_lock(&ringacc->req_lock);

	if (--ring->use_count)
		goto out;

	if (!(ring->flags & K3_RING_FLAG_BUSY))
		goto no_init;

	k3_ringacc_ring_free_sci(ring);

	dma_free_coherent(ringacc->dev,
			  ring->size * (4 << ring->elm_size),
			  ring->ring_mem_virt, ring->ring_mem_dma);
	ring->flags = 0;
	ring->ops = NULL;
	if (ring->proxy_id != K3_RINGACC_PROXY_NOT_USED) {
		clear_bit(ring->proxy_id, ringacc->proxy_inuse);
		ring->proxy = NULL;
		ring->proxy_id = K3_RINGACC_PROXY_NOT_USED;
	}

no_init:
	clear_bit(ring->ring_id, ringacc->rings_inuse);

	module_put(ringacc->dev->driver->owner);

out:
	mutex_unlock(&ringacc->req_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_free);

u32 k3_ringacc_get_ring_id(struct k3_ring *ring)
{
	if (!ring)
		return -EINVAL;

	return ring->ring_id;
}
EXPORT_SYMBOL_GPL(k3_ringacc_get_ring_id);

u32 k3_ringacc_get_tisci_dev_id(struct k3_ring *ring)
{
	if (!ring)
		return -EINVAL;

	return ring->parent->tisci_dev_id;
}
EXPORT_SYMBOL_GPL(k3_ringacc_get_tisci_dev_id);

static int k3_ringacc_ring_cfg_sci(struct k3_ring *ring)
{
	struct k3_ringacc *ringacc = ring->parent;
	u32 ring_idx;
	int ret;

	if (!ringacc->tisci)
		return -EINVAL;

	ring_idx = ring->ring_id;
	ret = ringacc->tisci_ring_ops->config(ringacc->tisci,
					      TI_SCI_MSG_VALUE_RM_ALL_NO_ORDER,
					      ringacc->tisci_dev_id,
					      ring_idx,
					      lower_32_bits(ring->ring_mem_dma),
					      upper_32_bits(ring->ring_mem_dma),
					      ring->size,
					      ring->mode,
					      ring->elm_size,
					      0);
	if (ret)
		dev_err(ringacc->dev, "TISCI config ring fail (%d) ring_idx %d\n",
			ret, ring_idx);

	return ret;
}

int k3_ringacc_ring_cfg(struct k3_ring *ring, struct k3_ring_cfg *cfg)
{
	struct k3_ringacc *ringacc = ring->parent;
	int ret = 0;

	if (!ring || !cfg)
		return -EINVAL;
	if (cfg->elm_size > K3_RINGACC_RING_ELSIZE_256 ||
	    cfg->mode > K3_RINGACC_RING_MODE_QM ||
	    cfg->size & ~K3_RINGACC_CFG_RING_SIZE_ELCNT_MASK ||
	    !test_bit(ring->ring_id, ringacc->rings_inuse))
		return -EINVAL;

	if (ring->use_count != 1)
		return 0;

	ring->size = cfg->size;
	ring->elm_size = cfg->elm_size;
	ring->mode = cfg->mode;
	ring->occ = 0;
	ring->free = 0;
	ring->rindex = 0;
	ring->windex = 0;

	if (ring->proxy_id != K3_RINGACC_PROXY_NOT_USED)
		ring->proxy = ringacc->proxy_target_base +
			      ring->proxy_id * K3_RINGACC_PROXY_TARGET_STEP;

	switch (ring->mode) {
	case K3_RINGACC_RING_MODE_RING:
		ring->ops = &k3_ring_mode_ring_ops;
		break;
	case K3_RINGACC_RING_MODE_QM:
		/*
		 * In Queue mode elm_size can be 8 only and each operation
		 * uses 2 element slots
		 */
		if (cfg->elm_size != K3_RINGACC_RING_ELSIZE_8 ||
		    cfg->size % 2)
			goto err_free_proxy;
	case K3_RINGACC_RING_MODE_MESSAGE:
		if (ring->proxy)
			ring->ops = &k3_ring_mode_proxy_ops;
		else
			ring->ops = &k3_ring_mode_msg_ops;
		break;
	default:
		ring->ops = NULL;
		ret = -EINVAL;
		goto err_free_proxy;
	};

	ring->ring_mem_virt =
			dma_zalloc_coherent(ringacc->dev,
					    ring->size * (4 << ring->elm_size),
					    &ring->ring_mem_dma, GFP_KERNEL);
	if (!ring->ring_mem_virt) {
		dev_err(ringacc->dev, "Failed to alloc ring mem\n");
		ret = -ENOMEM;
		goto err_free_ops;
	}

	ret = k3_ringacc_ring_cfg_sci(ring);

	if (ret)
		goto err_free_mem;

	ring->flags |= K3_RING_FLAG_BUSY;
	ring->flags |= (cfg->flags & K3_RINGACC_RING_SHARED) ?
			K3_RING_FLAG_SHARED : 0;

	k3_ringacc_ring_dump(ring);

	return 0;

err_free_mem:
	dma_free_coherent(ringacc->dev,
			  ring->size * (4 << ring->elm_size),
			  ring->ring_mem_virt,
			  ring->ring_mem_dma);
err_free_ops:
	ring->ops = NULL;
err_free_proxy:
	ring->proxy = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_cfg);

u32 k3_ringacc_ring_get_size(struct k3_ring *ring)
{
	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return -EINVAL;

	return ring->size;
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_get_size);

u32 k3_ringacc_ring_get_free(struct k3_ring *ring)
{
	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return -EINVAL;

	if (!ring->free)
		ring->free = ring->size - dbg_readl(&ring->rt->occ);

	return ring->free;
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_get_free);

u32 k3_ringacc_ring_get_occ(struct k3_ring *ring)
{
	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return -EINVAL;

	return dbg_readl(&ring->rt->occ);
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_get_occ);

u32 k3_ringacc_ring_is_full(struct k3_ring *ring)
{
	return !k3_ringacc_ring_get_free(ring);
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_is_full);

enum k3_ringacc_access_mode {
	K3_RINGACC_ACCESS_MODE_PUSH_HEAD,
	K3_RINGACC_ACCESS_MODE_POP_HEAD,
	K3_RINGACC_ACCESS_MODE_PUSH_TAIL,
	K3_RINGACC_ACCESS_MODE_POP_TAIL,
	K3_RINGACC_ACCESS_MODE_PEEK_HEAD,
	K3_RINGACC_ACCESS_MODE_PEEK_TAIL,
};

static int k3_ringacc_ring_cfg_proxy(struct k3_ring *ring,
				     enum k3_ringacc_proxy_access_mode mode)
{
	u32 val;

	val = ring->ring_id;
	val |= mode << 16;
	val |= ring->elm_size << 24;
	dbg_writel(val, &ring->proxy->control);
	return 0;
}

static int k3_ringacc_ring_access_proxy(struct k3_ring *ring, void *elem,
					enum k3_ringacc_access_mode access_mode)
{
	void __iomem *ptr;

	ptr = (void __iomem *)&ring->proxy->data;

	switch (access_mode) {
	case K3_RINGACC_ACCESS_MODE_PUSH_HEAD:
	case K3_RINGACC_ACCESS_MODE_POP_HEAD:
		k3_ringacc_ring_cfg_proxy(ring, PROXY_ACCESS_MODE_HEAD);
		break;
	case K3_RINGACC_ACCESS_MODE_PUSH_TAIL:
	case K3_RINGACC_ACCESS_MODE_POP_TAIL:
		k3_ringacc_ring_cfg_proxy(ring, PROXY_ACCESS_MODE_TAIL);
		break;
	default:
		return -EINVAL;
	}

	ptr += k3_ringacc_ring_get_fifo_pos(ring);

	switch (access_mode) {
	case K3_RINGACC_ACCESS_MODE_POP_HEAD:
	case K3_RINGACC_ACCESS_MODE_POP_TAIL:
		k3_nav_dbg(ring->parent->dev, "proxy:memcpy_fromio(x): --> ptr(%p), mode:%d\n",
			   ptr, access_mode);
		memcpy_fromio(elem, ptr, (4 << ring->elm_size));
		ring->occ--;
		break;
	case K3_RINGACC_ACCESS_MODE_PUSH_TAIL:
	case K3_RINGACC_ACCESS_MODE_PUSH_HEAD:
		k3_nav_dbg(ring->parent->dev, "proxy:memcpy_toio(x): --> ptr(%p), mode:%d\n",
			   ptr, access_mode);
		memcpy_toio(ptr, elem, (4 << ring->elm_size));
		ring->free--;
		break;
	default:
		return -EINVAL;
	}

	k3_nav_dbg(ring->parent->dev, "proxy: free%d occ%d\n",
		   ring->free, ring->occ);
	return 0;
}

static int k3_ringacc_ring_push_head_proxy(struct k3_ring *ring, void *elem)
{
	return k3_ringacc_ring_access_proxy(ring, elem,
					    K3_RINGACC_ACCESS_MODE_PUSH_HEAD);
}

static int k3_ringacc_ring_push_tail_proxy(struct k3_ring *ring, void *elem)
{
	return k3_ringacc_ring_access_proxy(ring, elem,
					    K3_RINGACC_ACCESS_MODE_PUSH_TAIL);
}

static int k3_ringacc_ring_pop_head_proxy(struct k3_ring *ring, void *elem)
{
	return k3_ringacc_ring_access_proxy(ring, elem,
					    K3_RINGACC_ACCESS_MODE_POP_HEAD);
}

static int k3_ringacc_ring_pop_tail_proxy(struct k3_ring *ring, void *elem)
{
	return k3_ringacc_ring_access_proxy(ring, elem,
					    K3_RINGACC_ACCESS_MODE_POP_HEAD);
}

static int k3_ringacc_ring_access_io(struct k3_ring *ring, void *elem,
				     enum k3_ringacc_access_mode access_mode)
{
	void __iomem *ptr;

	switch (access_mode) {
	case K3_RINGACC_ACCESS_MODE_PUSH_HEAD:
	case K3_RINGACC_ACCESS_MODE_POP_HEAD:
		ptr = (void __iomem *)&ring->fifos->head_data;
		break;
	case K3_RINGACC_ACCESS_MODE_PUSH_TAIL:
	case K3_RINGACC_ACCESS_MODE_POP_TAIL:
		ptr = (void __iomem *)&ring->fifos->tail_data;
		break;
	default:
		return -EINVAL;
	}

	ptr += k3_ringacc_ring_get_fifo_pos(ring);

	switch (access_mode) {
	case K3_RINGACC_ACCESS_MODE_POP_HEAD:
	case K3_RINGACC_ACCESS_MODE_POP_TAIL:
		k3_nav_dbg(ring->parent->dev, "memcpy_fromio(x): --> ptr(%p), mode:%d\n",
			   ptr, access_mode);
		memcpy_fromio(elem, ptr, (4 << ring->elm_size));
		ring->occ--;
		break;
	case K3_RINGACC_ACCESS_MODE_PUSH_TAIL:
	case K3_RINGACC_ACCESS_MODE_PUSH_HEAD:
		k3_nav_dbg(ring->parent->dev, "memcpy_toio(x): --> ptr(%p), mode:%d\n",
			   ptr, access_mode);
		memcpy_toio(ptr, elem, (4 << ring->elm_size));
		ring->free--;
		break;
	default:
		return -EINVAL;
	}

	k3_nav_dbg(ring->parent->dev, "free%d index%d occ%d index%d\n",
		   ring->free, ring->windex, ring->occ, ring->rindex);
	return 0;
}

static int k3_ringacc_ring_push_head_io(struct k3_ring *ring, void *elem)
{
	return k3_ringacc_ring_access_io(ring, elem,
					 K3_RINGACC_ACCESS_MODE_PUSH_HEAD);
}

static int k3_ringacc_ring_push_io(struct k3_ring *ring, void *elem)
{
	return k3_ringacc_ring_access_io(ring, elem,
					 K3_RINGACC_ACCESS_MODE_PUSH_TAIL);
}

static int k3_ringacc_ring_pop_io(struct k3_ring *ring, void *elem)
{
	return k3_ringacc_ring_access_io(ring, elem,
					 K3_RINGACC_ACCESS_MODE_POP_HEAD);
}

static int k3_ringacc_ring_pop_tail_io(struct k3_ring *ring, void *elem)
{
	return k3_ringacc_ring_access_io(ring, elem,
					 K3_RINGACC_ACCESS_MODE_POP_HEAD);
}

static int k3_ringacc_ring_push_mem(struct k3_ring *ring, void *elem)
{
	void *elem_ptr;

	elem_ptr = k3_ringacc_get_elm_addr(ring, ring->windex);

	memcpy(elem_ptr, elem, (4 << ring->elm_size));

	ring->windex = (ring->windex + 1) % ring->size;
	ring->free--;
	dbg_writel(1, &ring->rt->db);

	k3_nav_dbg(ring->parent->dev, "ring_push_mem: free%d index%d\n",
		   ring->free, ring->windex);

	return 0;
}

static int k3_ringacc_ring_pop_mem(struct k3_ring *ring, void *elem)
{
	void *elem_ptr;

	elem_ptr = k3_ringacc_get_elm_addr(ring, ring->rindex);

	memcpy(elem, elem_ptr, (4 << ring->elm_size));

	ring->rindex = (ring->rindex + 1) % ring->size;
	ring->occ--;
	dbg_writel(-1, &ring->rt->db);

	k3_nav_dbg(ring->parent->dev, "ring_pop_mem: occ%d index%d pos_ptr%p\n",
		   ring->occ, ring->rindex, elem_ptr);
	return 0;
}

int k3_ringacc_ring_push(struct k3_ring *ring, void *elem)
{
	int ret = -EOPNOTSUPP;

	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return -EINVAL;

	k3_nav_dbg(ring->parent->dev, "ring_push: free%d index%d\n",
		   ring->free, ring->windex);

	if (k3_ringacc_ring_is_full(ring))
		return -ENOMEM;

	if (ring->ops && ring->ops->push_tail)
		ret = ring->ops->push_tail(ring, elem);

	return ret;
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_push);

int k3_ringacc_ring_push_head(struct k3_ring *ring, void *elem)
{
	int ret = -EOPNOTSUPP;

	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return -EINVAL;

	k3_nav_dbg(ring->parent->dev, "ring_push_head: free%d index%d\n",
		   ring->free, ring->windex);

	if (k3_ringacc_ring_is_full(ring))
		return -ENOMEM;

	if (ring->ops && ring->ops->push_head)
		ret = ring->ops->push_head(ring, elem);

	return ret;
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_push_head);

int k3_ringacc_ring_pop(struct k3_ring *ring, void *elem)
{
	int ret = -EOPNOTSUPP;

	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return -EINVAL;

	if (!ring->occ)
		ring->occ = k3_ringacc_ring_get_occ(ring);

	k3_nav_dbg(ring->parent->dev, "ring_pop: occ%d index%d\n",
		   ring->occ, ring->rindex);

	if (!ring->occ)
		return -ENODATA;

	if (ring->ops && ring->ops->pop_head)
		ret = ring->ops->pop_head(ring, elem);

	return ret;
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_pop);

int k3_ringacc_ring_pop_tail(struct k3_ring *ring, void *elem)
{
	int ret = -EOPNOTSUPP;

	if (!ring || !(ring->flags & K3_RING_FLAG_BUSY))
		return -EINVAL;

	if (!ring->occ)
		ring->occ = k3_ringacc_ring_get_occ(ring);

	k3_nav_dbg(ring->parent->dev, "ring_pop_tail: occ%d index%d\n",
		   ring->occ, ring->rindex);

	if (!ring->occ)
		return -ENODATA;

	if (ring->ops && ring->ops->pop_tail)
		ret = ring->ops->pop_tail(ring, elem);

	return ret;
}
EXPORT_SYMBOL_GPL(k3_ringacc_ring_pop_tail);

struct k3_ringacc *of_k3_ringacc_get_by_phandle(struct device_node *np,
						const char *property)
{
	struct device_node *ringacc_np;
	struct k3_ringacc *ringacc = ERR_PTR(-EPROBE_DEFER);
	struct k3_ringacc *entry;

	ringacc_np = of_parse_phandle(np, property, 0);
	if (!ringacc_np)
		return ERR_PTR(-ENODEV);

	mutex_lock(&k3_ringacc_list_lock);
	list_for_each_entry(entry, &k3_ringacc_list, list)
		if (entry->dev->of_node == ringacc_np) {
			ringacc = entry;
			break;
		}
	mutex_unlock(&k3_ringacc_list_lock);
	of_node_put(ringacc_np);

	return ringacc;
}
EXPORT_SYMBOL_GPL(of_k3_ringacc_get_by_phandle);

static int k3_ringacc_probe_dt(struct k3_ringacc *ringacc)
{
	struct device_node *node = ringacc->dev->of_node;
	struct device *dev = ringacc->dev;
	int ret;

	if (!node) {
		dev_err(dev, "device tree info unavailable\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(node, "ti,num-rings", &ringacc->num_rings);
	if (ret) {
		dev_err(dev, "ti,num-rings read failure %d\n", ret);
		return ret;
	}

	ringacc->dma_ring_reset_quirk =
			of_property_read_bool(node, "ti,dma-ring-reset-quirk");

	ringacc->tisci = ti_sci_get_by_phandle(node, "ti,sci");
	if (IS_ERR(ringacc->tisci)) {
		ret = PTR_ERR(ringacc->tisci);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "ti,sci read fail %d\n", ret);
		ringacc->tisci = NULL;
		return ret;
	}

	ret = of_property_read_u32(node, "ti,sci-dev-id",
				   &ringacc->tisci_dev_id);
	if (ret)
		dev_err(dev, "ti,sci-dev-id read fail %d\n", ret);

	ringacc->rm_gp_range = devm_ti_sci_get_of_resource(ringacc->tisci, dev,
						ringacc->tisci_dev_id,
						"ti,sci-rm-range-gp-rings");
	if (IS_ERR(ringacc->rm_gp_range))
		ret = PTR_ERR(ringacc->rm_gp_range);

	return ret;
}

static int k3_ringacc_probe(struct platform_device *pdev)
{
	struct k3_ringacc *ringacc;
	void __iomem *base_fifo, *base_rt;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret, i;

	ringacc = devm_kzalloc(dev, sizeof(*ringacc), GFP_KERNEL);
	if (!ringacc)
		return -ENOMEM;

	ringacc->dev = dev;
	mutex_init(&ringacc->req_lock);

	ret = k3_ringacc_probe_dt(ringacc);
	if (ret)
		return ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rt");
	base_rt = devm_ioremap_resource(dev, res);
	if (IS_ERR(base_rt))
		return PTR_ERR(base_rt);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fifos");
	base_fifo = devm_ioremap_resource(dev, res);
	if (IS_ERR(base_fifo))
		return PTR_ERR(base_fifo);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "proxy_gcfg");
	ringacc->proxy_gcfg = devm_ioremap_resource(dev, res);
	if (IS_ERR(ringacc->proxy_gcfg))
		return PTR_ERR(ringacc->proxy_gcfg);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "proxy_target");
	ringacc->proxy_target_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(ringacc->proxy_target_base))
		return PTR_ERR(ringacc->proxy_target_base);

	ringacc->num_proxies = dbg_readl(&ringacc->proxy_gcfg->config) &
					 K3_RINGACC_PROXY_CFG_THREADS_MASK;

	ringacc->rings = devm_kzalloc(dev,
				      sizeof(*ringacc->rings) *
				      ringacc->num_rings,
				      GFP_KERNEL);
	ringacc->rings_inuse = devm_kcalloc(dev,
					    BITS_TO_LONGS(ringacc->num_rings),
					    sizeof(unsigned long), GFP_KERNEL);
	ringacc->proxy_inuse = devm_kcalloc(dev,
					    BITS_TO_LONGS(ringacc->num_proxies),
					    sizeof(unsigned long), GFP_KERNEL);

	if (!ringacc->rings || !ringacc->rings_inuse || !ringacc->proxy_inuse)
		return -ENOMEM;

	for (i = 0; i < ringacc->num_rings; i++) {
		ringacc->rings[i].rt = base_rt +
				       K3_RINGACC_RT_REGS_STEP * i;
		ringacc->rings[i].fifos = base_fifo +
					  K3_RINGACC_FIFO_REGS_STEP * i;
		ringacc->rings[i].parent = ringacc;
		ringacc->rings[i].ring_id = i;
		ringacc->rings[i].proxy_id = K3_RINGACC_PROXY_NOT_USED;
	}
	dev_set_drvdata(dev, ringacc);

	ringacc->tisci_ring_ops = &ringacc->tisci->ops.rm_ring_ops;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_put_noidle(dev);
		dev_err(dev, "Failed to enable pm %d\n", ret);
		goto err;
	}

	mutex_lock(&k3_ringacc_list_lock);
	list_add_tail(&ringacc->list, &k3_ringacc_list);
	mutex_unlock(&k3_ringacc_list_lock);

	dev_info(dev, "Ring Accelerator probed rings:%u, gp-rings[%u,%u] sci-dev-id:%u\n",
		 ringacc->num_rings,
		 ringacc->rm_gp_range->desc[0].start,
		 ringacc->rm_gp_range->desc[0].num,
		 ringacc->tisci_dev_id);
	dev_info(dev, "dma-ring-reset-quirk: %s\n",
		 ringacc->dma_ring_reset_quirk ? "enabled" : "disabled");
	dev_info(dev, "RA Proxy rev. %08x, num_proxies:%u\n",
		 dbg_readl(&ringacc->proxy_gcfg->revision),
		 ringacc->num_proxies);
	return 0;

err:
	pm_runtime_disable(dev);
	return ret;
}

static int k3_ringacc_remove(struct platform_device *pdev)
{
	struct k3_ringacc *ringacc = dev_get_drvdata(&pdev->dev);

	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	mutex_lock(&k3_ringacc_list_lock);
	list_del(&ringacc->list);
	mutex_unlock(&k3_ringacc_list_lock);
	return 0;
}

/* Match table for of_platform binding */
static const struct of_device_id k3_ringacc_of_match[] = {
	{ .compatible = "ti,am654-navss-ringacc", },
	{},
};
MODULE_DEVICE_TABLE(of, k3_ringacc_of_match);

static struct platform_driver k3_ringacc_driver = {
	.probe		= k3_ringacc_probe,
	.remove		= k3_ringacc_remove,
	.driver		= {
		.name	= "k3-ringacc",
		.of_match_table = k3_ringacc_of_match,
	},
};
module_platform_driver(k3_ringacc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TI Ringacc driver for K3 SOCs");
MODULE_AUTHOR("Grygorii Strashko <grygorii.strashko@ti.com>");
