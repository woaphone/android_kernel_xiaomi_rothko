// SPDX-License-Identifier: GPL-2.0
/*
 * Southchip SC858x dual charge pump driver for Redmi K70 Ultra (rothko)
 * Register and fault flag mapping from the stock Android 15 driver.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/rothko_hwid.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "charger_class.h"
#include "sc858x_charger.h"

#define SC858X_DEVICE_ID		0x81
#define SC858X_REG_MAX			0x7f

#define SC858X_REG_VOUT_OVP		0x00
#define SC858X_REG_VBAT_OVP		0x01
#define SC858X_REG_IBAT_OCP		0x02
#define SC858X_REG_VUSB_OVP		0x03
#define SC858X_REG_VWPC_OVP		0x04
#define SC858X_REG_VBUS_OVP		0x05
#define SC858X_REG_IBUS_OCP		0x06
#define SC858X_REG_IBUS_UCP		0x07
#define SC858X_REG_PMID2OUT_OVP		0x08
#define SC858X_REG_PMID2OUT_UVP		0x09
#define SC858X_REG_SWITCH_STAT		0x0a
#define SC858X_REG_CHG_CTRL		0x0b
#define SC858X_REG_FSW_CTRL		0x0c
#define SC858X_REG_WD_SS_CTRL		0x0d
#define SC858X_REG_MODE_CTRL		0x0e
#define SC858X_REG_PROT_DIS		0x0f
#define SC858X_REG_STAT_10		0x10
#define SC858X_REG_INT_FLAG_11		0x11
#define SC858X_REG_INT_FLAG_13		0x13
#define SC858X_REG_INT_MASK_14		0x14
#define SC858X_REG_ADC_CTRL		0x15
#define SC858X_REG_ADC_FN_DIS		0x16
#define SC858X_REG_IBUS_ADC		0x17
#define SC858X_REG_VBUS_ADC		0x19
#define SC858X_REG_VUSB_ADC		0x1b
#define SC858X_REG_VWPC_ADC		0x1d
#define SC858X_REG_VOUT_ADC		0x1f
#define SC858X_REG_VBAT_ADC		0x21
#define SC858X_REG_IBAT_ADC		0x23
#define SC858X_REG_TDIE_ADC		0x27
#define SC858X_REG_CTRL_40		0x40
#define SC858X_REG_DEVICE_ID		0x6e
#define SC858X_REG_CTRL_7C		0x7c

enum sc858x_fields {
	F_REG_00_ALL = 0,
	F_BAT_OVP_DIS,
	F_BAT_OVP_FLAG,
	F_BAT_OVP_TH,
	F_BAT_OCP_DIS,
	F_BAT_OCP_FLAG,
	F_BAT_OCP_TH,
	F_USB_OVP_FLAG,
	F_USB_OVP_TH,
	F_WPC_OVP_FLAG,
	F_WPC_OVP_TH,
	F_BUS_OVP_TH,
	F_OUT_OVP_TH,
	F_BUS_OCP_DIS,
	F_BUS_OCP_FLAG,
	F_BUS_OCP_TH,
	F_BUS_UCP_DIS,
	F_BUS_UCP_FALL_DG,
	F_PMID2OUT_OVP_DIS,
	F_PMID2OUT_OVP_FLAG,
	F_PMID2OUT_OVP_TH,
	F_PMID2OUT_UVP_DIS,
	F_PMID2OUT_UVP_FLAG,
	F_PMID2OUT_UVP_TH,
	F_SWITCHING_STAT,
	F_STAT_10_3,
	F_STAT_10_2,
	F_STAT_10_1,
	F_STAT_10_0,
	F_CHG_EN,
	F_REG_RST,
	F_ACDRV_MANUAL,
	F_WPCGATE_EN,
	F_OVPGATE_EN,
	F_EN_0B_2,
	F_EN_0B_1,
	F_EN_0B_0,
	F_FSW_SET,
	F_ACDRV_HI_EN,
	F_SS_0D_7,
	F_SS_TIMEOUT,
	F_WD_TIMEOUT,
	F_MS_MASTER,
	F_MS_SLAVE,
	F_SENSE_R_MOHM,
	F_MODE_0E_3,
	F_OP_MODE,
	F_TSBAT_FLT_DIS,
	F_TSHUT_DIS,
	F_WPC_OVP_DIS,
	F_USB_OVP_DIS,
	F_BUS_OVP_DIS,
	F_OUT_OVP_DIS,
	F_ADC_EN,
	F_ADC_RATE,
	F_CTRL_40_7,
	F_CTRL_40_2,
	F_CTRL_40_1,
	F_CTRL_40_0,
	F_DEVICE_ID,
	F_MAX_FIELDS
};

static const struct reg_field sc858x_reg_fields[F_MAX_FIELDS] = {
	[F_REG_00_ALL]		= REG_FIELD(0x00, 0, 7),
	[F_BAT_OVP_DIS]		= REG_FIELD(0x01, 7, 7),
	[F_BAT_OVP_FLAG]	= REG_FIELD(0x01, 5, 5),
	[F_BAT_OVP_TH]		= REG_FIELD(0x01, 0, 4),
	[F_BAT_OCP_DIS]		= REG_FIELD(0x02, 7, 7),
	[F_BAT_OCP_FLAG]	= REG_FIELD(0x02, 4, 4),
	[F_BAT_OCP_TH]		= REG_FIELD(0x02, 0, 3),
	[F_USB_OVP_FLAG]	= REG_FIELD(0x03, 5, 5),
	[F_USB_OVP_TH]		= REG_FIELD(0x03, 0, 3),
	[F_WPC_OVP_FLAG]	= REG_FIELD(0x04, 5, 5),
	[F_WPC_OVP_TH]		= REG_FIELD(0x04, 0, 3),
	[F_BUS_OVP_TH]		= REG_FIELD(0x05, 2, 7),
	[F_OUT_OVP_TH]		= REG_FIELD(0x05, 0, 1),
	[F_BUS_OCP_DIS]		= REG_FIELD(0x06, 7, 7),
	[F_BUS_OCP_FLAG]	= REG_FIELD(0x06, 5, 5),
	[F_BUS_OCP_TH]		= REG_FIELD(0x06, 0, 4),
	[F_BUS_UCP_DIS]		= REG_FIELD(0x07, 7, 7),
	[F_BUS_UCP_FALL_DG]	= REG_FIELD(0x07, 4, 5),
	[F_PMID2OUT_OVP_DIS]	= REG_FIELD(0x08, 7, 7),
	[F_PMID2OUT_OVP_FLAG]	= REG_FIELD(0x08, 3, 3),
	[F_PMID2OUT_OVP_TH]	= REG_FIELD(0x08, 0, 2),
	[F_PMID2OUT_UVP_DIS]	= REG_FIELD(0x09, 7, 7),
	[F_PMID2OUT_UVP_FLAG]	= REG_FIELD(0x09, 3, 3),
	[F_PMID2OUT_UVP_TH]	= REG_FIELD(0x09, 0, 2),
	[F_SWITCHING_STAT]	= REG_FIELD(0x0a, 1, 1),
	[F_STAT_10_3]		= REG_FIELD(0x10, 3, 3),
	[F_STAT_10_2]		= REG_FIELD(0x10, 2, 2),
	[F_STAT_10_1]		= REG_FIELD(0x10, 1, 1),
	[F_STAT_10_0]		= REG_FIELD(0x10, 0, 0),
	[F_CHG_EN]		= REG_FIELD(0x0b, 7, 7),
	[F_REG_RST]		= REG_FIELD(0x0b, 6, 6),
	[F_ACDRV_MANUAL]	= REG_FIELD(0x0b, 5, 5),
	[F_WPCGATE_EN]		= REG_FIELD(0x0b, 4, 4),
	[F_OVPGATE_EN]		= REG_FIELD(0x0b, 3, 3),
	[F_EN_0B_2]		= REG_FIELD(0x0b, 2, 2),
	[F_EN_0B_1]		= REG_FIELD(0x0b, 1, 1),
	[F_EN_0B_0]		= REG_FIELD(0x0b, 0, 0),
	[F_FSW_SET]		= REG_FIELD(0x0c, 3, 7),
	[F_ACDRV_HI_EN]		= REG_FIELD(0x0c, 0, 0),
	[F_SS_0D_7]		= REG_FIELD(0x0d, 7, 7),
	[F_SS_TIMEOUT]		= REG_FIELD(0x0d, 3, 6),
	[F_WD_TIMEOUT]		= REG_FIELD(0x0d, 0, 2),
	[F_MS_MASTER]		= REG_FIELD(0x0e, 7, 7),
	[F_MS_SLAVE]		= REG_FIELD(0x0e, 6, 6),
	[F_SENSE_R_MOHM]	= REG_FIELD(0x0e, 4, 4),
	[F_MODE_0E_3]		= REG_FIELD(0x0e, 3, 3),
	[F_OP_MODE]		= REG_FIELD(0x0e, 0, 2),
	[F_TSBAT_FLT_DIS]	= REG_FIELD(0x0f, 5, 5),
	[F_TSHUT_DIS]		= REG_FIELD(0x0f, 4, 4),
	[F_WPC_OVP_DIS]		= REG_FIELD(0x0f, 3, 3),
	[F_USB_OVP_DIS]		= REG_FIELD(0x0f, 2, 2),
	[F_BUS_OVP_DIS]		= REG_FIELD(0x0f, 1, 1),
	[F_OUT_OVP_DIS]		= REG_FIELD(0x0f, 0, 0),
	[F_ADC_EN]		= REG_FIELD(0x15, 7, 7),
	[F_ADC_RATE]		= REG_FIELD(0x15, 6, 6),
	[F_CTRL_40_7]		= REG_FIELD(0x40, 7, 7),
	[F_CTRL_40_2]		= REG_FIELD(0x40, 2, 2),
	[F_CTRL_40_1]		= REG_FIELD(0x40, 1, 1),
	[F_CTRL_40_0]		= REG_FIELD(0x40, 0, 0),
	[F_DEVICE_ID]		= REG_FIELD(0x6e, 0, 7),
};

static const u8 sc858x_dump_regs[] = {
	0x05, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x14, 0x15, 0x16, 0x6e,
};

struct sc858x_cfg {
	u32 bat_ovp_threshold;
	u32 bat_ocp_threshold;
	u32 wpc_ovp_threshold;
	u32 out_ovp_threshold;
	u32 bus_ucp_fall_dg;
	u32 pmid2out_ovp_threshold;
	u32 pmid2out_uvp_threshold;
	u32 fsw_set;
	u32 acdrv_hi_en;
	u32 ss_timeout;
	u32 mode;
	u32 sense_r_mohm;
	bool disable_tsbat;
};

struct sc858x_chip {
	struct device *dev;
	struct regmap *regmap;
	struct regmap_field *fields[F_MAX_FIELDS];
	struct mutex lock;
	struct charger_device *chgdev;
	struct power_supply *psy;
	struct gpio_desc *irq_gpiod;
	int irq;
	int work_mode;
	int op_mode;
	int product_code;
	bool chip_ok;
	bool configured;
	u32 fault_type;
	char log_tag[24];
	struct sc858x_cfg cfg;
};

struct sc858x_intr_bit {
	u8 mask;
	s8 fault_bit;
	int notify_evt;
	const char *name;
};

struct sc858x_intr_reg {
	u8 reg;
	u8 count;
	struct sc858x_intr_bit bits[8];
};

static const struct sc858x_intr_reg sc858x_intr_tbl[] = {
	{
		.reg = 0x01, .count = 1,
		.bits = {
			{ 0x20, 7, CHARGER_DEV_NOTIFY_BAT_OVP, "vbat ovp flag" },
		},
	},
	{
		.reg = 0x02, .count = 1,
		.bits = {
			{ 0x10, 6, CHARGER_DEV_NOTIFY_IBATOCP, "ibat ocp flag" },
		},
	},
	{
		.reg = 0x03, .count = 1,
		.bits = {
			{ 0x20, 5, CHARGER_DEV_NOTIFY_VBUS_OVP, "vusb ovp flag" },
		},
	},
	{
		.reg = 0x04, .count = 1,
		.bits = {
			{ 0x20, 5, CHARGER_DEV_NOTIFY_VBUS_OVP, "vwpc ovp flag" },
		},
	},
	{
		.reg = 0x06, .count = 1,
		.bits = {
			{ 0x20, 4, CHARGER_DEV_NOTIFY_IBUSOCP, "ibus ocp flag" },
		},
	},
	{
		.reg = 0x07, .count = 2,
		.bits = {
			{ 0x04, -1, -1, "ibus ucp rising flag" },
			{ 0x01, SC858X_FAULT_IBUS_UCP_FALL_BIT, CHARGER_DEV_NOTIFY_IBUSUCP_FALL,
			  "ibus ucp falling flag" },
		},
	},
	{
		.reg = 0x08, .count = 1,
		.bits = {
			{ 0x08, 5, CHARGER_DEV_NOTIFY_VBUS_OVP, "pmid2out ovp flag" },
		},
	},
	{
		.reg = 0x09, .count = 1,
		.bits = {
			{ 0x08, 5, -1, "pmid2out uvp flag" },
		},
	},
	{
		.reg = 0x0a, .count = 2,
		.bits = {
			{ 0x80, -1, -1, "por flag" },
			{ 0x01, 0, -1, "pin diag fail flag" },
		},
	},
	{
		.reg = 0x11, .count = 7,
		.bits = {
			{ 0x40, -1, -1, "vout ok sw range flag" },
			{ 0x20, -1, -1, "vout ok rev flag" },
			{ 0x10, -1, -1, "vout ok chg flag" },
			{ 0x08, -1, -1, "vout insert flag" },
			{ 0x04, -1, -1, "vbus present flag" },
			{ 0x02, -1, -1, "vwpc insert flag" },
			{ 0x01, -1, -1, "vusb insert flag" },
		},
	},
	{
		.reg = 0x13, .count = 8,
		.bits = {
			{ 0x80, SC858X_FAULT_TSBAT_BIT, -1, "tsbat flt flag" },
			{ 0x40, 0, -1, "tshut flag" },
			{ 0x20, 0, -1, "ss timeout flag" },
			{ 0x10, 0, -1, "wd timeout flag" },
			{ 0x08, 0, -1, "conv ocp flag" },
			{ 0x04, 0, -1, "ss fail flag" },
			{ 0x02, 5, CHARGER_DEV_NOTIFY_VBUS_OVP, "vbus ovp flag" },
			{ 0x01, 5, CHARGER_DEV_NOTIFY_VOUTOVP, "vout ovp flag" },
		},
	},
};

static const struct regmap_config sc858x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SC858X_REG_MAX,
	.cache_type = REGCACHE_NONE,
};

static int sc858x_field_read(struct sc858x_chip *chip, enum sc858x_fields fid,
			     unsigned int *val)
{
	int ret = regmap_field_read(chip->fields[fid], val);

	if (ret < 0)
		dev_err(chip->dev, "%s: read field %d failed: %d\n",
			chip->log_tag, fid, ret);
	return ret;
}

static int sc858x_field_write(struct sc858x_chip *chip, enum sc858x_fields fid,
			      unsigned int val)
{
	int ret = regmap_field_write(chip->fields[fid], val);

	if (ret < 0)
		dev_err(chip->dev, "%s: write field %d val %u failed: %d\n",
			chip->log_tag, fid, val, ret);
	return ret;
}

static int sc858x_read_adc_word(struct sc858x_chip *chip, u8 reg, u16 *raw)
{
	u8 buf[2];
	int ret;

	ret = regmap_bulk_read(chip->regmap, reg, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	*raw = ((u16)buf[0] << 8) | buf[1];
	return 0;
}

static int sc858x_read_adc_cooked(struct sc858x_chip *chip, u8 reg, int *val)
{
	unsigned int adc_en = 0;
	u16 raw = 0;
	int ret;

	ret = sc858x_field_read(chip, F_ADC_EN, &adc_en);
	if (ret < 0)
		return ret;
	if (!adc_en)
		return -ENODATA;

	ret = sc858x_read_adc_word(chip, reg, &raw);
	if (ret < 0)
		return ret;

	switch (reg) {
	case SC858X_REG_VBUS_ADC:
	case SC858X_REG_VUSB_ADC:
	case SC858X_REG_VWPC_ADC:
		*val = (int)raw * 625 / 100;
		return 0;
	case SC858X_REG_VOUT_ADC:
	case SC858X_REG_VBAT_ADC:
		*val = (int)raw * 125 / 100;
		return 0;
	case SC858X_REG_IBUS_ADC:
		*val = (int)raw * 15625 / 10000;
		return 0;
	case SC858X_REG_IBAT_ADC:
		*val = (int)raw * 375 / 100;
		return 0;
	case SC858X_REG_TDIE_ADC:
		if (!raw)
			return -ENODATA;
		*val = raw >> 1;
		return 0;
	default:
		return -EINVAL;
	}
}

static int sc858x_set_busovp_th(struct sc858x_chip *chip, int mode, int mv)
{
	unsigned int val;

	switch (mode) {
	case SC858X_MODE_4_1:
		if (mv < 14000)
			mv = 14000;
		val = (mv - 14000) / 200;
		if (val > 0x3f)
			val = 0x3f;
		break;
	case SC858X_MODE_2_1:
		mv = clamp(mv, 7000, 13300);
		val = (mv - 7000) / 100;
		break;
	default:
		val = 40;
		break;
	}

	return sc858x_field_write(chip, F_BUS_OVP_TH, val);
}

static void sc858x_dump_registers_locked(struct sc858x_chip *chip)
{
	char line[256];
	int pos = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(sc858x_dump_regs); i++) {
		unsigned int val = 0;
		int ret = regmap_read(chip->regmap, sc858x_dump_regs[i], &val);

		if (ret < 0)
			continue;
		pos += scnprintf(line + pos, sizeof(line) - pos,
				 "[0x%02X]=0x%02X,", sc858x_dump_regs[i], val);
	}
	dev_info(chip->dev, "%s: %s\n", chip->log_tag, line);
}

static int sc858x_stop_locked(struct sc858x_chip *chip)
{
	unsigned int status;
	int ret;

	ret = sc858x_field_write(chip, F_CHG_EN, 0);
	if (ret)
		return ret;

	ret = sc858x_field_read(chip, F_CHG_EN, &status);
	if (ret)
		return ret;
	if (status)
		return -EIO;

	return regmap_read_poll_timeout(chip->regmap, SC858X_REG_SWITCH_STAT,
					status, !(status & BIT(1)), 1000, 20000);
}

static int sc858x_init_device(struct sc858x_chip *chip, int mode)
{
	struct sc858x_cfg *cfg = &chip->cfg;
	const struct {
		enum sc858x_fields field;
		u32 val;
	} init_tbl[] = {
		{ F_BAT_OVP_TH, cfg->bat_ovp_threshold },
		{ F_BAT_OCP_TH, cfg->bat_ocp_threshold },
		{ F_WPC_OVP_TH, cfg->wpc_ovp_threshold },
		{ F_OUT_OVP_TH, cfg->out_ovp_threshold },
		{ F_BUS_UCP_FALL_DG, cfg->bus_ucp_fall_dg },
		{ F_PMID2OUT_OVP_TH, cfg->pmid2out_ovp_threshold },
		{ F_PMID2OUT_UVP_TH, cfg->pmid2out_uvp_threshold },
		{ F_FSW_SET, cfg->fsw_set },
		{ F_ACDRV_HI_EN, cfg->acdrv_hi_en },
		{ F_SS_TIMEOUT, cfg->ss_timeout },
		{ F_SENSE_R_MOHM, cfg->sense_r_mohm },
	};
	unsigned int bus_ovp_mv, bus_ocp, usb_ovp;
	int i, ret;

	switch (mode) {
	case SC858X_MODE_4_1:
		bus_ovp_mv = 22000;
		bus_ocp = 15;
		usb_ovp = 11;
		break;
	case SC858X_MODE_2_1:
		bus_ovp_mv = 11000;
		bus_ocp = chip->product_code >= 2 && chip->product_code <= 4 ? 31 : 20;
		usb_ovp = 3;
		break;
	case SC858X_MODE_1_1:
		bus_ovp_mv = 6000;
		bus_ocp = 24;
		usb_ovp = 15;
		break;
	default:
		return -EINVAL;
	}

	chip->configured = false;
	ret = sc858x_stop_locked(chip);
	if (ret)
		return ret;
	ret = sc858x_field_write(chip, F_ADC_EN, 0);
	if (ret)
		return ret;

	/* Keep the existing protection enable and watchdog settings. */
	for (i = 0; i < ARRAY_SIZE(init_tbl); i++) {
		ret = sc858x_field_write(chip, init_tbl[i].field, init_tbl[i].val);
		if (ret < 0)
			return ret;
	}

	ret = sc858x_set_busovp_th(chip, mode, bus_ovp_mv);
	if (ret)
		return ret;
	ret = sc858x_field_write(chip, F_BUS_OCP_TH, bus_ocp);
	if (ret)
		return ret;
	ret = sc858x_field_write(chip, F_USB_OVP_TH, usb_ovp);
	if (ret)
		return ret;
	if (mode == SC858X_MODE_4_1 && chip->product_code == 4) {
		ret = sc858x_field_write(chip, F_WPC_OVP_TH, 14);
		if (ret)
			return ret;
	}
	ret = sc858x_field_write(chip, F_OP_MODE, mode);
	if (ret)
		return ret;
	ret = sc858x_field_write(chip, F_ACDRV_MANUAL, 0);
	if (ret)
		return ret;
	ret = sc858x_field_write(chip, F_OVPGATE_EN, 1);
	if (ret)
		return ret;

	chip->op_mode = mode;
	chip->configured = true;

	return 0;
}

