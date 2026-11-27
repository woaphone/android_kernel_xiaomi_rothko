// SPDX-License-Identifier: GPL-2.0-only
/* Rothko Goodix FOD hardware controls for the Xiaomi TEE HAL. */

#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pid.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <net/net_namespace.h>
#include <net/netlink.h>

#define FP_NAME			"xiaomi-fp"
#define FP_HAL_UID		KUIDT_INIT(1000)
#define FP_NL_IRQ		1
#define FP_NL_PAYLOAD_SIZE	16
#define FP_VOLTAGE_UV		3300000

struct fp_key_event {
	__u32 key;
	__s32 value;
};

/* Request words verified against stock xiaomi_spi_tee and libgf_hal. */
#define FP_IOC_INIT		_IOR('g', 0, __u8)
#define FP_IOC_EXIT		_IO('g', 1)
#define FP_IOC_RESET		_IO('g', 2)
#define FP_IOC_ENABLE_IRQ	_IO('g', 3)
#define FP_IOC_DISABLE_IRQ	_IO('g', 4)
#define FP_IOC_ENABLE_SPI_CLK	_IOW('g', 5, __u32)
#define FP_IOC_DISABLE_SPI_CLK	_IO('g', 6)
#define FP_IOC_ENABLE_POWER	_IO('g', 7)
#define FP_IOC_DISABLE_POWER	_IO('g', 8)
#define FP_IOC_KEY_EVENT	_IOW('g', 9, struct fp_key_event)
#define FP_IOC_ENTER_SLEEP	_IO('g', 10)
#define FP_IOC_REMOVE		_IO('g', 12)
#define FP_IOC_GET_FW_INFO	_IOR('g', 11, __u8)
#define FP_IOC_CHIP_INFO	_IOW('g', 13, __u8[8])

struct rothko_fp {
	struct device *dev;
	struct device *spi;
	struct regulator *vfp;
	struct pinctrl *pins;
	struct pinctrl_state *reset_low;
	struct pinctrl_state *reset_high;
	struct input_dev *input;
	struct sock *events;
	struct miscdevice misc;
	struct mutex lock;
	struct kref ref;
	unsigned int users;
	unsigned int clock_users;
	int irq;
	u8 protocol;
	bool powered;
	bool irq_enabled;
	bool irq_wake;
	bool removed;
};

/* One sensor and one init_net endpoint, owned by the process opening the node. */
static DEFINE_SPINLOCK(fp_peer_lock);
static struct pid *fp_peer_owner;
static u32 fp_peer_port;
static struct sock *fp_peer_listener;

static void rothko_fp_netlink_recv(struct sk_buff *skb)
{
	if (!nlmsg_ok(nlmsg_hdr(skb), skb->len) || !NETLINK_CB(skb).portid)
		return;

	/* The kernel invokes this callback synchronously from netlink_sendmsg. */
	spin_lock(&fp_peer_lock);
	if (fp_peer_listener == skb->sk && fp_peer_owner == task_tgid(current))
		fp_peer_port = NETLINK_CB(skb).portid;
	spin_unlock(&fp_peer_lock);
}

static void rothko_fp_set_listener(struct sock *listener)
{
	spin_lock(&fp_peer_lock);
	fp_peer_listener = listener;
	if (!listener)
		fp_peer_port = 0;
	spin_unlock(&fp_peer_lock);
}

static int rothko_fp_open_events(struct rothko_fp *fp)
{
	struct netlink_kernel_cfg cfg = { .input = rothko_fp_netlink_recv };

	if (fp->events)
		return 0;

	fp->events = netlink_kernel_create(&init_net, fp->protocol, &cfg);
	return fp->events ? 0 : -EADDRINUSE;
}

/* In the ioctl and teardown paths, the threaded IRQ is stopped first. */
static void rothko_fp_close_events(void *data)
{
	struct rothko_fp *fp = data;

	if (!fp->events)
		return;

	rothko_fp_set_listener(NULL);
	netlink_kernel_release(fp->events);
	fp->events = NULL;
}

