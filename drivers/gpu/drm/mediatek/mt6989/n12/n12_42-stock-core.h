/* SPDX-License-Identifier: GPL-2.0 */
/*
 * N12 42/02/0a transport and initialization recovered from the stock module.
 * The command tables match Android15 OS2.0.207.0.VNNTWXM byte-for-byte.
 * Used by panel-n12-42-02-0a-dsc-cmd.c, with the shared Xiaomi DDIC tables.
 * Parent integration remains gated on the real VIS/OEM dependency closure.
 * Regulator errors and enable references are handled rather than hidden.
 */
#ifndef N12_42_STOCK_CORE_H
#define N12_42_STOCK_CORE_H

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include "n12_42-stock-tables.h"
#include "n12_36-stock-tables.h"

struct n12_42 {
	struct device *dev;
	struct device *panel_info_dev;
	struct mipi_dsi_device *dsi;
	struct drm_panel panel;
	struct regulator *vci;  /* vibr, 3000000 uV */
	struct regulator *vddi; /* vcn18io, 1800000 uV */
	bool vci_enable_ref;
	bool vddi_enable_ref;
	bool prepared;
	bool enabled;
	bool peak_hdr;
	bool doze_active;
	bool doze_suspended;
	bool doze_backlight_blocked;
	bool variant_36;
	bool lhbm_active;
	struct backlight_device *backlight;
	unsigned int last_backlight;
	unsigned int doze_brightness;
	int error;
	u8 build_id;
	unsigned int refresh_rate;
	unsigned int gir_status;
	unsigned int gray_level;
};

static void n12_42_write(struct n12_42 *ctx, u8 command,
			 const u8 *payload, unsigned int count)
{
	u8 buffer[129];
	ssize_t ret;

	if (ctx->error < 0)
		return;
	if (count > sizeof(buffer) - 1 || (count && !payload)) {
		ctx->error = -EINVAL;
		return;
	}
	buffer[0] = command;
	if (count)
		memcpy(buffer + 1, payload, count);
	if (command <= 0xaf)
		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buffer, count + 1);
	else
		ret = mipi_dsi_generic_write(ctx->dsi, buffer, count + 1);
	if (ret < 0) {
		dev_err(ctx->dev, "DSI command %#02x failed: %zd\n", command, ret);
		ctx->error = ret;
	}
}

static void n12_42_push_table(struct n12_42 *ctx,
			    const struct mi_ddic_cmd *table,
			    unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (table[i].command == 0xfffc)
			msleep(table[i].count);
		else if (table[i].command != 0xfffd)
			n12_42_write(ctx, table[i].command, table[i].para_list,
				     table[i].count);
	}
}

