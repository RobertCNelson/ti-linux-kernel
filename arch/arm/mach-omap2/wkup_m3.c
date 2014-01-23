/*
* AM33XX Power Management Routines
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
 * Vaibhav Bedia <vaibhav.bedia@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/omap-mailbox.h>
#include <linux/reset.h>
#include "wkup_m3.h"

#define WKUP_M3_WAKE_SRC_MASK			0xFF

#define WKUP_M3_STATUS_RESP_SHIFT		16
#define WKUP_M3_STATUS_RESP_MASK		(0xffff << 16)

#define WKUP_M3_FW_VERSION_SHIFT		0
#define WKUP_M3_FW_VERSION_MASK			0xffff

/* AM33XX M3_TXEV_EOI register */
#define AM33XX_CONTROL_M3_TXEV_EOI	0x00

#define AM33XX_M3_TXEV_ACK		(0x1 << 0)
#define AM33XX_M3_TXEV_ENABLE		(0x0 << 0)

/* AM33XX IPC message registers */
#define AM33XX_CONTROL_IPC_MSG_REG0	0x04
#define AM33XX_CONTROL_IPC_MSG_REG1	0x08
#define AM33XX_CONTROL_IPC_MSG_REG2	0x0c
#define AM33XX_CONTROL_IPC_MSG_REG3	0x10
#define AM33XX_CONTROL_IPC_MSG_REG4	0x14
#define AM33XX_CONTROL_IPC_MSG_REG5	0x18
#define AM33XX_CONTROL_IPC_MSG_REG6	0x1c
#define AM33XX_CONTROL_IPC_MSG_REG7	0x20

struct wkup_m3_context {
	struct device	*dev;
	void __iomem	*code;
	void __iomem	*data;
	void __iomem	*data_end;
	size_t		data_size;
	void __iomem	*ipc;
	u8		is_valid;
	struct wkup_m3_ops *ops;
	struct omap_mbox *mbox;
};

struct wkup_m3_wakeup_src wakeups[] = {
	{.irq_nr = 35,	.src = "USB0_PHY"},
	{.irq_nr = 36,	.src = "USB1_PHY"},
	{.irq_nr = 40,	.src = "I2C0"},
	{.irq_nr = 41,	.src = "RTC Timer"},
	{.irq_nr = 42,	.src = "RTC Alarm"},
	{.irq_nr = 43,	.src = "Timer0"},
	{.irq_nr = 44,	.src = "Timer1"},
	{.irq_nr = 45,	.src = "UART"},
	{.irq_nr = 46,	.src = "GPIO0"},
	{.irq_nr = 48,	.src = "MPU_WAKE"},
	{.irq_nr = 49,	.src = "WDT0"},
	{.irq_nr = 50,	.src = "WDT1"},
	{.irq_nr = 51,	.src = "ADC_TSC"},
	{.irq_nr = 0,	.src = "Unknown"},
};

static struct wkup_m3_context *wkup_m3;

static void am33xx_txev_eoi(void)
{
	writel(AM33XX_M3_TXEV_ACK,
		wkup_m3->ipc + AM33XX_CONTROL_M3_TXEV_EOI);
}

static void am33xx_txev_enable(void)
{
	writel(AM33XX_M3_TXEV_ENABLE,
		wkup_m3->ipc + AM33XX_CONTROL_M3_TXEV_EOI);
}

static void am33xx_ctrl_ipc_write(struct am33xx_ipc_regs *ipc_regs)
{
	writel(ipc_regs->reg0,
		wkup_m3->ipc + AM33XX_CONTROL_IPC_MSG_REG0);
	writel(ipc_regs->reg1,
		wkup_m3->ipc + AM33XX_CONTROL_IPC_MSG_REG1);
	writel(ipc_regs->reg2,
		wkup_m3->ipc + AM33XX_CONTROL_IPC_MSG_REG2);
	writel(ipc_regs->reg3,
		wkup_m3->ipc + AM33XX_CONTROL_IPC_MSG_REG3);
	writel(ipc_regs->reg4,
		wkup_m3->ipc + AM33XX_CONTROL_IPC_MSG_REG4);
	writel(ipc_regs->reg5,
		wkup_m3->ipc + AM33XX_CONTROL_IPC_MSG_REG5);
	writel(ipc_regs->reg6,
		wkup_m3->ipc + AM33XX_CONTROL_IPC_MSG_REG6);
	writel(ipc_regs->reg7,
		wkup_m3->ipc + AM33XX_CONTROL_IPC_MSG_REG7);
}

