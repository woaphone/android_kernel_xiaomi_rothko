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
#include "n12_42-stock-core.h"
#include "n12_42-stock-dsc.h"
#include "vis_display.h"

static char build_id[16];
module_param_string(build_id, build_id, sizeof(build_id), 0600);
MODULE_PARM_DESC(build_id, "Panel revision supplied by LK, hexadecimal");

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
	struct class *display_class;
	struct device *parent, *panel_dev;
	int ret;

	display_class = class_create("mi_display");
	if (IS_ERR(display_class))
		return PTR_ERR(display_class);

	/* Match the stock sysfs hierarchy used by DisplayFeature HAL. */
	parent = device_create(display_class, NULL, 0, NULL, "disp_feature");
	if (IS_ERR(parent)) {
		ret = PTR_ERR(parent);
		goto destroy_class;
	}
	panel_dev = device_create_with_groups(display_class, parent, 0, ctx,
					      n12_42_info_groups, "disp-DSI-0");
	if (IS_ERR(panel_dev)) {
		ret = PTR_ERR(panel_dev);
		goto unregister_parent;
	}
	ctx->panel_info_dev = panel_dev;
	return 0;

unregister_parent:
	device_unregister(parent);
destroy_class:
	class_destroy(display_class);
	return ret;
}

static void n12_42_unregister_panel_info(struct n12_42 *ctx)
{
	struct device *parent = ctx->panel_info_dev->parent;
	const struct class *display_class = parent->class;

	device_unregister(ctx->panel_info_dev);
	device_unregister(parent);
	class_destroy(display_class);
}

static const struct drm_display_mode * const n12_42_modes[] = {
	&n12_42_default_mode, &n12_42_middle_mode,
	&n12_42_performence_mode, &n12_42_performence_mode_144,
};

/* Stock consumer: ESD +0xa48, colour +0xa94, dimensions +0xad8/adc,
 * output +0xcc8, DBI +0xee92, TE +0x102fc, brightness delay +0x10304.
 * Pointer-bearing DSC PPS is rebuilt for the current consumer, not copied raw.
 */
#define N12_42_PARAMS(te_us, bl_delay_us) { \
	.pll_clk = 560, .data_rate = 1120, \
	.lcm_esd_check_table = { { .cmd = 0x0a, .count = 1, .para_list = { 0x1c } } }, \
	.lcm_color_mode = 9, \
	.physical_width_um = 69540, .physical_height_um = 154584, \
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT, \
	.is_support_dbi = true, .real_te_duration = (te_us), \
	.SilkyBrightnessDelay = (bl_delay_us), \
}
static struct mtk_panel_params n12_42_params[] = {
	N12_42_PARAMS(16666, 3000), N12_42_PARAMS(11111, 0),
	N12_42_PARAMS(8333, 0), N12_42_PARAMS(6944, 0),
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

static int n12_42_unprepare(struct drm_panel *panel)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);

	if (!ctx->prepared)
		return 0;
	n12_42_sleep_in(ctx);
	ctx->doze_active = false;
	ctx->doze_suspended = false;
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
	if (is_mi_dev_support_nova())
		vis_dsi_rate_notify(find_panel_ext(panel)->params->data_rate * 1000000U);
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
	if (is_mi_dev_support_nova())
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
	if (ctx->doze_active)
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

static int n12_doze_enable(struct drm_panel *panel, void *dsi_drv,
			   dcs_write_gce cb, void *handle)
{
	n12_42_from_panel(panel)->doze_active = true;
	return 0;
}

static int n12_doze_disable(struct drm_panel *panel, void *dsi_drv,
			    dcs_write_gce cb, void *handle)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	static const u8 normal[] = { 0x38, 0x00 };
	static const u8 dimming[] = { 0x5a, 0x01 };
	static const u8 te[] = { 0x6d, 0x00 };
	u8 rate[] = { 0x2f, 0x00 };

	cb(dsi_drv, handle, normal, sizeof(normal));
	switch (ctx->refresh_rate) {
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

reset_doze:
	ctx->doze_suspended = false;
	ctx->doze_active = false;
	return 0;
}

static int n12_doze_suspend(struct drm_panel *panel, void *dsi_drv,
			    dcs_write_gce cb, void *handle)
{
	struct n12_42 *ctx = n12_42_from_panel(panel);
	static const u8 idle[] = { 0x39, 0x00 };
	static const u8 rate_42[] = { 0x2f, 0x00 };
	static const u8 rate_36[] = { 0x2f, 0x01 };
	static const u8 brightness_42[] = { 0x51, 0x00, 0x3d, 0x00, 0x3d, 0x05, 0x55 };
	static const u8 brightness_36[] = { 0x51, 0x00, 0x3d, 0x00, 0x3d, 0x07, 0xfc };

	if (ctx->doze_suspended)
		return 0;

	if (ctx->variant_36) {
		cb(dsi_drv, handle, rate_36, sizeof(rate_36));
		cb(dsi_drv, handle, brightness_36, sizeof(brightness_36));
		cb(dsi_drv, handle, idle, sizeof(idle));
	} else {
		cb(dsi_drv, handle, rate_42, sizeof(rate_42));
		cb(dsi_drv, handle, idle, sizeof(idle));
		cb(dsi_drv, handle, brightness_42, sizeof(brightness_42));
	}
	ctx->doze_suspended = true;
	return 0;
}

static const struct drm_panel_funcs n12_42_panel_funcs = {
	.prepare = n12_42_prepare, .unprepare = n12_42_unprepare,
	.enable = n12_42_enable, .disable = n12_42_disable,
	.get_modes = n12_42_get_modes,
};
static struct mtk_panel_funcs n12_42_ext_funcs = {
	.reset = n12_42_ext_reset,
	.panel_poweron = n12_42_panel_poweron,
	.panel_poweroff = n12_42_panel_poweroff,
	.ext_param_set = n12_42_ext_param_set,
	.ext_param_get = n12_42_ext_param_get,
	.mode_switch = n12_42_mode_switch,
	.set_backlight_cmdq = n12_42_set_backlight,
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
