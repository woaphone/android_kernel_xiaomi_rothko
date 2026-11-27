// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Goodix "Berlin" Touchscreen IC driver
 * Copyright (C) 2020 - 2021 Goodix, Inc.
 * Copyright (C) 2023 Linaro Ltd.
 *
 * Based on goodix_ts_berlin driver.
 *
 * This driver is distinct from goodix.c since hardware interface
 * is different enough to require a new driver.
 * None of the register address or data structure are close enough
 * to the previous generations.
 *
 * Currently the driver only handles Multitouch events with already
 * programmed firmware and "config" for "Revision A/D" Berlin IC.
 *
 * Support is missing for:
 * - ESD Management
 * - Firmware update/flashing
 * - "Config" update/flashing
 * - Stylus Events
 * - Gesture Events
 * - Support for revision B
 */

#include <linux/bitfield.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/mutex.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "goodix_berlin.h"
#include "rothko/ft3683/xiaomi_touch_common.h"

#define GOODIX_BERLIN_MAX_TOUCH			10

#define GOODIX_BERLIN_NORMAL_RESET_DELAY_MS	100

#define GOODIX_BERLIN_TOUCH_EVENT		BIT(7)
#define GOODIX_BERLIN_REQUEST_EVENT		BIT(6)
#define GOODIX_BERLIN_GESTURE_EVENT		BIT(5)
#define GOODIX_BERLIN_FOD_STATUS		BIT(3)
#define GOODIX_BERLIN_TOUCH_COUNT_MASK		GENMASK(3, 0)
#define GOODIX_BERLIN_PALM_STATUS		BIT(4)

#define GOODIX_BERLIN_REQUEST_CODE_RESET	3

#define GOODIX_BERLIN_POINT_TYPE_MASK		GENMASK(3, 0)
#define GOODIX_BERLIN_POINT_TYPE_STYLUS_HOVER	1
#define GOODIX_BERLIN_POINT_TYPE_STYLUS		3

#define GOODIX_BERLIN_TOUCH_ID_MASK		GENMASK(7, 4)

#define GOODIX_BERLIN_DEV_CONFIRM_VAL		0xAA
#define GOODIX_BERLIN_BOOTOPTION_ADDR		0x10000

#define GOODIX_BERLIN_IC_INFO_MAX_LEN		SZ_1K

#define GOODIX_BERLIN_CHECKSUM_SIZE		sizeof(u16)
#define GOODIX_BERLIN_CMD_SIZE			16
#define GOODIX_BERLIN_CMD_ACK_OK		0x80
#define GOODIX_BERLIN_CMD_PALM			0x70
#define GOODIX_BERLIN_CMD_HTC_SCAN		0x90
#define GOODIX_BERLIN_CMD_REPORT_MODE		0x91
#define GOODIX_BERLIN_CMD_GESTURE		0xa6
#define GOODIX_BERLIN_CMD_REPORT_RATE		0xc1
#define GOODIX_BERLIN_REPORT_DIRECT		0x81

#define GOODIX_XIAOMI_CMD_SET_MODE		0
#define GOODIX_XIAOMI_MODE_FOD_ENABLE		10
#define GOODIX_XIAOMI_MODE_NONUI		17
#define GOODIX_XIAOMI_NONUI_NO_GESTURE		2
#define GOODIX_XIAOMI_MODE_REPORT_RATE		202
#define GOODIX_XIAOMI_FOD_KEY			0x152
#define GOODIX_BERLIN_GESTURE_FOD_DOWN		0x46
#define GOODIX_BERLIN_GESTURE_FOD_UP		0x55
#define GOODIX_BERLIN_CMD_GAME_ON		0x17
#define GOODIX_BERLIN_CMD_GAME_OFF		0x18

struct goodix_berlin_fw_version {
	u8 rom_pid[6];
	u8 rom_vid[3];
	u8 rom_vid_reserved;
	u8 patch_pid[8];
	u8 patch_vid[4];
	u8 patch_vid_reserved;
	u8 sensor_id;
	u8 reserved[2];
	__le16 checksum;
};

struct goodix_berlin_ic_info_version {
	u8 info_customer_id;
	u8 info_version_id;
	u8 ic_die_id;
	u8 ic_version_id;
	__le32 config_id;
	u8 config_version;
	u8 frame_data_customer_id;
	u8 frame_data_version_id;
	u8 touch_data_customer_id;
	u8 touch_data_version_id;
	u8 reserved[3];
} __packed;

struct goodix_berlin_ic_info_feature {
	__le16 freqhop_feature;
	__le16 calibration_feature;
	__le16 gesture_feature;
	__le16 side_touch_feature;
	__le16 stylus_feature;
} __packed;

struct goodix_berlin_ic_info_misc {
	__le32 cmd_addr;
	__le16 cmd_max_len;
	__le32 cmd_reply_addr;
	__le16 cmd_reply_len;
	__le32 fw_state_addr;
	__le16 fw_state_len;
	__le32 fw_buffer_addr;
	__le16 fw_buffer_max_len;
	__le32 frame_data_addr;
	__le16 frame_data_head_len;
	__le16 fw_attr_len;
	__le16 fw_log_len;
	u8 pack_max_num;
	u8 pack_compress_version;
	__le16 stylus_struct_len;
	__le16 mutual_struct_len;
	__le16 self_struct_len;
	__le16 noise_struct_len;
	__le32 touch_data_addr;
	__le16 touch_data_head_len;
	__le16 point_struct_len;
	__le16 reserved1;
	__le16 reserved2;
	__le32 mutual_rawdata_addr;
	__le32 mutual_diffdata_addr;
	__le32 mutual_refdata_addr;
	__le32 self_rawdata_addr;
	__le32 self_diffdata_addr;
	__le32 self_refdata_addr;
	__le32 iq_rawdata_addr;
	__le32 iq_refdata_addr;
	__le32 im_rawdata_addr;
	__le16 im_readata_len;
	__le32 noise_rawdata_addr;
	__le16 noise_rawdata_len;
	__le32 stylus_rawdata_addr;
	__le16 stylus_rawdata_len;
	__le32 noise_data_addr;
	__le32 esd_addr;
} __packed;

struct goodix_berlin_touch {
	u8 status;
	u8 reserved;
	__le16 x;
	__le16 y;
	__le16 w;
};
#define GOODIX_BERLIN_TOUCH_SIZE	sizeof(struct goodix_berlin_touch)