static void am33xx_ctrl_ipc_read(struct am33xx_ipc_regs *ipc_regs)
{
	ipc_regs->reg0 = readl(wkup_m3->ipc
					+ AM33XX_CONTROL_IPC_MSG_REG0);
	ipc_regs->reg1 = readl(wkup_m3->ipc
					+ AM33XX_CONTROL_IPC_MSG_REG1);
	ipc_regs->reg2 = readl(wkup_m3->ipc
					+ AM33XX_CONTROL_IPC_MSG_REG2);
	ipc_regs->reg3 = readl(wkup_m3->ipc
					+ AM33XX_CONTROL_IPC_MSG_REG3);
	ipc_regs->reg4 = readl(wkup_m3->ipc
					+ AM33XX_CONTROL_IPC_MSG_REG4);
	ipc_regs->reg5 = readl(wkup_m3->ipc
					+ AM33XX_CONTROL_IPC_MSG_REG5);
	ipc_regs->reg6 = readl(wkup_m3->ipc
					+ AM33XX_CONTROL_IPC_MSG_REG6);
	ipc_regs->reg7 = readl(wkup_m3->ipc
					+ AM33XX_CONTROL_IPC_MSG_REG7);
}

int wkup_m3_is_valid()
{
	return wkup_m3->is_valid;
}

int wkup_m3_ping(void)
{
	int ret = 0;

	if (!wkup_m3->mbox) {
		pr_err("PM: No IPC channel to communicate with wkup_m3!\n");
		return -EIO;
	}

	/*
	 * Write a dummy message to the mailbox in order to trigger the RX
	 * interrupt to alert the M3 that data is available in the IPC
	 * registers.
	 */
	ret = omap_mbox_msg_send(wkup_m3->mbox, 0xABCDABCD);

	return ret;
}

/*
 * This pair of functions allows data to be stuffed into the end of the
 * CM3 data memory. This is currently used for passing the I2C sleep/wake
 * sequences to the firmware.
 */

/* Clear out the pointer for data stored at the end of DMEM */
void wkup_m3_reset_data_pos(void)
{
	wkup_m3->data_end = wkup_m3->data + wkup_m3->data_size;
}

/*
 * Store a block of data at the end of DMEM, return the offset within DMEM
 * that the data is stored at, or -ENOMEM if the data did not fit
 */
int wkup_m3_copy_data(const u8 *data, size_t size)
{
	if (wkup_m3->data + size > wkup_m3->data_end)
		return -ENOMEM;
	wkup_m3->data_end -= size;
	memcpy_toio(wkup_m3->data_end, data, size);
	return wkup_m3->data_end - wkup_m3->data;
}

int wkup_m3_ping_noirq(void)
{
	int ret = 0;

	if (!wkup_m3->mbox) {
		pr_err("PM: No IPC channel to communicate with wkup_m3!\n");
		return -EIO;
	}

	/*
	 * Write a dummy message to the mailbox in order to trigger the RX
	 * interrupt to alert the M3 that data is available in the IPC
	 * registers.
	 */
	ret = omap_mbox_msg_send_noirq(wkup_m3->mbox, 0xABCDABCD);

	return ret;
}

struct wkup_m3_wakeup_src wkup_m3_wake_src(void)
{
	struct am33xx_ipc_regs ipc_regs;
	unsigned int wakeup_src_idx;
	int j;

	am33xx_ctrl_ipc_read(&ipc_regs);

	wakeup_src_idx = ipc_regs.reg6 & WKUP_M3_WAKE_SRC_MASK;

	for (j = 0; j < ARRAY_SIZE(wakeups)-1; j++) {
		if (wakeups[j].irq_nr == wakeup_src_idx)
			return wakeups[j];
	}

	return wakeups[j];
}


int wkup_m3_pm_status(void)
{
	unsigned int i;
	struct am33xx_ipc_regs ipc_regs;

	am33xx_ctrl_ipc_read(&ipc_regs);

	i = WKUP_M3_STATUS_RESP_MASK & ipc_regs.reg1;
	i >>= __ffs(WKUP_M3_STATUS_RESP_MASK);

	return i;
}

/*
 * Invalidate M3 firmware version before hardreset.
 * Write invalid version in lower 4 nibbles of parameter
 * register (ipc_regs + 0x8).
 */

static void wkup_m3_fw_version_clear(void)
{
	struct am33xx_ipc_regs ipc_regs;

	am33xx_ctrl_ipc_read(&ipc_regs);
	ipc_regs.reg2 = 0xFFFF0000;
	am33xx_ctrl_ipc_write(&ipc_regs);

	return;
}

int wkup_m3_fw_version_read(void)
{
	struct am33xx_ipc_regs ipc_regs;

	am33xx_ctrl_ipc_read(&ipc_regs);

	return ipc_regs.reg2 & WKUP_M3_FW_VERSION_MASK;
}

void wkup_m3_pm_set_cmd(struct am33xx_ipc_regs *ipc_regs)
{
	am33xx_ctrl_ipc_write(ipc_regs);
}

void wkup_m3_set_ops(struct wkup_m3_ops *ops)
{
	wkup_m3->ops = ops;
}

static irqreturn_t wkup_m3_txev_handler(int irq, void *unused)
{
	am33xx_txev_eoi();

	if (wkup_m3->ops && wkup_m3->ops->firmware_loaded)
		wkup_m3->ops->txev_handler();

	am33xx_txev_enable();

	return IRQ_HANDLED;
}

