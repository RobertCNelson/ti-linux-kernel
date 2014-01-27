/*
 * OMAP mailbox driver
 *
 * Copyright (C) 2006-2009 Nokia Corporation. All rights reserved.
 *
 * Contact: Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/err.h>
#include <linux/notifier.h>
#include <linux/module.h>

#include "omap-mbox.h"

/* global variables for the mailbox devices */
static DEFINE_MUTEX(omap_mbox_devices_lock);
static LIST_HEAD(omap_mbox_devices);

/* default size for the fifos, configured through kernel menuconfig */
static unsigned int mbox_kfifo_size = CONFIG_OMAP_MBOX_KFIFO_SIZE;
module_param(mbox_kfifo_size, uint, S_IRUGO);
MODULE_PARM_DESC(mbox_kfifo_size, "Size of omap's mailbox kfifo (bytes)");

/* mailbox h/w transport communication handler helper functions */
static inline mbox_msg_t mbox_fifo_read(struct omap_mbox *mbox)
{
	return mbox->ops->fifo_read(mbox);
}
static inline void mbox_fifo_write(struct omap_mbox *mbox, mbox_msg_t msg)
{
	mbox->ops->fifo_write(mbox, msg);
}
static inline int mbox_fifo_empty(struct omap_mbox *mbox)
{
	return mbox->ops->fifo_empty(mbox);
}
/*
 * local helper to check if the h/w transport is busy or free.
 * Returns 0 if free, and non-zero otherwise
 */
static inline int mbox_poll_for_space(struct omap_mbox *mbox)
{
	return mbox->ops->poll_for_space(mbox);
}

/* mailbox h/w irq handler helper functions */
static inline void ack_mbox_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	if (mbox->ops->ack_irq)
		mbox->ops->ack_irq(mbox, irq);
}
static inline int is_mbox_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	return mbox->ops->is_irq(mbox, irq);
}

/**
 * omap_mbox_msg_send() - send a mailbox message
 * @mbox: handle to the acquired mailbox on which to send the message
 * @msg: the mailbox message to be sent
 *
 * This API is called by a client user to send a mailbox message on an
 * acquired mailbox. The API transmits the message immediately on the h/w
 * communication transport if it is available, otherwise buffers the
 * message for transmission as soon as the h/w transport is ready.
 *
 * The only failure from this function is when neither the h/w transport
 * is available nor the s/w buffer fifo is empty.
 *
 * Returns 0 on success, or an error otherwise
 */
int omap_mbox_msg_send_noirq(struct omap_mbox *mbox, mbox_msg_t msg)
{
	struct omap_mbox_queue *mq = mbox->txq;
	int len;

	if (kfifo_avail(&mq->fifo) < sizeof(msg)) {
		return -ENOMEM;
	}

	if (kfifo_is_empty(&mq->fifo) && !mbox_poll_for_space(mbox)) {
		mbox_fifo_write(mbox, msg);
		return 0;
	}

	len = kfifo_in(&mq->fifo, (unsigned char *)&msg, sizeof(msg));
	WARN_ON(len != sizeof(msg));

	tasklet_schedule(&mbox->txq->tasklet);

	return 0;
}
EXPORT_SYMBOL(omap_mbox_msg_send_noirq);

int omap_mbox_msg_send(struct omap_mbox *mbox, mbox_msg_t msg)
{
	struct omap_mbox_queue *mq = mbox->txq;
	int ret;

	spin_lock_bh(&mq->lock);
	ret = omap_mbox_msg_send_noirq(mbox, msg);
	spin_unlock_bh(&mq->lock);

	return ret;
}
EXPORT_SYMBOL(omap_mbox_msg_send);

/**
 * omap_mbox_save_ctx: save the context of a mailbox
 * @mbox: handle to the acquired mailbox
 *
 * This allows a client (controlling a remote) to request a mailbox to
 * save its context when it is powering down the remote.
 *
 * NOTE: This will be eventually deprecated, new clients should not use this.
 *	 The same feature can be enabled through runtime_pm enablement of
 *	 mailbox.
 */
