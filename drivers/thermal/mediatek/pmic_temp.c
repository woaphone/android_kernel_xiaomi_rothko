// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2020 MediaTek Inc. */

#include <linux/bitops.h>
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#define PMIC_TEMP_CHANNELS	4
#define PMIC_TEMP_FACTOR	1863
#define PMIC_TEMP_PULLUP_MV	1840

struct pmic_temp_variant {
	const char *cell_name;
	size_t cell_size;
	const char *channels[PMIC_TEMP_CHANNELS];
};

struct pmic_temp_sensor {
	struct iio_channel *channel;
	int intercept;
};

static int pmic_temp_get(struct thermal_zone_device *tz, int *temp)
{
	struct pmic_temp_sensor *sensor = thermal_zone_device_priv(tz);
	int mv, ret;

	ret = iio_read_channel_processed(sensor->channel, &mv);
	if (ret < 0)
		return ret;

	*temp = sensor->intercept - div_s64((s64)mv * 1000000, PMIC_TEMP_FACTOR);
	return 0;
}

static const struct thermal_zone_device_ops pmic_temp_ops = {
	.get_temp = pmic_temp_get,
};

static int pmic_temp_read_efuse(struct device *dev,
			       const struct pmic_temp_variant *variant,
			       struct pmic_temp_sensor *sensors)
{
	int vts[PMIC_TEMP_CHANNELS] = { 1600, 1600, 1600, 1600 };
	struct nvmem_cell *cell;
	int half_degc = 50;
	size_t len;
	u8 *cal;
	int i;

	cell = devm_nvmem_cell_get(dev, variant->cell_name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	cal = nvmem_cell_read(cell, &len);
	if (IS_ERR(cal))
		return PTR_ERR(cal);
	if (len != variant->cell_size) {
		kfree(cal);
		return -EINVAL;
	}

	/* The MT6373 donor reads this same packed layout in the OTP shadow. */
	if (cal[0] & BIT(6)) {
		half_degc = cal[0] & GENMASK(5, 0);
		if (half_degc < 38 || half_degc > 60)
			half_degc = 53;
		vts[0] = cal[1] | ((cal[2] & GENMASK(4, 0)) << 8);
		vts[1] = cal[3] | ((cal[2] & GENMASK(7, 5)) << 3) |
			 ((cal[5] & GENMASK(6, 5)) << 6);
		vts[2] = cal[4] | ((cal[5] & GENMASK(4, 0)) << 8);
		vts[3] = cal[6] | ((cal[7] & GENMASK(4, 0)) << 8);
	}

	kfree(cal);
	for (i = 0; i < PMIC_TEMP_CHANNELS; i++) {
		int mv = vts[i] * PMIC_TEMP_PULLUP_MV / 4096;

		sensors[i].intercept = half_degc * 500 +
			div_s64((s64)mv * 1000000, PMIC_TEMP_FACTOR);
	}

	return 0;
}

static int pmic_temp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct pmic_temp_variant *variant = device_get_match_data(dev);
	struct pmic_temp_sensor *sensors;
	struct thermal_zone_device *tz;
	int i, ret;

	sensors = devm_kcalloc(dev, PMIC_TEMP_CHANNELS, sizeof(*sensors), GFP_KERNEL);
	if (!sensors)
		return -ENOMEM;

	ret = pmic_temp_read_efuse(dev, variant, sensors);
	if (ret)
		return dev_err_probe(dev, ret, "reading %s calibration\n", variant->cell_name);

	for (i = 0; i < PMIC_TEMP_CHANNELS; i++) {
		sensors[i].channel = devm_iio_channel_get(dev, variant->channels[i]);
		if (IS_ERR(sensors[i].channel))
			return dev_err_probe(dev, PTR_ERR(sensors[i].channel),
					     "getting %s ADC channel\n", variant->channels[i]);
	}

	for (i = 0; i < PMIC_TEMP_CHANNELS; i++) {
		tz = devm_thermal_of_zone_register(dev, i, &sensors[i], &pmic_temp_ops);
		if (IS_ERR(tz))
			return dev_err_probe(dev, PTR_ERR(tz), "registering sensor %d\n", i);
	}

	return 0;
}

static const struct pmic_temp_variant mt6363_temp = {
	.cell_name = "mt6363_e_data",
	.cell_size = 8,
	.channels = { "pmic6363_ts1", "pmic6363_ts2", "pmic6363_ts3", "pmic6363_ts4" },
};

static const struct pmic_temp_variant mt6373_temp = {
	.cell_name = "mt6373_e_data",
	.cell_size = 10,
	.channels = { "pmic6373_ts1", "pmic6373_ts2", "pmic6373_ts3", "pmic6373_ts4" },
};

static const struct of_device_id pmic_temp_of_match[] = {
	{ .compatible = "mediatek,mt6363-pmic-temp", .data = &mt6363_temp },
	{ .compatible = "mediatek,mt6373-pmic-temp", .data = &mt6373_temp },
	{ }
};
MODULE_DEVICE_TABLE(of, pmic_temp_of_match);

static struct platform_driver pmic_temp_driver = {
	.probe = pmic_temp_probe,
	.driver = {
		.name = "mtk-pmic-temp",
		.of_match_table = pmic_temp_of_match,
	},
};
module_platform_driver(pmic_temp_driver);

MODULE_SOFTDEP("pre: nvmem_mt635x_efuse mtk_spmi_pmic_adc");
MODULE_AUTHOR("Henry Huang <henry.huang@mediatek.com>");
MODULE_DESCRIPTION("MediaTek MT6363/MT6373 PMIC temperature sensors");
MODULE_LICENSE("GPL");
