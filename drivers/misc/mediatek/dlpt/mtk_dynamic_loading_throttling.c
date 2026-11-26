// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/device.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/math64.h>
#include <linux/mfd/mt6363/registers.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/sort.h>

#define DLPT_VOLT_MIN_MV		3100
#define DLPT_CURRENT_MAX_MA		5500
#define DLPT_NUM_SAMPLES		5

struct rothko_dlpt {
	struct regmap *regmap;
	struct iio_channel *ptim;
	struct iio_channel *imix_r;
	struct iio_channel *batadc;
	struct mutex lock;
	s64 last_budget_ma;
	bool have_budget;
};

static int dlpt_cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a;
	int y = *(const int *)b;

	return (x > y) - (x < y);
}

static int dlpt_middle_mean(int samples[DLPT_NUM_SAMPLES])
{
	sort(samples, DLPT_NUM_SAMPLES, sizeof(*samples), dlpt_cmp_int, NULL);
	return div_s64((s64)samples[1] + samples[2] + samples[3], 3);
}

static int dlpt_discharge_budget(struct rothko_dlpt *dlpt, int imix_r,
				 s64 *budget_ma)
{
	int voltage[DLPT_NUM_SAMPLES], current_samples[DLPT_NUM_SAMPLES];
	int vbat_mv, i, ret;
	s64 headroom;

	for (i = 0; i < DLPT_NUM_SAMPLES; i++) {
		/* MT6375 PTIM returns a matched pair in mV and 0.1 mA. */
		ret = iio_read_channel_attribute(dlpt->ptim, &vbat_mv,
						 &current_samples[i],
						 IIO_CHAN_INFO_RAW);
		if (ret < 0)
			return ret;
		if (ret != IIO_VAL_INT_MULTIPLE)
			return -EPROTO;

		/* Preserve the original 0.1 mV precision before averaging. */
		voltage[i] = vbat_mv * 10;
	}

	headroom = dlpt_middle_mean(voltage) - DLPT_VOLT_MIN_MV * 10;
	*budget_ma = div_s64(dlpt_middle_mean(current_samples) +
			     div_s64(headroom * 1000, imix_r), 10);

	/* The original retains its previous budget after a negative estimate. */
	if (*budget_ma < 0) {
		if (!dlpt->have_budget)
			return -ENODATA;
		*budget_ma = dlpt->last_budget_ma;
	}

	return 0;
}

static int dlpt_read_budget(struct rothko_dlpt *dlpt, s64 *budget_ma)
{
	unsigned int status;
	int batadc_mv, imix_r, ret;
	s64 budget;

	ret = iio_read_channel_raw(dlpt->imix_r, &imix_r);
	if (ret < 0)
		return ret;
	if (ret != IIO_VAL_INT)
		return -EPROTO;
	if (!imix_r)
		return -ENODATA;
	if (imix_r < 0)
		return -ERANGE;

	ret = regmap_read(dlpt->regmap, MT6363_CHRDET_DEB_ADDR, &status);
	if (ret)
		return ret;

	if (status & (MT6363_CHRDET_DEB_MASK << MT6363_CHRDET_DEB_SHIFT)) {
		/* The 6.18 BATSNS RAW provider already returns mV. */
		ret = iio_read_channel_raw(dlpt->batadc, &batadc_mv);
		if (ret < 0)
			return ret;
		if (ret != IIO_VAL_INT)
			return -EPROTO;

		budget = div_s64(((s64)batadc_mv - DLPT_VOLT_MIN_MV) * 1000, imix_r);
		budget = div_s64(budget * 9, 10);
		if (budget < 0)
			return -ENODATA;
	} else {
		ret = dlpt_discharge_budget(dlpt, imix_r, &budget);
		if (ret)
			return ret;
	}

	*budget_ma = min_t(s64, budget, DLPT_CURRENT_MAX_MA);
	dlpt->last_budget_ma = *budget_ma;
	dlpt->have_budget = true;

	return 0;
}

static ssize_t budget_ma_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct rothko_dlpt *dlpt = dev_get_drvdata(dev);
	s64 budget;
	int ret;

	mutex_lock(&dlpt->lock);
	ret = dlpt_read_budget(dlpt, &budget);
	mutex_unlock(&dlpt->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lld\n", budget);
}
static DEVICE_ATTR_ADMIN_RO(budget_ma);

static struct attribute *rothko_dlpt_attrs[] = {
	&dev_attr_budget_ma.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rothko_dlpt);

static int rothko_dlpt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct rothko_dlpt *dlpt;

	if (!dev->parent ||
	    !of_device_is_compatible(dev->of_node, "mediatek,mt6363-dynamic_loading_throttling"))
		return -ENODEV;

	dlpt = devm_kzalloc(dev, sizeof(*dlpt), GFP_KERNEL);
	if (!dlpt)
		return -ENOMEM;

	dlpt->regmap = dev_get_regmap(dev->parent, NULL);
	if (!dlpt->regmap)
		return dev_err_probe(dev, -EPROBE_DEFER, "MT6363 regmap unavailable\n");

	dlpt->ptim = devm_fwnode_iio_channel_get_by_name(dev, fwnode, "pmic_ptim");
	if (IS_ERR(dlpt->ptim))
		return dev_err_probe(dev, PTR_ERR(dlpt->ptim), "PTIM channel unavailable\n");

	dlpt->imix_r = devm_fwnode_iio_channel_get_by_name(dev, fwnode, "pmic_imix_r");
	if (IS_ERR(dlpt->imix_r))
		return dev_err_probe(dev, PTR_ERR(dlpt->imix_r), "IMIX_R channel unavailable\n");

	dlpt->batadc = devm_fwnode_iio_channel_get_by_name(dev, fwnode, "pmic_batadc");
	if (IS_ERR(dlpt->batadc))
		return dev_err_probe(dev, PTR_ERR(dlpt->batadc), "BATADC channel unavailable\n");

	mutex_init(&dlpt->lock);
	platform_set_drvdata(pdev, dlpt);
	return 0;
}

static const struct of_device_id rothko_dlpt_of_match[] = {
	{ .compatible = "mediatek,mt6363-dynamic_loading_throttling" },
	{ }
};
MODULE_DEVICE_TABLE(of, rothko_dlpt_of_match);

static struct platform_driver rothko_dlpt_driver = {
	.probe = rothko_dlpt_probe,
	.driver = {
		.name = "mtk-dynamic-loading-throttling",
		.of_match_table = rothko_dlpt_of_match,
		.dev_groups = rothko_dlpt_groups,
	},
};
module_platform_driver(rothko_dlpt_driver);

MODULE_DESCRIPTION("MediaTek rothko dynamic loading current budget");
MODULE_LICENSE("GPL");