static void rothko_fp_release_peer(void)
{
	struct pid *owner;

	spin_lock(&fp_peer_lock);
	owner = fp_peer_owner;
	fp_peer_owner = NULL;
	spin_unlock(&fp_peer_lock);
	put_pid(owner);
}

static irqreturn_t rothko_fp_irq(int irq, void *data)
{
	struct rothko_fp *fp = data;
	struct sk_buff *skb;
	struct nlmsghdr *hdr;
	struct sock *listener;
	u32 portid;
	u8 *payload;

	pm_wakeup_event(fp->dev, 2000);
	spin_lock(&fp_peer_lock);
	listener = fp_peer_listener;
	portid = fp_peer_port;
	spin_unlock(&fp_peer_lock);
	if (!portid)
		return IRQ_HANDLED;

	skb = nlmsg_new(FP_NL_PAYLOAD_SIZE, GFP_KERNEL);
	if (!skb)
		return IRQ_HANDLED;

	hdr = nlmsg_put(skb, 0, 0, 0, FP_NL_PAYLOAD_SIZE, 0);
	if (!hdr) {
		kfree_skb(skb);
		return IRQ_HANDLED;
	}

	payload = nlmsg_data(hdr);
	memset(payload, 0, FP_NL_PAYLOAD_SIZE);
	payload[0] = FP_NL_IRQ;
	netlink_unicast(listener, skb, portid, MSG_DONTWAIT);
	return IRQ_HANDLED;
}

static int rothko_fp_enable_irq(struct rothko_fp *fp)
{
	int ret;

	if (fp->irq_enabled)
		return 0;
	if (!fp->powered)
		return -EHOSTDOWN;

	if (device_may_wakeup(fp->dev)) {
		ret = enable_irq_wake(fp->irq);
		if (ret)
			return ret;
		fp->irq_wake = true;
	}
	enable_irq(fp->irq);
	fp->irq_enabled = true;
	return 0;
}

static void rothko_fp_disable_irq(struct rothko_fp *fp)
{
	if (!fp->irq_enabled)
		return;

	disable_irq(fp->irq);
	if (fp->irq_wake) {
		disable_irq_wake(fp->irq);
		fp->irq_wake = false;
	}
	fp->irq_enabled = false;
}

static int rothko_fp_power_on(struct rothko_fp *fp)
{
	int ret;

	if (fp->powered)
		return 0;

	ret = regulator_enable(fp->vfp);
	if (ret)
		return ret;
	/* Stock lets the sensor rail settle before releasing reset. */
	fsleep(10000);
	fp->powered = true;
	return 0;
}

static int rothko_fp_power_off(struct rothko_fp *fp)
{
	int ret;

	if (!fp->powered)
		return 0;

	rothko_fp_disable_irq(fp);
	ret = regulator_disable(fp->vfp);
	if (!ret)
		fp->powered = false;
	return ret;
}

static int rothko_fp_reset(struct rothko_fp *fp)
{
	int ret;

	if (!fp->powered)
		return -EHOSTDOWN;

	ret = pinctrl_select_state(fp->pins, fp->reset_low);
	if (ret)
		return ret;
	usleep_range(5000, 5500);
	ret = pinctrl_select_state(fp->pins, fp->reset_high);
	if (ret)
		return ret;
	msleep(60);
	return 0;
}

static int rothko_fp_clock_on(struct rothko_fp *fp)
{
	int ret;

	if (fp->clock_users == UINT_MAX)
		return -EOVERFLOW;

	/* SPI3's runtime PM enables the gate used by secure-world transfers. */
	if (!fp->clock_users) {
		ret = pm_runtime_resume_and_get(fp->spi);
		if (ret)
			return ret;
	}
	fp->clock_users++;
	return 0;
}

