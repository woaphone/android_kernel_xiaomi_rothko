// SPDX-License-Identifier: GPL-2.0
/* N12-42/02/0a and N12-36/02/0b, recovered from OS2.0.207.0.VNNTWXM. */
#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <drm/drm_connector.h>
#include <video/mipi_display.h>
#include "../mediatek_v2/mtk_panel_ext.h"
#include "../mediatek_v2/mtk_dsi.h"
#include "../mediatek_v2/rothko_fod.h"
#include "n12_42-stock-core.h"
#include "n12_42-stock-dsc.h"
#include "vis_display.h"

static char build_id[16];
module_param_string(build_id, build_id, sizeof(build_id), 0600);
MODULE_PARM_DESC(build_id, "Panel revision supplied by LK, hexadecimal");

/* N12-42 LK supplies 21 four-hex-digit calibration fields. */
static char oled_lhbm[90];
module_param_string(oled_lhbm, oled_lhbm, sizeof(oled_lhbm), 0);

/* This driver is built in. Keep the separate 0b LK parameter namespace. */
static char n12_36_build_id[16] = "03";
static const struct kparam_string n12_36_build_id_param = {
	.maxlen = sizeof(n12_36_build_id),
	.string = n12_36_build_id,
};
static const struct kernel_param n12_36_revision_param
	__used __section("__param") __aligned(__alignof__(struct kernel_param)) = {
	.name = "panel_n12_36_02_0b_dsc_cmd.build_id",
	.mod = THIS_MODULE,
	.ops = &param_ops_string,
	.level = -1,
	.str = &n12_36_build_id_param,
};

static struct n12_42 *n12_42_active;

static ssize_t panel_info_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct n12_42 *ctx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "panel_name=dsi_n12_%s_dsc_cmd\n",
			  ctx->variant_36 ? "36_02_0b" : "42_02_0a");
}
static DEVICE_ATTR_RO(panel_info);

static struct attribute *n12_42_info_attrs[] = {
	&dev_attr_panel_info.attr,
	NULL,
};
ATTRIBUTE_GROUPS(n12_42_info);

static int n12_42_register_panel_info(struct n12_42 *ctx)
{
	struct device *parent, *panel_dev;

	/* The real character device also parents the stock panel_info node. */
	parent = rothko_fod_device_create();
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	panel_dev = device_create_with_groups(parent->class, parent, 0, ctx,
					      n12_42_info_groups, "disp-DSI-0");
	if (IS_ERR(panel_dev)) {
		rothko_fod_device_destroy(parent);
		return PTR_ERR(panel_dev);
	}
	ctx->panel_info_dev = panel_dev;
	return 0;
}

static void n12_42_unregister_panel_info(struct n12_42 *ctx)
{
	struct device *parent = ctx->panel_info_dev->parent;

	device_unregister(ctx->panel_info_dev);
	rothko_fod_device_destroy(parent);
}

#ifdef CONFIG_DRM_ROTHKO_N12_HIGH_REFRESH_TEST
/* Same porches as the stock 144 Hz mode; DRM pixel clock is in kHz. */
#define N12_HIGH_REFRESH_MODE(fps) { \
	.clock = (1386 * 2786 * (fps) + 500) / 1000, \
	.hdisplay = 1220, .hsync_start = 1380, .hsync_end = 1382, \
	.htotal = 1386, \
	.vdisplay = 2712, .vsync_start = 2766, .vsync_end = 2776, \
	.vtotal = 2786, \
}
static const struct drm_display_mode n12_mode_165 = N12_HIGH_REFRESH_MODE(165);
static const struct drm_display_mode n12_mode_185 = N12_HIGH_REFRESH_MODE(185);
#endif

static const struct drm_display_mode * const n12_42_modes[] = {
	&n12_42_default_mode, &n12_42_middle_mode,
	&n12_42_performence_mode, &n12_42_performence_mode_144,
#ifdef CONFIG_DRM_ROTHKO_N12_HIGH_REFRESH_TEST
	&n12_mode_165, &n12_mode_185,
#endif
};

/* Stock consumer: ESD +0xa48, colour +0xa94, dimensions +0xad8/adc,
 * output +0xcc8, DBI +0xee92, TE +0x102fc, brightness delay +0x10304.
 * Pointer-bearing DSC PPS is rebuilt for the current consumer, not copied raw.
 */
#define N12_42_PARAMS(te_us, bl_delay_us, pll_mhz) { \
	.pll_clk = (pll_mhz), .data_rate = 2 * (pll_mhz), \
	.lcm_esd_check_table = { { .cmd = 0x0a, .count = 1, .para_list = { 0x1c } } }, \
	.lcm_color_mode = 9, \
	.physical_width_um = 69540, .physical_height_um = 154584, \
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT, \
	.is_support_dbi = true, .real_te_duration = (te_us), \
	.SilkyBrightnessDelay = (bl_delay_us), \
}
static struct mtk_panel_params n12_42_params[] = {
	N12_42_PARAMS(16666, 3000, 560), N12_42_PARAMS(11111, 0, 560),
	N12_42_PARAMS(8333, 0, 560), N12_42_PARAMS(6944, 0, 560),
#ifdef CONFIG_DRM_ROTHKO_N12_HIGH_REFRESH_TEST
	/* Scale the stock link rate by fps, rounding PLL MHz up. */
	N12_42_PARAMS(6061, 0, 642), N12_42_PARAMS(5405, 0, 720),
#endif
};

