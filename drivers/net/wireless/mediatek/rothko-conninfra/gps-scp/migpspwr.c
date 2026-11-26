// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 - 2023 Xiaomi Inc. */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include <conn_power_throttling.h>

#define MI_GPS_PWR_DEVNAME "mi_gps_pwr"

static int mi_gps_pwr_major;
static struct class *mi_gps_pwr_class;
static DEFINE_MUTEX(mi_gps_pwr_lock);
static char mi_gps_pwr_request[10] = "0";

static ssize_t sys_device_file_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	ssize_t len;

	mutex_lock(&mi_gps_pwr_lock);
	len = sysfs_emit(buf, "%s\n", mi_gps_pwr_request);
	mutex_unlock(&mi_gps_pwr_lock);
	return len;
}

static ssize_t sys_device_file_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	enum conn_pwr_low_battery_level level;

	if (count >= sizeof(mi_gps_pwr_request))
		return -E2BIG;

	level = simple_strtol(buf, NULL, 10) == 1 ?
		CONN_PWR_THR_LV_4 : CONN_PWR_THR_LV_0;

	mutex_lock(&mi_gps_pwr_lock);
	if (conn_pwr_set_customer_level(CONN_PWR_DRV_GPS, level) < 0)
		dev_info(dev, "GPS power level %d request failed\n", level);
	/* The stock attribute reads back the last request, even if rate limited. */
	memcpy(mi_gps_pwr_request, buf, count);
	mi_gps_pwr_request[count] = 0;
	mutex_unlock(&mi_gps_pwr_lock);

	return count;
}

static DEVICE_ATTR(sys_device_file, 0664,
		   sys_device_file_show, sys_device_file_store);

static struct attribute *mi_gps_pwr_attrs[] = {
	&dev_attr_sys_device_file.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mi_gps_pwr);

static const struct file_operations mi_gps_pwr_fops = {
	.owner = THIS_MODULE,
};

static int __init mi_gps_pwr_init(void)
{
	struct device *dev;
	int ret;

	mi_gps_pwr_major = register_chrdev(0, MI_GPS_PWR_DEVNAME,
					   &mi_gps_pwr_fops);
	if (mi_gps_pwr_major < 0)
		return mi_gps_pwr_major;

	mi_gps_pwr_class = class_create(MI_GPS_PWR_DEVNAME);
	if (IS_ERR(mi_gps_pwr_class)) {
		ret = PTR_ERR(mi_gps_pwr_class);
		goto err_unregister_chrdev;
	}

	dev = device_create_with_groups(mi_gps_pwr_class, NULL,
					MKDEV(mi_gps_pwr_major, 0), NULL,
					mi_gps_pwr_groups, MI_GPS_PWR_DEVNAME);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_destroy_class;
	}

	return 0;

err_destroy_class:
	class_destroy(mi_gps_pwr_class);
err_unregister_chrdev:
	unregister_chrdev(mi_gps_pwr_major, MI_GPS_PWR_DEVNAME);
	return ret;
}
late_initcall(mi_gps_pwr_init);

static void __exit mi_gps_pwr_exit(void)
{
	device_destroy(mi_gps_pwr_class, MKDEV(mi_gps_pwr_major, 0));
	class_destroy(mi_gps_pwr_class);
	unregister_chrdev(mi_gps_pwr_major, MI_GPS_PWR_DEVNAME);
}
module_exit(mi_gps_pwr_exit);

MODULE_AUTHOR("Chen Yucheng <chenyucheng@xiaomi.com>");
MODULE_DESCRIPTION("MI_GPS_PWR dev");
MODULE_LICENSE("GPL");
