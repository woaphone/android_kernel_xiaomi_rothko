// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 MediaTek Inc.
 * Battery ioctl ABI and gauge property mapping from MediaTek MBrain.
 */

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/uaccess.h>

#include "mbraink_ioctl_struct_def.h"

/* This vendor command encodes the pointer width, not the payload size. */
#define RO_BATTERY_INFO _IOR('k', 'e', struct mbraink_battery_data *)
#define COMPAT_RO_BATTERY_INFO _IOR('k', 'e', compat_uptr_t)

static_assert(sizeof(struct mbraink_battery_data) == 24);

static dev_t mbraink_devt;
static struct cdev mbraink_cdev;
static const struct class mbraink_class = {
	.name = "mbraink_host",
};

static int mbraink_read_battery(struct mbraink_battery_data *data)
{
	const struct {
		enum power_supply_property prop;
		int *value;
	} fields[] = {
		{ POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN, &data->qmaxt },
		{ POWER_SUPPLY_PROP_ENERGY_FULL, &data->quse },
		{ POWER_SUPPLY_PROP_ENERGY_NOW, &data->precise_soc },
		{ POWER_SUPPLY_PROP_CAPACITY_LEVEL, &data->precise_uisoc },
	};
	struct power_supply *psy;
	unsigned int i;
	int ret = 0;

	psy = power_supply_get_by_name("mtk-gauge");
	if (!psy)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		union power_supply_propval val = { .intval = INT_MIN };

		ret = power_supply_get_property(psy, fields[i].prop, &val);
		if (ret)
			break;

		/* The gauge leaves val untouched until its battery data exists. */
		if (val.intval == INT_MIN) {
			ret = -ENODATA;
			break;
		}
		*fields[i].value = val.intval;
	}
	power_supply_put(psy);

	return ret;
}

static long mbraink_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mbraink_battery_data data = {};
	int ret;

	if (cmd != RO_BATTERY_INFO)
		return -ENOTTY;

	/* The original direct query leaves the suspend timestamp at zero. */
	ret = mbraink_read_battery(&data);
	if (ret)
		return ret;

	return copy_to_user((void __user *)arg, &data, sizeof(data)) ? -EFAULT : 0;
}

#ifdef CONFIG_COMPAT
static long mbraink_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	if (cmd == COMPAT_RO_BATTERY_INFO)
		cmd = RO_BATTERY_INFO;

	return mbraink_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations mbraink_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mbraink_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mbraink_compat_ioctl,
#endif
};

static int __init mbraink_init(void)
{
	struct device *dev;
	int ret;

	ret = alloc_chrdev_region(&mbraink_devt, 0, 1, "mbraink");
	if (ret)
		return ret;

	ret = class_register(&mbraink_class);
	if (ret)
		goto unregister;

	cdev_init(&mbraink_cdev, &mbraink_fops);
	mbraink_cdev.owner = THIS_MODULE;
	ret = cdev_add(&mbraink_cdev, mbraink_devt, 1);
	if (ret)
		goto unregister_class;

	dev = device_create(&mbraink_class, NULL, mbraink_devt, NULL, "mbraink");
	if (!IS_ERR(dev))
		return 0;

	ret = PTR_ERR(dev);
	cdev_del(&mbraink_cdev);
 unregister_class:
	class_unregister(&mbraink_class);
 unregister:
	unregister_chrdev_region(mbraink_devt, 1);
	return ret;
}

static void __exit mbraink_exit(void)
{
	device_destroy(&mbraink_class, mbraink_devt);
	cdev_del(&mbraink_cdev);
	class_unregister(&mbraink_class);
	unregister_chrdev_region(mbraink_devt, 1);
}

module_init(mbraink_init);
module_exit(mbraink_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MBrain battery telemetry");
