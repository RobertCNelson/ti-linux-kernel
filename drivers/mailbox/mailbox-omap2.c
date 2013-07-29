/*
 * Mailbox reservation modules for OMAP2/3
 *
 * Copyright (C) 2006-2009 Nokia Corporation
 * Written by: Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 *        and  Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/platform_data/mailbox-omap.h>

#include "omap-mbox.h"

#define MAILBOX_REVISION		0x000
#define MAILBOX_MESSAGE(m)		(0x040 + 4 * (m))
#define MAILBOX_FIFOSTATUS(m)		(0x080 + 4 * (m))
#define MAILBOX_MSGSTATUS(m)		(0x0c0 + 4 * (m))
#define MAILBOX_IRQSTATUS(u)		(0x100 + 8 * (u))
#define MAILBOX_IRQENABLE(u)		(0x104 + 8 * (u))

#define OMAP4_MAILBOX_IRQSTATUS(u)	(0x104 + 0x10 * (u))
#define OMAP4_MAILBOX_IRQENABLE(u)	(0x108 + 0x10 * (u))
#define OMAP4_MAILBOX_IRQENABLE_CLR(u)	(0x10c + 0x10 * (u))

#define MAILBOX_IRQ_NEWMSG(m)		(1 << (2 * (m)))
#define MAILBOX_IRQ_NOTFULL(m)		(1 << (2 * (m) + 1))

#define AM33X_MBOX_WKUPM3_USR		3

#define MBOX_REG_SIZE			0x120

#define OMAP4_MBOX_REG_SIZE		0x130

#define MBOX_NR_REGS			(MBOX_REG_SIZE / sizeof(u32))
#define OMAP4_MBOX_NR_REGS		(OMAP4_MBOX_REG_SIZE / sizeof(u32))

struct omap_mbox2_fifo {
	unsigned long msg;
	unsigned long fifo_stat;
	unsigned long msg_stat;
};

struct omap_mbox2_priv {
	struct omap_mbox2_fifo tx_fifo;
	struct omap_mbox2_fifo rx_fifo;
	unsigned long irqenable;
	unsigned long irqstatus;
	u32 newmsg_bit;
	u32 notfull_bit;
	u32 ctx[OMAP4_MBOX_NR_REGS];
	unsigned long irqdisable;
	u32 intr_type;
};

static inline
unsigned int mbox_read_reg(struct omap_mbox_device *mdev, size_t ofs)
{
	return __raw_readl(mdev->mbox_base + ofs);
}

static inline
void mbox_write_reg(struct omap_mbox_device *mdev, u32 val, size_t ofs)
{
	__raw_writel(val, mdev->mbox_base + ofs);
}

/* Mailbox H/W preparations */
static int omap2_mbox_startup(struct omap_mbox *mbox)
{
	pm_runtime_get_sync(mbox->parent->dev);

	/*
	 * just print the raw revision register, the format is not
	 * uniform across all SoCs
	 */
	if (!mbox->use_count) {
		u32 l = mbox_read_reg(mbox->parent, MAILBOX_REVISION);
		pr_debug("omap mailbox rev 0x%x\n", l);
	}

	return 0;
}

static void omap2_mbox_shutdown(struct omap_mbox *mbox)
{
	pm_runtime_put_sync(mbox->parent->dev);
}

/* Mailbox FIFO handle functions */
static mbox_msg_t omap2_mbox_fifo_read(struct omap_mbox *mbox)
{
	struct omap_mbox2_fifo *fifo =
		&((struct omap_mbox2_priv *)mbox->priv)->rx_fifo;
	return (mbox_msg_t) mbox_read_reg(mbox->parent, fifo->msg);
}

static void omap2_mbox_fifo_write(struct omap_mbox *mbox, mbox_msg_t msg)
{
	struct omap_mbox2_fifo *fifo =
		&((struct omap_mbox2_priv *)mbox->priv)->tx_fifo;
	mbox_write_reg(mbox->parent, msg, fifo->msg);
}

