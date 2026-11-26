// SPDX-License-Identifier: GPL-2.0-only
/* MT6989 combined DDR/VCORE OPP selector on the firmware's helper node. */
#include <linux/arm-smccc.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/string.h>

#include "mtk-dvfsrc-rothko-helper.h"

#define DVFSRC_GET_VCORE_UV	4
#define DVFSRC_GET_DRAM_FREQ	5

struct mt6989_helper {
	struct mutex lock;
	u32 requested;
	bool suspended;
	struct seq_buf table;
};

static int mt6989_read_gear(u32 cmd, u32 gear, u32 *value)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_DVFSRC_VCOREFS_CONTROL, cmd, gear,
		      0, 0, 0, 0, 0, &res);
	if (res.a0 || !res.a1 || res.a1 > U32_MAX)
		return -EIO;
	*value = res.a1;
	return 0;
}

static int mt6989_init_table(struct device *dev, struct seq_buf *table)
{
	const struct dvfsrc_opp_desc *desc = mt6989_dvfsrc_get_opps(dev->parent);
	const struct dvfsrc_opp *highest = &desc->opps[desc->num_opp - 1];
	u32 uv[8] = {}, rate[16] = {};
	size_t size = 96 + 48 * desc->num_opp;
	char *buf;
	u32 index;
	int ret;

	buf = devm_kmalloc(dev, size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	seq_buf_init(table, buf, size);
	seq_buf_printf(table, "NUM_VCORE_OPP : %u\nNUM_DDR_OPP : %u\nNUM_DVFSRC_OPP : %u\n\n",
		       highest->vcore_opp + 1, highest->dram_opp + 1, desc->num_opp);

	/* Hardware index zero is the last entry in the parent's decoded array. */
	for (index = 0; index < desc->num_opp; index++) {
		const struct dvfsrc_opp *opp = &desc->opps[desc->num_opp - 1 - index];
		u32 *vcore = &uv[opp->vcore_opp], *dram = &rate[opp->dram_opp];

		if (!*vcore) {
			ret = mt6989_read_gear(DVFSRC_GET_VCORE_UV, opp->vcore_opp, vcore);
			if (ret)
				return ret;
		}
		if (!*dram) {
			ret = mt6989_read_gear(DVFSRC_GET_DRAM_FREQ, opp->dram_opp, dram);
			if (ret)
				return ret;
		}
		seq_buf_printf(table, "[OPP%-2u]: %-8u uv %-8u kbps\n", index, *vcore, *dram);
	}
	return seq_buf_has_overflowed(table) ? -EOVERFLOW : 0;
}

/* A full 128-level ASCII table can exceed one sysfs page. */
static ssize_t dvfsrc_opp_table_read(struct file *file, struct kobject *kobj,
				     const struct bin_attribute *attr, char *buf,
				     loff_t off, size_t count)
{
	struct mt6989_helper *helper = dev_get_drvdata(kobj_to_dev(kobj));

	return memory_read_from_buffer(buf, count, &off, helper->table.buffer,
				       seq_buf_used(&helper->table));
}
static const BIN_ATTR_RO(dvfsrc_opp_table, 0);

static ssize_t dvfsrc_force_vcore_dvfs_opp_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t count)
{
	struct mt6989_helper *helper = dev_get_drvdata(dev);
	u32 index;
	int ret;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return ret;
	if (index != MT6989_FORCE_RELEASE && index >= mt6989_dvfsrc_get_opps(dev->parent)->num_opp)
		return -EINVAL;

	mutex_lock(&helper->lock);
	if (helper->suspended) {
		ret = -EBUSY;
	} else {
		ret = mt6989_dvfsrc_force_opp(dev->parent, index);
		helper->requested = ret ? MT6989_FORCE_RELEASE : index;
	}
	mutex_unlock(&helper->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(dvfsrc_force_vcore_dvfs_opp);

static struct attribute *mt6989_helper_attrs[] = {
	&dev_attr_dvfsrc_force_vcore_dvfs_opp.attr,
	NULL,
};

static const struct bin_attribute *const mt6989_helper_bin_attrs[] = {
	&bin_attr_dvfsrc_opp_table,
	NULL,
};

static size_t mt6989_table_size(struct kobject *kobj, const struct bin_attribute *attr, int n)
{
	struct mt6989_helper *helper = dev_get_drvdata(kobj_to_dev(kobj));

	return seq_buf_used(&helper->table);
}

static const struct attribute_group mt6989_helper_group = {
	.attrs = mt6989_helper_attrs,
	.bin_attrs = mt6989_helper_bin_attrs,
	.bin_size = mt6989_table_size,
};
__ATTRIBUTE_GROUPS(mt6989_helper);

static int mt6989_helper_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6989_helper *helper;
	int ret;

	if (!dev->parent || !of_device_is_compatible(dev->parent->of_node, "mediatek,mt6989-dvfsrc"))
		return -ENODEV;
	if (!dev_get_drvdata(dev->parent))
		return -EPROBE_DEFER;

	helper = devm_kzalloc(dev, sizeof(*helper), GFP_KERNEL);
	if (!helper)
		return -ENOMEM;
	mutex_init(&helper->lock);
	helper->requested = MT6989_FORCE_RELEASE;
	ret = mt6989_init_table(dev, &helper->table);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read combined firmware OPP table\n");
	platform_set_drvdata(pdev, helper);
	return 0;
}

static int mt6989_helper_suspend(struct device *dev)
{
	struct mt6989_helper *helper = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&helper->lock);
	ret = mt6989_dvfsrc_force_opp(dev->parent, MT6989_FORCE_RELEASE);
	if (!ret)
		helper->suspended = true;
	else
		helper->requested = MT6989_FORCE_RELEASE;
	mutex_unlock(&helper->lock);
	return ret;
}

static int mt6989_helper_resume(struct device *dev)
{
	struct mt6989_helper *helper = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&helper->lock);
	ret = mt6989_dvfsrc_force_opp(dev->parent, helper->requested);
	if (ret)
		helper->requested = MT6989_FORCE_RELEASE;
	helper->suspended = false;
	mutex_unlock(&helper->lock);
	return ret;
}
static DEFINE_SIMPLE_DEV_PM_OPS(mt6989_helper_pm, mt6989_helper_suspend, mt6989_helper_resume);

static void mt6989_helper_stop(struct platform_device *pdev)
{
	struct mt6989_helper *helper = platform_get_drvdata(pdev);

	mutex_lock(&helper->lock);
	helper->suspended = true;
	helper->requested = MT6989_FORCE_RELEASE;
	if (mt6989_dvfsrc_force_opp(pdev->dev.parent, MT6989_FORCE_RELEASE))
		dev_warn(&pdev->dev, "timed out releasing forced OPP\n");
	mutex_unlock(&helper->lock);
}

static const struct of_device_id mt6989_helper_match[] = {
	{ .compatible = "mediatek,dvfsrc-helper" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6989_helper_match);

static struct platform_driver mt6989_helper_driver = {
	.probe = mt6989_helper_probe,
	.remove = mt6989_helper_stop,
	.shutdown = mt6989_helper_stop,
	.driver = {
		.name = "mt6989-dvfsrc-helper",
		.of_match_table = mt6989_helper_match,
		.dev_groups = mt6989_helper_groups,
		.pm = pm_sleep_ptr(&mt6989_helper_pm),
	},
};
module_platform_driver(mt6989_helper_driver);

MODULE_DESCRIPTION("MediaTek MT6989 combined DDR/VCORE OPP selector");
MODULE_LICENSE("GPL");
