// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2020 MediaTek Inc. */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>

struct charger_cooling {
	struct power_supply *psy;
	unsigned long state;
};

/* MT6375 charge current limits in microamps; -1 releases the limit. */
static const int charger_limits_ua[] = {
	-1, 5200000, 4600000, 4200000, 3800000, 3400000, 3000000, 2600000,
	2200000, 1800000, 1400000, 1000000, 700000, 500000, 0,
};

static int charger_cooling_apply(struct charger_cooling *cooler, unsigned long state)
{
	const union power_supply_propval val = { .intval = charger_limits_ua[state] };
	int ret;

	if (cooler->state == state)
		return 0;

	ret = power_supply_set_property(cooler->psy,
					POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, &val);
	if (ret)
		return ret;

	WRITE_ONCE(cooler->state, state);
	power_supply_changed(cooler->psy);
	return 0;
}

static int charger_cooling_max(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = ARRAY_SIZE(charger_limits_ua) - 1;
	return 0;
}

static int charger_cooling_get(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct charger_cooling *cooler = cdev->devdata;

	*state = READ_ONCE(cooler->state);
	return 0;
}

static int charger_cooling_set(struct thermal_cooling_device *cdev, unsigned long state)
{
	if (state >= ARRAY_SIZE(charger_limits_ua))
		return -EINVAL;
	return charger_cooling_apply(cdev->devdata, state);
}

static const struct thermal_cooling_device_ops charger_cooling_ops = {
	.get_max_state = charger_cooling_max,
	.get_cur_state = charger_cooling_get,
	.set_cur_state = charger_cooling_set,
};

static void charger_cooling_release(void *data)
{
	struct charger_cooling *cooler = data;
	int ret = charger_cooling_apply(cooler, 0);

	if (ret)
		dev_warn(&cooler->psy->dev, "failed to release charger cooling limit: %d\n", ret);
	power_supply_put(cooler->psy);
}

static int charger_cooling_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct charger_cooling *cooler;
	struct thermal_cooling_device *cdev;
	struct device *supplier;
	int ret;

	cooler = devm_kzalloc(dev, sizeof(*cooler), GFP_KERNEL);
	if (!cooler)
		return -ENOMEM;

	cooler->psy = power_supply_get_by_name("mtk-master-charger");
	if (!cooler->psy)
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for charger manager\n");
	ret = devm_add_action_or_reset(dev, charger_cooling_release, cooler);
	if (ret)
		return ret;

	supplier = cooler->psy->dev.parent;
	ret = -EPROBE_DEFER;
	if (device_trylock(supplier)) {
		if (device_is_bound(supplier))
			ret = device_link_add(dev, supplier, DL_FLAG_AUTOREMOVE_CONSUMER) ?
				0 : -EINVAL;
		device_unlock(supplier);
	}
	if (ret)
		return dev_err_probe(dev, ret, "linking to charger manager\n");
	if (!cooler->psy->desc->set_property || !cooler->psy->desc->property_is_writeable ||
	    cooler->psy->desc->property_is_writeable(cooler->psy,
						POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX) <= 0)
		return dev_err_probe(dev, -EOPNOTSUPP, "charger has no thermal current limit\n");

	cdev = devm_thermal_of_cooling_device_register(dev, dev->of_node,
						       dev->of_node->name, cooler,
						       &charger_cooling_ops);
	return PTR_ERR_OR_ZERO(cdev);
}

static const struct of_device_id charger_cooling_of_match[] = {
	{ .compatible = "mediatek,mt6375-charger-cooler" },
	{ }
};
MODULE_DEVICE_TABLE(of, charger_cooling_of_match);

static struct platform_driver charger_cooling_driver = {
	.probe = charger_cooling_probe,
	.driver = {
		.name = "mtk-charger-cooling",
		.of_match_table = charger_cooling_of_match,
	},
};
module_platform_driver(charger_cooling_driver);

MODULE_SOFTDEP("pre: mt6375_charger mtk_charger");
MODULE_AUTHOR("Henry Huang <henry.huang@mediatek.com>");
MODULE_DESCRIPTION("MediaTek MT6375 charger cooling driver");
MODULE_LICENSE("GPL");
