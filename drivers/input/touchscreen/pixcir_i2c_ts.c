/*
 * Driver for Pixcir I2C touchscreen controllers.
 *
 * Copyright (C) 2010-2011 Pixcir, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/pixcir_ts.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>

#define MAX_FINGERS	5	/* Maximum supported by the driver */

struct pixcir_i2c_ts_data {
	struct i2c_client *client;
	struct input_dev *input;
	const struct pixcir_ts_platform_data *pdata;
	bool exiting;
	u8 max_fingers;		/* Maximum supported by the chip */
};

static void pixcir_ts_typea_report(struct pixcir_i2c_ts_data *tsdata)
{
	const struct pixcir_ts_platform_data *pdata = tsdata->pdata;
	u8 rdbuf[10], wrbuf[1] = { 0 };
	u8 touch;
	int ret;

	while (!tsdata->exiting) {

		ret = i2c_master_send(tsdata->client, wrbuf, sizeof(wrbuf));
		if (ret != sizeof(wrbuf)) {
			dev_err(&tsdata->client->dev,
				 "%s: i2c_master_send failed(), ret=%d\n",
				 __func__, ret);
			return;
		}

		ret = i2c_master_recv(tsdata->client, rdbuf, sizeof(rdbuf));
		if (ret != sizeof(rdbuf)) {
			dev_err(&tsdata->client->dev,
				 "%s: i2c_master_recv failed(), ret=%d\n",
				 __func__, ret);
			return;
		}

		touch = rdbuf[0];
		if (touch) {
			u16 posx1 = (rdbuf[3] << 8) | rdbuf[2];
			u16 posy1 = (rdbuf[5] << 8) | rdbuf[4];
			u16 posx2 = (rdbuf[7] << 8) | rdbuf[6];
			u16 posy2 = (rdbuf[9] << 8) | rdbuf[8];

			input_report_key(tsdata->input, BTN_TOUCH, 1);
			input_report_abs(tsdata->input, ABS_X, posx1);
			input_report_abs(tsdata->input, ABS_Y, posy1);

			input_report_abs(tsdata->input, ABS_MT_POSITION_X,
									posx1);
			input_report_abs(tsdata->input, ABS_MT_POSITION_Y,
									posy1);
			input_mt_sync(tsdata->input);

			if (touch == 2) {
				input_report_abs(tsdata->input,
						ABS_MT_POSITION_X, posx2);
				input_report_abs(tsdata->input,
						ABS_MT_POSITION_Y, posy2);
				input_mt_sync(tsdata->input);
			}
		} else {
			input_report_key(tsdata->input, BTN_TOUCH, 0);
		}

		input_sync(tsdata->input);

		if (gpio_get_value(pdata->gpio_attb))
			break;

		msleep(20);
	}
}

static void pixcir_ts_typeb_report(struct pixcir_i2c_ts_data *ts)
{
	const struct pixcir_ts_platform_data *pdata = ts->pdata;
	struct device *dev = &ts->client->dev;
	u8 rdbuf[32], wrbuf[1] = { 0 };
	u8 *bufptr;
	u8 num_fingers;
	u8 unreliable;
	int ret, i;

	while (!ts->exiting) {

		ret = i2c_master_send(ts->client, wrbuf, sizeof(wrbuf));
		if (ret != sizeof(wrbuf)) {
			dev_err(dev, "%s: i2c_master_send failed(), ret=%d\n",
				 __func__, ret);
			return;
		}

		ret = i2c_master_recv(ts->client, rdbuf, sizeof(rdbuf));
		if (ret != sizeof(rdbuf)) {
			dev_err(dev, "%s: i2c_master_recv failed(), ret=%d\n",
				 __func__, ret);
			return;
		}

		unreliable = rdbuf[0] & 0xe0;

		if (unreliable)
			goto next;	/* ignore unreliable data */

		num_fingers = rdbuf[0] & 0x7;
		bufptr = &rdbuf[2];

		if (num_fingers > ts->max_fingers) {
			num_fingers = ts->max_fingers;
			dev_dbg(dev, "limiting num_fingers to %d\n",
								num_fingers);
		}

		for (i = 0; i < num_fingers; i++) {
			u8 id;
			unsigned int x, y;
			int slot;

			id = bufptr[4];
			slot = input_mt_get_slot_by_key(ts->input, id);
			if (slot < 0) {
				dev_dbg(dev, "no free slot for id 0x%x\n", id);
				continue;
			}


			x = bufptr[1] << 8 | bufptr[0];
			y = bufptr[3] << 8 | bufptr[2];

			input_mt_slot(ts->input, slot);
			input_mt_report_slot_state(ts->input,
							MT_TOOL_FINGER, true);

			input_event(ts->input, EV_ABS, ABS_MT_POSITION_X, x);
			input_event(ts->input, EV_ABS, ABS_MT_POSITION_Y, y);

			bufptr = &bufptr[5];
			dev_dbg(dev, "%d: id 0x%x slot %d, x %d, y %d\n",
							i, id, slot, x, y);
		}

		/* One frame is complete so sync it */
		input_mt_sync_frame(ts->input);
		input_sync(ts->input);

next:
		if (gpio_get_value(pdata->gpio_attb))
			break;

		usleep_range(2000, 5000);
	}
}