static irqreturn_t sc858x_irq_handler(int irq, void *data)
{
	struct sc858x_chip *chip = data;
	struct charger_device *chgdev;
	unsigned long events = 0;
	unsigned long evt;
	unsigned int val;
	int i, j, ret;
	u32 faults = 0;

	mutex_lock(&chip->lock);
	for (i = 0; i < ARRAY_SIZE(sc858x_intr_tbl); i++) {
		const struct sc858x_intr_reg *entry = &sc858x_intr_tbl[i];

		ret = regmap_read(chip->regmap, entry->reg, &val);
		if (ret) {
			faults |= SC858X_FAULT_MISC_MASK;
			continue;
		}

		for (j = 0; j < entry->count; j++) {
			const struct sc858x_intr_bit *bit = &entry->bits[j];

			if (!(val & bit->mask) || bit->fault_bit < 0)
				continue;
			faults |= BIT(bit->fault_bit);
			if (bit->notify_evt >= 0)
				events |= BIT(bit->notify_evt);
			dev_warn_ratelimited(chip->dev, "%s: %s (reg 0x%02x=0x%02x)\n",
					     chip->log_tag, bit->name, entry->reg, val);
		}
	}
	chip->fault_type |= faults;
	chgdev = chip->chgdev;
	if (faults) {
		ret = sc858x_stop_locked(chip);
		if (ret)
			dev_err_ratelimited(chip->dev, "fault stop failed: %d\n", ret);
	}
	mutex_unlock(&chip->lock);

	if (chgdev)
		for_each_set_bit(evt, &events, BITS_PER_LONG)
			charger_dev_notify(chgdev, evt);
	return IRQ_HANDLED;
}