static struct n12_42 *n12_42_from_panel(struct drm_panel *panel)
{
	return container_of(panel, struct n12_42, panel);
}

static struct drm_display_mode *n12_42_mode_by_id(struct drm_connector *connector,
						unsigned int id)
{
	struct drm_display_mode *mode;
	unsigned int i = 0;

	list_for_each_entry(mode, &connector->modes, head)
		if (i++ == id)
			return mode;
	return NULL;
}

static int n12_42_param_index(struct drm_display_mode *mode)
{
	if (!mode)
		return -EINVAL;
	switch (drm_mode_vrefresh(mode)) {
	case 60: return 0;
	case 90: return 1;
	case 120: return 2;
	case 144: return 3;
#ifdef CONFIG_DRM_ROTHKO_N12_HIGH_REFRESH_TEST
	case 165: return 4;
	case 185: return 5;
#endif
	default: return -EINVAL;
	}
}

static int n12_42_ext_param_get(struct drm_panel *panel,
		struct drm_connector *connector, struct mtk_panel_params **params,
		unsigned int mode_id)
{
	int i = n12_42_param_index(n12_42_mode_by_id(connector, mode_id));

	if (i < 0)
		return i;
	*params = &n12_42_params[i];
	return 0;
}

static int n12_42_ext_param_set(struct drm_panel *panel,
		struct drm_connector *connector, unsigned int mode_id)
{
	struct mtk_panel_params *params;
	int ret;

	ret = n12_42_ext_param_get(panel, connector, &params, mode_id);
	if (ret)
		return ret;
	find_panel_ext(panel)->params = params;
	return 0;
}

static bool n12_42_vis_test_fps(unsigned int fps)
{
	return n12_ddic_refresh(fps) != fps;
}

static void n12_42_vis_sync_mode(struct n12_42 *ctx, unsigned int fps,
				const struct mtk_panel_params *params)
{
	struct mtk_dsi *dsi = container_of(ctx->dsi->host, struct mtk_dsi, host);

	/* ext_param_set selects the destination before the host changes rate. */
	if (ctx->prepared && !ctx->error && dsi->data_rate == params->data_rate)
		n12_vis_mode_notify(fps, dsi->data_rate * 1000000U);
}

static void n12_42_vis_restore(struct n12_42 *ctx, bool preparing)
{
	const struct mtk_panel_params *params;
	unsigned int fps;

	if (!is_mi_dev_support_nova())
		return;

	params = find_panel_ext(&ctx->panel)->params;
	if (IS_ENABLED(CONFIG_DRM_ROTHKO_N12_HIGH_REFRESH_TEST)) {
		/* The mode and parameter arrays use the same refresh order. */
		fps = drm_mode_vrefresh(n12_42_modes[params - n12_42_params]);
		if (n12_42_vis_test_fps(fps) || n12_42_vis_test_fps(ctx->refresh_rate)) {
			n12_42_vis_sync_mode(ctx, fps, params);
			return;
		}
	}
	if (preparing)
		vis_dsi_rate_notify(params->data_rate * 1000000U);
}

static int n12_42_unprepare(struct drm_panel *panel)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);

	if (!ctx->prepared)
		return 0;
	n12_42_sleep_in(ctx);
	ctx->doze_active = false;
	ctx->doze_suspended = false;
	ctx->doze_backlight_blocked = false;
	ctx->peak_hdr = false;
	return 0;
}

static int n12_42_prepare(struct drm_panel *panel)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	u8 brightness[2];
	u8 power_mode;
	ssize_t read;
	int ret;

	if (ctx->prepared)
		return 0;
	if (ctx->variant_36)
		ret = n12_36_initialize(ctx, get_hw_country_version());
	else
		ret = n12_42_initialize(ctx, get_hw_country_version());
	if (ret < 0) {
		/* DRM 6.18 leaves the panel unprepared on failure. */
		ctx->error = 0;
		return ret;
	}
	read = mipi_dsi_dcs_read(ctx->dsi, MIPI_DCS_GET_POWER_MODE,
				 &power_mode, sizeof(power_mode));
	if (read == sizeof(power_mode))
		dev_info(ctx->dev, "DDIC power mode after prepare: %#02x\n",
			 power_mode);
	else
		dev_warn(ctx->dev, "DDIC power mode read returned %zd\n", read);

	read = mipi_dsi_dcs_read(ctx->dsi, MIPI_DCS_GET_DISPLAY_BRIGHTNESS,
				 brightness, sizeof(brightness));
	if (read == sizeof(brightness))
		dev_info(ctx->dev, "DDIC brightness after prepare: %#02x %#02x\n",
			 brightness[0], brightness[1]);
	else
		dev_warn(ctx->dev, "DDIC brightness read returned %zd\n", read);

	ctx->prepared = true;
	ctx->gir_status = 1;
	ctx->peak_hdr = false;
	mtk_panel_tch_rst(panel);
	n12_42_vis_restore(ctx, true);
	return ret;
}