static irqreturn_t pixcir_ts_isr(int irq, void *dev_id)
{
	struct pixcir_i2c_ts_data *tsdata = dev_id;

	if (tsdata->input->mt)
		pixcir_ts_typeb_report(tsdata);
	else
		pixcir_ts_typea_report(tsdata);

	return IRQ_HANDLED;
}

static int pixcir_set_power_mode(struct pixcir_i2c_ts_data *ts,
						enum pixcir_power_mode mode)
{
	struct device *dev = &ts->client->dev;
	int ret;

	ret = i2c_smbus_read_byte_data(ts->client, PIXCIR_REG_POWER_MODE);
	if (ret < 0) {
		dev_err(dev, "%s: can't read reg 0x%x : %d\n",
					__func__, PIXCIR_REG_POWER_MODE, ret);
		return ret;
	}

	ret &= ~PIXCIR_POWER_MODE_MASK;
	ret |= mode;

	/* Always AUTO_IDLE */
	ret |= PIXCIR_POWER_ALLOW_IDLE;

	ret = i2c_smbus_write_byte_data(ts->client, PIXCIR_REG_POWER_MODE, ret);
	if (ret < 0) {
		dev_err(dev, "%s: can't write reg 0x%x : %d\n",
					__func__, PIXCIR_REG_POWER_MODE, ret);
		return ret;
	}

	return 0;
}

/*
 * Set the interrupt mode for the device i.e. ATTB line behaviour
 *
 * @polarity : 1 for active high, 0 for active low.
 */
static int pixcir_set_int_mode(struct pixcir_i2c_ts_data *ts,
						enum pixcir_int_mode mode,
					bool polarity)
{
	struct device *dev = &ts->client->dev;
	int ret;

	ret = i2c_smbus_read_byte_data(ts->client, PIXCIR_REG_INT_MODE);
	if (ret < 0) {
		dev_err(dev, "%s: can't read reg 0x%x : %d\n",
					__func__, PIXCIR_REG_INT_MODE, ret);
		return ret;
	}

	ret &= ~PIXCIR_INT_MODE_MASK;
	ret |= mode;

	if (polarity)
		ret |= PIXCIR_INT_POL_HIGH;
	else
		ret &= ~PIXCIR_INT_POL_HIGH;

	ret = i2c_smbus_write_byte_data(ts->client, PIXCIR_REG_INT_MODE, ret);
	if (ret < 0) {
		dev_err(dev, "%s: can't write reg 0x%x : %d\n",
					__func__, PIXCIR_REG_INT_MODE, ret);
		return ret;
	}

	return 0;
}

/*
 * Enable/disable interrupt generation
 */
static int pixcir_int_enable(struct pixcir_i2c_ts_data *ts, bool enable)
{
	struct device *dev = &ts->client->dev;
	int ret;

	ret = i2c_smbus_read_byte_data(ts->client, PIXCIR_REG_INT_MODE);
	if (ret < 0) {
		dev_err(dev, "%s: can't read reg 0x%x : %d\n",
					__func__, PIXCIR_REG_INT_MODE, ret);
		return ret;
	}

	if (enable)
		ret |= PIXCIR_INT_ENABLE;
	else
		ret &= ~PIXCIR_INT_ENABLE;

	ret = i2c_smbus_write_byte_data(ts->client, PIXCIR_REG_INT_MODE, ret);
	if (ret < 0) {
		dev_err(dev, "%s: can't write reg 0x%x : %d\n",
					__func__, PIXCIR_REG_INT_MODE, ret);
		return ret;
	}

	return 0;
}