static int omap2_mbox_fifo_empty(struct omap_mbox *mbox)
{
	struct omap_mbox2_fifo *fifo =
		&((struct omap_mbox2_priv *)mbox->priv)->rx_fifo;
	return (mbox_read_reg(mbox->parent, fifo->msg_stat) == 0);
}

static int omap2_mbox_fifo_full(struct omap_mbox *mbox)
{
	struct omap_mbox2_fifo *fifo =
		&((struct omap_mbox2_priv *)mbox->priv)->tx_fifo;
	return mbox_read_reg(mbox->parent, fifo->fifo_stat);
}

static int omap2_mbox_poll_for_space(struct omap_mbox *mbox)
{
	if (omap2_mbox_fifo_full(mbox))
		return -1;

	return 0;
}

/* Mailbox IRQ handle functions */
static void omap2_mbox_enable_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 l, bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;

	l = mbox_read_reg(mbox->parent, p->irqenable);
	l |= bit;
	mbox_write_reg(mbox->parent, l, p->irqenable);
}

static void omap2_mbox_disable_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;

	/*
	 * Read and update the interrupt configuration register for pre-OMAP4.
	 * OMAP4 and later SoCs have a dedicated interrupt disabling register.
	 */
	if (!p->intr_type)
		bit = mbox_read_reg(mbox->parent, p->irqdisable) & ~bit;

	mbox_write_reg(mbox->parent, bit, p->irqdisable);
}

static void omap2_mbox_ack_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;

	mbox_write_reg(mbox->parent, bit, p->irqstatus);

	/* Flush posted write for irq status to avoid spurious interrupts */
	mbox_read_reg(mbox->parent, p->irqstatus);
}

static int omap2_mbox_is_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;
	u32 enable = mbox_read_reg(mbox->parent, p->irqenable);
	u32 status = mbox_read_reg(mbox->parent, p->irqstatus);

	return (int)(enable & status & bit);
}

static void wkupm3_mbox_enable_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 l, bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;
	unsigned long irqenable = ((irq == IRQ_RX) ?
		OMAP4_MAILBOX_IRQENABLE(AM33X_MBOX_WKUPM3_USR) : p->irqenable);

	l = mbox_read_reg(mbox->parent, irqenable);
	l |= bit;
	mbox_write_reg(mbox->parent, l, irqenable);
}

static void wkupm3_mbox_disable_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;
	unsigned long irqdisable = ((irq == IRQ_RX) ?
	    OMAP4_MAILBOX_IRQENABLE_CLR(AM33X_MBOX_WKUPM3_USR) : p->irqdisable);

	mbox_write_reg(mbox->parent, bit, irqdisable);
}

static void wkupm3_mbox_ack_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;
	unsigned long irqstatus = ((irq == IRQ_RX) ?
		OMAP4_MAILBOX_IRQSTATUS(AM33X_MBOX_WKUPM3_USR) : p->irqstatus);

	mbox_write_reg(mbox->parent, bit, irqstatus);

	/* Flush posted write for irq status to avoid spurious interrupts */
	mbox_read_reg(mbox->parent, irqstatus);
}

static int wkupm3_mbox_is_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;
	u32 enable, status;

	/* WkupM3 mailbox does not use a receive queue */
	if (irq == IRQ_RX)
		return 0;

	enable = mbox_read_reg(mbox->parent, p->irqenable);
	status = mbox_read_reg(mbox->parent, p->irqstatus);

	return (int)(enable & status & bit);
}

static void omap2_mbox_save_ctx(struct omap_mbox *mbox)
{
	int i;
	struct omap_mbox2_priv *p = mbox->priv;
	int nr_regs;

	if (p->intr_type)
		nr_regs = OMAP4_MBOX_NR_REGS;
	else
		nr_regs = MBOX_NR_REGS;
	for (i = 0; i < nr_regs; i++) {
		p->ctx[i] = mbox_read_reg(mbox->parent, i * sizeof(u32));

		dev_dbg(mbox->dev, "%s: [%02x] %08x\n", __func__,
			i, p->ctx[i]);
	}
}