static void rothko_fp_clock_off(struct rothko_fp *fp)
{
	if (fp->clock_users && !--fp->clock_users)
		pm_runtime_put_sync(fp->spi);
}

static void rothko_fp_stop(struct rothko_fp *fp)
{
	int ret;

	rothko_fp_disable_irq(fp);
	rothko_fp_close_events(fp);
	rothko_fp_release_peer();
	if (fp->clock_users) {
		fp->clock_users = 1;
		rothko_fp_clock_off(fp);
	}
	input_report_key(fp->input, KEY_SELECT, 0);
	input_sync(fp->input);
	ret = rothko_fp_power_off(fp);
	if (ret)
		dev_warn(fp->dev, "failed to disable vfp: %d\n", ret);
}

static int rothko_fp_key_event(struct rothko_fp *fp, void __user *arg)
{
	struct fp_key_event event;
	unsigned int key;

	if (copy_from_user(&event, arg, sizeof(event)))
		return -EFAULT;

	switch (event.key) {
	case 1:
		input_report_key(fp->input, KEY_SELECT, !!event.value);
		input_sync(fp->input);
		return 0;
	case 2:
		key = KEY_POWER;
		break;
	case 5:
		key = KEY_CAMERA;
		break;
	default:
		return 0;
	}

	if (event.value == 1) {
		input_report_key(fp->input, key, 1);
		input_sync(fp->input);
		input_report_key(fp->input, key, 0);
		input_sync(fp->input);
	}
	return 0;
}

static long rothko_fp_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct rothko_fp *fp = file->private_data;
	void __user *argp = (void __user *)arg;
	u8 chip_info[8];
	int ret = 0;

	mutex_lock(&fp->lock);
	if (fp->removed) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case FP_IOC_INIT:
		ret = rothko_fp_open_events(fp);
		if (ret)
			break;
		ret = put_user(fp->protocol, (u8 __user *)argp);
		if (!ret)
			rothko_fp_set_listener(fp->events);
		break;
	case FP_IOC_EXIT:
		rothko_fp_disable_irq(fp);
		rothko_fp_close_events(fp);
		break;
	case FP_IOC_RESET:
		ret = rothko_fp_reset(fp);
		break;
	case FP_IOC_ENABLE_IRQ:
		ret = rothko_fp_enable_irq(fp);
		break;
	case FP_IOC_DISABLE_IRQ:
		rothko_fp_disable_irq(fp);
		break;
	case FP_IOC_ENABLE_SPI_CLK:
		ret = rothko_fp_clock_on(fp);
		break;
	case FP_IOC_DISABLE_SPI_CLK:
		rothko_fp_clock_off(fp);
		break;
	case FP_IOC_ENABLE_POWER:
		ret = rothko_fp_power_on(fp);
		break;
	case FP_IOC_DISABLE_POWER:
		ret = rothko_fp_power_off(fp);
		break;
	case FP_IOC_KEY_EVENT:
		ret = rothko_fp_key_event(fp, argp);
		break;
	case FP_IOC_GET_FW_INFO:
		/* Stock reports no kernel firmware update; TEE owns the sensor. */
		ret = put_user(0, (u8 __user *)argp);
		break;
	case FP_IOC_CHIP_INFO:
		/* This is an informational report from userspace in the stock ABI. */
		if (copy_from_user(chip_info, argp, sizeof(chip_info)))
			ret = -EFAULT;
		else
			dev_dbg(fp->dev, "HAL chip=%u mode=%u operation=%u\n",
				chip_info[0], chip_info[1], chip_info[2]);
		break;
	case FP_IOC_ENTER_SLEEP:
	case FP_IOC_REMOVE:
		/* Reserved no-ops in Xiaomi's stock ioctl implementation. */
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&fp->lock);
	return ret;
}

static void rothko_fp_free(struct kref *ref)
{
	struct rothko_fp *fp = container_of(ref, struct rothko_fp, ref);

	kfree(fp);
}

