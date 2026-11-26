// SPDX-License-Identifier: GPL-2.0-or-later
/* Rothko transport based on the TsingTeng MicroSystem THN31 driver (2022). */
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#define THN31_BUFSIZE	4096
#define THN31_POWER	_IOW(0xe9, 1, long)
#define THN31_ESE_POWER	_IOW(0xe9, 2, long)
#define THN31_ESE_STATE	_IOR(0xe9, 3, long)
#define THN31_IRQ_STATE	_IOW(0xe9, 12, long)

struct thn31 {
	struct i2c_client *client;
	struct gpio_desc *irq_gpio, *ven, *download;
	struct miscdevice misc;
	struct kref ref;
	struct rw_semaphore live;
	struct mutex read_lock, bus_lock;
	wait_queue_head_t readq;
	atomic_t flush_seq, armed;
	bool dead, irq_wake, nfc_power, ese_power, spi_ese_power, sysfs_link;
	int irq;
	size_t last_count;
	ktime_t last_io;
};

static void thn31_free(struct kref *ref)
{
	kfree(container_of(ref, struct thn31, ref));
}

static int thn31_open(struct inode *inode, struct file *file)
{
	struct thn31 *nfc = container_of(file->private_data, struct thn31, misc);

	kref_get(&nfc->ref);
	file->private_data = nfc;
	return nonseekable_open(inode, file);
}

static int thn31_release(struct inode *inode, struct file *file)
{
	struct thn31 *nfc = file->private_data;

	kref_put(&nfc->ref, thn31_free);
	return 0;
}

static int thn31_flush(struct file *file, fl_owner_t id)
{
	struct thn31 *nfc = file->private_data;

	atomic_inc(&nfc->flush_seq);
	wake_up_interruptible(&nfc->readq);
	return 0;
}

static irqreturn_t thn31_irq(int irq, void *data)
{
	struct thn31 *nfc = data;

	if (device_may_wakeup(&nfc->client->dev))
		pm_wakeup_event(&nfc->client->dev, 2000);
	/* The level stays high until we consume the pending frame. */
	if (atomic_xchg(&nfc->armed, 0))
		disable_irq_nosync(irq);
	wake_up_interruptible(&nfc->readq);
	return IRQ_HANDLED;
}

static ssize_t thn31_read(struct file *file, char __user *buf, size_t count,
			  loff_t *offset)
{
	struct thn31 *nfc = file->private_data;
	int seq = atomic_read(&nfc->flush_seq);
	u8 *data;
	int ret, len;

	if (!count)
		return 0;
	count = min_t(size_t, count, THN31_BUFSIZE);
	data = kmalloc(count + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	ret = mutex_lock_interruptible(&nfc->read_lock);
	if (ret)
		goto free;
	down_read(&nfc->live);
	if (READ_ONCE(nfc->dead)) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = gpiod_get_value_cansleep(nfc->irq_gpio);
	if (!ret) {
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto unlock;
		}
		atomic_set(&nfc->armed, 1);
		enable_irq(nfc->irq);
		ret = wait_event_interruptible(nfc->readq,
			!atomic_read(&nfc->armed) || READ_ONCE(nfc->dead) ||
			atomic_read(&nfc->flush_seq) != seq);
		if (atomic_xchg(&nfc->armed, 0))
			disable_irq(nfc->irq);
		else
			synchronize_irq(nfc->irq);
		if (ret)
			goto unlock;
		if (READ_ONCE(nfc->dead)) {
			ret = -ENODEV;
			goto unlock;
		}
		if (atomic_read(&nfc->flush_seq) != seq) {
			ret = -EWOULDBLOCK;
			goto unlock;
		}
		ret = gpiod_get_value_cansleep(nfc->irq_gpio);
	}
	if (ret <= 0) {
		ret = ret ?: -EIO;
		goto unlock;
	}

	/* THN31 pads a one-byte payload after a three-byte NCI header. */
	len = count + (nfc->last_count == 3 && count == 1);
	mutex_lock(&nfc->bus_lock);
	ret = i2c_master_recv(nfc->client, data, len);
	if (ret == len)
		nfc->last_io = ktime_get_boottime();
	mutex_unlock(&nfc->bus_lock);
	if (ret == len) {
		nfc->last_count = count;
		ret = count;
		if (copy_to_user(buf, data, count))
			ret = -EFAULT;
	} else if (ret >= 0) {
		ret = -EIO;
	}

unlock:
	up_read(&nfc->live);
	mutex_unlock(&nfc->read_lock);
free:
	kfree(data);
	return ret;
}