int wkup_m3_prepare(void)
{
	int ret = 0;
	struct reset_control *rst_ctrl;
	struct platform_device *pdev = to_platform_device(wkup_m3->dev);

	wkup_m3->mbox = omap_mbox_get("wkup_m3", NULL);

	if (IS_ERR(wkup_m3->mbox)) {
		ret = -EBUSY;
		pr_err("PM: IPC Request for A8->M3 Channel failed!\n");
		return ret;
	}

	wkup_m3_fw_version_clear();

	/* check that the code is loaded */
	rst_ctrl = reset_control_get(&pdev->dev, NULL);

	if (IS_ERR(rst_ctrl)) {
		dev_err(wkup_m3->dev, "Unable to get reset control\n");
		return -EINVAL;
	}

	ret = reset_control_deassert(rst_ctrl);
	reset_control_put(rst_ctrl);

	return ret;
}

static int wkup_m3_copy_code(const u8 *data, size_t size)
{
	if (size > SZ_16K)
		return -ENOMEM;

	memcpy_toio(wkup_m3->code, data, size);

	return 0;
}

static void wkup_m3_firmware_cb(const struct firmware *fw, void *context)
{
	int ret = 0;

	/* no firmware found */
	if (!fw) {
		pr_err("PM: request_firmware failed\n");
		return;
	}

	ret = wkup_m3_copy_code(fw->data, fw->size);

	if (ret) {
		pr_info("PM: Failed to copy firmware for M3");
	} else {
		if (wkup_m3->ops && wkup_m3->ops->firmware_loaded)
			wkup_m3->ops->firmware_loaded();

		wkup_m3->is_valid = true;
	}

	return;
}

static int wkup_m3_probe(struct platform_device *pdev)
{
	int irq, ret = 0;
	struct resource *res;

	pm_runtime_enable(&pdev->dev);

	ret = pm_runtime_get_sync(&pdev->dev);
	if (IS_ERR_VALUE(ret)) {
		dev_err(&pdev->dev, "pm_runtime_get_sync() failed\n");
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (!irq) {
		dev_err(&pdev->dev, "no irq resource\n");
		ret = -ENXIO;
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "m3_umem");
	if (!res) {
		dev_err(&pdev->dev, "no memory resource\n");
		ret = -ENXIO;
		goto err;
	}

	wkup_m3 = devm_kzalloc(&pdev->dev, sizeof(*wkup_m3), GFP_KERNEL);
	if (!wkup_m3) {
		pr_err("Memory allocation failed\n");
		ret = -ENOMEM;
		goto err;
	}

	wkup_m3->dev = &pdev->dev;

	wkup_m3->code = devm_request_and_ioremap(wkup_m3->dev, res);
	if (!wkup_m3->code) {
		dev_err(wkup_m3->dev, "could not ioremap\n");
		ret = -EADDRNOTAVAIL;
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ipc_regs");
	if (!res) {
		dev_err(&pdev->dev, "no memory resource for ipc\n");
		ret = -ENXIO;
		goto err;
	}

	wkup_m3->ipc = devm_request_and_ioremap(wkup_m3->dev, res);
	if (!wkup_m3->ipc) {
		dev_err(wkup_m3->dev, "could not ioremap ipc_mem\n");
		ret = -EADDRNOTAVAIL;
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "m3_dmem");
	if (!res) {
		dev_err(&pdev->dev, "no memory resource for dmem\n");
		ret = -ENXIO;
		goto err;
	}

	wkup_m3->data = devm_request_and_ioremap(wkup_m3->dev, res);
	if (!wkup_m3->data) {
		dev_err(wkup_m3->dev, "could not ioremap dmem\n");
		ret = -EADDRNOTAVAIL;
		goto err;
	}

	wkup_m3->data_size = resource_size(res);
	wkup_m3_reset_data_pos();

	ret = devm_request_irq(wkup_m3->dev, irq, wkup_m3_txev_handler,
		  IRQF_DISABLED, "wkup_m3_txev", NULL);
	if (ret) {
		dev_err(wkup_m3->dev, "request_irq failed\n");
		goto err;
	}

	wkup_m3->is_valid = false;

	pr_info("PM: Loading am335x-pm-firmware.bin");

	/* We don't want to delay boot */
	ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
			"am335x-pm-firmware.bin", &pdev->dev, GFP_KERNEL, NULL,
			wkup_m3_firmware_cb);

err:
	return ret;
}

static int wkup_m3_remove(struct platform_device *pdev)
{
	return 0;
}

static struct of_device_id wkup_m3_dt_ids[] = {
	{ .compatible = "ti,am3353-wkup-m3" },
	{ }
};
MODULE_DEVICE_TABLE(of, wkup_m3_dt_ids);

static int wkup_m3_rpm_suspend(struct device *dev)
{
	return -EBUSY;
}

static int wkup_m3_rpm_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops wkup_m3_ops = {
	SET_RUNTIME_PM_OPS(wkup_m3_rpm_suspend, wkup_m3_rpm_resume, NULL)
};

static struct platform_driver wkup_m3_driver = {
	.probe		= wkup_m3_probe,
	.remove		= wkup_m3_remove,
	.driver		= {
		.name	= "wkup_m3",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(wkup_m3_dt_ids),
		.pm	= &wkup_m3_ops,
	},
};

module_platform_driver(wkup_m3_driver);