static int n12_42_enable(struct drm_panel *panel)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	int ret;

	if (ctx->enabled)
		return 0;
	if (ctx->backlight) {
		ret = backlight_enable(ctx->backlight);
		if (ret)
			return ret;
	}
	ctx->enabled = true;
	return 0;
}

static int n12_42_disable(struct drm_panel *panel)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	int ret;

	if (!ctx->enabled)
		return 0;
	if (ctx->backlight) {
		ret = backlight_disable(ctx->backlight);
		if (ret)
			return ret;
	}
	ctx->enabled = false;
	return 0;
}

static int n12_42_get_modes(struct drm_panel *panel,
			  struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(n12_42_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, n12_42_modes[i]);
		if (!mode)
			return -ENOMEM;
		drm_mode_set_name(mode);
		mode->type = DRM_MODE_TYPE_DRIVER;
		if (!i)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
	}
	connector->display_info.width_mm = 69;
	connector->display_info.height_mm = 154;
	return ARRAY_SIZE(n12_42_modes);
}

static int n12_42_panel_poweron(struct drm_panel *panel)
{
	return n12_42_poweron(n12_42_from_panel(panel));
}

static int n12_42_panel_poweroff(struct drm_panel *panel)
{
	return n12_42_poweroff(n12_42_from_panel(panel));
}

static int n12_42_ext_reset(struct drm_panel *panel, int on)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	struct gpio_desc *reset;
	int ret;

	reset = devm_gpiod_get(ctx->dev, "reset", GPIOD_ASIS);
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	ret = gpiod_direction_output(reset, on);
	devm_gpiod_put(ctx->dev, reset);
	return ret;
}

static int n12_42_mode_switch(struct drm_panel *panel,
		struct drm_connector *connector, unsigned int cur_mode,
		unsigned int dst_mode, enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	struct drm_display_mode *dst;
	unsigned int old_fps = ctx->refresh_rate;
	unsigned int fps;
	int i;

	if (cur_mode == dst_mode)
		return 0;
	dst = n12_42_mode_by_id(connector, dst_mode);
	i = n12_42_param_index(dst);
	if (i < 0)
		return i;
	fps = drm_mode_vrefresh(dst);
	if (stage == BEFORE_DSI_POWERDOWN) {
		/* Analog bypass: this port does not submit a digital VIS UseCase. */
		n12_42_set_refresh(ctx, fps);
		ctx->refresh_rate = fps;
	}
	if (!is_mi_dev_support_nova())
		return ctx->error;

	if (IS_ENABLED(CONFIG_DRM_ROTHKO_N12_HIGH_REFRESH_TEST)) {
		struct drm_display_mode *src = n12_42_mode_by_id(connector, cur_mode);

		if (n12_42_vis_test_fps(fps) || n12_42_vis_test_fps(old_fps) ||
		    (src && n12_42_vis_test_fps(drm_mode_vrefresh(src)))) {
			if (stage == AFTER_DSI_POWERON)
				n12_42_vis_sync_mode(ctx, fps, &n12_42_params[i]);
			return ctx->error;
		}
	}
	vis_fps_notify(fps);
	return ctx->error;
}