void omap_mbox_save_ctx(struct omap_mbox *mbox)
{
	if (!mbox->ops->save_ctx) {
		dev_err(mbox->dev, "%s:\tno save\n", __func__);
		return;
	}

	mbox->ops->save_ctx(mbox);
}
EXPORT_SYMBOL(omap_mbox_save_ctx);

/**
 * omap_mbox_restore_ctx: restore the context of a mailbox
 * @mbox: handle to the acquired mailbox
 *
 * This allows a client (controlling a remote) to request a mailbox to
 * restore its context after restoring the remote, so that it can
 * communicate with the remote as it would normally.
 *
 * NOTE: This will be deprecated, new clients should not use this.
 *	 The same feature can be enabled through runtime_pm enablement
 *	 of mailbox.
 */
void omap_mbox_restore_ctx(struct omap_mbox *mbox)
{
	if (!mbox->ops->restore_ctx) {
		dev_err(mbox->dev, "%s:\tno restore\n", __func__);
		return;
	}

	mbox->ops->restore_ctx(mbox);
}
EXPORT_SYMBOL(omap_mbox_restore_ctx);

/**
 * omap_mbox_enable_irq: enable a specific mailbox Rx or Tx interrupt source
 * @mbox: handle to the acquired mailbox
 * @irq: interrupt type associated with either the Rx or Tx
 *
 * This allows a client (having its own shared memory communication protocol
 * with the remote) to request a mailbox to enable a particular interrupt
 * signal source of the mailbox, as part of its communication state machine.
 *
 * NOTE: This will be deprecated, new clients should not use this. It is
 *	 being exported for TI DSP/Bridge driver.
 */
void omap_mbox_enable_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	mbox->ops->enable_irq(mbox, irq);
}
EXPORT_SYMBOL(omap_mbox_enable_irq);

/**
 * omap_mbox_disable_irq: disable a specific mailbox Rx or Tx interrupt source
 * @mbox: handle to the acquired mailbox
 * @irq: interrupt type associated with either the Rx or Tx
 *
 * This allows a client (having its own shared memory communication protocal
 * with the remote) to request a mailbox to disable a particular interrupt
 * signal source of the mailbox, as part of its communication state machine.
 *
 * NOTE: This will be deprecated, new clients should not use this. It is
 *	 being exported for TI DSP/Bridge driver.
 */
void omap_mbox_disable_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	mbox->ops->disable_irq(mbox, irq);
}
EXPORT_SYMBOL(omap_mbox_disable_irq);

/*
 * This is the tasklet function in which all the buffered messages are
 * sent until the h/w transport is busy again. The tasklet is scheduled
 * upon receiving an interrupt indicating the availability of the h/w
 * transport.
 */
static void mbox_tx_tasklet(unsigned long tx_data)
{
	struct omap_mbox *mbox = (struct omap_mbox *)tx_data;
	struct omap_mbox_queue *mq = mbox->txq;
	mbox_msg_t msg;
	int ret;

	while (kfifo_len(&mq->fifo)) {
		if (mbox_poll_for_space(mbox)) {
			omap_mbox_enable_irq(mbox, IRQ_TX);
			break;
		}

		ret = kfifo_out(&mq->fifo, (unsigned char *)&msg,
								sizeof(msg));
		WARN_ON(ret != sizeof(msg));

		mbox_fifo_write(mbox, msg);
	}
}

/*
 * This is the message receiver workqueue function, which is responsible
 * for delivering all the received messages stored in the receive kfifo
 * to the clients. Each message is delivered to all the registered mailbox
 * clients. It also re-enables the receive interrupt on the mailbox (disabled
 * when the s/w kfifo is full) after emptying atleast a message from the
 * fifo.
 */
static void mbox_rx_work(struct work_struct *work)
{
	struct omap_mbox_queue *mq =
			container_of(work, struct omap_mbox_queue, work);
	mbox_msg_t msg;
	int len;

	while (kfifo_len(&mq->fifo) >= sizeof(msg)) {
		len = kfifo_out(&mq->fifo, (unsigned char *)&msg, sizeof(msg));
		WARN_ON(len != sizeof(msg));

		blocking_notifier_call_chain(&mq->mbox->notifier, len,
								(void *)msg);
		spin_lock_irq(&mq->lock);
		if (mq->full) {
			mq->full = false;
			omap_mbox_enable_irq(mq->mbox, IRQ_RX);
		}
		spin_unlock_irq(&mq->lock);
	}
}