static ssize_t thn31_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *offset)
{
	struct thn31 *nfc = file->private_data;
	ktime_t now;
	u8 wake = 0, *data;
	int ret, attempt;

	if (!count)
		return 0;
	count = min_t(size_t, count, THN31_BUFSIZE);
	data = memdup_user(buf, count);
	if (IS_ERR(data))
		return PTR_ERR(data);

	down_read(&nfc->live);
	if (READ_ONCE(nfc->dead)) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&nfc->bus_lock);
	now = ktime_get_boottime();
	if (data[0] != 0x5a && ktime_ms_delta(now, nfc->last_io) >= 1500) {
		for (attempt = 0; attempt < 2; attempt++) {
			ret = i2c_master_send(nfc->client, &wake, 1);
			usleep_range(5000, 5100);
			if (ret == 1)
				break;
		}
	}
	ret = i2c_master_send(nfc->client, data, count);
	if (ret == count && data[0] != 0x5a)
		nfc->last_io = now;
	else if (ret >= 0 && ret != count)
		ret = -EIO;
	mutex_unlock(&nfc->bus_lock);
out:
	up_read(&nfc->live);
	kfree(data);
	return ret;
}

static void thn31_reset(struct thn31 *nfc)
{
	msleep(20);
	gpiod_set_value_cansleep(nfc->ven, 0);
	msleep(20);
	gpiod_set_value_cansleep(nfc->ven, 1);
	msleep(20);
	nfc->nfc_power = true;
}

static void thn31_update_ven(struct thn31 *nfc)
{
	lockdep_assert_held(&nfc->bus_lock);
	gpiod_set_value_cansleep(nfc->ven, nfc->nfc_power || nfc->ese_power ||
				 nfc->spi_ese_power);
}

static long thn31_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct thn31 *nfc = file->private_data;
	int ret = 0;

	down_read(&nfc->live);
	if (READ_ONCE(nfc->dead)) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&nfc->bus_lock);
	switch (cmd) {
	case THN31_POWER:
		switch (arg) {
		case 0:
		case 1:
		case 10:
		case 11:
			nfc->nfc_power = arg == 1 || arg == 10;
			thn31_update_ven(nfc);
			if (!nfc->nfc_power)
				thn31_flush(file, NULL);
			break;
		case 4:
		case 6:
		case 12:
		case 13:
			gpiod_set_value_cansleep(nfc->download, arg == 4 || arg == 12);
			usleep_range(10000, 10100);
			break;
		case 5:
			if (nfc->spi_ese_power)
				ret = -EBUSY;
			else
				thn31_reset(nfc);
			break;
		case 14:
			thn31_flush(file, NULL);
			break;
		default:
			ret = -ENOIOCTLCMD;
		}
		break;
	case THN31_ESE_POWER:
		if (arg > 1) {
			ret = -ENOIOCTLCMD;
			break;
		}
		nfc->ese_power = arg == 0;
		thn31_update_ven(nfc);
		break;
	case THN31_ESE_STATE:
		ret = arg == 2 ? gpiod_get_value_cansleep(nfc->ven) : -ENOIOCTLCMD;
		break;
	case THN31_IRQ_STATE:
		ret = gpiod_get_value_cansleep(nfc->irq_gpio);
		break;
	default:
		ret = -ENOIOCTLCMD;
	}
	mutex_unlock(&nfc->bus_lock);