/* VIS OSC enumeration 0..5 dispatch recovered from .rodata's byte jump table. */
static int n12_42_osc_tricking(unsigned int setting)
{
	struct n12_42 *ctx = n12_42_active;
	const struct mi_ddic_cmd *table;
	unsigned int count = 2;

	if (!ctx)
		return -ENODEV;
	if (ctx->variant_36) {
		static const struct mi_ddic_cmd * const tables[] = {
			n12_36_cmd_set_osc_tricking_on_no_pram,
			n12_36_cmd_set_osc_tricking_on_1_12_G,
			n12_36_cmd_set_osc_tricking_on_1_487_G,
			n12_36_cmd_set_osc_tricking_off_no_param,
			n12_36_cmd_set_osc_tricking_off_1_12_G,
			n12_36_cmd_set_osc_tricking_off_1_487_G,
		};

		if (setting >= ARRAY_SIZE(tables))
			return -EINVAL;
		return mi_disp_panel_ddic_send_cmd(tables[setting],
						 setting % 3 ? 2 : 6, 0);
	}
	switch (setting) {
	case 0:
		table = n12_42_cmd_set_osc_tricking_on_no_pram;
		count = ARRAY_SIZE(n12_42_cmd_set_osc_tricking_on_no_pram);
		break;
	case 1:
		table = ctx->build_id >= 0x2a ? n12_42_cmd_set_osc_tricking_on_1_12_G_p11 :
			n12_42_cmd_set_osc_tricking_on_1_12_G;
		break;
	case 2:
		table = ctx->build_id >= 0x2a ? n12_42_cmd_set_osc_tricking_on_1_487_G_p11 :
			n12_42_cmd_set_osc_tricking_on_1_487_G;
		break;
	case 3:
		table = n12_42_cmd_set_osc_tricking_off_no_param;
		count = ARRAY_SIZE(n12_42_cmd_set_osc_tricking_off_no_param);
		break;
	case 4:
		table = ctx->build_id >= 0x2a ? n12_42_cmd_set_osc_tricking_off_1_12_G_p11 :
			n12_42_cmd_set_osc_tricking_off_1_12_G;
		break;
	case 5:
		table = ctx->build_id >= 0x2a ? n12_42_cmd_set_osc_tricking_off_1_487_G_p11 :
			n12_42_cmd_set_osc_tricking_off_1_487_G;
		break;
	default:
		return -EINVAL;
	}
	return mi_disp_panel_ddic_send_cmd(table, count, 0);
}

static unsigned int n12_36_lcm_id(void)
{
	return 0x36;
}

static unsigned int n12_42_lcm_id(void)
{
	return 0x42;
}

static int n12_42_set_backlight(void *dsi_drv, dcs_write_gce cb,
			       void *handle, unsigned int level)
{
	struct mtk_dsi *dsi = dsi_drv;
	struct n12_42 *ctx;
	u8 command[] = { 0x51, level >> 8, level & 0xff };
	u8 peak[] = { 0x5f, 0 };
	bool peak_hdr;

	if (!dsi || !dsi->panel || !cb)
		return -EINVAL;
	ctx = n12_42_from_panel(dsi->panel);
	if (level)
		ctx->last_backlight = level;
	if (ctx->doze_backlight_blocked)
		return 0;
	/* Stock peak HDR enters at 15604..16383 and exits at <=15603. */
	peak_hdr = ctx->peak_hdr;
	if (ctx->enabled && level && level < 16384)
		peak_hdr = level >= 15604;
	if (peak_hdr != ctx->peak_hdr) {
		peak[1] = peak_hdr;
		cb(dsi, handle, peak, sizeof(peak));
		ctx->peak_hdr = peak_hdr;
		ctx->gir_status = !peak_hdr;
	}
	cb(dsi, handle, command, sizeof(command));
	return 0;
}

/* MI Display uses level 0 to release the normal DBV gate, not to exit AOD. */
enum n12_doze_brightness {
	N12_DOZE_NORMAL,
	N12_DOZE_HBM,
	N12_DOZE_LBM,
};

/* The 0A and 0B stock modules have different 6-byte AOD DBV payloads. */
static const struct mi_ddic_cmd n12_doze_brightness_table[2][2] = {
	{
		{ 0x51, 6, { 0x04, 0x00, 0x04, 0x00, 0x3f, 0xff } },
		{ 0x51, 6, { 0x00, 0x3d, 0x00, 0x3d, 0x05, 0x55 } },
	}, {
		{ 0x51, 6, { 0x04, 0x00, 0x04, 0x00, 0x0f, 0xff } },
		{ 0x51, 6, { 0x00, 0x3d, 0x00, 0x3d, 0x07, 0xfc } },
	},
};

static const struct mi_ddic_cmd *n12_doze_dbv(struct n12_42 *ctx, unsigned int level)
{
	return &n12_doze_brightness_table[ctx->variant_36][level != N12_DOZE_HBM];
}

static void n12_doze_write_dbv(struct n12_42 *ctx, void *dsi_drv,
			       dcs_write_gce cb, void *handle)
{
	const struct mi_ddic_cmd *entry = n12_doze_dbv(ctx, ctx->doze_brightness);
	u8 command[7] = { 0x51 };

	memcpy(command + 1, entry->para_list, entry->count);
	cb(dsi_drv, handle, command, sizeof(command));
}

static int n12_set_doze_brightness(struct drm_panel *panel, unsigned int level)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	struct mtk_dsi *dsi = container_of(ctx->dsi->host, struct mtk_dsi, host);
	int ret;

	if (level > N12_DOZE_LBM)
		return -EINVAL;
	if (level == N12_DOZE_NORMAL) {
		ctx->doze_backlight_blocked = false;
	} else if (ctx->doze_active && level != ctx->doze_brightness) {
		if (!dsi->output_en)
			return -EAGAIN;
		ret = mi_disp_panel_ddic_send_cmd(n12_doze_dbv(ctx, level), 1,
						 MI_DDIC_BLOCKING | MI_DDIC_SKIP_LOCK);
		if (ret)
			return ret;
	}
	/* Before DOZE_ACTIVE, keep the requested mode for the entry callback. */
	ctx->doze_brightness = level;
	return 0;
}