/*
 * Interrupt handler for Tx interrupt source for each of the mailboxes.
 * This schedules the tasklet to transmit the messages buffered in the
 * Tx fifo.
 */
static void __mbox_tx_interrupt(struct omap_mbox *mbox)
{
	omap_mbox_disable_irq(mbox, IRQ_TX);
	ack_mbox_irq(mbox, IRQ_TX);
	tasklet_schedule(&mbox->txq->tasklet);
}

/*
 * Interrupt handler for Rx interrupt source for each of the mailboxes.
 * This performs the read from the h/w mailbox until the transport is
 * free of any incoming messages, and buffers the read message. The
 * buffers are delivered to clients by scheduling a work-queue.
 */
static void __mbox_rx_interrupt(struct omap_mbox *mbox)
{
	struct omap_mbox_queue *mq = mbox->rxq;
	mbox_msg_t msg;
	int len;

	while (!mbox_fifo_empty(mbox)) {
		if (unlikely(kfifo_avail(&mq->fifo) < sizeof(msg))) {
			omap_mbox_disable_irq(mbox, IRQ_RX);
			mq->full = true;
			goto nomem;
		}

		msg = mbox_fifo_read(mbox);

		len = kfifo_in(&mq->fifo, (unsigned char *)&msg, sizeof(msg));
		WARN_ON(len != sizeof(msg));
	}

	/* no more messages in the fifo. clear IRQ source. */
	ack_mbox_irq(mbox, IRQ_RX);
nomem:
	schedule_work(&mbox->rxq->work);
}

/*
 * The core mailbox interrupt handler function. The interrupt core would
 * call this for each of the mailboxes the interrupt is configured.
 */
static irqreturn_t mbox_interrupt(int irq, void *p)
{
	struct omap_mbox *mbox = p;

	if (is_mbox_irq(mbox, IRQ_TX))
		__mbox_tx_interrupt(mbox);

	if (is_mbox_irq(mbox, IRQ_RX))
		__mbox_rx_interrupt(mbox);

	return IRQ_HANDLED;
}

/*
 * Helper function to allocate a mailbox queue object. This function
 * also creates either or both of the work-queue or tasklet to
 * deal with processing of messages on the kfifo associated with
 * the mailbox queue object.
 */
static struct omap_mbox_queue *mbox_queue_alloc(struct omap_mbox *mbox,
					void (*work) (struct work_struct *),
					void (*tasklet)(unsigned long))
{
	struct omap_mbox_queue *mq;

	mq = kzalloc(sizeof(struct omap_mbox_queue), GFP_KERNEL);
	if (!mq)
		return NULL;

	spin_lock_init(&mq->lock);

	if (kfifo_alloc(&mq->fifo, mbox_kfifo_size, GFP_KERNEL))
		goto error;

	if (work)
		INIT_WORK(&mq->work, work);

	if (tasklet)
		tasklet_init(&mq->tasklet, tasklet, (unsigned long)mbox);
	return mq;
error:
	kfree(mq);
	return NULL;
}

/*
 * Helper function to free a mailbox queue object.
 */
static void mbox_queue_free(struct omap_mbox_queue *q)
{
	kfifo_free(&q->fifo);
	kfree(q);
}

/*
 * Helper function to initialize a mailbox. This function creates
 * the mailbox queue objects associated with the mailbox h/w channel
 * and plugs-in the interrupt associated with the mailbox, when the
 * mailbox h/w channel is requested for the first time.
 */