static int sc858x_ops_enable(struct charger_device *chgdev, bool enable)
{
	struct sc858x_chip *chip = charger_get_data(chgdev);
	int ret;

	mutex_lock(&chip->lock);
	if (!enable)
		ret = sc858x_stop_locked(chip);
	else if (!chip->configured)
		ret = -EAGAIN;
	else if (chip->fault_type)
		ret = -EIO;
	else
		ret = sc858x_field_write(chip, F_CHG_EN, 1);
	mutex_unlock(&chip->lock);
	return ret;
}

static int sc858x_ops_is_enabled(struct charger_device *chgdev, bool *enabled)
{
	struct sc858x_chip *chip = charger_get_data(chgdev);
	unsigned int val = 0;
	int ret;

	mutex_lock(&chip->lock);
	ret = sc858x_field_read(chip, F_SWITCHING_STAT, &val);
	if (ret == 0)
		*enabled = !!val;
	mutex_unlock(&chip->lock);
	return ret;
}

static int sc858x_ops_get_adc(struct charger_device *chgdev,
			      enum adc_channel chan, int *min, int *max)
{
	struct sc858x_chip *chip = charger_get_data(chgdev);
	u8 reg;
	int val = 0;
	int ret;

	switch (chan) {
	case ADC_CHANNEL_VBUS:
		reg = SC858X_REG_VBUS_ADC;
		break;
	case ADC_CHANNEL_IBUS:
		reg = SC858X_REG_IBUS_ADC;
		break;
	case ADC_CHANNEL_VBAT:
		reg = SC858X_REG_VBAT_ADC;
		break;
	case ADC_CHANNEL_VOUT:
		reg = SC858X_REG_VOUT_ADC;
		break;
	case ADC_CHANNEL_IBAT:
		reg = SC858X_REG_IBAT_ADC;
		break;
	case ADC_CHANNEL_TEMP_JC:
		reg = SC858X_REG_TDIE_ADC;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, reg, &val);
	mutex_unlock(&chip->lock);
	if (ret < 0)
		return ret;