#define n12_42_send(ctx, name) \
	n12_42_push_table(ctx, n12_42_##name, ARRAY_SIZE(n12_42_##name))

/* Acquire one consumer reference, even if bootloader already enabled the rail. */
static int n12_42_supply_enable(struct regulator *supply, int voltage,
			       bool *enable_ref)
{
	int ret;

	ret = regulator_set_voltage(supply, voltage, voltage);
	if (ret)
		return ret;
	if (*enable_ref)
		return 0;
	ret = regulator_enable(supply);
	if (!ret)
		*enable_ref = true;
	return ret;
}

static int n12_42_supply_disable(struct regulator *supply, bool *enable_ref)
{
	int ret;

	if (!*enable_ref)
		return 0;
	ret = regulator_disable(supply);
	if (!ret)
		*enable_ref = false;
	return ret;
}

static int n12_42_vci_enable(struct n12_42 *ctx)
{
	return n12_42_supply_enable(ctx->vci, 3000000,
				    &ctx->vci_enable_ref);
}

static int n12_42_vddi_enable(struct n12_42 *ctx)
{
	return n12_42_supply_enable(ctx->vddi, 1800000,
				    &ctx->vddi_enable_ref);
}

static int n12_42_poweron(struct n12_42 *ctx)
{
	struct gpio_desc *dvdd;
	bool had_vddi_ref = ctx->vddi_enable_ref;
	int ret, rollback_ret;

	if (ctx->prepared)
		return 0;

	ret = n12_42_vddi_enable(ctx);
	if (ret)
		return ret;
	udelay(5000);
	dvdd = devm_gpiod_get_index(ctx->dev, "dvdd", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(dvdd)) {
		ret = PTR_ERR(dvdd);
		goto rollback_vddi;
	}
	gpiod_set_value(dvdd, 1);
	udelay(5000);
	ret = n12_42_vci_enable(ctx);
	if (ret) {
		gpiod_set_value(dvdd, 0);
		devm_gpiod_put(ctx->dev, dvdd);
		goto rollback_vddi;
	}
	devm_gpiod_put(ctx->dev, dvdd);
	udelay(1000);
	return 0;

rollback_vddi:
	if (!had_vddi_ref) {
		rollback_ret = n12_42_supply_disable(ctx->vddi,
						   &ctx->vddi_enable_ref);
		if (rollback_ret)
			dev_err(ctx->dev, "VDDI rollback failed: %d\n", rollback_ret);
	}
	return ret;
}

static int n12_42_poweroff(struct n12_42 *ctx)
{
	struct gpio_desc *reset, *dvdd;
	int ret;

	if (ctx->prepared)
		return 0;

	/* Resolve both GPIOs before starting a sequence that removes panel power. */
	reset = devm_gpiod_get(ctx->dev, "reset", GPIOD_ASIS);
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	dvdd = devm_gpiod_get_index(ctx->dev, "dvdd", 0, GPIOD_ASIS);
	if (IS_ERR(dvdd)) {
		ret = PTR_ERR(dvdd);
		goto put_reset;
	}
	ret = gpiod_direction_output(reset, 0);
	if (ret)
		goto put_dvdd;
	udelay(2000);
	ret = n12_42_supply_disable(ctx->vci, &ctx->vci_enable_ref);
	if (ret)
		goto put_dvdd;
	udelay(2000);
	ret = gpiod_direction_output(dvdd, 0);
	if (ret)
		goto put_dvdd;
	udelay(2000);
	ret = n12_42_supply_disable(ctx->vddi, &ctx->vddi_enable_ref);
	udelay(2000);
put_dvdd:
	devm_gpiod_put(ctx->dev, dvdd);
put_reset:
	devm_gpiod_put(ctx->dev, reset);
	return ret;
}

static int n12_42_reset_for_init(struct n12_42 *ctx)
{
	struct gpio_desc *reset;

	reset = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	gpiod_set_value(reset, 0);
	udelay(12000);
	gpiod_set_value(reset, 1);
	udelay(1000);
	gpiod_set_value(reset, 0);
	udelay(10000);
	gpiod_set_value(reset, 1);
	udelay(12000);
	devm_gpiod_put(ctx->dev, reset);
	udelay(10000);
	return 0;
}

#define n12_36_send(ctx, name) \
	n12_42_push_table(ctx, n12_36_##name, ARRAY_SIZE(n12_36_##name))

static void n12_36_set_refresh(struct n12_42 *ctx, unsigned int refresh)
{
	switch (refresh) {
	case 60:
		if (ctx->build_id >= 2)
			n12_36_send(ctx, mode_60hz_setting_p2);
		else
			n12_36_send(ctx, mode_60hz_setting);
		break;
	case 90:
		if (ctx->build_id >= 2)
			n12_36_send(ctx, mode_90hz_setting_p2);
		else
			n12_36_send(ctx, mode_90hz_setting);
		break;
	case 120:
		if (ctx->build_id >= 2)
			n12_36_send(ctx, mode_120hz_setting_p2);
		else
			n12_36_send(ctx, mode_120hz_setting);
		break;
	case 144:
		if (ctx->build_id >= 2)
			n12_36_send(ctx, mode_144hz_setting_p2);
		else
			n12_36_send(ctx, mode_144hz_setting);
		break;
	default:
		ctx->error = -EINVAL;
		break;
	}
}

/* Experimental host modes keep the stock 144 Hz DDIC programming. */
static unsigned int n12_ddic_refresh(unsigned int refresh)
{
	if (IS_ENABLED(CONFIG_DRM_ROTHKO_N12_HIGH_REFRESH_TEST) &&
	    (refresh == 165 || refresh == 185))
		return 144;
	return refresh;
}

static void n12_42_set_refresh(struct n12_42 *ctx, unsigned int refresh)
{
	refresh = n12_ddic_refresh(refresh);
	if (ctx->variant_36) {
		n12_36_set_refresh(ctx, refresh);
		return;
	}

	switch (refresh) {
	case 60:
		n12_42_send(ctx, mode_60hz_setting);
		break;
	case 90:
		n12_42_send(ctx, mode_90hz_setting);
		break;
	case 120:
		n12_42_send(ctx, mode_120hz_setting);
		break;
	case 144:
		n12_42_send(ctx, mode_144hz_setting);
		break;
	default:
		ctx->error = -EINVAL;
		break;
	}
}

static int n12_36_initialize(struct n12_42 *ctx, int country_version)
{
	int ret = n12_42_reset_for_init(ctx);

	if (ret)
		return ret;

	/* China and global revisions use separate DDIC initialization tables. */
	if (!country_version) {
		if (ctx->build_id == 0)
			n12_36_send(ctx, init_setting);
		else if (ctx->build_id == 1)
			n12_36_send(ctx, init_setting_p1);
		else
			n12_36_send(ctx, init_setting_p2);
	} else {
		switch (ctx->build_id) {
		case 0:
			n12_36_send(ctx, init_setting_GL);
			break;
		case 1:
			n12_36_send(ctx, init_setting_GL_p1);
			break;
		case 2:
			n12_36_send(ctx, init_setting_GL_p2);
			break;
		default:
			n12_36_send(ctx, init_setting_GL_mp);
			break;
		}
	}
	if (!ctx->build_id)
		n12_36_send(ctx, lhbm_lut_init);
	if (!ctx->gir_status)
		n12_36_send(ctx, gir_off_settings);
	n12_36_set_refresh(ctx, n12_ddic_refresh(ctx->refresh_rate));
	return ctx->error;
}

/* country_version must come from the real get_hw_country_version() provider. */
static int n12_42_initialize(struct n12_42 *ctx, int country_version)
{
	struct mi_ddic_cmd gray = n12_42_gray_3d_lut[0];
	bool set_gray = true;
	int ret;

	ret = n12_42_reset_for_init(ctx);
	if (ret)
		return ret;

	if (ctx->build_id == 0x50 || ctx->build_id == 0x51 ||
	    ctx->build_id == 0x5a)
		n12_42_send(ctx, ffd_setting);
	if (ctx->build_id == 0x50 || ctx->build_id == 0x51 ||
	    ctx->build_id == 0x5a || ctx->build_id == 0x5b)
		n12_42_send(ctx, ffd_on);
	n12_42_send(ctx, pcd_hiz);
	if (country_version)
		n12_42_send(ctx, init_setting_GL);
	else
		n12_42_send(ctx, init_setting);
	if (ctx->build_id < 2 || ctx->build_id == 0x28 ||
	    ctx->build_id == 0x29 || ctx->build_id == 0x2a ||
	    ctx->build_id == 0x50 || ctx->build_id == 0x51)
		n12_42_send(ctx, lhbm_demura_setting);
	if (!ctx->gir_status)
		n12_42_send(ctx, gir_off_settings);
	n12_42_set_refresh(ctx, ctx->refresh_rate);
	if (ctx->build_id >= 0x2a) {
		switch (ctx->gray_level) {
		case 1:
			gray.para_list[0] = 0x82;
			break;
		case 32:
			gray.para_list[0] = 0x81;
			break;
		case 64:
			gray.para_list[0] = 0x80;
			break;
		case 128:
			gray.para_list[0] = 0;
			break;
		default:
			set_gray = false;
			break;
		}
		if (set_gray)
			n12_42_push_table(ctx, &gray, 1);
		n12_42_send(ctx, cmd_set_osc_tricking_on_1_12_G_p11);
	} else {
		n12_42_send(ctx, cmd_set_osc_tricking_on_1_12_G);
	}
	return ctx->error;
}

static void n12_42_sleep_in(struct n12_42 *ctx)
{
	n12_42_write(ctx, 0x28, NULL, 0);
	msleep(20);
	n12_42_write(ctx, 0x10, NULL, 0);
	msleep(125);
	ctx->error = 0;
	ctx->lhbm_active = false;
	ctx->prepared = false;
}

#endif