out:
	up_read(&nfc->live);
	return ret;
}

#ifdef CONFIG_COMPAT
static long thn31_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != 0xe9 || _IOC_SIZE(cmd) != sizeof(u32))
		return -ENOIOCTLCMD;
	return thn31_ioctl(file, _IOC(_IOC_DIR(cmd), _IOC_TYPE(cmd), _IOC_NR(cmd),
				    sizeof(long)), arg);
}
#endif

static const struct file_operations thn31_fops = {
	.owner = THIS_MODULE,
	.open = thn31_open,
	.release = thn31_release,
	.flush = thn31_flush,
	.read = thn31_read,
	.write = thn31_write,
	.unlocked_ioctl = thn31_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = thn31_compat_ioctl,
#endif
};

static ssize_t chip_name_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "thn31\n");
}
static DEVICE_ATTR_RO(chip_name);

static struct attribute *thn31_attrs[] = {
	&dev_attr_chip_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(thn31);

static void thn31_put(void *data)
{
	struct thn31 *nfc = data;

	kref_put(&nfc->ref, thn31_free);
}

static int thn31_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct thn31 *nfc;
	int ret;

	/* Stock uses the same compatible for several NFC part numbers. */
	if (client->addr != 0x28 ||
	    !of_property_present(dev->of_node, "tms,irq-gpio") ||
	    !of_property_present(dev->of_node, "tms,ven-gpio") ||
	    !of_property_present(dev->of_node, "tms,download-gpio"))
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	nfc = kzalloc(sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;
	kref_init(&nfc->ref);
	ret = devm_add_action_or_reset(dev, thn31_put, nfc);
	if (ret)
		return ret;
	nfc->client = client;
	init_rwsem(&nfc->live);
	mutex_init(&nfc->read_lock);
	mutex_init(&nfc->bus_lock);
	init_waitqueue_head(&nfc->readq);
	atomic_set(&nfc->flush_seq, 0);
	atomic_set(&nfc->armed, 0);

	/*
	 * Stock Rothko probes through the three GPIOs directly. Its legacy
	 * nfc_active state has pull arguments incompatible with these pads.
	 */
	nfc->irq_gpio = devm_gpiod_get(dev, "tms,irq", GPIOD_IN);
	if (IS_ERR(nfc->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(nfc->irq_gpio), "IRQ GPIO\n");
	nfc->ven = devm_gpiod_get(dev, "tms,ven", GPIOD_OUT_LOW);
	if (IS_ERR(nfc->ven))
		return dev_err_probe(dev, PTR_ERR(nfc->ven), "VEN GPIO\n");
	nfc->download = devm_gpiod_get(dev, "tms,download", GPIOD_OUT_LOW);
	if (IS_ERR(nfc->download))
		return dev_err_probe(dev, PTR_ERR(nfc->download), "download GPIO\n");
	nfc->irq = gpiod_to_irq(nfc->irq_gpio);
	if (nfc->irq < 0)
		return dev_err_probe(dev, nfc->irq, "GPIO IRQ mapping\n");
	ret = devm_request_irq(dev, nfc->irq, thn31_irq,
			       IRQF_TRIGGER_HIGH | IRQF_NO_AUTOEN, "tms_nfc", nfc);
	if (ret)
		return dev_err_probe(dev, ret, "NFC IRQ\n");

	nfc->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "tms_nfc",
		.fops = &thn31_fops,
		.parent = dev,
		.groups = thn31_groups,
	};
	thn31_reset(nfc);
	i2c_set_clientdata(client, nfc);
	ret = misc_register(&nfc->misc);
	if (ret) {
		gpiod_set_value_cansleep(nfc->ven, 0);
		return dev_err_probe(dev, ret, "tms_nfc registration\n");
	}
	/* Keep the stock Android ABI after the real transport is registered. */
	ret = sysfs_create_link(NULL, &nfc->misc.this_device->kobj, "nfc");
	if (ret)
		dev_warn(dev, "failed to link /sys/nfc: %d\n", ret);
	else
		nfc->sysfs_link = true;
	device_init_wakeup(dev, true);
	return 0;
}

static void thn31_shutdown(struct i2c_client *client)
{
	struct thn31 *nfc = i2c_get_clientdata(client);

	mutex_lock(&nfc->bus_lock);
	gpiod_set_value_cansleep(nfc->download, 0);
	gpiod_set_value_cansleep(nfc->ven, 0);
	msleep(20);
	mutex_unlock(&nfc->bus_lock);
}

static void thn31_remove(struct i2c_client *client)
{
	struct thn31 *nfc = i2c_get_clientdata(client);

	if (nfc->sysfs_link)
		sysfs_remove_link(NULL, "nfc");
	misc_deregister(&nfc->misc);
	WRITE_ONCE(nfc->dead, true);
	wake_up_interruptible(&nfc->readq);
	down_write(&nfc->live);
	device_init_wakeup(&client->dev, false);
	thn31_shutdown(client);
	up_write(&nfc->live);
}

static int thn31_suspend(struct device *dev)
{
	struct thn31 *nfc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev) && atomic_read(&nfc->armed) && !enable_irq_wake(nfc->irq))
		nfc->irq_wake = true;
	return 0;
}