static int n12_get_doze_brightness(struct drm_panel *panel, unsigned int *level)
{
	*level = n12_42_from_panel(panel)->doze_brightness;
	return 0;
}

static int n12_doze_enable(struct drm_panel *panel, void *dsi_drv,
			   dcs_write_gce cb, void *handle)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);

	if (!cb)
		return -EINVAL;
	/* Use the caller's command path; do not flush a second CMDQ packet. */
	if (ctx->lhbm_active) {
		u8 restore[] = { 0x51, ctx->last_backlight >> 8, ctx->last_backlight };
		static const u8 alpha_off[] = { 0x87, 0x24 };
		static const u8 spot_off[] = { 0x8b, 0x00 };

		cb(dsi_drv, handle, restore, sizeof(restore));
		cb(dsi_drv, handle, alpha_off, sizeof(alpha_off));
		cb(dsi_drv, handle, spot_off, sizeof(spot_off));
		/* Keep the state until a synchronous OFF or full panel reset. */
	}
	ctx->doze_active = true;
	ctx->doze_backlight_blocked = true;
	if (ctx->doze_brightness != N12_DOZE_NORMAL)
		n12_doze_write_dbv(ctx, dsi_drv, cb, handle);
	return 0;
}

/* The display transition owns the packet; never start another here. */
static void n12_doze_stop_lhbm(struct n12_42 *ctx, void *dsi_drv,
			      dcs_write_gce cb, void *handle)
{
	unsigned int dbv = ctx->last_backlight;
	u8 restore[] = { 0x51, dbv >> 8, dbv };
	static const u8 alpha_off[] = { 0x87, 0x24 };
	static const u8 spot_off[] = { 0x8b, 0x00 };

	if (!ctx->lhbm_active || !ctx->doze_active)
		return;
	cb(dsi_drv, handle, restore, sizeof(restore));
	cb(dsi_drv, handle, alpha_off, sizeof(alpha_off));
	cb(dsi_drv, handle, spot_off, sizeof(spot_off));
	ctx->lhbm_active = false;
}

static int n12_doze_disable(struct drm_panel *panel, void *dsi_drv,
			    dcs_write_gce cb, void *handle)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	static const u8 normal[] = { 0x38, 0x00 };
	static const u8 dimming[] = { 0x5a, 0x01 };
	static const u8 te[] = { 0x6d, 0x00 };
	u8 rate[] = { 0x2f, 0x00 };

	n12_doze_stop_lhbm(ctx, dsi_drv, cb, handle);
	cb(dsi_drv, handle, normal, sizeof(normal));
	switch (n12_ddic_refresh(ctx->refresh_rate)) {
	case 60:
		rate[1] = 0x30;
		if (!ctx->variant_36)
			cb(dsi_drv, handle, dimming, sizeof(dimming));
		break;
	case 90:
		rate[1] = ctx->variant_36 ? 0x02 : 0x01;
		break;
	case 120:
		rate[1] = ctx->variant_36 ? 0x01 : 0x00;
		break;
	case 144:
		rate[1] = ctx->variant_36 ? 0x00 : 0x02;
		break;
	default:
		goto reset_doze;
	}
	cb(dsi_drv, handle, rate, sizeof(rate));
	if (ctx->refresh_rate == 60 && !ctx->variant_36)
		cb(dsi_drv, handle, te, sizeof(te));
	if (IS_ENABLED(CONFIG_DRM_ROTHKO_N12_HIGH_REFRESH_TEST))
		n12_42_vis_restore(ctx, false);

reset_doze:
	ctx->doze_suspended = false;
	ctx->doze_active = false;
	ctx->doze_backlight_blocked = false;
	return 0;
}

static int n12_doze_suspend(struct drm_panel *panel, void *dsi_drv,
			    dcs_write_gce cb, void *handle)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	static const u8 idle[] = { 0x39, 0x00 };
	static const u8 rate_42[] = { 0x2f, 0x00 };
	static const u8 rate_36[] = { 0x2f, 0x01 };

	if (ctx->doze_suspended)
		return 0;

	n12_doze_stop_lhbm(ctx, dsi_drv, cb, handle);
	if (ctx->variant_36) {
		cb(dsi_drv, handle, rate_36, sizeof(rate_36));
		n12_doze_write_dbv(ctx, dsi_drv, cb, handle);
		cb(dsi_drv, handle, idle, sizeof(idle));
	} else {
		cb(dsi_drv, handle, rate_42, sizeof(rate_42));
		cb(dsi_drv, handle, idle, sizeof(idle));
		n12_doze_write_dbv(ctx, dsi_drv, cb, handle);
	}
	ctx->doze_suspended = true;
	return 0;
}

