// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi touch v3 userspace transport backed by the bound touch controller. */
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "xiaomi_touch_common.h"

struct xiaomi_touch_backend {
	struct class *class;
	struct device *sysdev;
	struct miscdevice misc;
	const struct xiaomi_touch_ops *ops;
	void *data;
	u32 factor;
	bool fod_pressed;
};

/* Also serializes unbind against open descriptors and backend requests. */
static DEFINE_MUTEX(xiaomi_touch_lock);
/* FOD reports can be made from a backend request holding xiaomi_touch_lock. */
static DEFINE_MUTEX(xiaomi_touch_fod_lock);
static struct xiaomi_touch_backend *xiaomi_touch;

static ssize_t resolution_factor_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_backend *backend = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", backend->factor);
}
static DEVICE_ATTR_RO(resolution_factor);

static ssize_t palm_sensor_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_backend *backend = dev_get_drvdata(dev);
	u8 palm;
	int ret;

	ret = backend->ops->read_palm(backend->data, &palm);
	return ret ? ret : sysfs_emit(buf, "%u\n", palm);
}

static ssize_t palm_sensor_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct xiaomi_touch_backend *backend = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	ret = backend->ops->write_palm(backend->data, enable);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(palm_sensor);

static ssize_t fod_press_status_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_backend *backend = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(backend->fod_pressed));
}
static DEVICE_ATTR_RO(fod_press_status);

static struct attribute *xiaomi_touch_attrs[] = {
	&dev_attr_resolution_factor.attr,
	&dev_attr_palm_sensor.attr,
	&dev_attr_fod_press_status.attr,
	NULL,
};
static umode_t xiaomi_touch_attr_is_visible(struct kobject *kobj,
					   struct attribute *attr, int index)
{
	struct xiaomi_touch_backend *backend = dev_get_drvdata(kobj_to_dev(kobj));

	if (attr == &dev_attr_palm_sensor.attr && !backend->ops->write_palm)
		return 0444;
	if (attr == &dev_attr_fod_press_status.attr && !backend->ops->reports_fod)
		return 0;
	return attr->mode;
}

static const struct attribute_group xiaomi_touch_group = {
	.attrs = xiaomi_touch_attrs,
	.is_visible = xiaomi_touch_attr_is_visible,
};
__ATTRIBUTE_GROUPS(xiaomi_touch);

static long xiaomi_touch_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct xiaomi_touch_request request;
	int ret;

	if (_IOC_TYPE(cmd) != 'T' || _IOC_NR(cmd) != 0)
		return -ENOTTY;
	if (_IOC_SIZE(cmd) != sizeof(request))
		return -EINVAL;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.touch_id || request.length > ARRAY_SIZE(request.values))
		return -EINVAL;
	mutex_lock(&xiaomi_touch_lock);
	if (!xiaomi_touch)
		ret = -ENODEV;
	else if (!xiaomi_touch->ops->request)
		ret = -EOPNOTSUPP;
	else
		ret = xiaomi_touch->ops->request(xiaomi_touch->data, &request);
	mutex_unlock(&xiaomi_touch_lock);
	if (ret)
		return ret;
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations xiaomi_touch_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = xiaomi_touch_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static void xiaomi_touch_unregister(void *arg)
{
	struct xiaomi_touch_backend *backend = arg;

	misc_deregister(&backend->misc);
	mutex_lock(&xiaomi_touch_lock);
	mutex_lock(&xiaomi_touch_fod_lock);
	xiaomi_touch = NULL;
	mutex_unlock(&xiaomi_touch_fod_lock);
	mutex_unlock(&xiaomi_touch_lock);
	device_unregister(backend->sysdev);
	class_destroy(backend->class);
}

int devm_xiaomi_touch_register(struct device *dev,
			      const struct xiaomi_touch_ops *ops,
			      void *data, u32 resolution_factor)
{
	struct xiaomi_touch_backend *backend;
	int ret;

	backend = devm_kzalloc(dev, sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return -ENOMEM;
	backend->ops = ops;
	backend->data = data;
	backend->factor = resolution_factor;
	backend->misc.minor = MISC_DYNAMIC_MINOR;
	backend->misc.name = "xiaomi-touch";
	backend->misc.fops = &xiaomi_touch_fops;
	backend->misc.parent = dev;

	mutex_lock(&xiaomi_touch_lock);
	if (xiaomi_touch) {
		ret = -EBUSY;
		goto out_unlock;
	}
	backend->class = class_create("touch");
	if (IS_ERR(backend->class)) {
		ret = PTR_ERR(backend->class);
		goto out_unlock;
	}
	/* Keep the vendor genfscon path under /devices/virtual/touch. */
	backend->sysdev = device_create_with_groups(backend->class, NULL, 0,
						   backend, xiaomi_touch_groups,
						   "touch_dev");
	if (IS_ERR(backend->sysdev)) {
		ret = PTR_ERR(backend->sysdev);
		class_destroy(backend->class);
		goto out_unlock;
	}
	ret = misc_register(&backend->misc);
	if (ret) {
		device_unregister(backend->sysdev);
		class_destroy(backend->class);
		goto out_unlock;
	}
	mutex_lock(&xiaomi_touch_fod_lock);
	xiaomi_touch = backend;
	mutex_unlock(&xiaomi_touch_fod_lock);
	mutex_unlock(&xiaomi_touch_lock);
	return devm_add_action_or_reset(dev, xiaomi_touch_unregister, backend);

out_unlock:
	mutex_unlock(&xiaomi_touch_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(devm_xiaomi_touch_register);

int xiaomi_touch_panel_blank(bool blank)
{
	int ret = 0;

	mutex_lock(&xiaomi_touch_lock);
	if (xiaomi_touch && xiaomi_touch->ops->panel_blank)
		ret = xiaomi_touch->ops->panel_blank(xiaomi_touch->data, blank);
	mutex_unlock(&xiaomi_touch_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xiaomi_touch_panel_blank);

void xiaomi_touch_notify_palm(struct device *dev)
{
	mutex_lock(&xiaomi_touch_lock);
	if (xiaomi_touch && xiaomi_touch->misc.parent == dev)
		sysfs_notify(&xiaomi_touch->sysdev->kobj, NULL, "palm_sensor");
	mutex_unlock(&xiaomi_touch_lock);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_notify_palm);

void xiaomi_touch_report_fod(struct device *dev, bool pressed)
{
	struct xiaomi_touch_backend *backend;

	mutex_lock(&xiaomi_touch_fod_lock);
	backend = xiaomi_touch;
	if (backend && backend->misc.parent == dev && backend->ops->reports_fod &&
	    backend->fod_pressed != pressed) {
		WRITE_ONCE(backend->fod_pressed, pressed);
		sysfs_notify(&backend->sysdev->kobj, NULL, "fod_press_status");
	}
	mutex_unlock(&xiaomi_touch_fod_lock);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_report_fod);

MODULE_DESCRIPTION("Xiaomi touch v3 transport and controller-backed status");
MODULE_LICENSE("GPL");
