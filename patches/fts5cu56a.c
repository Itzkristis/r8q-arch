// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal driver for the STMicroelectronics FTS5CU56A touchscreen
 * (Samsung TSP firmware, "stm,fts_touch" protocol generation) as found
 * in the Samsung Galaxy S20 FE (r8q).
 *
 * Protocol distilled from Samsung's downstream fts5cu56a driver
 * (drivers/input/touchscreen/stm/fts5cu56a). Raw-byte writes; reads are
 * a one-byte register write followed by a read. Events are 16-byte
 * records drained from a FIFO at reg 0x60/0x61.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#define FTS_READ_DEVICE_ID	0x22
#define FTS_ID0			0x48
#define FTS_ID1			0x36

#define FTS_READ_ONE_EVENT	0x60
#define FTS_READ_ALL_EVENT	0x61
#define FTS_CMD_CLEAR_ALL_EVENT	0x62
#define FTS_CMD_SET_TOUCHTYPE	0x30
#define FTS_TOUCHTYPE_DEFAULT	0x0061	/* TOUCH | PALM | WET */

#define FTS_EVENT_SIZE		16
#define FTS_FIFO_MAX		31
#define FTS_FINGER_MAX		10

#define FTS_EV_COORDINATE	0
#define FTS_EV_STATUS		1

#define FTS_ACT_PRESS		1
#define FTS_ACT_MOVE		2
#define FTS_ACT_RELEASE		3

#define FTS_STYPE_INFO		2
#define FTS_INFO_READY		0x00

#define FTS_MAX_COORD		4095	/* downstream stm,max_coords */

struct fts5_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator_bulk_data supplies[2];
	struct touchscreen_properties prop;
};

static int fts5_write(struct fts5_data *ts, const u8 *buf, int len)
{
	int ret = i2c_master_send(ts->client, buf, len);

	if (ret < 0)
		return ret;
	return ret != len ? -EIO : 0;
}

static int fts5_read(struct fts5_data *ts, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = ts->client->addr, .len = 1, .buf = &reg },
		{ .addr = ts->client->addr, .flags = I2C_M_RD,
		  .len = len, .buf = buf },
	};
	int ret = i2c_transfer(ts->client->adapter, msgs, 2);

	if (ret < 0)
		return ret;
	return ret != 2 ? -EIO : 0;
}

static int fts5_wait_for_ready(struct fts5_data *ts)
{
	u8 ev[FTS_EVENT_SIZE];
	int retry;

	for (retry = 0; retry < 50; retry++) {
		if (fts5_read(ts, FTS_READ_ONE_EVENT, ev, sizeof(ev)))
			return -EIO;

		if ((ev[0] & 0x3) == FTS_EV_STATUS &&
		    ((ev[0] >> 2) & 0xf) == FTS_STYPE_INFO &&
		    ev[1] == FTS_INFO_READY)
			return 0;

		if (ev[0] == 0xf3)	/* error report */
			dev_warn(&ts->client->dev,
				 "boot error event: %*ph\n", 8, ev);

		msleep(20);
	}
	return -ETIMEDOUT;
}

static int fts5_system_reset(struct fts5_data *ts)
{
	static const u8 cmd[] = { 0xfa, 0x20, 0x00, 0x00, 0x24, 0x81 };
	int ret;

	ret = fts5_write(ts, cmd, sizeof(cmd));
	if (ret)
		return ret;
	msleep(10);
	return fts5_wait_for_ready(ts);
}

static void fts5_report_event(struct fts5_data *ts, const u8 *ev)
{
	u8 id = ev[0] & 0x3;
	u8 tid, action;
	unsigned int x, y;
	u8 z, major, minor;

	if (id != FTS_EV_COORDINATE)
		return;

	tid = (ev[0] >> 2) & 0xf;
	action = ev[0] >> 6;
	if (tid >= FTS_FINGER_MAX)
		return;

	x = (ev[1] << 4) | (ev[3] >> 4);
	y = (ev[2] << 4) | (ev[3] & 0xf);
	major = ev[4];
	minor = ev[5];
	z = ev[6] & 0x3f;

	input_mt_slot(ts->input, tid);

	switch (action) {
	case FTS_ACT_PRESS:
	case FTS_ACT_MOVE:
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, major);
		input_report_abs(ts->input, ABS_MT_TOUCH_MINOR, minor);
		input_report_abs(ts->input, ABS_MT_PRESSURE, z ? z : 1);
		break;
	case FTS_ACT_RELEASE:
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
		break;
	}
}

