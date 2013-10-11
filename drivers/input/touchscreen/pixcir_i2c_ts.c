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

struct pixcir_slot {
	bool active;
	bool updated;	/* only updated slots will be reported */
	int id;
	int x;
	int y;
};

struct pixcir_i2c_ts_data {
	struct i2c_client *client;
	struct input_dev *input;
	const struct pixcir_ts_platform_data *pdata;
	bool exiting;
	int num_slots;	/* number of slots in slot table */
	struct pixcir_slot *slots;	/*slot table */
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

		if (gpio_is_valid(pdata->gpio_attb) &&
				!gpio_get_value(pdata->gpio_attb))
			break;

		msleep(20);
	}
}

static void pixcir_ts_typeb_report(struct pixcir_i2c_ts_data *ts)
{
	const struct pixcir_ts_platform_data *pdata = ts->pdata;
	struct device *dev = &ts->client->dev;
	u8 rdbuf[31], wrbuf[1] = { 0 };
	u8 *bufptr;
	u8 num_fingers;
	u8 unreliable;
	int ret, i, j;

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

		/* figure out updated slots */
		for (i = 0; i < ts->num_slots; i++) {
			ts->slots[i].updated = 0;
		}

		for (i = 0, j = 0; i < num_fingers; i++, j += 5) {
			ts->slots[i].updated = 1;
			ts->slots[i].x = bufptr[j + 1] << 8 | bufptr[j];
			ts->slots[i].y = bufptr[j + 3] << 8 | bufptr[j + 2];
			ts->slots[i].id = bufptr[j + 4];
		}

		/*
		 * if a previously active slot is not updated it means it has
		 * turned inactive and we need to report that to input subsys.
		 */
		for (i = 0; i < ts->num_slots; i++) {
			if (ts->slots[i].active &&
				!ts->slots[i].updated) {

				ts->slots[i].updated = 1;
				ts->slots[i].active = 0;
			}
		}

		/* report all updated slots */
		for (i = 0; i < ts->num_slots; i++) {
			if (!ts->slots[i].updated)
				continue;

			input_mt_slot(ts->input, i);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER,
							ts->slots[i].active);

			input_report_abs(ts->input, ABS_X, ts->slots[i].x);
			input_report_abs(ts->input, ABS_Y, ts->slots[i].y);
			input_report_abs(ts->input, ABS_MT_POSITION_X,
					 ts->slots[i].x);
			input_report_abs(ts->input, ABS_MT_POSITION_Y,
					 ts->slots[i].y);
			input_report_abs(ts->input, ABS_MT_TRACKING_ID,
					 ts->slots[i].id);
			input_mt_sync(ts->input);
		}

		input_mt_report_pointer_emulation(ts->input, true);
		input_sync(ts->input);

next:
		if (gpio_is_valid(pdata->gpio_attb) &&
				!gpio_get_value(pdata->gpio_attb))
			break;

		msleep(20);
	}
}

static irqreturn_t pixcir_ts_isr(int irq, void *dev_id)
{
	struct pixcir_i2c_ts_data *tsdata = dev_id;

	if (tsdata->num_slots)
		pixcir_ts_typeb_report(tsdata);
	else
		pixcir_ts_typea_report(tsdata);

	return IRQ_HANDLED;
}

