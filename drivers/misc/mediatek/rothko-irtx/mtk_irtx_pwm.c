// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 * MT6989 port with the original rothko /dev/irtx ABI.
 */

#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/kref.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/time64.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include "mtk-pwm.h"

#define IRTX_SET_CARRIER	_IOW('R', 0, __u32)
#define IRTX_GET_FEATURES	_IOR('R', 1, __u32)
#define IRTX_SET_DUTY		_IOW('R', 2, __u32)
#define IRTX_SET_LED		_IOW('R', 10, __u32)
#define IRTX_LED_ENABLE_MASK	GENMASK(3, 0)
#define IRTX_SUPPLY_UV		2800000
#define IRTX_MAX_DURATION_US	(5 * USEC_PER_SEC)

struct mtk_irtx {
	struct miscdevice miscdev;
	struct mutex lock;
	struct kref ref;
	struct device *pwm;
	struct regulator *supply;
	struct pinctrl *pinctrl;
	struct pinctrl_state *idle;
	struct pinctrl_state *active;
	u32 channel;
	u32 carrier;
	u32 cycle;
	u32 duty;
	bool invert;
	bool powered;
	bool opened;
	bool dead;
};

static void mtk_irtx_free(struct kref *ref)
{
	kfree(container_of(ref, struct mtk_irtx, ref));
}

static void mtk_irtx_put(void *data)
{
	struct mtk_irtx *ir = data;

	kref_put(&ir->ref, mtk_irtx_free);
}

static int mtk_irtx_select(struct mtk_irtx *ir, bool on)
{
	int ret, supply_ret;

	if (on) {
		if (!ir->powered) {
			ret = regulator_enable(ir->supply);
			if (ret)
				return ret;
			ir->powered = true;
		}
		ret = pinctrl_select_state(ir->pinctrl, ir->active);
		if (!ret)
			return 0;
	} else {
		ret = pinctrl_select_state(ir->pinctrl, ir->idle);
	}

	if (ir->powered) {
		supply_ret = regulator_disable(ir->supply);
		if (!supply_ret)
			ir->powered = false;
		if (!ret)
			ret = supply_ret;
	}
	return ret;
}

static int mtk_irtx_open(struct inode *inode, struct file *file)
{
	struct mtk_irtx *ir = container_of(file->private_data, struct mtk_irtx, miscdev);
	int ret;

	mutex_lock(&ir->lock);
	if (ir->dead) {
		ret = -ENODEV;
	} else if (ir->opened) {
		ret = -EBUSY;
	} else {
		ret = nonseekable_open(inode, file);
		if (!ret) {
			kref_get(&ir->ref);
			ir->opened = true;
			file->private_data = ir;
		}
	}
	mutex_unlock(&ir->lock);
	return ret;
}

static int mtk_irtx_release(struct inode *inode, struct file *file)
{
	struct mtk_irtx *ir = file->private_data;
	int ret = 0;

	mutex_lock(&ir->lock);
	if (!ir->dead)
		ret = mtk_irtx_select(ir, false);
	ir->opened = false;
	mutex_unlock(&ir->lock);
	mtk_irtx_put(ir);
	return ret;
}

static ssize_t mtk_irtx_read(struct file *file, char __user *buf, size_t len,
			     loff_t *pos)
{
	return 0;
}

static u64 mtk_irtx_ticks(u32 carrier, u32 cycle, u32 duty)
{
	u64 rate = (u64)carrier * cycle;

	if (!rate)
		return 0;
	return div64_u64((u64)MTK_PWM_TICKS_PER_US * USEC_PER_SEC * duty + rate / 2,
			 rate);
}

static bool mtk_irtx_valid_ticks(u64 ticks)
{
	return ticks && ticks <= MTK_PWM_MAX_TICKS;
}

