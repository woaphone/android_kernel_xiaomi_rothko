// SPDX-License-Identifier: GPL-2.0-only
/*
 * Redmi K70 Ultra FT3683 SPI touchscreen.
 * The wire protocol and report layouts are recovered from the device's
 * focaltech_touch_rothko.ko (OS2.0.207.0.VNNTWXM), not from an FT3681 ID table.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include "xiaomi_touch_common.h"

#define FT3683_POINTS		10
#define FT3683_REPORT_LEN	90
#define FT3683_DATA_OFFSET	7
#define FT3683_TRANSFER_LEN	(FT3683_REPORT_LEN + FT3683_DATA_OFFSET + 2)

struct ft3683 {
	struct spi_device *spi;
	struct input_dev *input;
	struct gpio_desc *reset;
	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *suspend;
	struct regulator_bulk_data supplies[2];
	struct mutex lock;
	u32 max_points;
	u32 factor;
	u8 tx[FT3683_TRANSFER_LEN];
	u8 rx[FT3683_TRANSFER_LEN];
};

static u16 ft3683_crc(const u8 *data, size_t len)
{
	u16 crc = 0xffff;
	int bit;

	while (len--) {
		crc ^= *data++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
	}
	return crc;
}

/* Caller holds lock: SPI buffers are DMA-safe storage in the device object. */
static int ft3683_read(struct ft3683 *ts, u8 reg, u8 *data, size_t len)
{
	struct spi_transfer transfer = {
		.tx_buf = ts->tx,
		.rx_buf = ts->rx,
		.len = FT3683_DATA_OFFSET + len + 2,
	};
	u8 *payload = ts->rx + FT3683_DATA_OFFSET;
	u16 crc;
	int ret;

	memset(ts->tx, 0, transfer.len);
	ts->tx[0] = reg;
	ts->tx[1] = 0xa0;
	ts->tx[2] = len >> 8;
	ts->tx[3] = len;
	ret = spi_sync_transfer(ts->spi, &transfer, 1);
	udelay(150);
	if (ret)
		return ret;
	if (ts->rx[3] & 0xa0)
		return -EIO;
	crc = payload[len] | (payload[len + 1] << 8);
	if (ft3683_crc(payload, len) != crc)
		return -EBADMSG;
	memcpy(data, payload, len);
	return 0;
}

static int ft3683_write_reg(struct ft3683 *ts, u8 reg, u8 value)
{
	struct spi_transfer transfer = {
		.tx_buf = ts->tx,
		.rx_buf = ts->rx,
		.len = 8,
	};
	int ret;

	memset(ts->tx, 0, transfer.len);
	ts->tx[0] = reg;
	ts->tx[3] = 1;
	ts->tx[7] = value;
	ret = spi_sync_transfer(ts->spi, &transfer, 1);
	udelay(150);
	if (ret)
		return ret;
	return (ts->rx[3] & 0xa0) ? -EIO : 0;
}

static void ft3683_reset(struct ft3683 *ts)
{
	gpiod_set_raw_value_cansleep(ts->reset, 0);
	usleep_range(1000, 2000);
	gpiod_set_raw_value_cansleep(ts->reset, 1);
	msleep(200);
}

static int ft3683_identify(struct ft3683 *ts)
{
	u8 high, low;
	int ret;

	ret = ft3683_read(ts, 0xa3, &high, 1);
	if (ret)
		return ret;
	ret = ft3683_read(ts, 0x9f, &low, 1);
	if (ret)
		return ret;
	/* fts_get_chip_types in the original module accepts runtime ID 36/83. */
	return high == 0x36 && low == 0x83 ? 0 : -ENODEV;
}