struct goodix_berlin_header {
	u8 status;
	u8 reserved1;
	u8 request_type;
	u8 reserved2[3];
	__le16 checksum;
};
#define GOODIX_BERLIN_HEADER_SIZE	sizeof(struct goodix_berlin_header)

struct goodix_berlin_event {
	struct goodix_berlin_header hdr;
	/* The data below is u16/__le16 aligned */
	u8 data[GOODIX_BERLIN_TOUCH_SIZE * GOODIX_BERLIN_MAX_TOUCH +
		GOODIX_BERLIN_CHECKSUM_SIZE];
};

/* Rothko's gesture mailbox has an independent 8-byte checksummed header. */
struct goodix_berlin_gesture {
	u8 status;
	u8 reserved1[3];
	u8 type;
	u8 reserved2;
	__le16 checksum;
	__le16 x;
	__le16 y;
	u8 area;
	u8 reserved3[4];
	u8 slot;
} __packed;

struct goodix_berlin_game {
	u8 enabled;
	u8 up;
	u8 tolerance;
	u8 aim;
	u8 tap;
	u8 edge;
	u8 orientation;
};

static const struct goodix_berlin_game goodix_rothko_game_default = {
	.up = 3,
	.tolerance = 2,
	.aim = 2,
	.tap = 2,
	.edge = 2,
};

struct goodix_berlin_core {
	struct device *dev;
	struct regmap *regmap;
	struct regulator *avdd;
	struct regulator *vddio;
	struct gpio_desc *reset_gpio;
	struct touchscreen_properties props;
	struct goodix_berlin_fw_version fw_version;
	struct input_dev *input_dev;
	int irq;
	u32 resolution_factor;
	struct mutex cmd_lock;
	bool system_suspended;
	bool powered_off;
	bool panel_blank;
	s32 nonui_mode;
	bool gesture_armed;
	bool gesture_irq_pending;
	bool gesture_slot_active;
	u8 gesture_slot;
	bool palm_enabled;
	bool fod_enabled;
	bool fod_pressed;
	bool report_rate_set;
	u8 report_rate;
	bool game_configured;
	u8 pending_expert;
	struct goodix_berlin_game game;
	u8 game_data[2];
	u8 palm;

	/* Runtime parameters extracted from IC_INFO buffer  */
	u32 cmd_addr;
	u32 touch_data_addr;

	const struct goodix_berlin_ic_data *ic_data;

	struct goodix_berlin_event event;
};

static bool goodix_berlin_checksum_valid(const u8 *data, int size)
{
	u32 cal_checksum = 0;
	u16 r_checksum;
	int i;

	if (size < GOODIX_BERLIN_CHECKSUM_SIZE)
		return false;

	for (i = 0; i < size - GOODIX_BERLIN_CHECKSUM_SIZE; i++)
		cal_checksum += data[i];

	r_checksum = get_unaligned_le16(&data[i]);

	return (u16)cal_checksum == r_checksum;
}

static bool goodix_berlin_is_dummy_data(struct goodix_berlin_core *cd,
					const u8 *data, int size)
{
	int i;

	/*
	 * If the device is missing or doesn't respond the buffer
	 * could be filled with bus default line state, 0x00 or 0xff,
	 * so declare success the first time we encounter neither.
	 */
	for (i = 0; i < size; i++)
		if (data[i] > 0 && data[i] < 0xff)
			return false;

	return true;
}

static int goodix_berlin_dev_confirm(struct goodix_berlin_core *cd)
{
	u8 tx_buf[8], rx_buf[8];
	int retry = 3;
	int error;

	memset(tx_buf, GOODIX_BERLIN_DEV_CONFIRM_VAL, sizeof(tx_buf));
	while (retry--) {
		error = regmap_raw_write(cd->regmap,
					 GOODIX_BERLIN_BOOTOPTION_ADDR,
					 tx_buf, sizeof(tx_buf));
		if (error)
			return error;

		error = regmap_raw_read(cd->regmap,
					GOODIX_BERLIN_BOOTOPTION_ADDR,
					rx_buf, sizeof(rx_buf));
		if (error)
			return error;

		if (!memcmp(tx_buf, rx_buf, sizeof(tx_buf)))
			return 0;

		usleep_range(5000, 5100);
	}

	dev_err(cd->dev, "device confirm failed, rx_buf: %*ph\n",
		(int)sizeof(rx_buf), rx_buf);

	return -EINVAL;
}

static int goodix_berlin_power_on(struct goodix_berlin_core *cd)
{
	int error;

	error = regulator_enable(cd->vddio);
	if (error) {
		dev_err(cd->dev, "Failed to enable vddio: %d\n", error);
		return error;
	}

	/* Vendor waits 3ms for VDDIO to settle */
	usleep_range(3000, 3100);

	error = regulator_enable(cd->avdd);
	if (error) {
		dev_err(cd->dev, "Failed to enable avdd: %d\n", error);
		goto err_vddio_disable;
	}

	/* Vendor waits 15ms for AVDD to settle */
	usleep_range(15000, 15100);

	gpiod_set_value_cansleep(cd->reset_gpio, 0);

	/* Vendor waits 4ms for Firmware to initialize */
	if (device_is_compatible(cd->dev, "xiaomi,touch-spi"))
		msleep(GOODIX_BERLIN_NORMAL_RESET_DELAY_MS);
	else
		usleep_range(4000, 4100);

	error = goodix_berlin_dev_confirm(cd);
	if (error)
		goto err_dev_reset;

	/* Vendor waits 100ms for Firmware to fully boot */
	if (!device_is_compatible(cd->dev, "xiaomi,touch-spi"))
		msleep(GOODIX_BERLIN_NORMAL_RESET_DELAY_MS);

	return 0;

err_dev_reset:
	gpiod_set_value_cansleep(cd->reset_gpio, 1);
	regulator_disable(cd->avdd);
err_vddio_disable:
	regulator_disable(cd->vddio);
	return error;
}

static void goodix_berlin_power_off(struct goodix_berlin_core *cd)
{
	gpiod_set_value_cansleep(cd->reset_gpio, 1);
	regulator_disable(cd->avdd);
	regulator_disable(cd->vddio);
}

static int goodix_berlin_read_version(struct goodix_berlin_core *cd)
{
	int error;

	error = regmap_raw_read(cd->regmap, cd->ic_data->fw_version_info_addr,
				&cd->fw_version, sizeof(cd->fw_version));
	if (error) {
		dev_err(cd->dev, "error reading fw version, %d\n", error);
		return error;
	}

	if (!goodix_berlin_checksum_valid((u8 *)&cd->fw_version,
					  sizeof(cd->fw_version))) {
		dev_err(cd->dev, "invalid fw version: checksum error\n");
		return -EINVAL;
	}

	return 0;
}