static const struct drm_panel_funcs n12_42_panel_funcs = {
	.prepare = n12_42_prepare, .unprepare = n12_42_unprepare,
	.enable = n12_42_enable, .disable = n12_42_disable,
	.get_modes = n12_42_get_modes,
};
/* The stock N12-42 white-point transform uses these LK field indices. */
static int n12_42_lhbm_white(unsigned int dbv, bool low_white, u8 *rgb)
{
	static const u16 middle_end[] = {
		3101, 3610, 4119, 4628, 5646, 6155, 7173, 8190, 9828, 11467,
	};
	static const u16 normal_gain[][3] = {
		{ 4000, 4072, 4020 }, { 3984, 4060, 4020 },
		{ 3980, 4048, 4012 }, { 3972, 4040, 4000 },
		{ 3960, 4024, 3992 }, { 3960, 4012, 3992 },
		{ 3960, 4000, 3980 }, { 3952, 3992, 3968 },
		{ 3940, 3992, 3960 }, { 3940, 3984, 3960 },
	};
	static const u16 white_gain[][3] = {
		{ 3920, 3920, 3920 }, { 3924, 3940, 3928 },
		{ 4128, 4120, 4132 },
	};
	static const u16 normal_low_gain[] = { 4000, 4100, 4040 };
	static const u16 normal_high_gain[] = { 4124, 4120, 4168 };
	const u16 *gain;
	u8 calibration[42];
	unsigned int band, first, i;

	if (strlen(oled_lhbm) != 2 * sizeof(calibration) ||
	    hex2bin(calibration, oled_lhbm, sizeof(calibration)))
		return -ENODATA;

	if (dbv < 1309) {
		first = low_white ? 3 : 12;
		gain = low_white ? white_gain[1] : normal_low_gain;
	} else if (dbv <= 11467) {
		for (band = 0; dbv > middle_end[band]; band++)
			continue;
		first = low_white ? 0 : 9;
		gain = low_white ? white_gain[0] : normal_gain[band];
	} else {
		first = low_white ? 6 : 15;
		gain = low_white ? white_gain[2] : normal_high_gain;
	}

	for (i = 0; i < 3; i++) {
		u16 value = get_unaligned_be16(calibration + 2 * (first + i));

		put_unaligned_be16(value * gain[i] / 1000, rgb + 2 * i);
	}
	return 0;
}

/* Primary CRTC lock is held by the caller; wait for the DDIC batch. */
static int n12_42_send_lhbm(const struct mi_ddic_cmd *table, unsigned int count)
{
	return mi_disp_panel_ddic_send_cmd(table, count, MI_DDIC_BLOCKING |
					  MI_DDIC_USE_LPM | MI_DDIC_SKIP_LOCK);
}

static int n12_42_lhbm_off_locked(struct n12_42 *ctx, unsigned int dbv)
{
	struct mi_ddic_cmd table[ARRAY_SIZE(n12_42_lhbm_off)];
	int ret;

	memcpy(table, n12_42_lhbm_off, sizeof(table));
	put_unaligned_be16(dbv, table[0].para_list);
	ret = n12_42_send_lhbm(table, ARRAY_SIZE(table));
	if (!ret)
		ctx->lhbm_active = false;
	return ret;
}