static int omap_mbox_startup(struct omap_mbox *mbox)
{
	int ret = 0;
	struct omap_mbox_queue *mq;
	struct omap_mbox_device *mdev = mbox->parent;

	mutex_lock(&mdev->cfg_lock);
	if (mbox->ops->startup) {
		ret = mbox->ops->startup(mbox);
		if (ret)
			goto fail_startup;
	}

	if (!mbox->use_count++) {
		mq = mbox_queue_alloc(mbox, NULL, mbox_tx_tasklet);
		if (!mq) {
			ret = -ENOMEM;
			goto fail_alloc_txq;
		}
		mbox->txq = mq;

		mq = mbox_queue_alloc(mbox, mbox_rx_work, NULL);
		if (!mq) {
			ret = -ENOMEM;
			goto fail_alloc_rxq;
		}
		mbox->rxq = mq;
		mq->mbox = mbox;
		ret = request_irq(mbox->irq, mbox_interrupt, IRQF_SHARED,
							mbox->name, mbox);
		if (unlikely(ret)) {
			pr_err("failed to register mailbox interrupt:%d\n",
									ret);
			goto fail_request_irq;
		}

		omap_mbox_enable_irq(mbox, IRQ_RX);
	}
	mutex_unlock(&mdev->cfg_lock);
	return 0;

fail_request_irq:
	mbox_queue_free(mbox->rxq);
fail_alloc_rxq:
	mbox_queue_free(mbox->txq);
fail_alloc_txq:
	if (mbox->ops->shutdown)
		mbox->ops->shutdown(mbox);
	mbox->use_count--;
fail_startup:
	mutex_unlock(&mdev->cfg_lock);
	return ret;
}

/*
 * Helper function to de-initialize a mailbox
 */
static void omap_mbox_fini(struct omap_mbox *mbox)
{
	struct omap_mbox_device *mdev = mbox->parent;

	mutex_lock(&mdev->cfg_lock);

	if (!--mbox->use_count) {
		omap_mbox_disable_irq(mbox, IRQ_RX);
		free_irq(mbox->irq, mbox);
		tasklet_kill(&mbox->txq->tasklet);
		flush_work(&mbox->rxq->work);
		mbox_queue_free(mbox->txq);
		mbox_queue_free(mbox->rxq);
	}

	if (mbox->ops->shutdown)
		mbox->ops->shutdown(mbox);

	mutex_unlock(&mdev->cfg_lock);
}

/*
 * Helper function to find a mailbox. It is currently assumed that all the
 * mailbox names are unique among all the mailbox devices. This can be
 * easily extended if only a particular mailbox device is to searched.
 */
static struct omap_mbox *omap_mbox_device_find(struct omap_mbox_device *mdev,
						const char *mbox_name)
{
	struct omap_mbox *_mbox, *mbox = NULL;
	struct omap_mbox **mboxes = mdev->mboxes;
	int i;

	if (!mboxes)
		return NULL;

	for (i = 0; (_mbox = mboxes[i]); i++) {
		if (!strcmp(_mbox->name, mbox_name)) {
			mbox = _mbox;
			break;
		}
	}
	return mbox;
}

/**
 * omap_mbox_get() - acquire a mailbox
 * @name: name of the mailbox to acquire
 * @nb: notifier block to be invoked on received messages
 *
 * This API is called by a client user to use a mailbox. The returned handle
 * needs to be used by the client for invoking any other mailbox API. Any
 * message received on the mailbox is delivered to the client through the
 * 'nb' notifier. There are currently no restrictions on multiple clients
 * acquiring the same mailbox - the same message is delivered to each of the
 * clients through their respective notifiers.
 *
 * The function ensures that the mailbox is put into an operational state
 * before the function returns.
 *
 * Returns a usable mailbox handle on success, or NULL otherwise
 */
struct omap_mbox *omap_mbox_get(const char *name, struct notifier_block *nb)
{
	struct omap_mbox *mbox = NULL;
	struct omap_mbox_device *mdev;
	int ret;

	mutex_lock(&omap_mbox_devices_lock);
	list_for_each_entry(mdev, &omap_mbox_devices, elem) {
		mbox = omap_mbox_device_find(mdev, name);
		if (mbox)
			break;
	}
	mutex_unlock(&omap_mbox_devices_lock);

	if (!mbox)
		return ERR_PTR(-ENOENT);

	if (nb)
		blocking_notifier_chain_register(&mbox->notifier, nb);

	ret = omap_mbox_startup(mbox);
	if (ret) {
		blocking_notifier_chain_unregister(&mbox->notifier, nb);
		return ERR_PTR(-ENODEV);
	}

	return mbox;
}
EXPORT_SYMBOL(omap_mbox_get);