/* Only extract necessary data for runtime */
static int goodix_berlin_parse_ic_info(struct goodix_berlin_core *cd,
				       const u8 *data, u16 length)
{
	struct goodix_berlin_ic_info_misc *misc;
	unsigned int offset = 0;

	offset += sizeof(__le16); /* length */
	offset += sizeof(struct goodix_berlin_ic_info_version);
	offset += sizeof(struct goodix_berlin_ic_info_feature);

	/* IC_INFO Parameters, variable width structure */
	offset += 4 * sizeof(u8); /* drv_num, sen_num, button_num, force_num */
	if (offset >= length)
		goto invalid_offset;

#define ADVANCE_LE16_PARAMS()				\
	do {						\
		u8 param_num = data[offset++];		\
		offset += param_num * sizeof(__le16);	\
		if (offset >= length)			\
			goto invalid_offset;		\
	} while (0)
	ADVANCE_LE16_PARAMS(); /* active_scan_rate_num */
	ADVANCE_LE16_PARAMS(); /* mutual_freq_num*/
	ADVANCE_LE16_PARAMS(); /* self_tx_freq_num */
	ADVANCE_LE16_PARAMS(); /* self_rx_freq_num */
	ADVANCE_LE16_PARAMS(); /* stylus_freq_num */
#undef ADVANCE_LE16_PARAMS

	misc = (struct goodix_berlin_ic_info_misc *)&data[offset];
	cd->cmd_addr = le32_to_cpu(misc->cmd_addr);
	cd->touch_data_addr = le32_to_cpu(misc->touch_data_addr);

	return 0;

invalid_offset:
	dev_err(cd->dev, "ic_info length is invalid (offset %d length %d)\n",
		offset, length);
	return -EINVAL;
}

static int goodix_berlin_get_ic_info(struct goodix_berlin_core *cd)
{
	u8 *afe_data __free(kfree) = NULL;
	__le16 length_raw;
	u16 length;
	int error;

	afe_data = kzalloc(GOODIX_BERLIN_IC_INFO_MAX_LEN, GFP_KERNEL);
	if (!afe_data)
		return -ENOMEM;

	error = regmap_raw_read(cd->regmap, cd->ic_data->ic_info_addr,
				&length_raw, sizeof(length_raw));
	if (error) {
		dev_err(cd->dev, "failed get ic info length, %d\n", error);
		return error;
	}

	length = le16_to_cpu(length_raw);
	if (length >= GOODIX_BERLIN_IC_INFO_MAX_LEN) {
		dev_err(cd->dev, "invalid ic info length %d\n", length);
		return -EINVAL;
	}

	error = regmap_raw_read(cd->regmap, cd->ic_data->ic_info_addr, afe_data,
				length);
	if (error) {
		dev_err(cd->dev, "failed get ic info data, %d\n", error);
		return error;
	}

	/* check whether the data is valid (ex. bus default values) */
	if (goodix_berlin_is_dummy_data(cd, afe_data, length)) {
		dev_err(cd->dev, "fw info data invalid\n");
		return -EINVAL;
	}

	if (!goodix_berlin_checksum_valid(afe_data, length)) {
		dev_err(cd->dev, "fw info checksum error\n");
		return -EINVAL;
	}

	error = goodix_berlin_parse_ic_info(cd, afe_data, length);
	if (error)
		return error;

	/* check some key info */
	if (!cd->touch_data_addr) {
		dev_err(cd->dev, "touch_data_addr is null\n");
		return -EINVAL;
	}

	return 0;
}

static int goodix_berlin_get_remaining_contacts(struct goodix_berlin_core *cd,
						int n)
{
	size_t offset = 2 * GOODIX_BERLIN_TOUCH_SIZE +
				GOODIX_BERLIN_CHECKSUM_SIZE;
	u32 addr = cd->touch_data_addr + GOODIX_BERLIN_HEADER_SIZE + offset;
	int error;

	error = regmap_raw_read(cd->regmap, addr,
				&cd->event.data[offset],
				(n - 2) * GOODIX_BERLIN_TOUCH_SIZE);
	if (error) {
		dev_err_ratelimited(cd->dev, "failed to get touch data, %d\n",
				    error);
		return error;
	}

	return 0;
}

static int goodix_berlin_send_command(struct goodix_berlin_core *cd,
				     u8 command, u8 len, u8 arg, u8 arg2)
{
	u8 cmd[GOODIX_BERLIN_CMD_SIZE] = { 0, 0, len, command, arg, arg2 };
	u8 reply[GOODIX_BERLIN_CMD_SIZE];
	int error, attempt, poll;

	/* Length, opcode and argument are covered by the 16-bit byte sum. */
	put_unaligned_le16(len + command + arg + arg2, cmd + len);

	for (attempt = 0; attempt < 6; attempt++) {
		error = regmap_raw_write(cd->regmap, cd->cmd_addr, cmd, sizeof(cmd));
		if (error)
			return error;

		for (poll = 0; poll < 6; poll++) {
			error = regmap_raw_read(cd->regmap, cd->cmd_addr,
						reply, sizeof(reply));
			if (error)
				return error;

			switch (reply[1]) {
			case GOODIX_BERLIN_CMD_ACK_OK:
				usleep_range(4000, 4100);
				return 0;
			case 0:
			case 2: /* Busy */
				usleep_range(1000, 1100);
				continue;
			case 3: /* Command buffer overflow */
				usleep_range(10000, 11000);
				break;
			}
			usleep_range(1000, 1100);
			break;
		}
	}

	dev_err(cd->dev, "report command %#x not acknowledged: %#x\n",
		command, reply[1]);
	return -ETIMEDOUT;
}

static int goodix_berlin_send_report_command(struct goodix_berlin_core *cd,
					     u8 command, u8 arg)
{
	u8 len = (command == GOODIX_BERLIN_CMD_PALM ||
		  command == GOODIX_BERLIN_CMD_REPORT_RATE) ? 6 : 5;

	return goodix_berlin_send_command(cd, command, len, arg, 0);
}

static int goodix_berlin_send_game(struct goodix_berlin_core *cd,
				  bool enable, const u8 data[2])
{
	return goodix_berlin_send_command(cd, enable ? GOODIX_BERLIN_CMD_GAME_ON :
					  GOODIX_BERLIN_CMD_GAME_OFF,
					  6, data[0], data[1]);
}

