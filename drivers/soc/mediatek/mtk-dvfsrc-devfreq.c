// SPDX-License-Identifier: GPL-2.0-only
#include <linux/arm-smccc.h>
#include <linux/devfreq.h>
#include <linux/kobject.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/soc/mediatek/dvfsrc.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <soc/mediatek/dramc.h>

/* MT6989 firmware enumerates gears from lowest to highest. */
#define DVFSRC_DDR_GET_FREQ_INFO		5
#define DVFSRC_DDR_GET_FREQ_COUNT	7
#define DVFSRC_DDR_MAX_GEARS		12

struct mtk_dvfsrc_devfreq {
	struct devfreq_dev_profile profile;
	unsigned long rates[DVFSRC_DDR_MAX_GEARS];
	unsigned int num_gears;
};

static int mtk_ddr_get_cur_freq(struct device *dev, unsigned long *freq)
{
	/* Match the firmware OPP table in Hz. DRAMC returns MT/s. */
	*freq = (unsigned long)mtk_dramc_get_data_rate() * 1000000UL;
	return *freq ? 0 : -EAGAIN;
}

static int mtk_ddr_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct mtk_dvfsrc_devfreq *ddr = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned int gear;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	for (gear = 0; gear < ddr->num_gears; gear++) {
		if (ddr->rates[gear] == *freq)
			return mtk_dvfsrc_send_request(dev->parent,
						      MTK_DVFSRC_CMD_DRAM_GEAR, gear);
	}
	return -EINVAL;
}

static void mtk_ddr_remove_opps(void *data)
{
	dev_pm_opp_remove_all_dynamic(data);
}

static void mtk_ddr_clear_request(void *data)
{
	struct device *dev = data;

	mtk_dvfsrc_send_request(dev->parent, MTK_DVFSRC_CMD_DRAM_GEAR, 0);
}

static int mtk_ddr_read_opps(struct device *dev, struct mtk_dvfsrc_devfreq *ddr)
{
	struct arm_smccc_res res;
	unsigned int gear;
	int ret;

	arm_smccc_smc(MTK_SIP_DVFSRC_VCOREFS_CONTROL, DVFSRC_DDR_GET_FREQ_COUNT,
		      0, 0, 0, 0, 0, 0, &res);
	if (res.a0 || !res.a1 || res.a1 > ARRAY_SIZE(ddr->rates))
		return -EINVAL;
	ddr->num_gears = res.a1;

	for (gear = 0; gear < ddr->num_gears; gear++) {
		arm_smccc_smc(MTK_SIP_DVFSRC_VCOREFS_CONTROL,
			      DVFSRC_DDR_GET_FREQ_INFO, gear, 0, 0, 0, 0, 0, &res);
		if (res.a0 || !res.a1 || res.a1 > ULONG_MAX / 1000)
			return -EINVAL;
		ddr->rates[gear] = res.a1 * 1000;
		if (gear && ddr->rates[gear] < ddr->rates[gear - 1])
			return -EINVAL;
		if (gear && ddr->rates[gear] == ddr->rates[gear - 1])
			continue;
		ret = dev_pm_opp_add(dev, ddr->rates[gear], 0);
		if (ret)
			return ret;
	}
	return 0;
}

/* Older MediaTek clients consume this field from the read-only dump. */
static ssize_t dvfsrc_dump_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%-10s: %-8u Mbps\n", "DDR",
			  mtk_dramc_get_data_rate());
}
static struct kobj_attribute dvfsrc_dump_attr = __ATTR_RO(dvfsrc_dump);

static void mtk_ddr_put_monitor(void *data)
{
	kobject_put(data);
}

static int mtk_ddr_register_monitor(struct device *dev)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("helio-dvfsrc", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_file(kobj, &dvfsrc_dump_attr.attr);
	if (ret) {
		kobject_put(kobj);
		return ret;
	}
	return devm_add_action_or_reset(dev, mtk_ddr_put_monitor, kobj);
}

static int mtk_ddr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_dvfsrc_devfreq *ddr;
	struct devfreq *df;
	int ret;

	if (!mtk_dramc_get_data_rate())
		return -EPROBE_DEFER;

	ddr = devm_kzalloc(dev, sizeof(*ddr), GFP_KERNEL);
	if (!ddr)
		return -ENOMEM;
	platform_set_drvdata(pdev, ddr);

	ret = devm_add_action_or_reset(dev, mtk_ddr_remove_opps, dev);
	if (ret)
		return ret;
	ret = mtk_ddr_read_opps(dev, ddr);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read firmware DDR gears\n");

	ret = devm_add_action_or_reset(dev, mtk_ddr_clear_request, dev);
	if (ret)
		return ret;
	ddr->profile.target = mtk_ddr_target;
	ddr->profile.get_cur_freq = mtk_ddr_get_cur_freq;
	ddr->profile.initial_freq = ddr->rates[0];
	df = devm_devfreq_add_device(dev, &ddr->profile, DEVFREQ_GOV_USERSPACE, NULL);
	if (IS_ERR(df))
		return dev_err_probe(dev, PTR_ERR(df), "cannot register DDR devfreq\n");

	ret = mtk_ddr_register_monitor(dev);
	if (ret)
		dev_warn(dev, "cannot create legacy DDR monitor: %d\n", ret);

	dev_info(dev, "firmware DDR gears: %u, data rate %lu-%lu\n",
		 ddr->num_gears, ddr->rates[0] / 1000,
		 ddr->rates[ddr->num_gears - 1] / 1000);
	return 0;
}

static struct platform_driver mtk_ddr_driver = {
	.probe = mtk_ddr_probe,
	.driver = {
		.name = "mtk-dvfsrc-devfreq",
	},
};
module_platform_driver(mtk_ddr_driver);
MODULE_LICENSE("GPL");