static int pixcir_set_power_mode(struct pixcir_i2c_ts_data *ts, enum pixcir_power_mode mode)
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
static int pixcir_set_int_mode(struct pixcir_i2c_ts_data *ts, enum pixcir_int_mode mode,
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
		ret |= PIXCIR_INT_ENABLE | PIXCIR_INT_PULSE_TOUCH;
	else
		ret &= ~(PIXCIR_INT_ENABLE | PIXCIR_INT_PULSE_TOUCH);

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

	/* LEVEL_TOUCH interrupt with active high polarity */
	ret = pixcir_set_int_mode(ts, PIXCIR_INT_LEVEL_TOUCH, 1);
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

	/* switch to full power */
	ret = pixcir_set_power_mode(ts, PIXCIR_POWER_ACTIVE);
	if (ret) {
		dev_err(dev, "Failed to set ACTIVE power mode\n");
		pixcir_int_enable(ts, 0);
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

	/* switch to low power */
	ret = pixcir_set_power_mode(ts, PIXCIR_POWER_IDLE);
	if (ret) {
		dev_err(dev, "Failed to power down\n");
		return ret;
	}

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


#ifdef CONFIG_PM_SLEEP
static int pixcir_i2c_ts_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	if (device_may_wakeup(&client->dev))
		enable_irq_wake(client->irq);

	return 0;
}

static int pixcir_i2c_ts_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	if (device_may_wakeup(&client->dev))
		disable_irq_wake(client->irq);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(pixcir_dev_pm_ops,
			 pixcir_i2c_ts_suspend, pixcir_i2c_ts_resume);

#if defined (CONFIG_OF)
static const struct of_device_id pixcir_of_match[];
#endif

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
	if (!gpio_is_valid(pdata->gpio_attb))
		dev_err(dev, "Failed to get ATTB GPIO\n");

	if (of_property_read_u32(np, "x-size", &pdata->x_max)) {
		dev_err(dev, "Failed to get x-size property\n");
		return ERR_PTR(-EINVAL);
	}

	if (of_property_read_u32(np, "y-size", &pdata->y_max)) {
		dev_err(dev, "Failed to get y-size property\n");
		return ERR_PTR(-EINVAL);
	}

	return pdata;
}

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

	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_ABS, input->evbit);
	__set_bit(BTN_TOUCH, input->keybit);
	input_set_abs_params(input, ABS_X, 0, pdata->x_max, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, pdata->y_max, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, pdata->x_max, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, pdata->y_max, 0, 0);

	/* Type-B Multi-Touch support */
	if (pdata->chip.num_report_ids) {
		const struct pixcir_i2c_chip_data *chip = &pdata->chip;
		unsigned int num_mt_slots;

		num_mt_slots = chip->num_report_ids;
		tsdata->num_slots = num_mt_slots;

		tsdata->slots = devm_kzalloc(dev,
					num_mt_slots * sizeof (*tsdata->slots),
					GFP_KERNEL);
		if (!tsdata->slots)
			return -ENOMEM;

		error = input_mt_init_slots(input, num_mt_slots, 0);
		if (error) {
			dev_err(dev, "Error initializing Multi-Touch slots\n");
			return error;
		}
	}

	input_set_drvdata(input, tsdata);

	if (gpio_is_valid(pdata->gpio_attb)) {
		error = devm_gpio_request_one(dev, pdata->gpio_attb,
				GPIOF_DIR_IN, "pixcir_i2c_attb");
		if (error) {
			dev_err(dev, "Failed to request ATTB gpio\n");
			return error;
		}
	}

	error = devm_request_threaded_irq(dev, client->irq, NULL, pixcir_ts_isr,
				     IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				     client->name, tsdata);
	if (error) {
		dev_err(dev, "failed to request irq %d\n", client->irq);
		return error;
	}

	/* put device in low power till opened */
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

static const struct i2c_device_id pixcir_i2c_ts_id[] = {
	{ "pixcir_ts", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pixcir_i2c_ts_id);

#if defined (CONFIG_OF)
static const struct pixcir_i2c_chip_data tangoc_data = {
	.num_report_ids = 5,
	.reportid_min = 'A',
};

static const struct pixcir_i2c_chip_data pixcir_ts_data = {
	.num_report_ids = 5,
	.reportid_min = 'A',
};

static const struct of_device_id pixcir_of_match[] = {
	{ .compatible = "pixcir,pixcir_ts", .data = &pixcir_ts_data, },
	{ .compatible = "pixcir,tangoc", .data = &tangoc_data, },
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