static int goodix_berlin_restore_reporting(struct goodix_berlin_core *cd)
{
	int error;

	if (!device_is_compatible(cd->dev, "xiaomi,touch-spi"))
		return 0;

	/* The board firmware defaults to HTC raw frames; evdev needs contacts. */
	if (!cd->cmd_addr)
		return dev_err_probe(cd->dev, -EINVAL, "missing command mailbox\n");

	error = goodix_berlin_send_report_command(cd, GOODIX_BERLIN_CMD_HTC_SCAN, 0);
	if (error)
		return error;

	error = goodix_berlin_send_report_command(cd, GOODIX_BERLIN_CMD_REPORT_MODE,
						  GOODIX_BERLIN_REPORT_DIRECT);
	if (error)
		return error;

	if (cd->report_rate_set) {
		error = goodix_berlin_send_report_command(cd,
							 GOODIX_BERLIN_CMD_REPORT_RATE,
							 cd->report_rate);
		if (error)
			dev_warn(cd->dev, "failed to restore report rate: %d\n", error);
	}

	if (cd->game_configured) {
		error = goodix_berlin_send_game(cd, cd->game.enabled, cd->game_data);
		if (error)
			dev_warn(cd->dev, "failed to restore game settings: %d\n", error);
	}

	return 0;
}

static bool goodix_berlin_report_state(struct goodix_berlin_core *cd, int n)
{
	struct goodix_berlin_touch *touch_data =
			(struct goodix_berlin_touch *)cd->event.data;
	struct goodix_berlin_touch *t;
	bool has_contact = false;
	int i;
	u8 type, id;

	for (i = 0; i < n; i++) {
		t = &touch_data[i];

		type = FIELD_GET(GOODIX_BERLIN_POINT_TYPE_MASK, t->status);
		if (type == GOODIX_BERLIN_POINT_TYPE_STYLUS ||
		    type == GOODIX_BERLIN_POINT_TYPE_STYLUS_HOVER) {
			dev_warn_once(cd->dev, "Stylus event type not handled\n");
			continue;
		}

		id = FIELD_GET(GOODIX_BERLIN_TOUCH_ID_MASK, t->status);
		if (id >= GOODIX_BERLIN_MAX_TOUCH) {
			dev_warn_ratelimited(cd->dev, "invalid finger id %d\n", id);
			continue;
		}

		input_mt_slot(cd->input_dev, id);
		input_mt_report_slot_state(cd->input_dev, MT_TOOL_FINGER, true);

		touchscreen_report_pos(cd->input_dev, &cd->props,
				       __le16_to_cpu(t->x) * cd->resolution_factor,
				       __le16_to_cpu(t->y) * cd->resolution_factor,
				       true);
		input_report_abs(cd->input_dev, ABS_MT_TOUCH_MAJOR,
				 __le16_to_cpu(t->w));
		has_contact = true;
	}

	input_mt_sync_frame(cd->input_dev);
	return has_contact;
}

/* Serialize physical FOD edges, mode changes and power-down with cmd_lock. */
static void goodix_berlin_set_fod_pressed(struct goodix_berlin_core *cd, bool pressed)
{
	if (cd->fod_pressed == pressed)
		return;

	cd->fod_pressed = pressed;
	input_report_key(cd->input_dev, GOODIX_XIAOMI_FOD_KEY, pressed);
	input_sync(cd->input_dev);
	if (IS_REACHABLE(CONFIG_XIAOMI_TOUCH_COMMON_ROTHKO))
		xiaomi_touch_report_fod(cd->dev, pressed);
}

static bool goodix_berlin_wants_fod_gesture(struct goodix_berlin_core *cd)
{
	/* Stock maps Touch_Fod_Enable to Berlin bit 2, except in NonUI 2. */
	return cd->panel_blank && cd->fod_enabled &&
	       cd->nonui_mode != GOODIX_XIAOMI_NONUI_NO_GESTURE;
}

static int goodix_berlin_update_gesture(struct goodix_berlin_core *cd);
static int goodix_berlin_panel_blank(void *data, bool blank);

static int goodix_berlin_read_palm(void *data, u8 *palm)
{
	struct goodix_berlin_core *cd = data;

	*palm = READ_ONCE(cd->palm);
	return 0;
}

static int goodix_berlin_write_palm(void *data, bool enable)
{
	struct goodix_berlin_core *cd = data;
	int error;

	mutex_lock(&cd->cmd_lock);
	if (cd->system_suspended || cd->powered_off || cd->gesture_armed) {
		error = -EBUSY;
	} else {
		error = goodix_berlin_send_report_command(cd, GOODIX_BERLIN_CMD_PALM,
							 enable);
		if (!error) {
			cd->palm_enabled = enable;
			if (!enable)
				WRITE_ONCE(cd->palm, 0);
		}
	}
	mutex_unlock(&cd->cmd_lock);
	return error;
}

static int goodix_berlin_set_game(struct goodix_berlin_core *cd, u16 mode, s32 value)
{
	static const u8 orientation[] = { 0, 0x40, 0, 0x80 };
	static const u8 expert[][4] = { { 2, 3, 2, 2 }, { 4, 3, 3, 2 }, { 3, 3, 4, 2 } };
	static const u8 max_value[] = { 1, 0, 4, 4, 4, 4, 3, 3, 3 };
	struct goodix_berlin_game next = cd->game_configured ? cd->game : goodix_rothko_game_default;
	struct goodix_berlin_game effective;
	u8 data[2];
	int ret;

	if (mode >= ARRAY_SIZE(max_value) || mode == 1)
		return -EOPNOTSUPP;
	if (value < 0 || value > max_value[mode] || (mode == 6 && !value))
		return -EINVAL;

	switch (mode) {
	case 0:
		next.enabled = value;
		break;
	case 2:
		next.up = value;
		break;
	case 3:
		next.tolerance = value;
		break;
	case 4:
		next.aim = value;
		break;
	case 5:
		next.tap = value;
		break;
	case 6:
		cd->pending_expert = value;
		return 0;
	case 7:
		next.edge = value;
		break;
	case 8:
		next.orientation = value;
		break;
	}

	effective = next;
	if (cd->pending_expert) {
		const u8 *preset = expert[cd->pending_expert - 1];

		effective.tolerance = preset[0];
		effective.up = preset[1];
		effective.aim = preset[2];
		effective.tap = preset[3];
	}
	data[0] = effective.up | (effective.tolerance << 3) |
		  orientation[effective.orientation];
	data[1] = effective.tap | (effective.aim << 3) | (effective.edge << 6);
	ret = goodix_berlin_send_game(cd, next.enabled, data);
	if (ret)
		return ret;

	cd->game = next;
	cd->game_data[0] = data[0];
	cd->game_data[1] = data[1];
	cd->game_configured = true;
	cd->pending_expert = 0;
	return 0;
}