static int n12_42_set_local_hbm(struct drm_panel *panel, unsigned int mode)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	const struct mi_ddic_cmd *template;
	struct mi_ddic_cmd *table;
	unsigned int count, dbv, i;
	u16 alpha = 0;
	u8 rgb[6];
	bool hlpm = mode == 6 || mode == 7;
	int ret;

	if (ctx->variant_36 || ctx->build_id != 0x5b || get_hw_country_version())
		return -EOPNOTSUPP;
	if (!ctx->prepared || !ctx->enabled)
		return -EHOSTDOWN;

	switch (mode) {
	case 0: /* OFF_TO_NORMAL */
		if (ctx->doze_active)
			return -EOPNOTSUPP;
		if (!ctx->last_backlight)
			return -EAGAIN;
		return n12_42_lhbm_off_locked(ctx, ctx->last_backlight);
	case 10: /* OFF_TO_NORMAL_BACKLIGHT */
	case 11: /* OFF_TO_NORMAL_BACKLIGHT_RESTORE */
		if (!ctx->doze_active)
			return -EINVAL;
		if (!ctx->lhbm_active)
			return 0;
		dbv = ctx->last_backlight;
		if (mode == 10 && !dbv)
			dbv = ctx->doze_brightness == N12_DOZE_HBM ? 1024 : 61;
		return n12_42_lhbm_off_locked(ctx, dbv);
	case 1: /* NORMAL_WHITE */
	case 4: /* NORMAL_LOW_WHITE */
	case 6: /* HLPM_WHITE */
	case 7: /* HLPM_LOW_WHITE */
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (hlpm != ctx->doze_active || ctx->peak_hdr)
		return -EOPNOTSUPP;
	if (hlpm) {
		if (ctx->doze_brightness != N12_DOZE_HBM &&
		    ctx->doze_brightness != N12_DOZE_LBM)
			return -EAGAIN;
		dbv = ctx->doze_brightness == N12_DOZE_HBM ? 1024 : 61;
	} else {
		/* Every normal ON table starts with the 120 Hz DDIC setting. */
		if (ctx->refresh_rate != 120)
			return -EOPNOTSUPP;
		if (!ctx->last_backlight)
			return -EAGAIN;
		dbv = clamp(ctx->last_backlight, 15U, 15603U);
	}
	/* At AOD DBV <=1024, HLPM white uses fields 3..5 and low white 12..14. */
	ret = n12_42_lhbm_white(dbv, mode == 4 || mode == 6, rgb);
	if (ret)
		return ret;

	if (hlpm) {
		template = n12_42_lhbm_hlpm_80nit_2nit;
		count = ARRAY_SIZE(n12_42_lhbm_hlpm_80nit_2nit);
		alpha = n12_42_lhbm_alpha_low[dbv - 1];
	} else if (dbv < 1309) {
		template = n12_42_lhbm_normal_80nit_2nit;
		count = ARRAY_SIZE(n12_42_lhbm_normal_80nit_2nit);
		alpha = n12_42_lhbm_alpha_low[dbv - 1];
	} else if (dbv <= 11467) {
		template = n12_42_lhbm_normal_700nit_80nit;
		count = ARRAY_SIZE(n12_42_lhbm_normal_700nit_80nit);
	} else {
		template = n12_42_lhbm_normal_1600nit_700nit;
		count = ARRAY_SIZE(n12_42_lhbm_normal_1600nit_700nit);
		alpha = n12_42_lhbm_alpha_high[dbv - 11468];
	}

	table = kmemdup_array(template, count, sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		if (table[i].command == 0xd1)
			memcpy(table[i].para_list, rgb, sizeof(rgb));
		else if (table[i].command == 0x87 && alpha)
			put_unaligned_be16(alpha, table[i].para_list + 1);
	}
	ctx->lhbm_active = true;
	ret = n12_42_send_lhbm(table, count);
	kfree(table);
	if (hlpm)
		ctx->doze_suspended = false;
	if (ret)
		n12_42_lhbm_off_locked(ctx, hlpm ? dbv : ctx->last_backlight);
	return ret;
}

static struct mtk_panel_funcs n12_42_ext_funcs = {
	.reset = n12_42_ext_reset,
	.panel_poweron = n12_42_panel_poweron,
	.panel_poweroff = n12_42_panel_poweroff,
	.ext_param_set = n12_42_ext_param_set,
	.ext_param_get = n12_42_ext_param_get,
	.mode_switch = n12_42_mode_switch,
	.set_backlight_cmdq = n12_42_set_backlight,
	.set_local_hbm = n12_42_set_local_hbm,
	.set_doze_brightness = n12_set_doze_brightness,
	.get_doze_brightness = n12_get_doze_brightness,
	.doze_enable = n12_doze_enable,
	.doze_disable = n12_doze_disable,
	.doze_suspend = n12_doze_suspend,
};

/* Rothko LK places four bytes after VRAM, before the videolfb panel name. */
static bool n12_42_lk_handoff(struct n12_42 *ctx)
{
	const char *name = ctx->variant_36 ? "n12_36_02_0b_dsc_cmd_lcm_drv" :
					    "n12_42_02_0a_dsc_cmd_lcm_drv";
	size_t name_len = strlen(name) + 1;
	struct device_node *chosen;
	const u8 *tag;
	int len;
	bool handoff = false;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return false;
	tag = of_get_property(chosen, "atag,videolfb", &len);
	if (tag && len >= 24 + name_len)
		handoff = (get_unaligned_le32(tag + 8) & 0xffff) &&
			  !memcmp(tag + 24, name, name_len);
	of_node_put(chosen);
	return handoff;
}

