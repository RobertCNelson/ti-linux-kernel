/*
 * omap-mbox.h: OMAP mailbox internal definitions
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef OMAP_MBOX_H
#define OMAP_MBOX_H

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/omap-mailbox.h>

/**
 * struct omap_mbox_ops - function ops specific to a mailbox implementation
 * @startup: the startup function, essential for making the mailbox active.
 *	     This will be called when a client acquires the mailbox. The driver
 *	     implementation needs to take care of any refcounting if the same
 *	     mailbox is requested by multiple clients.
 * @shutdown: the shutdown function, essential for making the mailbox inactive
 *	      after usage. This will be called when a client releases the
 *	      mailbox. The driver implementation needs to take care of any
 *	      refcounting if the same mailbox is requested by multiple clients.
 * @fifo_read: read and return the h/w transport payload message. This hook
 *	       provides the omap mailbox core to read all the available messages
 *	       upon a Rx interrupt and buffer them. The messages are delivered
 *	       to the clients in a workqueue.
 * @fifo_write: send a mailbox message packet on the h/w transport channel. The
 *		individual drivers are responsible for configuring the h/w
 *		accordingly.
 * @fifo_empty: check if the h/w Rx transport has more messages. The function
 *		should return 0 if there are no more messages to be read from
 *		the transport, and non-zero if there are available messages.
 * @poll_for_space: check if the h/w Tx transport is busy. This hook should
 *		    return non-zero if the h/w Tx transport is busy, and 0 when
 *		    the h/w communication channel is free.
 * @enable_irq: This hook allows the mailbox core to allow a specific Rx or Tx
 *		interrupt signal to interrupt the processor, based on its state
 *		machine.
 * @disable_irq: This hooks allows the mailbox core to disable a specific Rx or
 *		 Tx interrupt signal from interrupting the processor, based on
 *		 its state machine.
 * @ack_irq: acknowledge the Tx or Rx interrupt signal internal to the mailbox.
 *	     This allows the h/w communication block to clear any internal
 *	     interrupt source status registers.
 * @is_irq: check if a particular Tx or Rx interrupt signal on the corresponding
 *	    mailbox is set. This hook is used by the mailbox core to process the
 *	    interrupt accordingly.
 * @save_ctx: Called by a client or the mailbox core to allow the individual
 *	      driver implementation to save the context of the mailbox registers
 *	      before the domain containing the h/w communication block can be
 *	      put into a low-power state.
 * @restore_ctx: Called by a client or the mailbox core to allow the individual
 *	      driver implementation to restore the context of the mailbox
 *	      registers after the domain containing the h/w communication block
 *	      is powered back to active state.
 */
struct omap_mbox_ops {
	int		(*startup)(struct omap_mbox *mbox);
	void		(*shutdown)(struct omap_mbox *mbox);
	/* mailbox access */
	mbox_msg_t	(*fifo_read)(struct omap_mbox *mbox);
	void		(*fifo_write)(struct omap_mbox *mbox, mbox_msg_t msg);
	int		(*fifo_empty)(struct omap_mbox *mbox);
	int		(*poll_for_space)(struct omap_mbox *mbox);
	/* irq */
	void		(*enable_irq)(struct omap_mbox *mbox,
						omap_mbox_irq_t irq);
	void		(*disable_irq)(struct omap_mbox *mbox,
						omap_mbox_irq_t irq);
	void		(*ack_irq)(struct omap_mbox *mbox, omap_mbox_irq_t irq);
	int		(*is_irq)(struct omap_mbox *mbox, omap_mbox_irq_t irq);
	/* context */
	void		(*save_ctx)(struct omap_mbox *mbox);
	void		(*restore_ctx)(struct omap_mbox *mbox);
};

/**
 * struct omap_mbox_queue - A queue object used for buffering messages
 * @lock: a spinlock providing synchronization in atomic context
 * @fifo: a kfifo object for buffering the messages. The size of the kfifo is
 *	  is currently configured either at build time using kernel menu
 *	  configuration or at runtime through a module parameter. The usage of
 *	  the kfifo depends on whether the queue object is for Rx or Tx. For Tx,
 *	  a message is buffered into the kfifo if the h/w transport is busy, and
 *	  is taken out when the h/w signals Tx readiness. For Rx, the messages
 *	  are buffered into the kfifo in the bottom-half processing of a Rx
 *	  interrupt, and taken out during the top-half processing.
 * @work: a workqueue object for scheduling top-half processing of rx messages
 * @tasklet: a tasklet object for processing tx messages in an atomic context
 * @mbox: reference to the containing parent mailbox
 * full: indicates the status of the fifo, and is set to true when there is no
 *	 room in the fifo.
 */
struct omap_mbox_queue {
	spinlock_t		lock;
	struct kfifo		fifo;
	struct work_struct	work;
	struct tasklet_struct	tasklet;
	struct omap_mbox	*mbox;
	bool full;
};

/**
 * struct omap_mbox_device - device structure for storing h/w mailbox block
 * @dev:	reference device pointer of the h/w mailbox block
 * @cfg_lock:	a configuration mutex lock used for protecting the mailbox
 *		device configuration operations
 * @mbox_base:	ioremapped base address of the h/w mailbox block
 * @num_users:	number of output interrupts from the h/w mailbox block, multiple
 *		interrupts can be routed to a particular processor sub-system
 * @num_fifos:	number of individual h/w fifo queues supported within a h/w
 *		mailbox block
 * @mboxes:	array of containing mailboxes within the h/w mailbox block
 * @elem:	list node
 */
struct omap_mbox_device {
	struct device *dev;
	struct mutex cfg_lock;
	void __iomem *mbox_base;
	u32 num_users;
	u32 num_fifos;
	struct omap_mbox **mboxes;
	struct list_head elem;
};

/**
 * struct omap_mbox - the base object describing a h/w communication channel.
 *	  there can be more than one object in a h/w communication block
 * @name: a unique name for the mailbox object. Client users acquire a
 *	  mailbox object using this name
 * @irq: IRQ number that the mailbox uses to interrupt the host processor.
 *	 the same IRQ number may be shared between different mailboxes
 * @txq: the mailbox queue object pertaining to Tx
 * @rxq: the mailbox queue object pertaining to Rx
 * @ops: function ops specific to the mailbox
 * @dev: the device pointer representing the mailbox object
 * @parent: back reference to the containing parent mailbox device object
 * @priv: a private structure specific to the driver implementation, this will
 *	  not be touched by the mailbox core
 * @use_count: number of current references to the mailbox, useful in
 *	       controlling the mailbox state
 * @notifier: notifier chain of clients, to which a received message is
 *	      communicated
 */
struct omap_mbox {
	const char		*name;
	int			irq;
	struct omap_mbox_queue	*txq, *rxq;
	struct omap_mbox_ops	*ops;
	struct device		*dev;
	struct omap_mbox_device *parent;
	void			*priv;
	int			use_count;
	struct blocking_notifier_head	notifier;
};

/*
 * mailbox objects registration and de-registration functions with the
 * mailbox core.
 */
int omap_mbox_register(struct omap_mbox_device *device);
int omap_mbox_unregister(struct omap_mbox_device *device);

#endif /* OMAP_MBOX_H */