static irqreturn_t fts5_irq_handler(int irq, void *dev_id)
{
	struct fts5_data *ts = dev_id;
	u8 data[FTS_FIFO_MAX * FTS_EVENT_SIZE];
	int left, i;

	if (fts5_read(ts, FTS_READ_ONE_EVENT, data, FTS_EVENT_SIZE))
		return IRQ_HANDLED;

	left = data[7] & 0x1f;
	if (left >= FTS_FIFO_MAX)
		left = FTS_FIFO_MAX - 1;
	if (left > 0 &&
	    fts5_read(ts, FTS_READ_ALL_EVENT, data + FTS_EVENT_SIZE,
		      left * FTS_EVENT_SIZE))
		left = 0;

	for (i = 0; i <= left; i++)
		fts5_report_event(ts, data + i * FTS_EVENT_SIZE);

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);

	return IRQ_HANDLED;
}

static int fts5_hw_init(struct fts5_data *ts)
{
	static const u8 clear_events = FTS_CMD_CLEAR_ALL_EVENT;
	static const u8 touchtype[] = { FTS_CMD_SET_TOUCHTYPE,
		FTS_TOUCHTYPE_DEFAULT & 0xff, FTS_TOUCHTYPE_DEFAULT >> 8 };
	static const u8 int_on[] = { 0xa4, 0x01, 0x01 };
	static const u8 scan_on[] = { 0xa0, 0x00, 0x01 };
	u8 id[5];
	int ret;

	ret = fts5_read(ts, FTS_READ_DEVICE_ID, id, sizeof(id));
	if (ret)
		return dev_err_probe(&ts->client->dev, ret, "no response\n");

	if (id[2] != FTS_ID0 || id[3] != FTS_ID1)
		return dev_err_probe(&ts->client->dev, -ENODEV,
				     "unexpected chip id %*ph\n", 5, id);

	dev_info(&ts->client->dev, "found %c%c %02x %02x rev %02x\n",
		 id[0], id[1], id[2], id[3], id[4]);

	ret = fts5_system_reset(ts);
	if (ret)
		dev_warn(&ts->client->dev,
			 "system reset not confirmed (%d), continuing\n", ret);

	fts5_write(ts, &clear_events, 1);
	fts5_write(ts, touchtype, sizeof(touchtype));
	fts5_write(ts, int_on, sizeof(int_on));
	msleep(10);
	fts5_write(ts, scan_on, sizeof(scan_on));
	msleep(50);

	return 0;
}

static int fts5_probe(struct i2c_client *client)
{
	struct fts5_data *ts;
	int ret;

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;

	ts->supplies[0].supply = "avdd";
	ts->supplies[1].supply = "vdd";
	ret = devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(ts->supplies),
				      ts->supplies);
	if (ret)
		return ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (ret)
		return ret;

	/* power-on to firmware-ready is a few hundred ms */
	msleep(300);

	ret = fts5_hw_init(ts);
	if (ret)
		goto err_disable;

	ts->input = devm_input_allocate_device(&client->dev);
	if (!ts->input) {
		ret = -ENOMEM;
		goto err_disable;
	}

	ts->input->name = "FTS5CU56A Touchscreen";
	ts->input->id.bustype = BUS_I2C;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0,
			     FTS_MAX_COORD, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0,
			     FTS_MAX_COORD, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 63, 0, 0);

	touchscreen_parse_properties(ts->input, true, &ts->prop);

	ret = input_mt_init_slots(ts->input, FTS_FINGER_MAX,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		goto err_disable;

	ret = input_register_device(ts->input);
	if (ret)
		goto err_disable;

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					fts5_irq_handler,
					IRQF_ONESHOT, "fts5cu56a", ts);
	if (ret)
		goto err_disable;

	i2c_set_clientdata(client, ts);
	return 0;

err_disable:
	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
	return ret;
}

static void fts5_remove(struct i2c_client *client)
{
	struct fts5_data *ts = i2c_get_clientdata(client);

	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static const struct of_device_id fts5_of_match[] = {
	{ .compatible = "samsung,fts5cu56a" },
	{ }
};
MODULE_DEVICE_TABLE(of, fts5_of_match);

static const struct i2c_device_id fts5_id[] = {
	{ "fts5cu56a" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fts5_id);

static struct i2c_driver fts5_driver = {
	.driver = {
		.name = "fts5cu56a",
		.of_match_table = fts5_of_match,
	},
	.probe = fts5_probe,
	.remove = fts5_remove,
	.id_table = fts5_id,
};
module_i2c_driver(fts5_driver);

MODULE_DESCRIPTION("STM FTS5CU56A (Samsung TSP) touchscreen driver");
MODULE_LICENSE("GPL");
