// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 - 2021 MediaTek Inc. */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <conn_power_throttling.h>

#define GPS_PWR_DEVNAME "gps_pwr"

static DECLARE_WAIT_QUEUE_HEAD(gps_pwr_wait);
static DEFINE_SPINLOCK(gps_pwr_lock);
static DEFINE_MUTEX(gps_pwr_open_lock);
static unsigned int gps_pwr_users;
static unsigned int gps_pwr_seq;
static unsigned int gps_pwr_read_seq;
static int gps_pwr_level;
static struct cdev gps_pwr_cdev;

static int gps_pwr_event(enum conn_pwr_event_type event, void *data)
{
	unsigned long flags;

	if (event != CONN_PWR_EVENT_LEVEL)
		return 0;

	spin_lock_irqsave(&gps_pwr_lock, flags);
	gps_pwr_level = *(enum conn_pwr_low_battery_level *)data;
	gps_pwr_seq++;
	spin_unlock_irqrestore(&gps_pwr_lock, flags);
	wake_up_interruptible(&gps_pwr_wait);
	return 0;
}

static int gps_pwr_open(struct inode *inode, struct file *file)
{
	enum conn_pwr_low_battery_level level = CONN_PWR_THR_LV_0;
	unsigned long flags;
	unsigned int seq;
	int ret = 0;

	mutex_lock(&gps_pwr_open_lock);
	spin_lock_irqsave(&gps_pwr_lock, flags);
	seq = gps_pwr_seq;
	spin_unlock_irqrestore(&gps_pwr_lock, flags);

	if (!gps_pwr_users) {
		ret = conn_pwr_drv_pre_on(CONN_PWR_DRV_GPS, &level);
		if (ret) {
			conn_pwr_drv_post_off(CONN_PWR_DRV_GPS);
			goto out;
		}
	}

	spin_lock_irqsave(&gps_pwr_lock, flags);
	/* A level callback may have run synchronously during pre_on. */
	if (!gps_pwr_users && seq == gps_pwr_seq)
		gps_pwr_level = level;
	gps_pwr_seq++;
	spin_unlock_irqrestore(&gps_pwr_lock, flags);
	gps_pwr_users++;
	wake_up_interruptible(&gps_pwr_wait);
out:
	mutex_unlock(&gps_pwr_open_lock);
	return ret;
}

static int gps_pwr_release(struct inode *inode, struct file *file)
{
	mutex_lock(&gps_pwr_open_lock);
	if (!--gps_pwr_users)
		conn_pwr_drv_post_off(CONN_PWR_DRV_GPS);
	mutex_unlock(&gps_pwr_open_lock);
	return 0;
}

static ssize_t gps_pwr_read(struct file *file, char __user *buf,
			    size_t count, loff_t *pos)
{
	unsigned long flags;
	unsigned int seq;
	int level;

	count = min(count, sizeof(level));
	if (!count)
		return 0;

	spin_lock_irqsave(&gps_pwr_lock, flags);
	seq = gps_pwr_seq;
	level = gps_pwr_level;
	spin_unlock_irqrestore(&gps_pwr_lock, flags);

	if (copy_to_user(buf, &level, count))
		return -EFAULT;

	spin_lock_irqsave(&gps_pwr_lock, flags);
	if (seq == gps_pwr_seq)
		gps_pwr_read_seq = seq;
	spin_unlock_irqrestore(&gps_pwr_lock, flags);
	return count;
}

static __poll_t gps_pwr_poll(struct file *file, poll_table *wait)
{
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(file, &gps_pwr_wait, wait);
	spin_lock_irqsave(&gps_pwr_lock, flags);
	if (gps_pwr_seq != gps_pwr_read_seq)
		mask = EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&gps_pwr_lock, flags);
	return mask;
}

/* The vendor ABI carries power levels through read/poll only. */
static long gps_pwr_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static ssize_t gps_pwr_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *pos)
{
	return 0;
}

static const struct file_operations gps_pwr_fops = {
	.owner = THIS_MODULE,
	.open = gps_pwr_open,
	.release = gps_pwr_release,
	.read = gps_pwr_read,
	.poll = gps_pwr_poll,
	.unlocked_ioctl = gps_pwr_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gps_pwr_ioctl,
#endif
	.write = gps_pwr_write,
};

static int __init gps_pwr_init(void)
{
	struct device *dev;
	struct class *class;
	dev_t devt;
	int ret;

	ret = alloc_chrdev_region(&devt, 0, 1, GPS_PWR_DEVNAME);
	if (ret)
		return ret;

	class = class_create(GPS_PWR_DEVNAME);
	if (IS_ERR(class)) {
		ret = PTR_ERR(class);
		goto unregister_region;
	}

	ret = conn_pwr_register_event_cb(CONN_PWR_DRV_GPS, gps_pwr_event);
	if (ret)
		goto destroy_class;

	cdev_init(&gps_pwr_cdev, &gps_pwr_fops);
	gps_pwr_cdev.owner = THIS_MODULE;
	ret = cdev_add(&gps_pwr_cdev, devt, 1);
	if (ret)
		goto unregister_callback;

	dev = device_create(class, NULL, devt, NULL, GPS_PWR_DEVNAME);
	if (!IS_ERR(dev))
		return 0;

	ret = PTR_ERR(dev);
	cdev_del(&gps_pwr_cdev);
unregister_callback:
	conn_pwr_register_event_cb(CONN_PWR_DRV_GPS, NULL);
destroy_class:
	class_destroy(class);
unregister_region:
	unregister_chrdev_region(devt, 1);
	return ret;
}
/* conn_pwr_init() clears its callback table during conninfra device init. */
late_initcall(gps_pwr_init);
