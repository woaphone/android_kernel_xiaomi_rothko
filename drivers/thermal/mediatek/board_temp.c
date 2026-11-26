// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2021 MediaTek Inc. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#define MT6685_TIA_ADC		GENMASK(14, 0)
#define MT6685_TIA_VALID	BIT(15)
#define MT6685_TIA_RSEL		GENMASK(17, 16)
#define MT6685_TIA_PULLUP_V	184000
#define MT6685_TIA_READS	3

struct board_ntc {
	void __iomem *data;
	int *table;
	int num_pairs;
};

static const unsigned int mt6685_pullup_ohms[] = { 100000, 30000, 400000 };

static int board_ntc_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct board_ntc *ntc = thermal_zone_device_priv(tz);
	const int *table = ntc->table;
	unsigned int val, sel, voltage;
	u64 resistance;
	int i;

	for (i = 0; i < MT6685_TIA_READS; i++) {
		val = readl(ntc->data);
		sel = FIELD_GET(MT6685_TIA_RSEL, val);
		if ((val & MT6685_TIA_VALID) && sel < ARRAY_SIZE(mt6685_pullup_ohms))
			break;
		if (i < MT6685_TIA_READS - 1)
			usleep_range(1000, 1100);
	}
	if (!(val & MT6685_TIA_VALID))
		return -EAGAIN;
	if (sel >= ARRAY_SIZE(mt6685_pullup_ohms))
		return -EINVAL;

	voltage = ((u64)FIELD_GET(MT6685_TIA_ADC, val) * MT6685_TIA_PULLUP_V) >> 15;
	if (!voltage)
		return -ERANGE;
	resistance = div_u64((u64)voltage * mt6685_pullup_ohms[sel],
			     MT6685_TIA_PULLUP_V - voltage);

	for (i = 0; i < ntc->num_pairs; i++) {
		if (resistance >= table[2 * i + 1])
			break;
	}

	if (i == 0) {
		*temp = table[0];
	} else if (i == ntc->num_pairs) {
		*temp = table[2 * (i - 1)];
	} else {
		int r_high = table[2 * i - 1];
		int r_low = table[2 * i + 1];
		int t_cold = table[2 * i - 2];
		int t_hot = table[2 * i];

		*temp = t_cold + div_s64(((s64)t_hot - t_cold) *
					(r_high - (int)resistance), r_high - r_low);
	}

	return 0;
}

static const struct thermal_zone_device_ops board_ntc_ops = {
	.get_temp = board_ntc_get_temp,
};

static int board_ntc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tz;
	struct board_ntc *ntc;
	int count, i, ret;

	count = of_property_count_u32_elems(dev->of_node, "temperature-lookup-table");
	if (count < 0)
		return count;
	if (count < 2 || count % 2)
		return -EINVAL;

	ntc = devm_kzalloc(dev, sizeof(*ntc), GFP_KERNEL);
	if (!ntc)
		return -ENOMEM;
	ntc->table = devm_kcalloc(dev, count, sizeof(*ntc->table), GFP_KERNEL);
	if (!ntc->table)
		return -ENOMEM;
	ret = of_property_read_u32_array(dev->of_node, "temperature-lookup-table",
					 (u32 *)ntc->table, count);
	if (ret)
		return ret;

	/* Interpolation requires positive, strictly descending resistances. */
	for (i = 1; i < count; i += 2) {
		if (ntc->table[i] <= 0 || (i > 1 && ntc->table[i] >= ntc->table[i - 2]))
			return -EINVAL;
	}
	ntc->num_pairs = count / 2;

	ntc->data = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ntc->data))
		return PTR_ERR(ntc->data);

	tz = devm_thermal_of_zone_register(dev, 0, ntc, &board_ntc_ops);
	return PTR_ERR_OR_ZERO(tz);
}

static const struct of_device_id board_ntc_of_match[] = {
	{ .compatible = "mediatek,mt6685-tia-ntc" },
	{ }
};
MODULE_DEVICE_TABLE(of, board_ntc_of_match);

static struct platform_driver board_ntc_driver = {
	.probe = board_ntc_probe,
	.driver = {
		.name = "mtk-board-ntc",
		.of_match_table = board_ntc_of_match,
	},
};
module_platform_driver(board_ntc_driver);

MODULE_DESCRIPTION("MediaTek MT6685 board thermistor driver");
MODULE_LICENSE("GPL");