static int ft3683_report(struct ft3683 *ts, const u8 *data)
{
	struct input_dev *input = ts->input;
	u8 type = data[1] >> 4;
	u8 count = data[1] & 0x0f;
	unsigned long seen = 0;
	unsigned int step, base, records, i;

	switch (type) {
	case 0:
		base = 2;
		step = 6;
		records = ts->max_points;
		break;
	case 2:
		base = 4;
		step = 8;
		records = count;
		break;
	case 8:
		base = 4;
		step = 6;
		records = count;
		break;
	default:
		/* Gesture, firmware status and pen records are not finger packets. */
		return -ENOMSG;
	}
	if (count > ts->max_points)
		return -EPROTO;

	/* Validate device-provided IDs before changing the input frame. */
	for (i = 0; i < records; i++) {
		const u8 *p = data + base + i * step;
		u8 id = p[2] >> 4;

		if (type == 0 && id == 0x0f) {
			records = i;
			break;
		}
		if (id >= ts->max_points || test_and_set_bit(id, &seen))
			return -EPROTO;
	}

	for (i = 0; i < records; i++) {
		const u8 *p = data + base + i * step;
		u8 id = p[2] >> 4;
		u8 event = p[0] >> 6;
		u32 x, y, pressure, area;

		input_mt_slot(input, id);
		input_mt_report_slot_state(input, MT_TOOL_FINGER,
					   event == 0 || event == 2);
		if (event != 0 && event != 2)
			continue;
		if (type == 2) {
			x = ((p[0] & 0x0f) << 12) | (p[1] << 4) | (p[4] >> 4);
			y = ((p[2] & 0x0f) << 12) | (p[3] << 4) | (p[4] & 0x0f);
			x = x * ts->factor / 16;
			y = y * ts->factor / 16;
			pressure = p[5];
			area = p[6];
		} else if (type == 0 && ts->factor == 10) {
			x = ((p[0] & 0x0f) << 11) | (p[1] << 3) |
			    ((p[4] & 0xc0) >> 5) | ((p[0] & 0x20) >> 5);
			y = ((p[2] & 0x0f) << 11) | (p[3] << 3) |
			    ((p[4] & 0x30) >> 3) | ((p[2] & 0x10) >> 4);
			pressure = p[4] & 0x0f;
			area = p[5] & 0x7f;
		} else {
			x = ((p[0] & 0x0f) << 8) | p[1];
			y = ((p[2] & 0x0f) << 8) | p[3];
			pressure = p[4];
			area = p[5];
		}
		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);
		input_report_abs(input, ABS_MT_PRESSURE, pressure ?: 63);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, area ?: 9);
	}
	input_mt_sync_frame(input);
	input_sync(input);
	return 0;
}

static irqreturn_t ft3683_irq(int irq, void *arg)
{
	struct ft3683 *ts = arg;
	u8 data[FT3683_REPORT_LEN];
	int ret;

	mutex_lock(&ts->lock);
	ret = ft3683_read(ts, 0x01, data, sizeof(data));
	if (!ret)
		ret = ft3683_report(ts, data);
	mutex_unlock(&ts->lock);
	return IRQ_HANDLED;
}

static ssize_t firmware_version_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ft3683 *ts = dev_get_drvdata(dev);
	u8 version;
	int ret;

	mutex_lock(&ts->lock);
	ret = ft3683_read(ts, 0xa6, &version, 1);
	mutex_unlock(&ts->lock);
	return ret ? ret : sysfs_emit(buf, "%02x\n", version);
}
static DEVICE_ATTR_RO(firmware_version);

static struct attribute *ft3683_attrs[] = {
	&dev_attr_firmware_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ft3683);

