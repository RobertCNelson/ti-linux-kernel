// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * TI OMAP mailbox test driver
 *
 * Copyright (C) 2013-2018 Texas Instruments Incorporated - http://www.ti.com
 *
 * Contact: Suman Anna <s-anna@ti.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/err.h>
#include <linux/sched.h>

#include <linux/mailbox_client.h>
#include <linux/omap-mailbox.h>
#include <linux/platform_device.h>

#define TEST_PTR_CLIENT

/* load-time options */
static int count = 16;
static int mbox_id;

module_param(count, int, 0644);
module_param(mbox_id, int, 0644);

static const char *name;
static int rx_count;

static void callback(struct mbox_client *client, void *data)
{
	mbox_msg_t msg = (mbox_msg_t)data;

	pr_info("rx: mbox msg: 0x%x\n", (u32)msg);
	rx_count++;
}

static struct mbox_chan *mbox;
#ifndef TEST_PTR_CLIENT
static struct mbox_client client;
#else
static struct mbox_client *pclient;
#endif

static void ti_mbox_framework_test_cleanup(void)
{
	const char *result = (count == rx_count ? "PASSED" : "FAILED");

	if (mbox)
		mbox_free_channel(mbox);

#ifdef TEST_PTR_CLIENT
	kfree(pclient);
#endif
	pr_info("%s: finished testing on %s, received %d messages, test %s\n",
		__func__, name, rx_count, result);
}

static int ti_mbox_framework_test_init(struct platform_device *pdev)
{
	int i, r, ret = 0;
	mbox_msg_t msg;
	char mbox_name[32];

	pr_info("%s: testing loopback on %s; sending %d messages\n",
		__func__, name, count);

#ifndef TEST_PTR_CLIENT
	client.dev = &pdev->dev;
	client.tx_done = NULL;
	client.rx_callback = callback;
	client.tx_block = false;
	client.knows_txdone = false;
	sprintf(mbox_name, "%s", name);

	mbox = mbox_request_channel(&client, mbox_id);
#else
	pclient = kzalloc(sizeof(*pclient), GFP_KERNEL);
	if (!pclient)
		return -ENOMEM;

	pclient->dev = &pdev->dev;
	pclient->rx_callback = callback;
	pclient->tx_block = false;
	pclient->knows_txdone = false;
	sprintf(mbox_name, "%s", name);

	mbox = mbox_request_channel(pclient, mbox_id);
#endif
	if (IS_ERR(mbox)) {
		ret = PTR_ERR(mbox);
		pr_err("%s: mbox_request_channel() failed on %s: %d\n",
		       __func__, mbox_name, ret);
		mbox = NULL;
		goto out;
	}

	for (i = 0; i < count; i++) {
		msg = i;
		r = mbox_send_message(mbox, (void *)msg);
		if (r < 0) {
			pr_err("%s: mbox_send_message() failed: %d\n",
			       __func__, r);
			/* Let callback empty fifo a bit, then continue */
			if (r != -EIO) {
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(HZ / 10);  /* 1/10 second */
				i--;
			}
			continue;
		} else {
			pr_err("%s: mbox_send_message() success, token: %d\n",
			       __func__, r);
		}
	}

	/* ti_mbox_framework_test_cleanup(); */
out:
	return ret;
}

static int ti_mbox_framework_test_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int ret = 0;

	if (!np) {
		pr_err("invalid node pointer\n");
		return -EINVAL;
	}

	ret = of_property_count_strings(np, "mbox-names");
	if (ret < 0) {
		pr_err("test node is either missing or has incorrect mbox-names property values\n");
		return ret;
	}

	if (mbox_id < 0 || mbox_id >= ret) {
		pr_err("invalid mbox_id value %d, should be >= 0 and < %d\n",
		       mbox_id, ret);
		return -EINVAL;
	}

	ret = of_property_read_string_index(np, "mbox-names", mbox_id, &name);
	if (ret < 0) {
		pr_err("unable to read invalid mbox-name for %d, ret = %d\n",
		       mbox_id, ret);
		return ret;
	}

	ret = ti_mbox_framework_test_init(pdev);
	if (ret)
		pr_err("ti_mbox_framework_test_init failed, ret = %d\n", ret);

	return ret;
}

static int ti_mbox_framework_test_remove(struct platform_device *pdev)
{
	ti_mbox_framework_test_cleanup();
	return 0;
}

static const struct of_device_id ti_mbox_framework_test_of_match[] = {
	{ .compatible = "ti,omap-mbox-test",  .data = NULL },
	{ /* end */ },
};

/* do not publish to userspace so avoid auto-load and probe by udev */
/* MODULE_DEVICE_TABLE(of, ti_mbox_framework_test_of_match); */

static struct platform_driver ti_mbox_framework_test_driver = {
	.probe	= ti_mbox_framework_test_probe,
	.remove	= ti_mbox_framework_test_remove,
	.driver	= {
		.name   = "ti_mbox_framework_test",
		.of_match_table = ti_mbox_framework_test_of_match,
	},
};

module_platform_driver(ti_mbox_framework_test_driver);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TI OMAP Mailbox Test driver");
MODULE_AUTHOR("Suman Anna <s-anna@ti.com>");