static int thn31_resume(struct device *dev)
{
	struct thn31 *nfc = dev_get_drvdata(dev);

	if (nfc->irq_wake) {
		disable_irq_wake(nfc->irq);
		nfc->irq_wake = false;
	}
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(thn31_pm, thn31_suspend, thn31_resume);

static const struct of_device_id thn31_match[] = {
	{ .compatible = "qcom,nq-nci" },
	{ .compatible = "tms,nfc" },
	{ }
};
MODULE_DEVICE_TABLE(of, thn31_match);

static struct i2c_driver thn31_driver = {
	.driver = {
		.name = "tms,nfc",
		.of_match_table = thn31_match,
		.pm = pm_sleep_ptr(&thn31_pm),
	},
	.probe = thn31_probe,
	.remove = thn31_remove,
	.shutdown = thn31_shutdown,
};

/*
 * Stock Rothko tms_device_modules 1.0.221230, confirmed by the 6.1.115
 * module and MiTEE logs on CN production hardware with VNNTWXM firmware.
 * The clock commands ignore arg and are paired across separate opens.
 */
#define THN31_SPI_ESE_POWER	_IOW(0xea, 0x04, __u64)
#define THN31_SPI_CLK_ON	_IOW(0xea, 0x0d, __u64)
#define THN31_SPI_CLK_OFF	_IOW(0xea, 0x0e, __u64)

enum thn31_spi_power {
	THN31_SPI_VEN_ON = 10,
	THN31_SPI_VEN_OFF = 11,
};

struct thn31_spi_ese {
	struct spi_device *spi;
	struct thn31 *nfc;
	struct miscdevice misc;
	struct kref ref;
	struct mutex lock;
	unsigned int clock_users;
	u8 *buf;
};

static void thn31_spi_free(struct kref *ref)
{
	struct thn31_spi_ese *ese = container_of(ref, struct thn31_spi_ese, ref);

	kfree(ese->buf);
	kfree(ese);
}

static int thn31_spi_open(struct inode *inode, struct file *file)
{
	struct thn31_spi_ese *ese = container_of(file->private_data,
					       struct thn31_spi_ese, misc);

	kref_get(&ese->ref);
	file->private_data = ese;
	return nonseekable_open(inode, file);
}

static int thn31_spi_release(struct inode *inode, struct file *file)
{
	struct thn31_spi_ese *ese = file->private_data;

	kref_put(&ese->ref, thn31_spi_free);
	return 0;
}

static void thn31_spi_set_ven(struct thn31_spi_ese *ese, bool on)
{
	struct thn31 *nfc = ese->nfc;

	mutex_lock(&nfc->bus_lock);
	nfc->spi_ese_power = on;
	thn31_update_ven(nfc);
	mutex_unlock(&nfc->bus_lock);
}

static long thn31_spi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct thn31_spi_ese *ese = file->private_data;
	struct device *host;
	long ret = 0;

	mutex_lock(&ese->lock);
	if (!ese->spi) {
		ret = -ENODEV;
		goto out;
	}

	host = ese->spi->controller->dev.parent;
	switch (cmd) {
	case THN31_SPI_ESE_POWER:
		if (arg != THN31_SPI_VEN_ON && arg != THN31_SPI_VEN_OFF) {
			ret = -ENOIOCTLCMD;
			break;
		}
		thn31_spi_set_ven(ese, arg == THN31_SPI_VEN_ON);
		break;
	case THN31_SPI_CLK_ON:
		if (ese->clock_users == UINT_MAX) {
			ret = -EOVERFLOW;
			break;
		}
		/* Hold the same controller PM reference used by the SPI core. */
		if (!ese->clock_users) {
			ret = pm_runtime_resume_and_get(host);
			if (ret < 0)
				break;
		}
		ese->clock_users++;
		break;
	case THN31_SPI_CLK_OFF:
		if (ese->clock_users && !--ese->clock_users)
			pm_runtime_put_autosuspend(host);
		break;
	default:
		ret = -ENOIOCTLCMD;
	}
out:
	mutex_unlock(&ese->lock);
	return ret;
}