	if (chan == ADC_CHANNEL_TEMP_JC)
		*min = *max = val;
	else
		*min = *max = val * 1000;
	return 0;
}

static int sc858x_ops_get_vbus(struct charger_device *chgdev, u32 *vbus)
{
	int min_v = 0, max_v = 0;
	int ret = sc858x_ops_get_adc(chgdev, ADC_CHANNEL_VBUS, &min_v, &max_v);

	if (ret == 0)
		*vbus = min_v;
	return ret;
}

static int sc858x_ops_get_ibus(struct charger_device *chgdev, u32 *ibus)
{
	int min_i = 0, max_i = 0;
	int ret = sc858x_ops_get_adc(chgdev, ADC_CHANNEL_IBUS, &min_i, &max_i);

	if (ret == 0)
		*ibus = min_i;
	return ret;
}

static int sc858x_ops_get_ibat(struct charger_device *chgdev, u32 *ibat)
{
	int min_i = 0, max_i = 0;
	int ret = sc858x_ops_get_adc(chgdev, ADC_CHANNEL_IBAT, &min_i, &max_i);

	if (ret == 0)
		*ibat = min_i;
	return ret;
}

static int sc858x_ops_get_tdie(struct charger_device *chgdev, int *min, int *max)
{
	return sc858x_ops_get_adc(chgdev, ADC_CHANNEL_TEMP_JC, min, max);
}