static void omap2_mbox_restore_ctx(struct omap_mbox *mbox)
{
	int i;
	struct omap_mbox2_priv *p = mbox->priv;
	int nr_regs;

	if (p->intr_type)
		nr_regs = OMAP4_MBOX_NR_REGS;
	else
		nr_regs = MBOX_NR_REGS;
	for (i = 0; i < nr_regs; i++) {
		mbox_write_reg(mbox->parent, p->ctx[i], i * sizeof(u32));

		dev_dbg(mbox->dev, "%s: [%02x] %08x\n", __func__,
			i, p->ctx[i]);
	}
}

static void wkupm3_mbox_send_data(struct omap_mbox *mbox, mbox_msg_t msg)
{
	mbox_msg_t rmsg;

	/* enable the mbox Rx interrupt for WkupM3 only briefly */
	wkupm3_mbox_enable_irq(mbox, IRQ_RX);
	omap2_mbox_fifo_write(mbox, msg);
	wkupm3_mbox_disable_irq(mbox, IRQ_RX);

	/* read back the message and ack the interrupt on behalf of WkupM3 */
	rmsg = omap2_mbox_fifo_read(mbox);
	wkupm3_mbox_ack_irq(mbox, IRQ_RX);
}

static struct omap_mbox_ops omap2_mbox_ops = {
	.startup	= omap2_mbox_startup,
	.shutdown	= omap2_mbox_shutdown,
	.fifo_read	= omap2_mbox_fifo_read,
	.fifo_write	= omap2_mbox_fifo_write,
	.fifo_empty	= omap2_mbox_fifo_empty,
	.poll_for_space	= omap2_mbox_poll_for_space,
	.enable_irq	= omap2_mbox_enable_irq,
	.disable_irq	= omap2_mbox_disable_irq,
	.ack_irq	= omap2_mbox_ack_irq,
	.is_irq		= omap2_mbox_is_irq,
	.save_ctx	= omap2_mbox_save_ctx,
	.restore_ctx	= omap2_mbox_restore_ctx,
};

static struct omap_mbox_ops wkupm3_mbox_ops = {
	.startup	= omap2_mbox_startup,
	.shutdown	= omap2_mbox_shutdown,
	.fifo_read	= omap2_mbox_fifo_read,
	.fifo_write	= wkupm3_mbox_send_data,
	.fifo_empty	= omap2_mbox_fifo_empty,
	.poll_for_space	= omap2_mbox_poll_for_space,
	.enable_irq	= wkupm3_mbox_enable_irq,
	.disable_irq	= wkupm3_mbox_disable_irq,
	.ack_irq	= wkupm3_mbox_ack_irq,
	.is_irq		= wkupm3_mbox_is_irq,
	.save_ctx	= omap2_mbox_save_ctx,
	.restore_ctx	= omap2_mbox_restore_ctx,
};

static const struct of_device_id omap_mailbox_of_match[] = {
	{
		.compatible	= "ti,omap2-mailbox",
		.data		= (void *) MBOX_INTR_CFG_TYPE1,
	},
	{
		.compatible	= "ti,omap4-mailbox",
		.data		= (void *) MBOX_INTR_CFG_TYPE2,
	},
	{
		/* end */
	},
};
MODULE_DEVICE_TABLE(of, omap_mailbox_of_match);