static int n12_42_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *parent, *endpoint, *remote;
	struct n12_42 *ctx;
	struct mtk_panel_ctx *panel_ctx;
	struct gpio_desc *gpio;
	unsigned int i;
	u8 revision;
	int ret;

	parent = of_get_parent(dev->of_node);
	endpoint = of_graph_get_next_endpoint(parent, NULL);
	remote = endpoint ? of_graph_get_remote_port_parent(endpoint) : NULL;
	of_node_put(endpoint);
	of_node_put(parent);
	if (remote != dev->of_node) {
		of_node_put(remote);
		return -ENODEV;
	}
	of_node_put(remote);
	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;
	ctx->dsi = dsi;
	ctx->refresh_rate = 120;
	ctx->gray_level = 1;
	ctx->gir_status = 1;
	ctx->variant_36 = of_device_is_compatible(dev->of_node,
						 "n12_36_02_0b_dsc_cmd,lcm");
	ret = kstrtou8(ctx->variant_36 ? n12_36_build_id : build_id, 16, &revision);
	if (ret)
		return dev_err_probe(dev, ret, "missing/invalid LK panel build_id\n");
	ctx->build_id = revision;
	ctx->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(ctx->backlight))
		return PTR_ERR(ctx->backlight);
	ctx->vci = devm_regulator_get(dev, "vibr");
	if (IS_ERR(ctx->vci))
		return dev_err_probe(dev, PTR_ERR(ctx->vci), "VCI supply unavailable\n");
	ctx->vddi = devm_regulator_get(dev, "vcn18io");
	if (IS_ERR(ctx->vddi))
		return dev_err_probe(dev, PTR_ERR(ctx->vddi), "VDDI supply unavailable\n");
	gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);
	devm_gpiod_put(dev, gpio);
	gpio = devm_gpiod_get(dev, "dvdd", GPIOD_ASIS);
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);
	devm_gpiod_put(dev, gpio);
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_NO_EOT_PACKET |
		MIPI_DSI_CLOCK_NON_CONTINUOUS;
	mipi_dsi_set_drvdata(dsi, ctx);
	for (i = 0; i < ARRAY_SIZE(n12_42_params); i++)
		n12_42_params[i].dsc_params = n12_42_stock_dsc;
	ctx->prepared = n12_42_lk_handoff(ctx);
	ctx->enabled = ctx->prepared;
	if (ctx->prepared) {
		/* Stock probe acquires both references while preserving LK's GPIOs. */
		ret = n12_42_vci_enable(ctx);
		if (ret)
			return ret;
		ret = n12_42_vddi_enable(ctx);
		if (ret)
			goto disable_vci;
	}
	drm_panel_init(&ctx->panel, dev, &n12_42_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	/* Core tracks power state independently of the callbacks in 6.18. */
	ctx->panel.prepared = ctx->prepared;
	ctx->panel.enabled = ctx->enabled;
	drm_panel_add(&ctx->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret)
		goto remove_panel;
	ret = mtk_panel_ext_create(dev, &n12_42_params[2], &n12_42_ext_funcs, &ctx->panel);
	if (ret)
		goto detach_dsi;
	ret = n12_42_register_panel_info(ctx);
	if (ret)
		goto remove_ext;
	ret = mtk_panel_tch_handle_reg(&ctx->panel);
	if (ret)
		goto unregister_panel_info;
	n12_42_active = ctx;
	if (is_mi_dev_support_nova()) {
		nvt_get_lcm_id_notify(ctx->variant_36 ? n12_36_lcm_id : n12_42_lcm_id);
		vis_display_ops_osc_tricking_notify(n12_42_osc_tricking);
	}
	dev_info(dev, "N12-%s build %02x, LK handoff %u\n",
		ctx->variant_36 ? "36/02/0b" : "42/02/0a",
		ctx->build_id, ctx->prepared);
	return 0;
unregister_panel_info:
	n12_42_unregister_panel_info(ctx);
remove_ext:
	panel_ctx = find_panel_ctx(&ctx->panel);
	mtk_panel_detach(panel_ctx);
	mtk_panel_remove(panel_ctx);
detach_dsi:
	mipi_dsi_detach(dsi);
remove_panel:
	drm_panel_remove(&ctx->panel);
	n12_42_supply_disable(ctx->vddi, &ctx->vddi_enable_ref);
disable_vci:
	n12_42_supply_disable(ctx->vci, &ctx->vci_enable_ref);
	return ret;
}

static void n12_42_remove(struct mipi_dsi_device *dsi)
{
	struct n12_42 *ctx = mipi_dsi_get_drvdata(dsi);
	struct mtk_panel_ctx *panel_ctx = find_panel_ctx(&ctx->panel);

	if (is_mi_dev_support_nova()) {
		nvt_get_lcm_id_notify(NULL);
		vis_display_ops_osc_tricking_notify(NULL);
	}
	n12_42_unregister_panel_info(ctx);
	n12_42_active = NULL;
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	mtk_panel_detach(panel_ctx);
	mtk_panel_remove(panel_ctx);
	n12_42_supply_disable(ctx->vci, &ctx->vci_enable_ref);
	n12_42_supply_disable(ctx->vddi, &ctx->vddi_enable_ref);
}

static const struct of_device_id n12_42_of_match[] = {
	{ .compatible = "n12_42_02_0a_dsc_cmd,lcm" },
	{ .compatible = "n12_36_02_0b_dsc_cmd,lcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, n12_42_of_match);
static struct mipi_dsi_driver n12_42_driver = {
	.probe = n12_42_probe, .remove = n12_42_remove,
	.driver = {
		.name = "n12_42_02_0a_dsc_cmd,lcm",
		.of_match_table = n12_42_of_match,
	},
};
module_mipi_dsi_driver(n12_42_driver);
MODULE_DESCRIPTION("Recovered N12 42/02/0a and 36/02/0b command-mode DSC panels");
MODULE_LICENSE("GPL");