static void rothko_fp_put(void *data)
{
	struct rothko_fp *fp = data;

	kref_put(&fp->ref, rothko_fp_free);
}

static int rothko_fp_open(struct inode *inode, struct file *file)
{
	struct rothko_fp *fp = container_of(file->private_data, struct rothko_fp, misc);
	int ret = 0;

	/* Some stock ROMs chmod the node 0666; do not grant apps FOD controls. */
	if (!uid_eq(current_euid(), FP_HAL_UID) &&
	    !uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EACCES;

	mutex_lock(&fp->lock);
	if (fp->users) {
		if (fp_peer_owner != task_tgid(current)) {
			ret = -EBUSY;
			goto out;
		}
	} else {
		ret = rothko_fp_power_on(fp);
		if (ret)
			goto out;

		spin_lock(&fp_peer_lock);
		fp_peer_owner = get_task_pid(current, PIDTYPE_TGID);
		spin_unlock(&fp_peer_lock);
	}

	fp->users++;
	kref_get(&fp->ref);
	file->private_data = fp;
	ret = nonseekable_open(inode, file);
out:
	mutex_unlock(&fp->lock);
	return ret;
}

static int rothko_fp_release(struct inode *inode, struct file *file)
{
	struct rothko_fp *fp = file->private_data;

	mutex_lock(&fp->lock);
	if (!--fp->users && !fp->removed)
		rothko_fp_stop(fp);
	mutex_unlock(&fp->lock);
	rothko_fp_put(fp);
	return 0;
}

static const struct file_operations rothko_fp_fops = {
	.owner = THIS_MODULE,
	.open = rothko_fp_open,
	.release = rothko_fp_release,
	.unlocked_ioctl = rothko_fp_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static void rothko_fp_put_spi(void *data)
{
	put_device(data);
}

static int rothko_fp_link_spi(struct rothko_fp *fp)
{
	struct device_node *node;
	struct platform_device *spi;
	int ret = -EPROBE_DEFER;

	/* Stock spi-mt65xx selects SPI3 on this board, not the touch SPI7. */
	node = of_find_node_by_path("/soc/spi3@11013000");
	if (!node)
		return -ENODEV;
	spi = of_find_device_by_node(node);
	of_node_put(node);
	if (!spi)
		return -EPROBE_DEFER;

	if (device_trylock(&spi->dev)) {
		if (device_is_bound(&spi->dev)) {
			if (device_link_add(fp->dev, &spi->dev,
					    DL_FLAG_AUTOREMOVE_CONSUMER))
				ret = 0;
			else
				ret = -EINVAL;
		}
		device_unlock(&spi->dev);
	}
	if (ret) {
		put_device(&spi->dev);
		return ret;
	}

	fp->spi = &spi->dev;
	return devm_add_action_or_reset(fp->dev, rothko_fp_put_spi, fp->spi);
}

static int rothko_fp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pinctrl_state *state;
	struct rothko_fp *fp;
	u32 protocol;
	int ret;

	ret = of_property_read_u32(dev->of_node, "netlink-event", &protocol);
	if (ret || protocol != 29)
		return dev_err_probe(dev, ret ?: -EINVAL, "invalid netlink event protocol\n");

	fp = kzalloc(sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;
	fp->dev = dev;
	fp->protocol = protocol;
	mutex_init(&fp->lock);
	kref_init(&fp->ref);
	ret = devm_add_action_or_reset(dev, rothko_fp_put, fp);
	if (ret)
		return ret;

	ret = rothko_fp_link_spi(fp);
	if (ret)
		return dev_err_probe(dev, ret, "SPI3 controller unavailable\n");

	fp->vfp = devm_regulator_get(dev, "vfp");
	if (IS_ERR(fp->vfp))
		return dev_err_probe(dev, PTR_ERR(fp->vfp), "failed to acquire vfp\n");
	ret = regulator_set_voltage(fp->vfp, FP_VOLTAGE_UV, FP_VOLTAGE_UV);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set vfp voltage\n");

	fp->pins = devm_pinctrl_get(dev);
	if (IS_ERR(fp->pins))
		return dev_err_probe(dev, PTR_ERR(fp->pins), "failed to get pinctrl\n");
	fp->reset_low = pinctrl_lookup_state(fp->pins, "reset_low");
	if (IS_ERR(fp->reset_low))
		return dev_err_probe(dev, PTR_ERR(fp->reset_low), "missing reset_low\n");
	fp->reset_high = pinctrl_lookup_state(fp->pins, "reset_high");
	if (IS_ERR(fp->reset_high))
		return dev_err_probe(dev, PTR_ERR(fp->reset_high), "missing reset_high\n");
	state = pinctrl_lookup_state(fp->pins, "spiio_spi_mode");
	if (IS_ERR(state))
		return dev_err_probe(dev, PTR_ERR(state), "missing spiio_spi_mode\n");
	ret = pinctrl_select_state(fp->pins, state);
	if (ret)
		return dev_err_probe(dev, ret, "failed to select SPI pins\n");
	state = pinctrl_lookup_state(fp->pins, "eint_default");
	if (IS_ERR(state))
		return dev_err_probe(dev, PTR_ERR(state), "missing eint_default\n");
	ret = pinctrl_select_state(fp->pins, state);
	if (ret)
		return dev_err_probe(dev, ret, "failed to select EINT pin\n");
	ret = pinctrl_select_state(fp->pins, fp->reset_low);
	if (ret)
		return dev_err_probe(dev, ret, "failed to assert reset\n");

	fp->irq = platform_get_irq(pdev, 0);
	if (fp->irq < 0)
		return fp->irq;
	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;

	ret = rothko_fp_open_events(fp);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create netlink endpoint\n");
	ret = devm_add_action_or_reset(dev, rothko_fp_close_events, fp);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, fp->irq, NULL, rothko_fp_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, FP_NAME, fp);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request sensor IRQ\n");

	fp->input = devm_input_allocate_device(dev);
	if (!fp->input)
		return -ENOMEM;
	fp->input->name = FP_NAME;
	fp->input->id.bustype = BUS_HOST;
	input_set_capability(fp->input, EV_KEY, KEY_SELECT);
	input_set_capability(fp->input, EV_KEY, KEY_POWER);
	input_set_capability(fp->input, EV_KEY, KEY_CAMERA);
	ret = input_register_device(fp->input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register fingerprint input\n");

	fp->misc.minor = MISC_DYNAMIC_MINOR;
	fp->misc.name = FP_NAME;
	fp->misc.fops = &rothko_fp_fops;
	fp->misc.parent = dev;
	platform_set_drvdata(pdev, fp);
	return misc_register(&fp->misc);
}

static void rothko_fp_remove(struct platform_device *pdev)
{
	struct rothko_fp *fp = platform_get_drvdata(pdev);

	misc_deregister(&fp->misc);
	mutex_lock(&fp->lock);
	fp->removed = true;
	rothko_fp_stop(fp);
	mutex_unlock(&fp->lock);
}

static const struct of_device_id rothko_fp_of_match[] = {
	{ .compatible = "xiaomi,xiaomi-fp" },
	{ }
};
MODULE_DEVICE_TABLE(of, rothko_fp_of_match);

static struct platform_driver rothko_fp_driver = {
	.probe = rothko_fp_probe,
	.remove = rothko_fp_remove,
	.driver = {
		.name = FP_NAME,
		.of_match_table = rothko_fp_of_match,
	},
};
module_platform_driver(rothko_fp_driver);

MODULE_DESCRIPTION("Rothko Goodix FOD TEE hardware transport");
MODULE_LICENSE("GPL");
