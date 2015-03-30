/*
* Copyright (c) 2014-2015 MediaTek Inc.
* Author: Tianping.Fang <tianping.fang@mediatek.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/irqdomain.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/mfd/mt6397/core.h>

#define RTC_BBPU		0x0000
#define RTC_WRTGR		0x003c
#define RTC_IRQ_EN		0x0004
#define RTC_IRQ_STA		0x0002

#define RTC_BBPU_CBUSY		(1 << 6)
#define RTC_BBPU_KEY		(0x43 << 8)
#define RTC_BBPU_AUTO		(1 << 3)
#define RTC_IRQ_STA_AL		(1 << 0)
#define RTC_IRQ_STA_LP		(1 << 3)

#define RTC_TC_SEC		0x000a
#define RTC_TC_MIN		0x000c
#define RTC_TC_HOU		0x000e
#define RTC_TC_DOM		0x0010
#define RTC_TC_MTH		0x0014
#define RTC_TC_YEA		0x0016
#define RTC_AL_SEC		0x0018
#define RTC_AL_MIN		0x001a

#define RTC_IRQ_EN_AL		(1 << 0)
#define RTC_IRQ_EN_ONESHOT	(1 << 2)
#define RTC_IRQ_EN_LP		(1 << 3)
#define RTC_IRQ_EN_ONESHOT_AL	(RTC_IRQ_EN_ONESHOT | RTC_IRQ_EN_AL)

#define RTC_TC_MIN_MASK		0x003f
#define RTC_TC_SEC_MASK		0x003f
#define RTC_TC_HOU_MASK		0x001f
#define RTC_TC_DOM_MASK		0x001f
#define RTC_TC_MTH_MASK		0x000f
#define RTC_TC_YEA_MASK		0x007f

#define RTC_AL_SEC_MASK		0x003f
#define RTC_AL_MIN_MASK		0x003f
#define RTC_AL_MASK_DOW		(1 << 4)

#define RTC_AL_HOU		0x001c
#define RTC_NEW_SPARE_FG_MASK	0xff00
#define RTC_NEW_SPARE_FG_SHIFT	8
#define RTC_AL_HOU_MASK		0x001f

#define RTC_AL_DOM		0x001e
#define RTC_NEW_SPARE1		0xff00
#define RTC_AL_DOM_MASK		0x001f
#define RTC_AL_MASK		0x0008

#define RTC_AL_MTH		0x0022
#define RTC_NEW_SPARE3		0xff00
#define RTC_AL_MTH_MASK		0x000f

#define RTC_AL_YEA		0x0024
#define RTC_AL_YEA_MASK		0x007f

#define RTC_PDN1		0x002c
#define RTC_PDN1_PWRON_TIME	(1 << 7)

#define RTC_PDN2			0x002e
#define RTC_PDN2_PWRON_MTH_MASK		0x000f
#define RTC_PDN2_PWRON_MTH_SHIFT	0
#define RTC_PDN2_PWRON_ALARM		(1 << 4)
#define RTC_PDN2_UART_MASK		0x0060
#define RTC_PDN2_UART_SHIFT		5
#define RTC_PDN2_PWRON_YEA_MASK		0x7f00
#define RTC_PDN2_PWRON_YEA_SHIFT	8
#define RTC_PDN2_PWRON_LOGO		(1 << 15)

#define RTC_MIN_YEAR		1968
#define RTC_BASE_YEAR		1900
#define RTC_NUM_YEARS		128
#define RTC_MIN_YEAR_OFFSET	(RTC_MIN_YEAR - RTC_BASE_YEAR)
#define RTC_RELPWR_WHEN_XRST	1

struct mt6397_rtc {
	struct device		*dev;
	struct rtc_device	*rtc_dev;
	struct mutex		lock;
	struct regmap		*regmap;
	int			irq;
	u32			addr_base;
	u32			addr_range;
};

static u16 rtc_read(struct mt6397_rtc *rtc, u32 offset)
{
	u32 rdata = 0;
	u32 addr = rtc->addr_base + offset;

	if (offset < rtc->addr_range)
		regmap_read(rtc->regmap, addr, &rdata);

	return (u16)rdata;
}

static void rtc_write(struct mt6397_rtc *rtc, u32 offset, u32 data)
{
	u32 addr;

	addr = rtc->addr_base + offset;

	if (offset < rtc->addr_range)
		regmap_write(rtc->regmap, addr, data);
}

static void rtc_write_trigger(struct mt6397_rtc *rtc)
{
	rtc_write(rtc, RTC_WRTGR, 1);
	while (rtc_read(rtc, RTC_BBPU) & RTC_BBPU_CBUSY)
		cpu_relax();
}

static irqreturn_t rtc_irq_handler_thread(int irq, void *data)
{
	struct mt6397_rtc *rtc = data;
	u16 irqsta, irqen;

	mutex_lock(&rtc->lock);
	irqsta = rtc_read(rtc, RTC_IRQ_STA);
	mutex_unlock(&rtc->lock);

	if (irqsta & RTC_IRQ_STA_AL) {
		rtc_update_irq(rtc->rtc_dev, 1, RTC_IRQF | RTC_AF);
		irqen = irqsta & ~RTC_IRQ_EN_AL;
		rtc_write(rtc, RTC_IRQ_EN, irqen);
		rtc_write_trigger(rtc);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int mtk_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	unsigned long time;
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);

	mutex_lock(&rtc->lock);
	do {
		tm->tm_sec = rtc_read(rtc, RTC_TC_SEC);
		tm->tm_min = rtc_read(rtc, RTC_TC_MIN);
		tm->tm_hour = rtc_read(rtc, RTC_TC_HOU);
		tm->tm_mday = rtc_read(rtc, RTC_TC_DOM);
		tm->tm_mon = rtc_read(rtc, RTC_TC_MTH);
		tm->tm_year = rtc_read(rtc, RTC_TC_YEA);
	} while (rtc_read(rtc, RTC_TC_SEC) < tm->tm_sec);
	mutex_unlock(&rtc->lock);

	tm->tm_year += RTC_MIN_YEAR_OFFSET;
	tm->tm_mon--;
	rtc_tm_to_time(tm, &time);

	tm->tm_wday = (time / 86400 + 4) % 7;

	return 0;
}

static int mtk_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);

	tm->tm_year -= RTC_MIN_YEAR_OFFSET;
	tm->tm_mon++;
	mutex_lock(&rtc->lock);
	rtc_write(rtc, RTC_TC_YEA, tm->tm_year);
	rtc_write(rtc, RTC_TC_MTH, tm->tm_mon);
	rtc_write(rtc, RTC_TC_DOM, tm->tm_mday);
	rtc_write(rtc, RTC_TC_HOU, tm->tm_hour);
	rtc_write(rtc, RTC_TC_MIN, tm->tm_min);
	rtc_write(rtc, RTC_TC_SEC, tm->tm_sec);
	rtc_write_trigger(rtc);
	mutex_unlock(&rtc->lock);

	return 0;
}

static int mtk_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct rtc_time *tm = &alm->time;
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	u16 irqen, pdn2;

	mutex_lock(&rtc->lock);
	irqen = rtc_read(rtc, RTC_IRQ_EN);
	pdn2 = rtc_read(rtc, RTC_PDN2);
	tm->tm_sec  = rtc_read(rtc, RTC_AL_SEC);
	tm->tm_min  = rtc_read(rtc, RTC_AL_MIN);
	tm->tm_hour = rtc_read(rtc, RTC_AL_HOU) & RTC_AL_HOU_MASK;
	tm->tm_mday = rtc_read(rtc, RTC_AL_DOM) & RTC_AL_DOM_MASK;
	tm->tm_mon  = rtc_read(rtc, RTC_AL_MTH) & RTC_AL_MTH_MASK;
	tm->tm_year = rtc_read(rtc, RTC_AL_YEA);
	mutex_unlock(&rtc->lock);

	alm->enabled = !!(irqen & RTC_IRQ_EN_AL);
	alm->pending = !!(pdn2 & RTC_PDN2_PWRON_ALARM);

	tm->tm_year += RTC_MIN_YEAR_OFFSET;
	tm->tm_mon--;

	return 0;
}

static int mtk_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct rtc_time *tm = &alm->time;
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	u16 irqen;

	tm->tm_year -= RTC_MIN_YEAR_OFFSET;
	tm->tm_mon++;

	if (alm->enabled) {
		mutex_lock(&rtc->lock);
		rtc_write(rtc, RTC_AL_YEA, tm->tm_year);
		rtc_write(rtc, RTC_AL_MTH, (rtc_read(rtc, RTC_AL_MTH) &
				RTC_NEW_SPARE3) | tm->tm_mon);
		rtc_write(rtc, RTC_AL_DOM, (rtc_read(rtc, RTC_AL_DOM) &
				RTC_NEW_SPARE1) | tm->tm_mday);
		rtc_write(rtc, RTC_AL_HOU, (rtc_read(rtc, RTC_AL_HOU) &
				RTC_NEW_SPARE_FG_MASK) | tm->tm_hour);
		rtc_write(rtc, RTC_AL_MIN, tm->tm_min);
		rtc_write(rtc, RTC_AL_SEC, tm->tm_sec);
		rtc_write(rtc, RTC_AL_MASK, RTC_AL_MASK_DOW);
		rtc_write_trigger(rtc);
		irqen = rtc_read(rtc, RTC_IRQ_EN) | RTC_IRQ_EN_ONESHOT_AL;
		rtc_write(rtc, RTC_IRQ_EN, irqen);
		rtc_write_trigger(rtc);
		mutex_unlock(&rtc->lock);
	}

	return 0;
}

static struct rtc_class_ops mtk_rtc_ops = {
	.read_time  = mtk_rtc_read_time,
	.set_time   = mtk_rtc_set_time,
	.read_alarm = mtk_rtc_read_alarm,
	.set_alarm  = mtk_rtc_set_alarm,
};

static int mtk_rtc_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397_chip = dev_get_drvdata(pdev->dev.parent);
	struct mt6397_rtc *rtc;
	u32 reg[2];
	int ret = 0;

	rtc = devm_kzalloc(&pdev->dev, sizeof(struct mt6397_rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	ret = of_property_read_u32_array(pdev->dev.of_node, "reg", reg, 2);
	if (ret) {
		dev_err(&pdev->dev, "couldn't read rtc base address!\n");
		return -EINVAL;
	}
	rtc->addr_base = reg[0];
	rtc->addr_range = reg[1];
	rtc->regmap = mt6397_chip->regmap;
	rtc->dev = &pdev->dev;
	mutex_init(&rtc->lock);

	platform_set_drvdata(pdev, rtc);

	rtc->rtc_dev = rtc_device_register("mt6397-rtc", &pdev->dev,
				&mtk_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc->rtc_dev)) {
		dev_err(&pdev->dev, "register rtc device failed\n");
		return PTR_ERR(rtc->rtc_dev);
	}

	rtc->irq = platform_get_irq(pdev, 0);
	if (rtc->irq < 0) {
		ret = rtc->irq;
		goto out_rtc;
	}

	ret = devm_request_threaded_irq(&pdev->dev, rtc->irq, NULL,
			rtc_irq_handler_thread, IRQF_ONESHOT,
			"mt6397-rtc", rtc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request alarm IRQ: %d: %d\n",
			rtc->irq, ret);
		goto out_rtc;
	}

	device_init_wakeup(&pdev->dev, 1);

	return 0;

out_rtc:
	rtc_device_unregister(rtc->rtc_dev);
	return ret;

}

static int mtk_rtc_remove(struct platform_device *pdev)
{
	struct mt6397_rtc *rtc = platform_get_drvdata(pdev);

	rtc_device_unregister(rtc->rtc_dev);
	return 0;
}

static const struct of_device_id mt6397_rtc_of_match[] = {
	{ .compatible = "mediatek,mt6397-rtc", },
	{ }
};

static struct platform_driver mtk_rtc_driver = {
	.driver = {
		.name = "mt6397-rtc",
		.of_match_table = mt6397_rtc_of_match,
	},
	.probe	= mtk_rtc_probe,
	.remove = mtk_rtc_remove,
};

module_platform_driver(mtk_rtc_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Tianping Fang <tianping.fang@mediatek.com>");
MODULE_DESCRIPTION("RTC Driver for MediaTek MT6397 PMIC");
MODULE_ALIAS("platform:mt6397-rtc");