/**
 * omap_mbox_put() - release a mailbox
 * @mbox: handle to the acquired mailbox
 * @nb: notifier block used while acquiring the mailbox
 *
 * This API is to be called by a client user once it is done using the
 * mailbox. The particular user's notifier function is removed from the
 * notifier list of received messages on this mailbox. It also undoes
 * any h/w configuration done during the acquisition of the mailbox.
 *
 * No return value
 */
void omap_mbox_put(struct omap_mbox *mbox, struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&mbox->notifier, nb);
	omap_mbox_fini(mbox);
}
EXPORT_SYMBOL(omap_mbox_put);

static struct class omap_mbox_class = { .name = "mbox", };

/**
 * omap_mbox_register() - register the list of mailboxes
 * @mdev: mailbox device handle containing the mailboxes that need to be
 *	  with the mailbox core
 *
 * This API is to be called by individual mailbox driver implementations
 * for registering the set of mailboxes contained in a h/w communication
 * block with the mailbox core. Each of the mailbox represents a h/w
 * communication channel, contained within the h/w communication block or ip.
 *
 * An associated device is also created for each of the mailboxes, and the
 * mailbox device is added to a global list of registered mailbox devices.
 *
 * Return 0 on success, or a failure code otherwise
 */
int omap_mbox_register(struct omap_mbox_device *mdev)
{
	int ret;
	int i;
	struct omap_mbox **mboxes;

	if (!mdev || !mdev->mboxes)
		return -EINVAL;

	mboxes = mdev->mboxes;
	for (i = 0; mboxes[i]; i++) {
		struct omap_mbox *mbox = mboxes[i];
		mbox->dev = device_create(&omap_mbox_class,
				mdev->dev, 0, mbox, "%s", mbox->name);
		if (IS_ERR(mbox->dev)) {
			ret = PTR_ERR(mbox->dev);
			goto err_out;
		}

		BLOCKING_INIT_NOTIFIER_HEAD(&mbox->notifier);
	}

	mutex_lock(&omap_mbox_devices_lock);
	list_add(&mdev->elem, &omap_mbox_devices);
	mutex_unlock(&omap_mbox_devices_lock);

	return 0;

err_out:
	while (i--)
		device_unregister(mboxes[i]->dev);
	return ret;
}
EXPORT_SYMBOL(omap_mbox_register);

/**
 * omap_mbox_unregister() - unregister the list of mailboxes
 * @mdev: parent mailbox device handle containing the mailboxes that need
 *	  to be unregistered
 *
 * This API is to be called by individual mailbox driver implementations
 * for unregistering the set of mailboxes contained in a h/w communication
 * block. Once unregistered, these mailboxes are not available for any
 * client users/drivers.
 *
 * Return 0 on success, or a failure code otherwise
 */
int omap_mbox_unregister(struct omap_mbox_device *mdev)
{
	int i;
	struct omap_mbox **mboxes;

	if (!mdev || !mdev->mboxes)
		return -EINVAL;

	mutex_lock(&omap_mbox_devices_lock);
	list_del(&mdev->elem);
	mutex_unlock(&omap_mbox_devices_lock);

	mboxes = mdev->mboxes;
	for (i = 0; mboxes[i]; i++)
		device_unregister(mboxes[i]->dev);
	return 0;
}
EXPORT_SYMBOL(omap_mbox_unregister);

static int __init omap_mbox_init(void)
{
	int err;

	err = class_register(&omap_mbox_class);
	if (err)
		return err;

	/* kfifo size sanity check: alignment and minimal size */
	mbox_kfifo_size = ALIGN(mbox_kfifo_size, sizeof(mbox_msg_t));
	mbox_kfifo_size = max_t(unsigned int, mbox_kfifo_size,
							sizeof(mbox_msg_t));

	return 0;
}
subsys_initcall(omap_mbox_init);

static void __exit omap_mbox_exit(void)
{
	class_unregister(&omap_mbox_class);
}
module_exit(omap_mbox_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("omap mailbox: interrupt driven messaging");
MODULE_AUTHOR("Toshihiro Kobayashi");
MODULE_AUTHOR("Hiroshi DOYU");
