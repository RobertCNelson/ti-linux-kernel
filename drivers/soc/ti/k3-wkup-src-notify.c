// SPDX-License-Identifier: GPL-2.0
/*
 * Wakeup source notify driver for TI K3 AM62L SoC.
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/arm-smccc.h>
#include <linux/irq.h>
#include <linux/irq_work.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define K3_SIP_GET_WKUP_REASON		0xC2000003

#define WKUP_LPM_RTC_DDR	0x6

#define WKUP_SRC_WKUP_RTC0		0x80
#define WKUP_SRC_MAIN_IO_DAISY_CHAIN	0x10000
#define WKUP_SRC_WKUP_IO_DAISY_CHAIN    0x20000

#define WKUP_PIN_UNKNOWN	0xFF

/*
 * struct k3_wkup_src - maps a wakeup source bitmask to a device virq.
 * @node:	list linkage in k3_wkup_src_notify_priv.srcs
 * @work:	irq work struct
 * @wkup_src_mask:	wakeup source bitmask
 * @virq:	Linux virq to fire when this wakeup source is detected
 * @wkup_pin:	pad pin that woke up the system; only valid for IO_DAISY_CHAIN wakeup
 */
struct k3_wkup_src {
	struct list_head node;
	struct irq_work work;
	u32 wkup_src_mask;
	int virq;
	u32 wkup_pin;
};

/*
 * struct k3_wkup_src_notify_priv - driver private data
 * @dev:	device pointer
 * @srcs:	list of k3_wkup_src
 */
struct k3_wkup_src_notify_priv {
	struct device *dev;
	struct list_head srcs;
};

/*
 * k3_wkup_src_notify_irq_work() - invoke the virq handler in hardirq context
 * @work:	irq_work struct
 */
static void k3_wkup_src_notify_irq_work(struct irq_work *work)
{
	struct k3_wkup_src *wkup_src =
		container_of(work, struct k3_wkup_src, work);

	generic_handle_irq_safe(wkup_src->virq);
}

/*
 * k3_wkup_src_notify_resume_noirq() - receives wakeup source reason then finds corresponding wakeup
 *				       source to fire irq
 * @dev:	device driver
 *
 * Sends SMC call to get the wakeup reason from firmware. Search through wakeup sources and if the
 * wakeup reason bitmask matches, fire the wakeup reason IRQ. It is valid if no wakeup source can
 * be found since some wakeup sources do not have IRQs. It is valid if multiple IRQs are fired,
 * such as in the case of firing for different GPIO banks.
 *
 * Return: 0 on success, error otherwise
 */