static ssize_t mtk_irtx_write(struct file *file, const char __user *buf,
			      size_t len, loff_t *pos)
{
	struct mtk_irtx *ir = file->private_data;
	u64 ticks, duration;
	s32 requested;
	u32 words;
	u8 *wave;
	int ret, idle_ret;
	size_t i;

	if (!len)
		return 0;
	/* Stock packets end with a signed little-endian duration in microseconds.
	 * That trailer is not part of the DMA waveform; BUF0_SIZE is words - 1.
	 */
	if (len < 2 * sizeof(u32) || len % sizeof(u32))
		return -EINVAL;
	if (len > (MTK_PWM_MAX_WORDS + 1) * sizeof(u32))
		return -EMSGSIZE;

	wave = memdup_user(buf, len);
	if (IS_ERR(wave))
		return PTR_ERR(wave);
	words = len / sizeof(u32) - 1;

	mutex_lock(&ir->lock);
	if (ir->dead) {
		ret = -ENODEV;
		goto out;
	}
	ticks = mtk_irtx_ticks(ir->carrier, ir->cycle, ir->duty);
	if (!mtk_irtx_valid_ticks(ticks)) {
		ret = -EINVAL;
		goto out;
	}
	if (ir->invert)
		for (i = 0; i < len; i++)
			wave[i] = ~wave[i];
	requested = get_unaligned_le32(wave + words * sizeof(u32));

	/* Never truncate the programmed waveform or sleep on an arbitrary u32. */
	duration = DIV_ROUND_UP_ULL((u64)words * 32 * ticks, MTK_PWM_TICKS_PER_US);
	if (requested > 0)
		duration = max_t(u64, duration, requested);
	if (duration > IRTX_MAX_DURATION_US) {
		ret = -EMSGSIZE;
		goto out;
	}

	ret = mtk_irtx_select(ir, true);
	if (ret)
		goto out;
	ret = mtk_rothko_pwm_send(ir->pwm, ir->channel, ticks, wave, words, duration);
	idle_ret = mtk_irtx_select(ir, false);
	if (!ret)
		ret = idle_ret;

out:
	mutex_unlock(&ir->lock);
	kfree(wave);
	return ret ? ret : (ssize_t)len;
}

static long mtk_irtx_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mtk_irtx *ir = file->private_data;
	u32 __user *value = (u32 __user *)arg;
	u32 val, cycle, duty;
	long ret = 0;

	switch (cmd) {
	case IRTX_GET_FEATURES:
		break;
	case IRTX_SET_CARRIER:
	case IRTX_SET_DUTY:
	case IRTX_SET_LED:
		if (get_user(val, value))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	mutex_lock(&ir->lock);
	if (ir->dead) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case IRTX_GET_FEATURES:
		ret = put_user(1U, value);
		break;
	case IRTX_SET_CARRIER:
		if (!mtk_irtx_valid_ticks(mtk_irtx_ticks(val, ir->cycle, ir->duty)))
			ret = -EINVAL;
		else
			ir->carrier = val;
		break;
	case IRTX_SET_DUTY:
		cycle = val & U16_MAX;
		duty = val >> 16;
		if (duty > cycle ||
		    !mtk_irtx_valid_ticks(mtk_irtx_ticks(ir->carrier, cycle, duty))) {
			ret = -EINVAL;
		} else {
			ir->cycle = cycle;
			ir->duty = duty;
		}
		break;
	case IRTX_SET_LED:
		ret = mtk_irtx_select(ir, !!(val & IRTX_LED_ENABLE_MASK));
		break;
	}

out:
	mutex_unlock(&ir->lock);
	return ret;
}