static int omap2_mbox_probe(struct platform_device *pdev)
{
	struct resource *mem;
	int ret;
	struct omap_mbox **list, *mbox, *mboxblk;
	struct omap_mbox2_priv *priv, *privblk;
	struct omap_mbox_pdata *pdata = pdev->dev.platform_data;
	struct omap_mbox_device *mdev;
	struct omap_mbox_dev_info *info, *of_info = NULL;
	struct device_node *node = pdev->dev.of_node;
	int i, j;
	u32 info_count = 0, intr_type = 0;
	u32 num_users = 0, num_fifos = 0;
	u32 dlen, dsize = 4;
	u32 *tmp;
	const __be32 *mbox_data;

	if (!node && (!pdata || !pdata->info_cnt || !pdata->info)) {
		pr_err("%s: platform not supported\n", __func__);
		return -ENODEV;
	}

	if (node) {
		intr_type = (u32)of_match_device(omap_mailbox_of_match,
							&pdev->dev)->data;
		if (intr_type != 0 && intr_type != 1) {
			dev_err(&pdev->dev, "invalid match data value\n");
			return -EINVAL;
		}

		if (of_property_read_u32(node, "ti,mbox-num-users",
								&num_users)) {
			dev_err(&pdev->dev,
				"no ti,mbox-num-users configuration found\n");
			return -ENODEV;
		}

		if (of_property_read_u32(node, "ti,mbox-num-fifos",
								&num_fifos)) {
			dev_err(&pdev->dev,
				"no ti,mbox-num-fifos configuration found\n");
			return -ENODEV;
		}

		info_count = of_property_count_strings(node, "ti,mbox-names");
		if (!info_count) {
			dev_err(&pdev->dev, "no mbox devices found\n");
			return -ENODEV;
		}

		mbox_data = of_get_property(node, "ti,mbox-data", &dlen);
		if (!mbox_data) {
			dev_err(&pdev->dev, "no mbox device data found\n");
			return -ENODEV;
		}
		dlen /= sizeof(dsize);
		if (dlen != dsize * info_count) {
			dev_err(&pdev->dev, "mbox device data is truncated\n");
			return -ENODEV;
		}

		of_info = kzalloc(info_count * sizeof(*of_info), GFP_KERNEL);
		if (!of_info)
			return -ENOMEM;

		i = 0;
		while (i < info_count) {
			info = of_info + i;
			if (of_property_read_string_index(node,
					"ti,mbox-names", i,  &info->name)) {
				dev_err(&pdev->dev,
					"mbox_name [%d] read failed\n", i);
				ret = -ENODEV;
				goto free_of;
			}

			tmp = &info->tx_id;
			for (j = 0; j < dsize; j++) {
				tmp[j] = of_read_number(
						mbox_data + j + (i * dsize), 1);
			}
			i++;
		}
	}

	if (!node) { /* non-DT device creation */
		info_count = pdata->info_cnt;
		info = pdata->info;
		intr_type = pdata->intr_type;
		num_users = pdata->num_users;
		num_fifos = pdata->num_fifos;
	} else {
		info = of_info;
	}

	mdev = kzalloc(sizeof(*mdev), GFP_KERNEL);
	if (!mdev) {
		ret = -ENOMEM;
		goto free_of;
	}

	/* allocate one extra for marking end of list */
	list = kzalloc((info_count + 1) * sizeof(*list), GFP_KERNEL);
	if (!list) {
		ret = -ENOMEM;
		goto free_mdev;
	}

	mboxblk = mbox = kzalloc(info_count * sizeof(*mbox), GFP_KERNEL);
	if (!mboxblk) {
		ret = -ENOMEM;
		goto free_list;
	}

	privblk = priv = kzalloc(info_count * sizeof(*priv), GFP_KERNEL);
	if (!privblk) {
		ret = -ENOMEM;
		goto free_mboxblk;
	}

	for (i = 0; i < info_count; i++, info++, priv++) {
		priv->tx_fifo.msg = MAILBOX_MESSAGE(info->tx_id);
		priv->tx_fifo.fifo_stat = MAILBOX_FIFOSTATUS(info->tx_id);
		priv->rx_fifo.msg =  MAILBOX_MESSAGE(info->rx_id);
		priv->rx_fifo.msg_stat =  MAILBOX_MSGSTATUS(info->rx_id);
		priv->notfull_bit = MAILBOX_IRQ_NOTFULL(info->tx_id);
		priv->newmsg_bit = MAILBOX_IRQ_NEWMSG(info->rx_id);
		if (intr_type) {
			priv->irqenable = OMAP4_MAILBOX_IRQENABLE(info->usr_id);
			priv->irqstatus = OMAP4_MAILBOX_IRQSTATUS(info->usr_id);
			priv->irqdisable =
				OMAP4_MAILBOX_IRQENABLE_CLR(info->usr_id);
		} else {
			priv->irqenable = MAILBOX_IRQENABLE(info->usr_id);
			priv->irqstatus = MAILBOX_IRQSTATUS(info->usr_id);
			priv->irqdisable = MAILBOX_IRQENABLE(info->usr_id);
		}
		priv->intr_type = intr_type;

		mbox->priv = priv;
		mbox->parent = mdev;
		mbox->name = info->name;
		if (!strcmp(mbox->name, "wkup_m3"))
			mbox->ops = &wkupm3_mbox_ops;
		else
			mbox->ops = &omap2_mbox_ops;
		mbox->irq = platform_get_irq(pdev, info->irq_id);
		if (mbox->irq < 0) {
			ret = mbox->irq;
			goto free_privblk;
		}
		list[i] = mbox++;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		ret = -ENOENT;
		goto free_privblk;
	}

	mdev->mbox_base = ioremap(mem->start, resource_size(mem));
	if (!mdev->mbox_base) {
		ret = -ENOMEM;
		goto free_privblk;
	}

	mutex_init(&mdev->cfg_lock);
	mdev->dev = &pdev->dev;
	mdev->num_users = num_users;
	mdev->num_fifos = num_fifos;
	mdev->mboxes = list;
	ret = omap_mbox_register(&pdev->dev, list);
	if (ret)
		goto unmap_mbox;
	platform_set_drvdata(pdev, mdev);

	pm_runtime_enable(mdev->dev);

	kfree(of_info);
	return 0;

unmap_mbox:
	iounmap(mdev->mbox_base);
free_privblk:
	kfree(privblk);
free_mboxblk:
	kfree(mboxblk);
free_list:
	kfree(list);
free_mdev:
	kfree(mdev);
free_of:
	kfree(of_info);
	return ret;
}