/* Preserve the stock length check, including its exact-size rejection. */
static size_t thn31_spi_io_count(size_t count)
{
	return count == THN31_BUFSIZE ? 0 : min_t(size_t, count, THN31_BUFSIZE);
}

static ssize_t thn31_spi_read(struct file *file, char __user *buf, size_t count,
			      loff_t *pos)
{
	struct thn31_spi_ese *ese = file->private_data;
	ssize_t ret;

	mutex_lock(&ese->lock);
	if (!ese->spi) {
		ret = -ENODEV;
		goto out;
	}
	count = thn31_spi_io_count(count);
	if (!count) {
		ret = -EPERM;
		goto out;
	}
	/* Stock enters an IRQ wait here, but Rothko has no dedicated eSE IRQ. */
	if (!(file->f_flags & O_NONBLOCK)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	memset(ese->buf, 0, count);
	mutex_lock(&ese->nfc->bus_lock);
	ret = spi_read(ese->spi, ese->buf, count);
	mutex_unlock(&ese->nfc->bus_lock);
	if (ret || copy_to_user(buf, ese->buf, count))
		ret = -EFAULT;
	else
		ret = count;
out:
	mutex_unlock(&ese->lock);
	return ret;
}

static ssize_t thn31_spi_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *pos)
{
	struct thn31_spi_ese *ese = file->private_data;
	ssize_t ret;

	mutex_lock(&ese->lock);
	if (!ese->spi) {
		ret = -ENODEV;
		goto out;
	}
	count = thn31_spi_io_count(count);
	if (!count) {
		ret = -EPERM;
		goto out;
	}
	if (copy_from_user(ese->buf, buf, count)) {
		ret = -EFAULT;
		goto out;
	}
	mutex_lock(&ese->nfc->bus_lock);
	ret = spi_write(ese->spi, ese->buf, count);
	mutex_unlock(&ese->nfc->bus_lock);
	ret = ret ? -EIO : (ssize_t)count;
out:
	mutex_unlock(&ese->lock);
	return ret;
}

static const struct file_operations thn31_spi_fops = {
	.owner = THIS_MODULE,
	.open = thn31_spi_open,
	.release = thn31_spi_release,
	.read = thn31_spi_read,
	.write = thn31_spi_write,
	.unlocked_ioctl = thn31_spi_ioctl,
};