static int goodix_berlin_xiaomi_request(void *data,
				      struct xiaomi_touch_request *request)
{
	struct goodix_berlin_core *cd = data;
	s32 value = request->values[0];
	u8 rate;
	int error;

	if (request->command != GOODIX_XIAOMI_CMD_SET_MODE)
		return -EOPNOTSUPP;
	if (request->mode != GOODIX_XIAOMI_MODE_REPORT_RATE && request->length != 1)
		return -EINVAL;
	if (value < 0)
		return -EINVAL;

	rate = !!value;
	mutex_lock(&cd->cmd_lock);
	if (request->mode == GOODIX_XIAOMI_MODE_FOD_ENABLE) {
		if (value > 3) {
			error = -EINVAL;
		} else {
			/* Stock accepts mode 3 for real FOD packets after resume. */
			cd->fod_enabled = value != 0;
			if (!cd->fod_enabled && !cd->gesture_slot_active)
				goodix_berlin_set_fod_pressed(cd, false);
			error = goodix_berlin_update_gesture(cd);
		}
	} else if (request->mode == GOODIX_XIAOMI_MODE_NONUI) {
		cd->nonui_mode = value;
		error = goodix_berlin_update_gesture(cd);
	} else if (cd->system_suspended || cd->powered_off || cd->gesture_armed) {
		error = -EBUSY;
	} else if (request->mode != GOODIX_XIAOMI_MODE_REPORT_RATE) {
		error = goodix_berlin_set_game(cd, request->mode, value);
	} else {
		error = goodix_berlin_send_report_command(cd,
							 GOODIX_BERLIN_CMD_REPORT_RATE,
							 rate);
		if (!error) {
			cd->report_rate = rate;
			cd->report_rate_set = true;
		}
	}
	mutex_unlock(&cd->cmd_lock);
	return error;
}

static const struct xiaomi_touch_ops goodix_berlin_xiaomi_ops = {
	.read_palm = goodix_berlin_read_palm,
	.write_palm = goodix_berlin_write_palm,
	.request = goodix_berlin_xiaomi_request,
	.panel_blank = goodix_berlin_panel_blank,
	.reports_fod = true,
};

static bool goodix_berlin_report_palm(struct goodix_berlin_core *cd)
{
	u8 palm = !!(cd->event.hdr.request_type & GOODIX_BERLIN_PALM_STATUS);

	if (!cd->palm_enabled || cd->palm == palm)
		return false;

	WRITE_ONCE(cd->palm, palm);
	return true;
}

static void goodix_berlin_restore_palm(struct goodix_berlin_core *cd)
{
	int error;

	if (!cd->palm_enabled)
		return;
	error = goodix_berlin_send_report_command(cd, GOODIX_BERLIN_CMD_PALM, 1);
	if (error)
		dev_warn(cd->dev, "failed to restore palm reporting: %d\n", error);
}

static bool goodix_berlin_touch_handler(struct goodix_berlin_core *cd)
{
	bool pressed, has_contact;
	u8 touch_num;
	int error;

	touch_num = FIELD_GET(GOODIX_BERLIN_TOUCH_COUNT_MASK,
			      cd->event.hdr.request_type);
	if (touch_num > GOODIX_BERLIN_MAX_TOUCH) {
		dev_warn(cd->dev, "invalid touch num %d\n", touch_num);
		return false;
	}

	if (touch_num > 2) {
		/* read additional contact data if more than 2 touch events */
		error = goodix_berlin_get_remaining_contacts(cd, touch_num);
		if (error)
			return false;
	}

	if (touch_num) {
		int len = touch_num * GOODIX_BERLIN_TOUCH_SIZE +
			  GOODIX_BERLIN_CHECKSUM_SIZE;
		if (!goodix_berlin_checksum_valid(cd->event.data, len)) {
			dev_err(cd->dev, "touch data checksum error: %*ph\n",
				len, cd->event.data);
			return false;
		}
	}

	if (!device_is_compatible(cd->dev, "xiaomi,touch-spi")) {
		goodix_berlin_report_state(cd, touch_num);
		input_sync(cd->input_dev);
		return false;
	}

	has_contact = goodix_berlin_report_state(cd, touch_num);
	pressed = cd->fod_enabled && has_contact &&
		  (cd->event.hdr.status & GOODIX_BERLIN_FOD_STATUS);
	if (pressed != cd->fod_pressed)
		goodix_berlin_set_fod_pressed(cd, pressed);
	else
		input_sync(cd->input_dev);
	return goodix_berlin_report_palm(cd);
}

static int goodix_berlin_request_handle_reset(struct goodix_berlin_core *cd)
{
	int error;

	goodix_berlin_set_fod_pressed(cd, false);
	gpiod_set_value_cansleep(cd->reset_gpio, 1);
	usleep_range(2000, 2100);
	gpiod_set_value_cansleep(cd->reset_gpio, 0);

	msleep(GOODIX_BERLIN_NORMAL_RESET_DELAY_MS);

	error = goodix_berlin_restore_reporting(cd);
	if (!error)
		goodix_berlin_restore_palm(cd);
	return error;
}