static int sc858x_ops_dump_registers(struct charger_device *chgdev)
{
	struct sc858x_chip *chip = charger_get_data(chgdev);

	mutex_lock(&chip->lock);
	sc858x_dump_registers_locked(chip);
	mutex_unlock(&chip->lock);
	return 0;
}

static int sc858x_ops_init_chip(struct charger_device *chgdev)
{
	struct sc858x_chip *chip = charger_get_data(chgdev);
	int ret;

	mutex_lock(&chip->lock);
	ret = sc858x_init_device(chip, chip->cfg.mode);
	mutex_unlock(&chip->lock);
	return ret;
}

static int sc858x_ops_set_vbusovp(struct charger_device *chgdev, u32 uv)
{
	struct sc858x_chip *chip = charger_get_data(chgdev);
	int ret;

	mutex_lock(&chip->lock);
	if (chip->configured)
		ret = sc858x_set_busovp_th(chip, chip->op_mode, uv / 1000);
	else
		ret = -EAGAIN;
	mutex_unlock(&chip->lock);
	return ret;
}

static int sc858x_ops_set_operation_mode(struct charger_device *chgdev, bool div2)
{
	return sc858x_cp_set_mode(chgdev, div2 ? SC858X_MODE_2_1 : SC858X_MODE_4_1);
}

static const struct charger_ops sc858x_charger_ops = {
	.enable = sc858x_ops_enable,
	.is_enabled = sc858x_ops_is_enabled,
	.get_adc = sc858x_ops_get_adc,
	.get_vbus_adc = sc858x_ops_get_vbus,
	.get_ibus_adc = sc858x_ops_get_ibus,
	.get_ibat_adc = sc858x_ops_get_ibat,
	.get_tchg_adc = sc858x_ops_get_tdie,
	.dump_registers = sc858x_ops_dump_registers,
	.init_chip = sc858x_ops_init_chip,
	.set_vbusovp = sc858x_ops_set_vbusovp,
	.set_operation_mode = sc858x_ops_set_operation_mode,
};