static int k3_wkup_src_notify_resume_noirq(struct device *dev)
{
	struct k3_wkup_src_notify_priv *priv = dev_get_drvdata(dev);
	struct arm_smccc_res res;
	struct k3_wkup_src *wkup_src;
	u32 lpm_mask;
	u32 wkup_src_mask;
	u32 wkup_pin;

	arm_smccc_smc(K3_SIP_GET_WKUP_REASON, 0, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return -EIO;

	lpm_mask = (u32)res.a1;
	wkup_src_mask = (u32)res.a2;
	wkup_pin = (u32)res.a3;

	/* if RTC + DDR doesn't return a wakeup source, wakeup source is RTC */
	if (lpm_mask & WKUP_LPM_RTC_DDR && !wkup_src_mask)
		wkup_src_mask = WKUP_SRC_WKUP_RTC0;

	dev_info(priv->dev, "wakeup source:0x%x, pin:0x%x, mode:0x%x\n", wkup_src_mask, wkup_pin,
		 lpm_mask);

	list_for_each_entry(wkup_src, &priv->srcs, node) {
		if (wkup_src_mask & wkup_src->wkup_src_mask && wkup_src->virq > 0 &&
		    wkup_src->wkup_pin == wkup_pin) {
			dev_dbg(priv->dev, "injecting virq %d for wakeup source 0x%x\n",
				wkup_src->virq, wkup_src->wkup_src_mask);
			irq_work_queue(&wkup_src->work);
		}
	}

	return 0;
}

static DEFINE_NOIRQ_DEV_PM_OPS(k3_wkup_src_notify_pm_ops, NULL, k3_wkup_src_notify_resume_noirq);

/*
 * k3_wkup_src_notify_parse_sources() - parse wakeup sources from device tree node
 * @priv:	driver private data
 * @np:	driver device tree node
 *
 * Parse ti,wkup-sources entries from driver device tree node. Each entry is a phandle with four
 * cells:
 * <&device wakeup_source_bitmask irq_index wkup_pin>
 * wakeup_source_bitmask is the value that matches the wakeup reason bitmask
 * irq_index selects the index of the interrupt in the target device's interrupts list
 * wkup_pin is the pin that woke up the system; only used for IO_DAISY_CHAIN wakeup
 *
 * Return: 0 on success, error otherwise
 */
static int k3_wkup_src_notify_parse_sources(struct k3_wkup_src_notify_priv *priv,
				     struct device_node *np)
{
	struct of_phandle_args args;
	int i = 0;

	while (!of_parse_phandle_with_fixed_args(np, "ti,wkup-sources", 3, i, &args)) {
		struct k3_wkup_src *wkup_src;
		int virq;

		virq = of_irq_get(args.np, args.args[1]);
		of_node_put(args.np);

		if (virq == -EPROBE_DEFER)
			return virq;
		if (virq <= 0)
			dev_dbg(priv->dev, "ti,wkup-sources[%d]: failed to get irq %d\n",
				i, virq);

		wkup_src = devm_kzalloc(priv->dev, sizeof(*wkup_src), GFP_KERNEL);
		if (!wkup_src)
			return -ENOMEM;

		wkup_src->wkup_src_mask = args.args[0];
		wkup_src->virq = virq;
		init_irq_work(&wkup_src->work, k3_wkup_src_notify_irq_work);

		/* wkup_pin only valid if wkup_src is IO_DAISY_CHAIN */
		if (wkup_src->wkup_src_mask & WKUP_SRC_MAIN_IO_DAISY_CHAIN
		    || wkup_src->wkup_src_mask & WKUP_SRC_WKUP_IO_DAISY_CHAIN)
			wkup_src->wkup_pin = args.args[2];
		else
			wkup_src->wkup_pin = WKUP_PIN_UNKNOWN;


		list_add_tail(&wkup_src->node, &priv->srcs);

		dev_dbg(priv->dev, "source 0x%x -> virq %d\n",
			wkup_src->wkup_src_mask, wkup_src->virq);
		i++;
	}

	return 0;
}

static int k3_wkup_src_notify_probe(struct platform_device *pdev)
{
	struct k3_wkup_src_notify_priv *priv;
	struct device *dev = &pdev->dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	INIT_LIST_HEAD(&priv->srcs);
	platform_set_drvdata(pdev, priv);

	ret = k3_wkup_src_notify_parse_sources(priv, dev_of_node(dev));
	if (ret)
		return ret;

	return 0;
}

static void k3_wkup_src_notify_remove(struct platform_device *pdev)
{
	struct k3_wkup_src_notify_priv *priv = platform_get_drvdata(pdev);
	struct k3_wkup_src *wkup_src;

	list_for_each_entry(wkup_src, &priv->srcs, node)
		irq_work_sync(&wkup_src->work);
}

static const struct of_device_id k3_wkup_src_notify_of_match[] = {
	{ .compatible = "ti,wkup-src-notify" },
	{ },
};
MODULE_DEVICE_TABLE(of, k3_wkup_src_notify_of_match);

static struct platform_driver k3_wkup_src_notify_driver = {
	.probe  = k3_wkup_src_notify_probe,
	.remove = k3_wkup_src_notify_remove,
	.driver = {
		.name           = "k3_wkup_src_notify",
		.of_match_table = k3_wkup_src_notify_of_match,
		.pm             = &k3_wkup_src_notify_pm_ops,
	},
};
module_platform_driver(k3_wkup_src_notify_driver);

MODULE_DESCRIPTION("TI K3 Wakeup Source Notify driver");
MODULE_AUTHOR("Kendall Willis");
MODULE_LICENSE("GPL");