static int omap2_mbox_remove(struct platform_device *pdev)
{
	struct omap_mbox2_priv *privblk;
	struct omap_mbox_device *mdev = platform_get_drvdata(pdev);
	struct omap_mbox **list = mdev->mboxes;
	struct omap_mbox *mboxblk = list[0];

	pm_runtime_disable(mdev->dev);

	privblk = mboxblk->priv;
	omap_mbox_unregister();
	iounmap(mdev->mbox_base);
	kfree(privblk);
	kfree(mboxblk);
	kfree(list);
	kfree(mdev);

	return 0;
}

static struct platform_driver omap2_mbox_driver = {
	.probe	= omap2_mbox_probe,
	.remove	= omap2_mbox_remove,
	.driver	= {
		.name = "omap-mailbox",
		.of_match_table = omap_mailbox_of_match,
	},
};

static int __init omap2_mbox_init(void)
{
	return platform_driver_register(&omap2_mbox_driver);
}

static void __exit omap2_mbox_exit(void)
{
	platform_driver_unregister(&omap2_mbox_driver);
}

module_init(omap2_mbox_init);
module_exit(omap2_mbox_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("omap mailbox: omap2/3/4 architecture specific functions");
MODULE_AUTHOR("Hiroshi DOYU <Hiroshi.DOYU@nokia.com>");
MODULE_AUTHOR("Paul Mundt");
MODULE_ALIAS("platform:omap2-mailbox");