static const struct file_operations mtk_irtx_fops = {
	.owner = THIS_MODULE,
	.open = mtk_irtx_open,
	.release = mtk_irtx_release,
	.read = mtk_irtx_read,
	.write = mtk_irtx_write,
	.unlocked_ioctl = mtk_irtx_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static int mtk_irtx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_irtx *ir;
	const char *supply;
	u32 invert;
	int ret;

	ir = kzalloc(sizeof(*ir), GFP_KERNEL);
	if (!ir)
		return -ENOMEM;
	mutex_init(&ir->lock);
	kref_init(&ir->ref);
	ret = devm_add_action_or_reset(dev, mtk_irtx_put, ir);
	if (ret)
		return ret;
	ret = of_property_read_u32(dev->of_node, "pwm_ch", &ir->channel);
	if (ret)
		ret = of_property_read_u32(dev->of_node, "pwm-ch", &ir->channel);
	if (ret || ir->channel >= MTK_PWM_CHANNELS)
		return -EINVAL;
	ret = of_property_read_u32(dev->of_node, "pwm_data_invert", &invert);
	if (ret)
		ret = of_property_read_u32(dev->of_node, "pwm-data-invert", &invert);
	if (!ret)
		ir->invert = !!invert;
	ir->carrier = 38009;
	ir->cycle = 3;
	ir->duty = 1;

	ir->pwm = mtk_rothko_pwm_get(dev);
	if (IS_ERR(ir->pwm))
		return dev_err_probe(dev, PTR_ERR(ir->pwm), "IR PWM controller\n");
	ir->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(ir->pinctrl))
		return dev_err_probe(dev, PTR_ERR(ir->pinctrl), "IR pinctrl\n");
	ir->idle = pinctrl_lookup_state(ir->pinctrl, "irtx_gpio_led_default");
	if (IS_ERR(ir->idle))
		return dev_err_probe(dev, PTR_ERR(ir->idle), "IR idle pin state\n");
	ir->active = pinctrl_lookup_state(ir->pinctrl, "irtx_gpio_led_set");
	if (IS_ERR(ir->active))
		return dev_err_probe(dev, PTR_ERR(ir->active), "IR PWM pin state\n");

	/* Original DT uses a regulator name string rather than a supply phandle. */
	ret = of_property_read_string(dev->of_node, "pwm-supply", &supply);
	if (ret)
		return dev_err_probe(dev, ret, "IR regulator name\n");
	ir->supply = devm_regulator_get_optional(dev, supply);
	if (IS_ERR(ir->supply)) {
		ret = PTR_ERR(ir->supply);
		return dev_err_probe(dev, ret == -ENODEV ? -EPROBE_DEFER : ret,
				     "IR regulator %s\n", supply);
	}
	ret = regulator_set_voltage(ir->supply, IRTX_SUPPLY_UV, IRTX_SUPPLY_UV);
	if (ret)
		return dev_err_probe(dev, ret, "IR regulator voltage\n");
	ret = mtk_irtx_select(ir, false);
	if (ret)
		return dev_err_probe(dev, ret, "IR idle pin state\n");

	ir->miscdev.minor = MISC_DYNAMIC_MINOR;
	ir->miscdev.name = "irtx";
	ir->miscdev.fops = &mtk_irtx_fops;
	ir->miscdev.parent = dev;
	ret = misc_register(&ir->miscdev);
	if (ret)
		return dev_err_probe(dev, ret, "register IR device\n");
	platform_set_drvdata(pdev, ir);
	return 0;
}

static void mtk_irtx_shutdown(struct platform_device *pdev)
{
	struct mtk_irtx *ir = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&ir->lock);
	ir->dead = true;
	ret = mtk_irtx_select(ir, false);
	mutex_unlock(&ir->lock);
	if (ret)
		dev_warn(&pdev->dev, "could not idle IR transmitter: %d\n", ret);
}

static void mtk_irtx_remove(struct platform_device *pdev)
{
	struct mtk_irtx *ir = platform_get_drvdata(pdev);

	misc_deregister(&ir->miscdev);
	mtk_irtx_shutdown(pdev);
}

static const struct of_device_id mtk_irtx_of_match[] = {
	{ .compatible = "mediatek,irtx-pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_irtx_of_match);

static struct platform_driver mtk_irtx_driver = {
	.probe = mtk_irtx_probe,
	.remove = mtk_irtx_remove,
	.shutdown = mtk_irtx_shutdown,
	.driver = {
		.name = "mt-irtx",
		.of_match_table = mtk_irtx_of_match,
	},
};
module_platform_driver(mtk_irtx_driver);

MODULE_DESCRIPTION("MediaTek MT6989 PWM infrared transmitter");
MODULE_LICENSE("GPL");