static void ft3683_power_off(void *arg)
{
	struct ft3683 *ts = arg;

	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static int ft3683_read_palm(void *data, u8 *palm)
{
	struct ft3683 *ts = data;
	int ret;

	mutex_lock(&ts->lock);
	ret = ft3683_read(ts, 0x9b, palm, 1);
	mutex_unlock(&ts->lock);
	return ret;
}

static const struct xiaomi_touch_ops ft3683_xiaomi_ops = {
	.read_palm = ft3683_read_palm,
};

static int ft3683_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct pinctrl_state *spi_mode;
	struct ft3683 *ts;
	u32 coords[4];
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->spi = spi;
	mutex_init(&ts->lock);
	spi_set_drvdata(spi, ts);
	ts->max_points = FT3683_POINTS;
	ts->factor = 1;
	device_property_read_u32(dev, "focaltech,max-touch-number", &ts->max_points);
	device_property_read_u32(dev, "focaltech,super-resolution-factors", &ts->factor);
	if (!ts->max_points || ts->max_points > FT3683_POINTS || !ts->factor)
		return -EINVAL;
	ret = device_property_read_u32_array(dev, "focaltech,display-coords", coords, 4);
	if (ret)
		return ret;

	ts->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(ts->pinctrl))
		return PTR_ERR(ts->pinctrl);
	ts->active = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_active");
	if (IS_ERR(ts->active))
		return PTR_ERR(ts->active);
	ts->suspend = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_suspend");
	if (IS_ERR(ts->suspend))
		return PTR_ERR(ts->suspend);
	ret = pinctrl_select_state(ts->pinctrl, ts->active);
	if (ret)
		return ret;
	spi_mode = pinctrl_lookup_state(ts->pinctrl, "pmx_gt_spi_mode");
	if (IS_ERR(spi_mode))
		return PTR_ERR(spi_mode);
	ret = pinctrl_select_state(ts->pinctrl, spi_mode);
	if (ret)
		return ret;

	ts->reset = devm_fwnode_gpiod_get_index(dev, dev_fwnode(dev),
					       "focaltech,reset", 0,
					       GPIOD_OUT_LOW, "ft3683-reset");
	if (IS_ERR(ts->reset))
		return PTR_ERR(ts->reset);
	ts->supplies[0].supply = "iovdd_focal";
	ts->supplies[1].supply = "avdd_focal";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies), ts->supplies);
	if (ret)
		return ret;
	ret = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, ft3683_power_off, ts);
	if (ret)
		return ret;
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	ret = spi_setup(spi);
	if (ret)
		return ret;
	ft3683_reset(ts);
	ret = ft3683_identify(ts);
	if (ret)
		return ret;

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "fts_ts";
	ts->input->id.bustype = BUS_SPI;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, coords[0], coords[2], 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, coords[1], coords[3], 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	ret = input_mt_init_slots(ts->input, ts->max_points,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;
	ret = input_register_device(ts->input);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, spi->irq, NULL, ft3683_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
					dev_name(dev), ts);
	if (ret)
		return ret;
	return devm_xiaomi_touch_register(dev, &ft3683_xiaomi_ops, ts, ts->factor);
}

static int ft3683_suspend(struct device *dev)
{
	struct ft3683 *ts = dev_get_drvdata(dev);
	int ret;

	disable_irq(ts->spi->irq);
	mutex_lock(&ts->lock);
	ret = ft3683_write_reg(ts, 0xa5, 3);
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	mutex_unlock(&ts->lock);
	if (ret) {
		enable_irq(ts->spi->irq);
		return ret;
	}
	ret = pinctrl_select_state(ts->pinctrl, ts->suspend);
	if (ret) {
		mutex_lock(&ts->lock);
		ft3683_reset(ts);
		mutex_unlock(&ts->lock);
		enable_irq(ts->spi->irq);
	}
	return ret;
}

static int ft3683_resume(struct device *dev)
{
	struct ft3683 *ts = dev_get_drvdata(dev);
	int ret;

	ret = pinctrl_select_state(ts->pinctrl, ts->active);
	if (ret)
		return ret;
	mutex_lock(&ts->lock);
	ft3683_reset(ts);
	ret = ft3683_identify(ts);
	mutex_unlock(&ts->lock);
	if (!ret)
		enable_irq(ts->spi->irq);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ft3683_pm_ops, ft3683_suspend, ft3683_resume);

static const struct of_device_id ft3683_of_match[] = {
	{ .compatible = "xiaomi,touch-spi" },
	{ .compatible = "focaltech,ft3683" },
	{ }
};
MODULE_DEVICE_TABLE(of, ft3683_of_match);

static const struct spi_device_id ft3683_ids[] = {
	{ "ft3683" },
	{ "touch-spi" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ft3683_ids);

static struct spi_driver ft3683_driver = {
	.driver = {
		.name = "focaltech-ft3683-rothko",
		.of_match_table = ft3683_of_match,
		.pm = pm_sleep_ptr(&ft3683_pm_ops),
		.dev_groups = ft3683_groups,
	},
	.probe = ft3683_probe,
	.id_table = ft3683_ids,
};
module_spi_driver(ft3683_driver);

MODULE_DESCRIPTION("Redmi K70 Ultra Focaltech FT3683 SPI touchscreen");
MODULE_LICENSE("GPL");