static int pixcir_start(struct pixcir_i2c_ts_data *ts)
{
	struct device *dev = &ts->client->dev;
	int ret;

	/* LEVEL_TOUCH interrupt with active low polarity */
	ret = pixcir_set_int_mode(ts, PIXCIR_INT_LEVEL_TOUCH, 0);
	if (ret) {
		dev_err(dev, "Failed to set interrupt mode\n");
		return ret;
	}

	enable_irq(ts->client->irq);

	/* enable interrupt generation */
	ret = pixcir_int_enable(ts, 1);
	if (ret) {
		dev_err(dev, "Failed to enable interrupt generation\n");
		return ret;
	}

	return 0;
}

static int pixcir_stop(struct pixcir_i2c_ts_data *ts)
{
	struct device *dev = &ts->client->dev;
	int ret;

	/* disable interrupt generation */
	ret = pixcir_int_enable(ts, 0);
	if (ret) {
		dev_err(dev, "Failed to disable interrupt generation\n");
		return ret;
	}

	disable_irq(ts->client->irq);

	return 0;
}

static int pixcir_input_open(struct input_dev *dev)
{
	struct pixcir_i2c_ts_data *ts = input_get_drvdata(dev);

	return pixcir_start(ts);
}

static void pixcir_input_close(struct input_dev *dev)
{
	struct pixcir_i2c_ts_data *ts = input_get_drvdata(dev);

	pixcir_stop(ts);

	return;
}

#if defined(CONFIG_OF)
static const struct of_device_id pixcir_of_match[];

static struct pixcir_ts_platform_data *pixcir_parse_dt(struct device *dev)
{
	struct pixcir_ts_platform_data *pdata;
	struct device_node *np = dev->of_node;
	const struct of_device_id *match;

	match = of_match_device(of_match_ptr(pixcir_of_match), dev);
	if (!match)
		return ERR_PTR(-EINVAL);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->chip = *(const struct pixcir_i2c_chip_data *)match->data;

	pdata->gpio_attb = of_get_named_gpio(np, "attb-gpio", 0);
	if (!gpio_is_valid(pdata->gpio_attb)) {
		dev_err(dev, "Failed to get ATTB GPIO\n");
		return ERR_PTR(-EINVAL);
	}

	if (of_property_read_u32(np, "x-size", &pdata->x_size)) {
		dev_err(dev, "Failed to get x-size property\n");
		return ERR_PTR(-EINVAL);
	}

	if (of_property_read_u32(np, "y-size", &pdata->y_size)) {
		dev_err(dev, "Failed to get y-size property\n");
		return ERR_PTR(-EINVAL);
	}

	dev_dbg(dev, "%s: x %d, y %d, gpio %d\n", __func__,
				pdata->x_size, pdata->y_size, pdata->gpio_attb);

	return pdata;
}
#else
static struct pixcir_ts_platform_data *pixcir_parse_dt(struct device *dev)
{
	return NULL;
}
#endif

static int pixcir_i2c_ts_probe(struct i2c_client *client,
					 const struct i2c_device_id *id)
{
	const struct pixcir_ts_platform_data *pdata = client->dev.platform_data;
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	struct pixcir_i2c_ts_data *tsdata;
	struct input_dev *input;
	int error;

	if (np) {
		pdata = pixcir_parse_dt(dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);

	} else if (!pdata) {
		dev_err(&client->dev, "platform data not defined\n");
		return -EINVAL;
	} else {
		if (!gpio_is_valid(pdata->gpio_attb)) {
			dev_err(dev, "Invalid gpio_attb in pdata\n");
			return -EINVAL;
		}
	}

	tsdata = devm_kzalloc(dev, sizeof(*tsdata), GFP_KERNEL);
	if (!tsdata)
		return -ENOMEM;

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(&client->dev, "Failed to allocate input device\n");
		return -ENOMEM;
	}

	tsdata->client = client;
	tsdata->input = input;
	tsdata->pdata = pdata;

	input->name = client->name;
	input->id.bustype = BUS_I2C;
	input->dev.parent = &client->dev;
	input->open = pixcir_input_open;
	input->close = pixcir_input_close;

	__set_bit(EV_ABS, input->evbit);
	__set_bit(BTN_TOUCH, input->keybit);

	input_set_abs_params(input, ABS_X,
					0, pdata->x_size - 1, 0, 0);
	input_set_abs_params(input, ABS_Y,
					0, pdata->y_size - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_X,
					0, pdata->x_size - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y,
					0, pdata->y_size - 1, 0, 0);

	/* Type-B Multi-Touch support */
	if (pdata->chip.num_report_ids) {
		const struct pixcir_i2c_chip_data *chip = &pdata->chip;

		tsdata->max_fingers = chip->num_report_ids;
		if (tsdata->max_fingers > MAX_FINGERS) {
			dev_info(dev, "Limiting maximum fingers to %d\n",
								MAX_FINGERS);
			tsdata->max_fingers = MAX_FINGERS;
		}

		error = input_mt_init_slots(input, tsdata->max_fingers,
					INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
		if (error) {
			dev_err(dev, "Error initializing Multi-Touch slots\n");
			return error;
		}
	}

	input_set_drvdata(input, tsdata);

	error = devm_gpio_request_one(dev, pdata->gpio_attb,
			GPIOF_DIR_IN, "pixcir_i2c_attb");
	if (error) {
		dev_err(dev, "Failed to request ATTB gpio\n");
		return error;
	}

	error = devm_request_threaded_irq(dev, client->irq, NULL, pixcir_ts_isr,
				     IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING
				     | IRQF_ONESHOT,
				     client->name, tsdata);
	if (error) {
		dev_err(dev, "failed to request irq %d\n", client->irq);
		return error;
	}

	/* Always be in IDLE mode to save power, device supports auto wake */
	error = pixcir_set_power_mode(tsdata, PIXCIR_POWER_IDLE);
	if (error) {
		dev_err(dev, "Failed to set IDLE mode\n");
		return error;
	}

	/* Stop device till opened */
	error = pixcir_stop(tsdata);
	if (error)
		return error;

	error = input_register_device(input);
	if (error)
		return error;

	i2c_set_clientdata(client, tsdata);
	device_init_wakeup(&client->dev, 1);

	return 0;
}

