/*
 * An I2C driver for the Abracon AB08xx
 *
 * Author: Alexandre Belloni <alexandre.belloni@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/log2.h>

#define ABX8XX_REG_HTH		0x00
#define ABX8XX_REG_SC		0x01
#define ABX8XX_REG_MN		0x02
#define ABX8XX_REG_HR		0x03
#define ABX8XX_REG_DA		0x04
#define ABX8XX_REG_MO		0x05
#define ABX8XX_REG_YR		0x06
#define ABX8XX_REG_WD		0x07

#define ABX8XX_REG_CTRL1	0x10

#define ABX8XX_REG_PART0	0x28
#define ABX8XX_REG_PART1	0x29

#define ABX8XX_CTRL_WRITE	BIT(1)
#define ABX8XX_CTRL_12_24	BIT(6)

enum abx80x_chip {ABX80X, AB0801, AB0802, AB0803, AB0804, AB0805,
	AB1801, AB1802, AB1803, AB1804, AB1805};

static int abx80x_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct i2c_client *client = to_i2c_client(dev);
	unsigned char date[8];
	int err;

	err = i2c_smbus_read_i2c_block_data(client, ABX8XX_REG_HTH,
					    8, date);
	if (err < 0) {
		dev_err(&client->dev, "Unable to read date\n");
		return -EIO;
	}

	tm->tm_sec = bcd2bin(date[ABX8XX_REG_SC] & 0x7F);
	tm->tm_min = bcd2bin(date[ABX8XX_REG_MN] & 0x7F);
	tm->tm_hour = bcd2bin(date[ABX8XX_REG_HR] & 0x3F);
	tm->tm_wday = date[ABX8XX_REG_WD] & 0x7;
	tm->tm_mday = bcd2bin(date[ABX8XX_REG_DA] & 0x3F);
	tm->tm_mon = bcd2bin(date[ABX8XX_REG_MO] & 0x1F) - 1;
	tm->tm_year = bcd2bin(date[ABX8XX_REG_YR]);
	if (tm->tm_year < 70)
		tm->tm_year += 100;

	err = rtc_valid_tm(tm);
	if (err < 0)
		dev_err(&client->dev, "retrieved date/time is not valid.\n");

	return err;
}

static int abx80x_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct i2c_client *client = to_i2c_client(dev);
	int data, err;
	unsigned char buf[8];

	buf[ABX8XX_REG_SC] = bin2bcd(tm->tm_sec);
	buf[ABX8XX_REG_MN] = bin2bcd(tm->tm_min);
	buf[ABX8XX_REG_HR] = bin2bcd(tm->tm_hour);
	buf[ABX8XX_REG_DA] = bin2bcd(tm->tm_mday);
	buf[ABX8XX_REG_MO] = bin2bcd(tm->tm_mon + 1);
	buf[ABX8XX_REG_YR] = bin2bcd(tm->tm_year % 100);
	buf[ABX8XX_REG_WD] = (0x1 << tm->tm_wday);

	data = i2c_smbus_read_byte_data(client, ABX8XX_REG_CTRL1);
	if (data < 0) {
		dev_err(&client->dev, "Unable to read control register\n");
		return -EIO;
	}

	err = i2c_smbus_write_byte_data(client, ABX8XX_REG_CTRL1,
					(data | ABX8XX_CTRL_WRITE));
	if (err < 0) {
		dev_err(&client->dev, "Unable to write control register\n");
		return -EIO;
	}

	err = i2c_smbus_write_i2c_block_data(client, ABX8XX_REG_SC, 7,
					     &buf[ABX8XX_REG_SC]);
	if (err < 0) {
		dev_err(&client->dev, "Unable to write to date registers\n");
		return -EIO;
	}

	err = i2c_smbus_write_byte_data(client, ABX8XX_REG_CTRL1,
					(data & ~ABX8XX_CTRL_WRITE));
	if (err < 0) {
		dev_err(&client->dev, "Unable to write control register\n");
		return -EIO;
	}

	return 0;
}

static const struct rtc_class_ops abx80x_rtc_ops = {
	.read_time	= abx80x_rtc_read_time,
	.set_time	= abx80x_rtc_set_time,
};

static int abx80x_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct rtc_device *rtc;
	int data, err, part0, part1;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	part0 = i2c_smbus_read_byte_data(client, ABX8XX_REG_PART0);
	part1 = i2c_smbus_read_byte_data(client, ABX8XX_REG_PART1);
	if ((part0 < 0) || (part1 < 0)) {
		dev_err(&client->dev, "Unable to read part number\n");
		return -EIO;
	}
	dev_info(&client->dev, "chip found %02x%02x\n", part0, part1);

	data = i2c_smbus_read_byte_data(client, ABX8XX_REG_CTRL1);
	if (data < 0) {
		dev_err(&client->dev, "Unable to read control register\n");
		return -EIO;
	}

	err = i2c_smbus_write_byte_data(client, ABX8XX_REG_CTRL1,
					(data & ~ABX8XX_CTRL_12_24));
	if (err < 0) {
		dev_err(&client->dev, "Unable to write control register\n");
		return -EIO;
	}

	rtc = devm_rtc_device_register(&client->dev, "rtc-abx80x",
				       &abx80x_rtc_ops, THIS_MODULE);

	if (IS_ERR(rtc))
		return PTR_ERR(rtc);

	i2c_set_clientdata(client, rtc);

	return 0;
}

static int abx80x_remove(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id abx80x_id[] = {
	{ "abx80x", ABX80X },
	{ "ab0801", AB0801 },
	{ "ab0802", AB0802 },
	{ "ab0803", AB0803 },
	{ "ab0804", AB0804 },
	{ "ab0805", AB0805 },
	{ "ab1801", AB1801 },
	{ "ab1802", AB1802 },
	{ "ab1803", AB1803 },
	{ "ab1804", AB1804 },
	{ "ab1805", AB1805 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, abx80x_id);

static struct i2c_driver abx80x_driver = {
	.driver		= {
		.name	= "rtc-abx80x",
		.owner	= THIS_MODULE,
	},
	.probe		= abx80x_probe,
	.remove		= abx80x_remove,
	.id_table	= abx80x_id,
};

module_i2c_driver(abx80x_driver);

MODULE_AUTHOR("Alexandre Belloni <alexandre.belloni@free-electrons.com>");
MODULE_DESCRIPTION("Abracon ABX80X RTC driver");
MODULE_LICENSE("GPL");
