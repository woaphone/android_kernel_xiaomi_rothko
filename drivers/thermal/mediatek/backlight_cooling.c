// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2020 MediaTek Inc. */

#include <linux/leds-mtk.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define BACKLIGHT_MAX_STATE	100

struct backlight_cooling {
	unsigned long state;
};

static int backlight_cooling_apply(struct backlight_cooling *cooler, unsigned long state)
{
	int ret;

	ret = setMaxBrightness(-1, BACKLIGHT_MAX_STATE - state, state != 0);
	if (ret)
		return ret;

	WRITE_ONCE(cooler->state, state);
	return 0;
}

static int backlight_cooling_max(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = BACKLIGHT_MAX_STATE;
	return 0;
}

static int backlight_cooling_get(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct backlight_cooling *cooler = cdev->devdata;

	*state = READ_ONCE(cooler->state);
	return 0;
}

static int backlight_cooling_set(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct backlight_cooling *cooler = cdev->devdata;

	if (state > BACKLIGHT_MAX_STATE)
		return -EINVAL;
	if (state == cooler->state)
		return 0;
	return backlight_cooling_apply(cooler, state);
}

static const struct thermal_cooling_device_ops backlight_cooling_ops = {
	.get_max_state = backlight_cooling_max,
	.get_cur_state = backlight_cooling_get,
	.set_cur_state = backlight_cooling_set,
};

static void backlight_cooling_release(void *data)
{
	struct backlight_cooling *cooler = data;

	if (cooler->state)
		backlight_cooling_apply(cooler, 0);
}

static int backlight_cooling_link(struct device *dev, const char *name)
{
	struct platform_device *supplier = NULL;
	struct device_node *node;
	const char *label;
	int ret = -EPROBE_DEFER;

	for_each_compatible_node(node, NULL, "mediatek,disp-leds") {
		if (!of_device_is_available(node))
			continue;
		for_each_available_child_of_node_scoped(node, child) {
			if (!of_property_read_string(child, "label", &label) &&
			    !strcmp(name, label)) {
				supplier = of_find_device_by_node(node);
				of_node_put(node);
				goto found;
			}
		}
	}
	return -ENODEV;

found:
	if (!supplier)
		return -EPROBE_DEFER;
	if (device_trylock(&supplier->dev)) {
		if (device_is_bound(&supplier->dev))
			ret = device_link_add(dev, &supplier->dev, DL_FLAG_AUTOREMOVE_CONSUMER) ?
				0 : -EINVAL;
		device_unlock(&supplier->dev);
	}
	put_device(&supplier->dev);
	return ret;
}

static int backlight_cooling_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_cooling *cooler;
	struct thermal_cooling_device *cdev;
	const char *name;
	int ret;

	ret = of_property_read_string(dev->of_node, "backlight-names", &name);
	if (ret)
		return dev_err_probe(dev, ret, "missing backlight-names\n");
	if (strcmp(name, "lcd-backlight"))
		return dev_err_probe(dev, -ENODEV, "unsupported backlight %s\n", name);
	ret = backlight_cooling_link(dev, name);
	if (ret)
		return dev_err_probe(dev, ret, "linking to %s provider\n", name);

	cooler = devm_kzalloc(dev, sizeof(*cooler), GFP_KERNEL);
	if (!cooler)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, backlight_cooling_release, cooler);
	if (ret)
		return ret;

	cdev = devm_thermal_of_cooling_device_register(dev, dev->of_node, name,
						       cooler, &backlight_cooling_ops);
	return PTR_ERR_OR_ZERO(cdev);
}

static const struct of_device_id backlight_cooling_of_match[] = {
	{ .compatible = "mediatek,backlight-cooler" },
	{ }
};
MODULE_DEVICE_TABLE(of, backlight_cooling_of_match);

static struct platform_driver backlight_cooling_driver = {
	.probe = backlight_cooling_probe,
	.driver = {
		.name = "mtk-backlight-cooling",
		.of_match_table = backlight_cooling_of_match,
	},
};
module_platform_driver(backlight_cooling_driver);

MODULE_SOFTDEP("pre: leds_mtk_disp");
MODULE_AUTHOR("Henry Huang <henry.huang@mediatek.com>");
MODULE_DESCRIPTION("MediaTek backlight cooling driver");
MODULE_LICENSE("GPL");