static int pixcir_i2c_ts_remove(struct i2c_client *client)
{
	struct pixcir_i2c_ts_data *tsdata = i2c_get_clientdata(client);

	device_init_wakeup(&client->dev, 0);

	tsdata->exiting = true;
	mb();

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int pixcir_i2c_ts_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pixcir_i2c_ts_data *ts = i2c_get_clientdata(client);
	struct input_dev *input = ts->input;
	int ret = 0;

	mutex_lock(&input->mutex);

	if (device_may_wakeup(&client->dev)) {
		/* need to start device if not open, to be wakeup source */
		if (!input->users) {
			ret = pixcir_start(ts);
			if (ret)
				goto unlock;
		}

		enable_irq_wake(client->irq);

	} else if (input->users) {
		ret = pixcir_stop(ts);
	}

unlock:
	mutex_unlock(&input->mutex);

	return ret;
}

static int pixcir_i2c_ts_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pixcir_i2c_ts_data *ts = i2c_get_clientdata(client);
	struct input_dev *input = ts->input;
	int ret = 0;

	mutex_lock(&input->mutex);

	if (device_may_wakeup(&client->dev)) {
		disable_irq_wake(client->irq);

		/* need to stop device if it was not open on suspend */
		if (!input->users) {
			ret = pixcir_stop(ts);
			if (ret)
				goto unlock;
		}

	} else if (input->users) {
		ret = pixcir_start(ts);
	}

unlock:
	mutex_unlock(&input->mutex);

	return ret;
}
#endif

static SIMPLE_DEV_PM_OPS(pixcir_dev_pm_ops,
				pixcir_i2c_ts_suspend, pixcir_i2c_ts_resume);

static const struct i2c_device_id pixcir_i2c_ts_id[] = {
	{ "pixcir_ts", 0 },
	{ "pixcir_tangoc", 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, pixcir_i2c_ts_id);

#if defined(CONFIG_OF)
static const struct pixcir_i2c_chip_data tangoc_data = {
	.num_report_ids = 5,
};

static const struct of_device_id pixcir_of_match[] = {
	{ .compatible = "pixcir,pixcir_ts", },
	{ .compatible = "pixcir,pixcir_tangoc", .data = &tangoc_data, },
	{ }
};
MODULE_DEVICE_TABLE(of, pixcir_of_match);
#endif

static struct i2c_driver pixcir_i2c_ts_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "pixcir_ts",
		.pm	= &pixcir_dev_pm_ops,
		.of_match_table = of_match_ptr(pixcir_of_match),
	},
	.probe		= pixcir_i2c_ts_probe,
	.remove		= pixcir_i2c_ts_remove,
	.id_table	= pixcir_i2c_ts_id,
};

module_i2c_driver(pixcir_i2c_ts_driver);

MODULE_AUTHOR("Jianchun Bian <jcbian@pixcir.com.cn>, Dequan Meng <dqmeng@pixcir.com.cn>");
MODULE_DESCRIPTION("Pixcir I2C Touchscreen Driver");
MODULE_LICENSE("GPL");