int sc858x_cp_set_mode(struct charger_device *chgdev, int mode)
{
	struct sc858x_chip *chip;
	int ret;

	if (!chgdev)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_init_device(chip, mode);
	mutex_unlock(&chip->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_set_mode);

/* The board PPS policy verifies the battery gauge before preparing each pump. */
int sc858x_cp_prepare_pps(struct charger_device *chgdev, bool after_detach)
{
	struct sc858x_chip *chip = charger_get_data(chgdev);
	unsigned int disabled;
	int ret;

	mutex_lock(&chip->lock);
	ret = sc858x_init_device(chip, SC858X_MODE_4_1);
	if (ret)
		goto out;
	if (after_detach)
		chip->fault_type &= ~BIT(SC858X_FAULT_IBUS_UCP_FALL_BIT);
	ret = regmap_write(chip->regmap, SC858X_REG_CTRL_7C, 0x01);
	if (ret) {
		dev_err(chip->dev, "PPS register 0x7c write failed: %d\n", ret);
		chip->configured = false;
		goto out;
	}
	if (!chip->cfg.disable_tsbat)
		goto out;

	ret = sc858x_field_write(chip, F_TSBAT_FLT_DIS, 1);
	if (!ret)
		ret = sc858x_field_read(chip, F_TSBAT_FLT_DIS, &disabled);
	if (!ret && !disabled)
		ret = -EIO;
	if (ret)
		chip->configured = false;
	else
		chip->fault_type &= ~BIT(SC858X_FAULT_TSBAT_BIT);
out:
	mutex_unlock(&chip->lock);
	return ret;
}

int sc858x_cp_enable_adc(struct charger_device *chgdev, bool enable)
{
	struct sc858x_chip *chip;
	int ret;

	if (!chgdev)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_field_write(chip, F_ADC_EN, enable ? 1 : 0);
	mutex_unlock(&chip->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_enable_adc);

int sc858x_cp_get_adc_enabled(struct charger_device *chgdev, bool *enabled)
{
	struct sc858x_chip *chip;
	unsigned int val = 0;
	int ret;

	if (!chgdev || !enabled)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_field_read(chip, F_ADC_EN, &val);
	if (ret == 0)
		*enabled = !!val;
	mutex_unlock(&chip->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_get_adc_enabled);

int sc858x_cp_get_vbat(struct charger_device *chgdev, u32 *vbat_mv)
{
	struct sc858x_chip *chip;
	int val = 0;
	int ret;

	if (!chgdev || !vbat_mv)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, SC858X_REG_VBAT_ADC, &val);
	mutex_unlock(&chip->lock);
	if (ret == 0)
		*vbat_mv = val;
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_get_vbat);

int sc858x_cp_get_vbus_mv(struct charger_device *chgdev, u32 *vbus_mv)
{
	struct sc858x_chip *chip;
	int val = 0;
	int ret;

	if (!chgdev || !vbus_mv)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, SC858X_REG_VBUS_ADC, &val);
	mutex_unlock(&chip->lock);
	if (ret == 0)
		*vbus_mv = val;
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_get_vbus_mv);

int sc858x_cp_get_ibus_ma(struct charger_device *chgdev, u32 *ibus_ma)
{
	struct sc858x_chip *chip;
	int val = 0;
	int ret;

	if (!chgdev || !ibus_ma)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, SC858X_REG_IBUS_ADC, &val);
	mutex_unlock(&chip->lock);
	if (ret == 0)
		*ibus_ma = val;
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_get_ibus_ma);

int sc858x_cp_get_ibat_ma(struct charger_device *chgdev, u32 *ibat_ma)
{
	struct sc858x_chip *chip;
	int val = 0;
	int ret;

	if (!chgdev || !ibat_ma)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, SC858X_REG_IBAT_ADC, &val);
	mutex_unlock(&chip->lock);
	if (ret == 0)
		*ibat_ma = val;
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_get_ibat_ma);

int sc858x_cp_get_tdie_c(struct charger_device *chgdev, int *tdie_c)
{
	struct sc858x_chip *chip;
	int val = 0;
	int ret;

	if (!chgdev || !tdie_c)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, SC858X_REG_TDIE_ADC, &val);
	mutex_unlock(&chip->lock);
	if (ret == 0)
		*tdie_c = val;
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_get_tdie_c);

int sc858x_cp_get_fault_type(struct charger_device *chgdev, u32 *fault)
{
	struct sc858x_chip *chip;

	if (!chgdev || !fault)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	*fault = chip->fault_type;
	mutex_unlock(&chip->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(sc858x_cp_get_fault_type);

int sc858x_cp_clear_fault_type(struct charger_device *chgdev)
{
	struct sc858x_chip *chip;
	int ret;

	if (!chgdev)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_stop_locked(chip);
	if (!ret)
		chip->fault_type = 0;
	mutex_unlock(&chip->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_clear_fault_type);

int sc858x_cp_get_chip_ok(struct charger_device *chgdev, bool *ok)
{
	struct sc858x_chip *chip;

	if (!chgdev || !ok)
		return -EINVAL;
	if (chgdev->ops != &sc858x_charger_ops)
		return -ENODEV;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	*ok = chip->chip_ok;
	mutex_unlock(&chip->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(sc858x_cp_get_chip_ok);

int sc858x_cp_set_usb_gate_en(struct charger_device *chgdev, bool enable)
{
	struct sc858x_chip *chip;
	int ret;

	if (!chgdev)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_field_write(chip, F_OVPGATE_EN, enable ? 1 : 0);
	mutex_unlock(&chip->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_set_usb_gate_en);

int sc858x_cp_enable_acdrv_manual(struct charger_device *chgdev, bool manual)
{
	struct sc858x_chip *chip;
	int ret;

	if (!chgdev)
		return -EINVAL;
	chip = charger_get_data(chgdev);
	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = sc858x_field_write(chip, F_ACDRV_MANUAL, manual ? 1 : 0);
	mutex_unlock(&chip->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc858x_cp_enable_acdrv_manual);

static enum power_supply_property sc858x_psy_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static int sc858x_psy_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct sc858x_chip *chip = power_supply_get_drvdata(psy);
	unsigned int reg_val = 0;
	int cooked = 0;
	int ret = 0;

	mutex_lock(&chip->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = chip->chip_ok ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = sc858x_field_read(chip, F_SWITCHING_STAT, &reg_val);
		if (ret == 0)
			val->intval = !!reg_val;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = sc858x_read_adc_cooked(chip, SC858X_REG_VBUS_ADC, &cooked);
		if (ret == 0)
			val->intval = cooked * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = sc858x_read_adc_cooked(chip, SC858X_REG_IBUS_ADC, &cooked);
		if (ret == 0)
			val->intval = cooked * 1000;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = sc858x_read_adc_cooked(chip, SC858X_REG_TDIE_ADC, &cooked);
		if (ret == 0)
			val->intval = cooked * 10;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&chip->lock);
	return ret;
}

static ssize_t cp_vbus_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct sc858x_chip *chip = dev_get_drvdata(dev);
	int val = 0;
	int ret;

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, SC858X_REG_VBUS_ADC, &val);
	mutex_unlock(&chip->lock);
	return ret < 0 ? ret : sysfs_emit(buf, "%d\n", val);
}
static DEVICE_ATTR_RO(cp_vbus);

static ssize_t cp_ibus_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct sc858x_chip *chip = dev_get_drvdata(dev);
	int val = 0;
	int ret;

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, SC858X_REG_IBUS_ADC, &val);
	mutex_unlock(&chip->lock);
	return ret < 0 ? ret : sysfs_emit(buf, "%d\n", val);
}
static DEVICE_ATTR_RO(cp_ibus);

static ssize_t cp_tdie_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct sc858x_chip *chip = dev_get_drvdata(dev);
	int val = 0;
	int ret;

	mutex_lock(&chip->lock);
	ret = sc858x_read_adc_cooked(chip, SC858X_REG_TDIE_ADC, &val);
	mutex_unlock(&chip->lock);
	return ret < 0 ? ret : sysfs_emit(buf, "%d\n", val);
}
static DEVICE_ATTR_RO(cp_tdie);

static ssize_t chip_ok_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct sc858x_chip *chip = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", chip->chip_ok ? 1 : 0);
}
static DEVICE_ATTR_RO(chip_ok);