static void goodix_berlin_release_gesture_slot(struct goodix_berlin_core *cd)
{
	struct input_dev *input = cd->input_dev;

	if (!cd->gesture_slot_active)
		return;

	cd->gesture_slot_active = false;
	cd->fod_pressed = false;
	input_report_key(input, GOODIX_XIAOMI_FOD_KEY, 0);
	input_report_abs(input, ABS_MT_WIDTH_MAJOR, 0);
	input_report_abs(input, ABS_MT_WIDTH_MINOR, 0);
	input_sync(input);
	input_mt_slot(input, cd->gesture_slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	input_report_key(input, BTN_TOUCH, 0);
	input_report_key(input, BTN_TOOL_FINGER, 0);
	input_sync(input);
	if (IS_REACHABLE(CONFIG_XIAOMI_TOUCH_COMMON_ROTHKO))
		xiaomi_touch_report_fod(cd->dev, false);
}

static int goodix_berlin_arm_fod(struct goodix_berlin_core *cd)
{
	/* Stock brl_gesture(gesture_type = BIT(2)), with other gestures off. */
	return goodix_berlin_send_command(cd, GOODIX_BERLIN_CMD_GESTURE,
					  6, 0xff, 0xdf);
}

static int goodix_berlin_update_gesture(struct goodix_berlin_core *cd)
{
	bool arm = goodix_berlin_wants_fod_gesture(cd);
	int error;

	if (cd->system_suspended || cd->powered_off || arm == cd->gesture_armed)
		return 0;

	if (arm) {
		error = enable_irq_wake(cd->irq);
		if (error)
			return error;

		error = goodix_berlin_arm_fod(cd);
		if (error) {
			disable_irq_wake(cd->irq);
			/* The IC may have accepted the command without an ACK. */
			goodix_berlin_request_handle_reset(cd);
			return error;
		}
		input_mt_sync_frame(cd->input_dev);
		if (cd->fod_pressed)
			goodix_berlin_set_fod_pressed(cd, false);
		else
			input_sync(cd->input_dev);
		WRITE_ONCE(cd->palm, 0);
		cd->gesture_armed = true;
		return 0;
	}
	/* Like stock, allow a real up to finish an outstanding blank press. */
	if (cd->panel_blank && cd->gesture_slot_active)
		return 0;

	goodix_berlin_release_gesture_slot(cd);
	disable_irq_wake(cd->irq);
	cd->gesture_armed = false;
	return goodix_berlin_request_handle_reset(cd);
}

static int goodix_berlin_panel_blank(void *data, bool blank)
{
	struct goodix_berlin_core *cd = data;
	int error;

	mutex_lock(&cd->cmd_lock);
	cd->panel_blank = blank;
	error = goodix_berlin_update_gesture(cd);
	mutex_unlock(&cd->cmd_lock);
	return error;
}

static void goodix_berlin_gesture_irq(struct goodix_berlin_core *cd)
{
	struct input_dev *input = cd->input_dev;
	struct goodix_berlin_gesture event;
	bool valid_header;
	u32 x, y;
	int error;

	error = regmap_raw_read(cd->regmap, cd->touch_data_addr, &event, sizeof(event));
	if (error)
		goto rearm;

	valid_header = goodix_berlin_checksum_valid((const u8 *)&event,
						   GOODIX_BERLIN_HEADER_SIZE);
	/* Stock also accepts status 0/type 0x55 with a bad sum, only for up. */
	if (event.type == GOODIX_BERLIN_GESTURE_FOD_UP &&
	    (valid_header ? event.status & GOODIX_BERLIN_GESTURE_EVENT : !event.status)) {
		/* End only the reported contact, never an arbitrary firmware slot. */
		if (cd->gesture_slot_active) {
			goodix_berlin_release_gesture_slot(cd);
			error = goodix_berlin_update_gesture(cd);
			if (error)
				dev_warn_ratelimited(cd->dev, "failed to leave FOD gesture: %d\n", error);
		}
		return;
	}
	if (!valid_header || !(event.status & GOODIX_BERLIN_GESTURE_EVENT) ||
	    event.type != GOODIX_BERLIN_GESTURE_FOD_DOWN)
		goto rearm;
	if (!goodix_berlin_wants_fod_gesture(cd) || cd->gesture_slot_active)
		return;

	x = le16_to_cpu(event.x) * cd->resolution_factor;
	y = le16_to_cpu(event.y) * cd->resolution_factor;
	if (event.slot >= GOODIX_BERLIN_MAX_TOUCH ||
	    x > cd->props.max_x || y > cd->props.max_y)
		goto rearm;

	/* Stock sends key/SYN, then the MT contact from the same packet. */
	cd->gesture_slot_active = true;
	cd->gesture_slot = event.slot;
	cd->fod_pressed = true;
	input_report_key(input, GOODIX_XIAOMI_FOD_KEY, 1);
	input_sync(input);
	input_mt_slot(input, event.slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
	input_report_key(input, BTN_TOUCH, 1);
	input_report_key(input, BTN_TOOL_FINGER, 1);
	touchscreen_report_pos(input, &cd->props, x, y, true);
	input_report_abs(input, ABS_MT_WIDTH_MAJOR, event.area);
	input_report_abs(input, ABS_MT_WIDTH_MINOR, event.area);
	input_sync(input);
	if (IS_REACHABLE(CONFIG_XIAOMI_TOUCH_COMMON_ROTHKO))
		xiaomi_touch_report_fod(cd->dev, true);
	return;

rearm:
	/* OEM Rothko direct-report mode rearms on rejected events, not valid edges. */
	if (goodix_berlin_wants_fod_gesture(cd)) {
		error = goodix_berlin_arm_fod(cd);
		if (error)
			dev_warn_ratelimited(cd->dev, "failed to rearm FOD gesture: %d\n", error);
	}
}

static irqreturn_t goodix_berlin_irq(int irq, void *data)
{
	struct goodix_berlin_core *cd = data;
	bool palm_changed = false;
	int error;

	mutex_lock(&cd->cmd_lock);
	if (cd->powered_off)
		goto out;
	if (cd->system_suspended) {
		/* System PM can deliver a wake IRQ before SPI is available. */
		if (cd->gesture_armed) {
			cd->gesture_irq_pending = true;
			pm_wakeup_event(cd->dev, 0);
		}
		goto out;
	}
	if (cd->gesture_armed) {
		pm_stay_awake(cd->dev);
		goodix_berlin_gesture_irq(cd);
		pm_relax(cd->dev);
		goto out;
	}

	/*
	 * First, read buffer with space for 2 touch events:
	 * - GOODIX_BERLIN_HEADER_SIZE = 8 bytes
	 * - GOODIX_BERLIN_TOUCH_SIZE * 2 = 16 bytes
	 * - GOODIX_BERLIN_CHECKLSUM_SIZE = 2 bytes
	 * For a total of 26 bytes.
	 *
	 * If only a single finger is reported, we will read 8 bytes more than
	 * needed:
	 * - bytes 0-7:   Header (GOODIX_BERLIN_HEADER_SIZE)
	 * - bytes 8-15:  Finger 0 Data
	 * - bytes 24-25: Checksum
	 * - bytes 18-25: Unused 8 bytes
	 *
	 * If 2 fingers are reported, we would have read the exact needed
	 * amount of data and checksum would be at the end of the buffer:
	 * - bytes 0-7:   Header (GOODIX_BERLIN_HEADER_SIZE)
	 * - bytes 8-15:  Finger 0 Bytes 0-7
	 * - bytes 16-23: Finger 1 Bytes 0-7
	 * - bytes 24-25: Checksum
	 *
	 * If more than 2 fingers were reported, the "Checksum" bytes would
	 * in fact contain part of the next finger data, and then
	 * goodix_berlin_get_remaining_contacts() would complete the buffer
	 * with the missing bytes, including the trailing checksum.
	 * For example, if 3 fingers are reported, then we would do:
	 * Read 1:
	 * - bytes 0-7:   Header (GOODIX_BERLIN_HEADER_SIZE)
	 * - bytes 8-15:  Finger 0 Bytes 0-7
	 * - bytes 16-23: Finger 1 Bytes 0-7
	 * - bytes 24-25: Finger 2 Bytes 0-1
	 * Read 2 (with length of (3 - 2) * 8 = 8 bytes):
	 * - bytes 26-31: Finger 2 Bytes 2-7
	 * - bytes 32-33: Checksum
	 */
	error = regmap_raw_read(cd->regmap, cd->touch_data_addr,
				&cd->event,
				GOODIX_BERLIN_HEADER_SIZE +
					2 * GOODIX_BERLIN_TOUCH_SIZE +
					GOODIX_BERLIN_CHECKSUM_SIZE);
	if (error) {
		dev_warn_ratelimited(cd->dev,
				     "failed get event head data: %d\n", error);
		goto out;
	}

	if (cd->event.hdr.status == 0)
		goto out;

	if (!goodix_berlin_checksum_valid((u8 *)&cd->event.hdr,
					  GOODIX_BERLIN_HEADER_SIZE)) {
		dev_warn_ratelimited(cd->dev,
				     "touch head checksum error: %*ph\n",
				     (int)GOODIX_BERLIN_HEADER_SIZE,
				     &cd->event.hdr);
		goto out_clear;
	}

	if (cd->event.hdr.status & GOODIX_BERLIN_TOUCH_EVENT)
		palm_changed = goodix_berlin_touch_handler(cd);

	if (cd->event.hdr.status & GOODIX_BERLIN_REQUEST_EVENT) {
		switch (cd->event.hdr.request_type) {
		case GOODIX_BERLIN_REQUEST_CODE_RESET:
			if (cd->reset_gpio) {
				error = goodix_berlin_request_handle_reset(cd);
				if (error)
					dev_warn_ratelimited(cd->dev,
							     "failed to restore touch reporting: %d\n",
							     error);
			}
			break;

		default:
			dev_warn(cd->dev, "unsupported request code 0x%x\n",
				 cd->event.hdr.request_type);
		}
	}


out_clear:
	/* Clear up status field */
	regmap_write(cd->regmap, cd->touch_data_addr, 0);

out:
	mutex_unlock(&cd->cmd_lock);
	if (palm_changed && IS_REACHABLE(CONFIG_XIAOMI_TOUCH_COMMON_ROTHKO))
		xiaomi_touch_notify_palm(cd->dev);
	return IRQ_HANDLED;
}

static int goodix_berlin_input_dev_config(struct goodix_berlin_core *cd,
					  const struct input_id *id)
{
	struct input_dev *input_dev;
	int error;

	input_dev = devm_input_allocate_device(cd->dev);
	if (!input_dev)
		return -ENOMEM;

	cd->input_dev = input_dev;
	input_set_drvdata(input_dev, cd);

	input_dev->name = "Goodix Berlin Capacitive TouchScreen";
	input_dev->phys = "input/ts";

	input_dev->id = *id;

	input_set_abs_params(cd->input_dev, ABS_MT_POSITION_X,
			     0, SZ_64K - 1, 0, 0);
	input_set_abs_params(cd->input_dev, ABS_MT_POSITION_Y,
			     0, SZ_64K - 1, 0, 0);
	input_set_abs_params(cd->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	touchscreen_parse_properties(cd->input_dev, true, &cd->props);
	if (device_is_compatible(cd->dev, "xiaomi,touch-spi")) {
		u32 max_x, max_y;

		/* The vendor FOD monitor looks up the stock evdev name. */
		input_dev->name = "goodix_ts";
		input_set_capability(input_dev, EV_KEY, GOODIX_XIAOMI_FOD_KEY);
		input_set_capability(input_dev, EV_KEY, BTN_TOOL_FINGER);
		input_set_abs_params(input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
		input_set_abs_params(input_dev, ABS_MT_WIDTH_MINOR, 0, 255, 0, 0);
		error = device_property_read_u32(cd->dev, "goodix,panel-max-x", &max_x);
		if (error)
			return error;
		error = device_property_read_u32(cd->dev, "goodix,panel-max-y", &max_y);
		if (error)
			return error;
		input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, max_x, 0, 0);
		input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, max_y, 0, 0);
		cd->props.max_x = max_x;
		cd->props.max_y = max_y;
	}

	error = input_mt_init_slots(cd->input_dev, GOODIX_BERLIN_MAX_TOUCH,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	error = input_register_device(cd->input_dev);
	if (error)
		return error;

	return 0;
}

static int goodix_berlin_suspend(struct device *dev)
{
	struct goodix_berlin_core *cd = dev_get_drvdata(dev);

	mutex_lock(&cd->cmd_lock);
	cd->system_suspended = true;
	if (!cd->gesture_armed && !cd->powered_off) {
		/* The threaded handler takes cmd_lock and checks powered_off. */
		disable_irq_nosync(cd->irq);
		WRITE_ONCE(cd->palm, 0);
		goodix_berlin_set_fod_pressed(cd, false);
		goodix_berlin_power_off(cd);
		cd->powered_off = true;
	}
	mutex_unlock(&cd->cmd_lock);

	return 0;
}

static int goodix_berlin_resume(struct device *dev)
{
	struct goodix_berlin_core *cd = dev_get_drvdata(dev);
	int error = 0;

	mutex_lock(&cd->cmd_lock);
	cd->system_suspended = false;
	if (cd->powered_off) {
		error = goodix_berlin_power_on(cd);
		if (error)
			goto out_unlock;

		error = goodix_berlin_restore_reporting(cd);
		if (error) {
			goodix_berlin_power_off(cd);
			goto out_unlock;
		}
		goodix_berlin_restore_palm(cd);
		cd->powered_off = false;
		enable_irq(cd->irq);
	}
	if (cd->gesture_irq_pending) {
		cd->gesture_irq_pending = false;
		if (cd->gesture_armed && cd->panel_blank)
			goodix_berlin_gesture_irq(cd);
	}
	error = goodix_berlin_update_gesture(cd);
out_unlock:
	mutex_unlock(&cd->cmd_lock);
	return error;
}

EXPORT_GPL_SIMPLE_DEV_PM_OPS(goodix_berlin_pm_ops,
			     goodix_berlin_suspend, goodix_berlin_resume);

static void goodix_berlin_power_off_act(void *data)
{
	struct goodix_berlin_core *cd = data;

	if (!cd->powered_off)
		goodix_berlin_power_off(cd);
}

static void goodix_berlin_release_wake(void *data)
{
	struct goodix_berlin_core *cd = data;

	mutex_lock(&cd->cmd_lock);
	if (cd->gesture_armed) {
		disable_irq_wake(cd->irq);
		cd->gesture_armed = false;
	}
	mutex_unlock(&cd->cmd_lock);
}

static ssize_t registers_read(struct file *filp, struct kobject *kobj,
			      const struct bin_attribute *bin_attr,
			      char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct goodix_berlin_core *cd = dev_get_drvdata(dev);
	int error;

	error = regmap_raw_read(cd->regmap, off, buf, count);

	return error ? error : count;
}

static ssize_t registers_write(struct file *filp, struct kobject *kobj,
			       const struct bin_attribute *bin_attr,
			       char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct goodix_berlin_core *cd = dev_get_drvdata(dev);
	int error;

	error = regmap_raw_write(cd->regmap, off, buf, count);

	return error ? error : count;
}

static const BIN_ATTR_ADMIN_RW(registers, 0);

static const struct bin_attribute *const goodix_berlin_bin_attrs[] = {
	&bin_attr_registers,
	NULL,
};

static const struct attribute_group goodix_berlin_attr_group = {
	.bin_attrs = goodix_berlin_bin_attrs,
};

const struct attribute_group *goodix_berlin_groups[] = {
	&goodix_berlin_attr_group,
	NULL,
};
EXPORT_SYMBOL_GPL(goodix_berlin_groups);

int goodix_berlin_probe(struct device *dev, int irq, const struct input_id *id,
			struct regmap *regmap,
			const struct goodix_berlin_ic_data *ic_data)
{
	struct goodix_berlin_core *cd;
	u32 irq_flags = 0;
	int error;

	if (irq <= 0) {
		dev_err(dev, "Missing interrupt number\n");
		return -EINVAL;
	}

	cd = devm_kzalloc(dev, sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	cd->dev = dev;
	cd->regmap = regmap;
	cd->irq = irq;
	cd->ic_data = ic_data;
	cd->resolution_factor = 1;
	mutex_init(&cd->cmd_lock);

	if (device_is_compatible(dev, "xiaomi,touch-spi")) {
		struct pinctrl *pinctrl;
		struct pinctrl_state *spi_state;

		pinctrl = devm_pinctrl_get_select(dev, "pmx_ts_active");
		if (IS_ERR(pinctrl))
			return PTR_ERR(pinctrl);
		spi_state = pinctrl_lookup_state(pinctrl, "pmx_gt_spi_mode");
		if (IS_ERR(spi_state))
			return PTR_ERR(spi_state);
		error = pinctrl_select_state(pinctrl, spi_state);
		if (error)
			return error;
		error = device_property_read_u32(dev, "goodix,super-resolution-factor",
						 &cd->resolution_factor);
		if (error)
			return error;
		error = device_property_read_u32(dev, "goodix,irq-flags", &irq_flags);
		if (error)
			return error;
		cd->reset_gpio = devm_gpiod_get(dev, "goodix,reset", GPIOD_OUT_LOW);
	} else {
		cd->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	}
	if (IS_ERR(cd->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(cd->reset_gpio),
				     "Failed to request reset gpio\n");
	if (device_is_compatible(dev, "xiaomi,touch-spi"))
		gpiod_toggle_active_low(cd->reset_gpio);

	cd->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(cd->avdd))
		return dev_err_probe(dev, PTR_ERR(cd->avdd),
				     "Failed to request avdd regulator\n");

	cd->vddio = devm_regulator_get(dev,
		device_is_compatible(dev, "xiaomi,touch-spi") ? "iovdd" : "vddio");
	if (IS_ERR(cd->vddio))
		return dev_err_probe(dev, PTR_ERR(cd->vddio),
				     "Failed to request vddio regulator\n");

	error = goodix_berlin_power_on(cd);
	if (error) {
		dev_err(dev, "failed power on");
		return error;
	}

	error = devm_add_action_or_reset(dev, goodix_berlin_power_off_act, cd);
	if (error)
		return error;

	error = goodix_berlin_read_version(cd);
	if (error) {
		dev_err(dev, "failed to get version info");
		return error;
	}

	error = goodix_berlin_get_ic_info(cd);
	if (error) {
		dev_err(dev, "invalid ic info, abort");
		return error;
	}

	error = goodix_berlin_restore_reporting(cd);
	if (error)
		return error;
	if (device_is_compatible(dev, "xiaomi,touch-spi"))
		dev_info(dev, "direct touch reporting acknowledged\n");

	error = goodix_berlin_input_dev_config(cd, id);
	if (error) {
		dev_err(dev, "failed set input device");
		return error;
	}

	error = devm_request_threaded_irq(dev, cd->irq, NULL, goodix_berlin_irq,
					  IRQF_ONESHOT | irq_flags, "goodix-berlin", cd);
	if (error) {
		dev_err(dev, "request threaded irq failed: %d\n", error);
		return error;
	}

	dev_set_drvdata(dev, cd);

	if (IS_REACHABLE(CONFIG_XIAOMI_TOUCH_COMMON_ROTHKO) &&
	    device_is_compatible(dev, "xiaomi,touch-spi")) {
		error = devm_device_init_wakeup(dev);
		if (error)
			return error;
		error = devm_add_action_or_reset(dev, goodix_berlin_release_wake, cd);
		if (error)
			return error;
		error = devm_xiaomi_touch_register(dev, &goodix_berlin_xiaomi_ops,
						  cd, cd->resolution_factor);
		if (error)
			dev_warn(dev, "failed to register Xiaomi touch features: %d\n",
				 error);
	}

	dev_dbg(dev, "Goodix Berlin %s Touchscreen Controller",
		cd->fw_version.patch_pid);

	return 0;
}
EXPORT_SYMBOL_GPL(goodix_berlin_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Goodix Berlin Core Touchscreen driver");
MODULE_AUTHOR("Neil Armstrong <neil.armstrong@linaro.org>");