static int thn31_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct device *host = spi->controller->dev.parent;
	struct thn31_spi_ese *ese;
	struct device_link *link;
	struct device *supplier;
	struct pinctrl *pins;
	struct thn31 *nfc;
	const char *nfcc;
	int ret;

	if (!host || !spi->controller->auto_runtime_pm ||
	    !of_device_is_compatible(host->of_node, "mediatek,mt6989-spi") ||
	    !of_node_name_eq(host->of_node, "spi5") || spi_get_chipselect(spi, 0) ||
	    of_property_present(dev->of_node, "independent_support") ||
	    of_property_read_string(dev->of_node, "nxp,nfcc", &nfcc) ||
	    strcmp(nfcc, "3-0028"))
		return -ENODEV;

	/* The stock string names the I2C client that owns the shared VEN. */
	supplier = bus_find_device_by_name(&i2c_bus_type, NULL, nfcc);
	if (!supplier)
		return -EPROBE_DEFER;
	link = device_link_add(dev, supplier, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link || READ_ONCE(link->status) != DL_STATE_CONSUMER_PROBE ||
	    READ_ONCE(supplier->driver) != &thn31_driver.driver) {
		put_device(supplier);
		return -EPROBE_DEFER;
	}
	nfc = dev_get_drvdata(supplier);
	put_device(supplier);

	pins = devm_pinctrl_get_select(dev, "nxp_ese_spi_mode");
	if (IS_ERR(pins))
		return dev_err_probe(dev, PTR_ERR(pins), "eSE SPI pinctrl\n");

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "eSE SPI setup\n");

	ese = kzalloc(sizeof(*ese), GFP_KERNEL);
	if (!ese)
		return -ENOMEM;
	kref_init(&ese->ref);
	ese->buf = kmalloc(THN31_BUFSIZE, GFP_KERNEL);
	if (!ese->buf) {
		kref_put(&ese->ref, thn31_spi_free);
		return -ENOMEM;
	}
	mutex_init(&ese->lock);
	ese->spi = spi;
	ese->nfc = nfc;
	ese->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "tms_ese",
		.fops = &thn31_spi_fops,
		.parent = dev,
	};
	/* Let the stock Android node policy supply DAC and the SELinux label. */
	ret = misc_register(&ese->misc);
	if (ret) {
		kref_put(&ese->ref, thn31_spi_free);
		return dev_err_probe(dev, ret, "tms_ese registration\n");
	}
	spi_set_drvdata(spi, ese);
	return 0;
}

static void thn31_spi_remove(struct spi_device *spi)
{
	struct thn31_spi_ese *ese = spi_get_drvdata(spi);

	misc_deregister(&ese->misc);
	mutex_lock(&ese->lock);
	ese->spi = NULL;
	if (ese->clock_users)
		pm_runtime_put_autosuspend(spi->controller->dev.parent);
	thn31_spi_set_ven(ese, false);
	mutex_unlock(&ese->lock);
	kref_put(&ese->ref, thn31_spi_free);
}

static const struct of_device_id thn31_spi_match[] = {
	{ .compatible = "nxp,p61" },
	{ }
};
MODULE_DEVICE_TABLE(of, thn31_spi_match);

static const struct spi_device_id thn31_spi_id[] = {
	{ "p61" },
	{ }
};
MODULE_DEVICE_TABLE(spi, thn31_spi_id);

static struct spi_driver thn31_spi_driver = {
	.driver = {
		.name = "rothko-thn31-ese",
		.of_match_table = thn31_spi_match,
	},
	.id_table = thn31_spi_id,
	.probe = thn31_spi_probe,
	.remove = thn31_spi_remove,
};

static int __init thn31_init(void)
{
	int ret;

	ret = i2c_add_driver(&thn31_driver);
	if (ret)
		return ret;
	ret = spi_register_driver(&thn31_spi_driver);
	if (ret)
		i2c_del_driver(&thn31_driver);
	return ret;
}
module_init(thn31_init);

static void __exit thn31_exit(void)
{
	spi_unregister_driver(&thn31_spi_driver);
	i2c_del_driver(&thn31_driver);
}
module_exit(thn31_exit);

MODULE_DESCRIPTION("Rothko TsingTeng THN31 NFC and eSE transports");
MODULE_LICENSE("GPL");