static ssize_t registers_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct sc858x_chip *chip = dev_get_drvdata(dev);
	int i, pos = 0;

	mutex_lock(&chip->lock);
	for (i = 0; i < ARRAY_SIZE(sc858x_dump_regs); i++) {
		unsigned int val = 0;

		if (regmap_read(chip->regmap, sc858x_dump_regs[i], &val) == 0)
			pos += sysfs_emit_at(buf, pos, "Reg[%.2X] = 0x%.2x\n",
					     sc858x_dump_regs[i], val);
	}
	mutex_unlock(&chip->lock);
	return pos;
}
static DEVICE_ATTR_RO(registers);

static struct attribute *sc858x_dev_attrs[] = {
	&dev_attr_cp_vbus.attr,
	&dev_attr_cp_ibus.attr,
	&dev_attr_cp_tdie.attr,
	&dev_attr_chip_ok.attr,
	&dev_attr_registers.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sc858x_dev);

static void sc858x_parse_dt(struct sc858x_chip *chip)
{
	struct device_node *np = chip->dev->of_node;
	struct sc858x_cfg *cfg = &chip->cfg;
	const char *product = product_name_get();
	u32 disable_tsbat;
	const struct {
		const char *prop;
		u32 *dst;
		u32 def;
	} props[] = {
		{ "sc,sc858x,bat-ovp-threshold", &cfg->bat_ovp_threshold, 11 },
		{ "sc,sc858x,bat-ocp-threshold", &cfg->bat_ocp_threshold, 4 },
		{ "sc,sc858x,wpc-ovp-threshold", &cfg->wpc_ovp_threshold, 11 },
		{ "sc,sc858x,out-ovp-threshold", &cfg->out_ovp_threshold, 1 },
		{ "sc,sc858x,bus-ucp-fall-dg", &cfg->bus_ucp_fall_dg, 1 },
		{ "sc,sc858x,pmid2out-ovp-threshold", &cfg->pmid2out_ovp_threshold, 2 },
		{ "sc,sc858x,pmid2out-uvp-threshold", &cfg->pmid2out_uvp_threshold, 7 },
		{ "sc,sc858x,fsw-set", &cfg->fsw_set, 20 },
		{ "sc,sc858x,acdrv-hi-en", &cfg->acdrv_hi_en, 1 },
		{ "sc,sc858x,ss-timeout", &cfg->ss_timeout, 5 },
		{ "sc,sc858x,mode", &cfg->mode, SC858X_MODE_2_1 },
		{ "sc,sc858x,sense-r-mohm", &cfg->sense_r_mohm, 1 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(props); i++) {
		if (of_property_read_u32(np, props[i].prop, props[i].dst))
			*props[i].dst = props[i].def;
	}

	/* The rothko dual-pump board monitors pack temperature through the BMS. */
	cfg->disable_tsbat = product && chip->work_mode != SC858X_WORK_STANDALONE &&
		(!strcmp(product, "rothko") || !strcmp(product, "rothko_global")) &&
		!of_property_read_u32(np, "sc,sc858x,tsbat-flt-disable", &disable_tsbat) &&
		disable_tsbat == 1;
}

static int sc858x_detect_product(void)
{
	const char *prod = product_name_get();
	int country = get_hw_country_version();

	if (!prod)
		return 3;
	if (!strcmp(prod, "rothko"))
		return country == 0 ? 3 : 4;
	if (!strcmp(prod, "duchamp"))
		return country == 0 ? 1 : 2;
	return 3;
}

static int sc858x_probe(struct i2c_client *client)
{
	struct sc858x_chip *chip;
	struct power_supply_desc *psy_desc;
	struct power_supply_config psy_cfg = {};
	const char *chg_name;
	const char *irq_name;
	struct charger_device *chgdev;
	unsigned int dev_id = 0;
	int i, ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &client->dev;
	chip->work_mode = (uintptr_t)of_device_get_match_data(&client->dev);
	if ((chip->work_mode == SC858X_WORK_MASTER && client->addr != 0x6e) ||
	    (chip->work_mode == SC858X_WORK_SLAVE && client->addr != 0x6f))
		return -ENODEV;
	chip->product_code = sc858x_detect_product();
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	switch (chip->work_mode) {
	case SC858X_WORK_MASTER:
		strscpy(chip->log_tag, "[SC858X_MASTER]", sizeof(chip->log_tag));
		chg_name = "cp_master";
		irq_name = "sc858x-master-irq";
		break;
	case SC858X_WORK_SLAVE:
		strscpy(chip->log_tag, "[SC858X_SLAVE]", sizeof(chip->log_tag));
		chg_name = "cp_slave";
		irq_name = "sc858x-slave-irq";
		break;
	default:
		strscpy(chip->log_tag, "[SC858X_STANDALONE]", sizeof(chip->log_tag));
		chg_name = "cp_standalone";
		irq_name = "sc858x-standalone-irq";
		break;
	}

	chip->regmap = devm_regmap_init_i2c(client, &sc858x_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(chip->dev, PTR_ERR(chip->regmap),
				     "failed to init regmap\n");

	for (i = 0; i < F_MAX_FIELDS; i++) {
		chip->fields[i] = devm_regmap_field_alloc(chip->dev,
							  chip->regmap,
							  sc858x_reg_fields[i]);
		if (IS_ERR(chip->fields[i]))
			return PTR_ERR(chip->fields[i]);
	}

	ret = sc858x_field_read(chip, F_DEVICE_ID, &dev_id);
	if (ret < 0)
		return dev_err_probe(chip->dev, ret,
				     "%s: failed to read device id\n",
				     chip->log_tag);
	if (dev_id != SC858X_DEVICE_ID) {
		dev_err(chip->dev, "%s: unexpected device id 0x%02x\n",
			chip->log_tag, dev_id);
		return -ENODEV;
	}

	sc858x_parse_dt(chip);
	if (chip->cfg.mode > SC858X_MODE_1_1)
		return -EINVAL;

	chip->irq_gpiod = devm_gpiod_get_optional(chip->dev, "sc,sc858x,irq",
						 GPIOD_IN);
	if (IS_ERR(chip->irq_gpiod))
		return dev_err_probe(chip->dev, PTR_ERR(chip->irq_gpiod),
				     "cannot get pump IRQ GPIO\n");

	chip->irq = chip->irq_gpiod ? gpiod_to_irq(chip->irq_gpiod) : client->irq;
	if (chip->irq <= 0)
		return dev_err_probe(chip->dev, chip->irq ?: -ENXIO, "missing pump IRQ\n");

	psy_desc = devm_kzalloc(chip->dev, sizeof(*psy_desc), GFP_KERNEL);
	if (!psy_desc)
		return -ENOMEM;
	psy_desc->name = chg_name;
	psy_desc->type = POWER_SUPPLY_TYPE_MAINS;
	psy_desc->properties = sc858x_psy_props;
	psy_desc->num_properties = ARRAY_SIZE(sc858x_psy_props);
	psy_desc->get_property = sc858x_psy_get_property;

	psy_cfg.drv_data = chip;
	psy_cfg.fwnode = dev_fwnode(chip->dev);
	chip->psy = devm_power_supply_register(chip->dev, psy_desc, &psy_cfg);
	if (IS_ERR(chip->psy))
		return dev_err_probe(chip->dev, PTR_ERR(chip->psy),
				     "cannot register pump power supply\n");

	ret = devm_request_threaded_irq(chip->dev, chip->irq, NULL, sc858x_irq_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT, irq_name, chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "cannot request pump IRQ\n");

	chgdev = charger_device_register(chg_name, chip->dev, chip,
					&sc858x_charger_ops, NULL);
	if (IS_ERR(chgdev))
		return dev_err_probe(chip->dev, PTR_ERR(chgdev),
				     "cannot register pump charger device\n");

	mutex_lock(&chip->lock);
	chip->chgdev = chgdev;
	chip->chip_ok = true;
	mutex_unlock(&chip->lock);
	dev_info(chip->dev, "%s: probe success (id=0x%02x product=%d)\n",
		 chip->log_tag, dev_id, chip->product_code);
	return 0;
}

static void sc858x_remove(struct i2c_client *client)
{
	struct sc858x_chip *chip = i2c_get_clientdata(client);

	if (!chip)
		return;
	devm_free_irq(chip->dev, chip->irq, chip);
	mutex_lock(&chip->lock);
	chip->configured = false;
	sc858x_stop_locked(chip);
	sc858x_field_write(chip, F_ADC_EN, 0);
	chip->chip_ok = false;
	mutex_unlock(&chip->lock);

	if (chip->chgdev)
		charger_device_unregister(chip->chgdev);
}

static void sc858x_shutdown(struct i2c_client *client)
{
	struct sc858x_chip *chip = i2c_get_clientdata(client);

	if (!chip)
		return;
	mutex_lock(&chip->lock);
	chip->configured = false;
	sc858x_stop_locked(chip);
	sc858x_field_write(chip, F_ADC_EN, 0);
	mutex_unlock(&chip->lock);
}

static const struct of_device_id sc858x_of_match[] = {
	{ .compatible = "sc,sc858x-standalone", .data = (void *)SC858X_WORK_STANDALONE },
	{ .compatible = "sc,sc858x-master", .data = (void *)SC858X_WORK_MASTER },
	{ .compatible = "sc,sc858x-slave", .data = (void *)SC858X_WORK_SLAVE },
	{ }
};
MODULE_DEVICE_TABLE(of, sc858x_of_match);

static struct i2c_driver sc858x_charger_driver = {
	.probe = sc858x_probe,
	.remove = sc858x_remove,
	.shutdown = sc858x_shutdown,
	.driver = {
		.name = "sc858x-charger",
		.of_match_table = sc858x_of_match,
		.suppress_bind_attrs = true,
		.dev_groups = sc858x_dev_groups,
	},
};
int sc858x_read_legacy(struct i2c_client *client, u8 reg)
{
	struct sc858x_chip *chip;
	unsigned int status = 0;
	int ret, val = 0;

	if (client->dev.driver != &sc858x_charger_driver.driver)
		return -EBUSY;

	chip = i2c_get_clientdata(client);
	mutex_lock(&chip->lock);
	if (reg == SC858X_REG_SWITCH_STAT) {
		ret = sc858x_field_read(chip, F_SWITCHING_STAT, &status);
		val = status;
	} else if (reg == SC858X_REG_ADC_CTRL) {
		ret = sc858x_field_read(chip, F_ADC_EN, &status);
		val = status;
	} else {
		ret = sc858x_read_adc_cooked(chip, reg, &val);
	}
	mutex_unlock(&chip->lock);

	return ret ? ret : val;
}

module_i2c_driver(sc858x_charger_driver);

MODULE_DESCRIPTION("SC858x Dual Charge Pump Driver for rothko");
MODULE_LICENSE("GPL");
