// SPDX-License-Identifier: GPL-2.0-only
/* Rothko OCP81373/SGM37ET1 and MediaTek strobe HAL. */
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/thermal.h>
#include <linux/uaccess.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#include "mtk_peak_power_budget.h"

#define OCP_ENABLE		0x01
#define OCP_TORCH0		0x05
#define OCP_FLAGS		0x0a
#define OCP_CHIP_ID		0x0c
#define OCP_ID			0x3a
#define SGM_ID			0x11
#define OCP_OUTPUTS		GENMASK(1, 0)
#define OCP_MODE		GENMASK(3, 2)
#define OCP_TORCH_MODE		0x08
#define OCP_TORCH_MIN_UA	2920
#define OCP_TORCH_MAX_UA	374000
#define OCP_TORCH_DEFAULT_UA	25000
/* Preserve the stock 922 mW torch vote floor. Use a 5 V equivalent
 * times the combined torch current as the bound at higher currents.
 */
#define OCP_TORCH_FLOOR_MW	922
#define OCP_TORCH_EQUIV_MV	5000

#define FL_GET_PART		_IOR('S', 166, int)
#define FL_GET_PROTOCOL		_IOR('S', 190, int)
#define FL_HW_READY		_IOR('S', 220, int)
#define FL_SET_DRIVER		_IOWR('S', 30, int)
#define FL_ONOFF		_IOR('S', 115, int)
#define FL_TORCH_CURRENT	_IOR('S', 233, int)
#define FL_MODE			_IOR('S', 234, int)
#define FL_ENABLE		_IOR('S', 235, int)
#define FL_CHIP_INFO		_IOR('S', 237, int)
#define FL_SET_SCENARIO		_IOWR('S', 215, int)
#define FL_CHARGER_READY	_IOR('S', 210, int)

/* The historical ioctls encode sizeof(int), but exchange three s32s. */
struct fl_user_arg {
	s32 type, ct, value;
};

struct ocp;
struct ocp_channel {
	struct ocp *chip;
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler controls;
};

struct ocp {
	struct kref refs;
	struct mutex lock;
	struct regmap *map;
	struct pinctrl *pinctrl;
	struct pinctrl_state *high, *low;
	struct thermal_cooling_device *cooling;
	struct ocp_channel channel[2];
	unsigned int id, users, torch_ua, reserved_mw;
	unsigned long cooling_state;
	bool present, powered, enabled, legacy_torch_mode;
};

struct fl_file {
	struct ocp *chip;
	unsigned int users;
};

static const unsigned int thermal_ua[] = {
	OCP_TORCH_MAX_UA, 200000, 150000, 100000, 50000, 25000,
};

static DEFINE_MUTEX(fl_lock);
static struct ocp *fl_chip;
static struct class *fl_class;
static struct cdev fl_cdev;
static dev_t fl_devt;

static void ocp_free(struct kref *ref)
{
	struct ocp *chip = container_of(ref, struct ocp, refs);

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(chip->channel); i++) {
		struct ocp_channel *channel = &chip->channel[i];

		if (!channel->sd.ctrl_handler)
			continue;
		v4l2_ctrl_handler_free(&channel->controls);
		media_entity_cleanup(&channel->sd.entity);
		of_node_put(to_of_node(channel->sd.fwnode));
	}
	mutex_destroy(&chip->lock);
	kfree(chip);
}

static int ocp_power_off(struct ocp *chip)
{
	int ret = 0;

	if (chip->powered) {
		ret = pinctrl_select_state(chip->pinctrl, chip->low);
		if (ret)
			return ret;
		chip->powered = false;
		chip->enabled = false;
	}
	if (chip->reserved_mw) {
		ret = rothko_ppb_set_flashlight_power(0);
		if (!ret)
			chip->reserved_mw = 0;
	}
	return ret;
}

static int ocp_power_get(struct ocp *chip)
{
	int ret;

	if (!chip->present)
		return -ENODEV;
	if (chip->users++)
		return 0;
	ret = pinctrl_select_state(chip->pinctrl, chip->high);
	if (ret)
		goto fail;
	chip->powered = true;
	usleep_range(1000, 2000);
	ret = regmap_update_bits(chip->map, OCP_ENABLE, OCP_MODE | OCP_OUTPUTS, 0);
	if (!ret)
		return 0;
	ocp_power_off(chip);
fail:
	chip->users--;
	return ret;
}

static int ocp_power_put(struct ocp *chip)
{
	if (!chip->users || --chip->users)
		return 0;
	return ocp_power_off(chip);
}

static unsigned int ocp_torch_code(struct ocp *chip, unsigned int ua)
{
	unsigned int emitter_ua = ua / 2;

	if (chip->id == OCP_ID)
		return min(emitter_ua > 2920 ? (emitter_ua - 2920) / 2920 : 0,
			   127U);
	if (ua <= 132001)
		return min((emitter_ua - 2350) / 2110, 127U);
	return min((emitter_ua - 7330) / 1970, 127U);
}

static int ocp_disable(struct ocp *chip)
{
	int ret;

	if (chip->powered && chip->enabled) {
		ret = regmap_update_bits(chip->map, OCP_ENABLE,
					 OCP_MODE | OCP_OUTPUTS, 0);
		if (ret) {
			ocp_power_off(chip);
			return ret;
		}
		chip->enabled = false;
	}
	if (!chip->reserved_mw)
		return 0;
	ret = rothko_ppb_set_flashlight_power(0);
	if (!ret)
		chip->reserved_mw = 0;
	return ret;
}

static int ocp_program_torch(struct ocp *chip, unsigned int ua)
{
	unsigned int code;
	int ret;

	if (!chip->powered)
		return -EPIPE;
	ua = min(ua, thermal_ua[chip->cooling_state]);
	if (ua < 5840)
		return ocp_disable(chip);
	code = max(OCP_TORCH_FLOOR_MW,
		   DIV_ROUND_UP(ua * OCP_TORCH_EQUIV_MV, 1000000U));
	if (code > chip->reserved_mw) {
		ret = rothko_ppb_set_flashlight_power(code);
		if (ret)
			return ret;
		chip->reserved_mw = code;
	}
	code = ocp_torch_code(chip, ua);
	ret = regmap_update_bits(chip->map, OCP_TORCH0, 0x7f, code);
	if (!ret)
		ret = regmap_update_bits(chip->map, OCP_TORCH0 + 1, 0x7f, code);
	return ret;
}

static int ocp_torch(struct ocp *chip)
{
	int ret;

	if (!chip->powered)
		return -EPIPE;
	if (chip->torch_ua < 5840)
		return ocp_disable(chip);
	ret = ocp_program_torch(chip, chip->torch_ua);
	if (!ret) {
		chip->enabled = true; /* Also quiesce on an ambiguous I2C error. */
		ret = regmap_update_bits(chip->map, OCP_ENABLE,
			OCP_MODE | OCP_OUTPUTS, OCP_TORCH_MODE | OCP_OUTPUTS);
	}
	if (ret)
		ocp_disable(chip);
	return ret;
}

static int ocp_cooling_max(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = ARRAY_SIZE(thermal_ua) - 1;
	return 0;
}

static int ocp_cooling_get(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct ocp *chip = cdev->devdata;

	*state = READ_ONCE(chip->cooling_state);
	return 0;
}

static int ocp_cooling_set(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct ocp *chip = cdev->devdata;
	int ret = 0;

	if (state >= ARRAY_SIZE(thermal_ua))
		return -EINVAL;
	mutex_lock(&chip->lock);
	chip->cooling_state = state;
	if (chip->enabled) {
		ret = ocp_program_torch(chip, chip->torch_ua);
		if (ret)
			ocp_disable(chip);
	}
	mutex_unlock(&chip->lock);
	return ret;
}

static const struct thermal_cooling_device_ops ocp_cooling_ops = {
	.get_max_state = ocp_cooling_max,
	.get_cur_state = ocp_cooling_get,
	.set_cur_state = ocp_cooling_set,
};

static int fl_open(struct inode *inode, struct file *file)
{
	struct fl_file *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mutex_lock(&fl_lock);
	ctx->chip = fl_chip;
	if (ctx->chip)
		kref_get(&ctx->chip->refs);
	mutex_unlock(&fl_lock);
	if (!ctx->chip) {
		kfree(ctx);
		return -ENODEV;
	}
	file->private_data = ctx;
	return 0;
}

static int fl_release(struct inode *inode, struct file *file)
{
	struct fl_file *ctx = file->private_data;
	struct ocp *chip = ctx->chip;

	mutex_lock(&chip->lock);
	while (chip->present && ctx->users) {
		ctx->users--;
		ocp_power_put(chip);
	}
	mutex_unlock(&chip->lock);
	kref_put(&chip->refs, ocp_free);
	kfree(ctx);
	return 0;
}

static long fl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fl_file *ctx = file->private_data;
	struct ocp *chip = ctx->chip;
	void __user *user = (void __user *)arg;
	struct fl_user_arg req;
	bool result = false;
	int ret = 0;

	if (copy_from_user(&req, user, sizeof(req)))
		return -EFAULT;
	if (req.type != 1 || (req.ct != 1 && req.ct != 2))
		return -EINVAL;
	mutex_lock(&chip->lock);
	if (!chip->present) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case FL_GET_PART:
	case FL_HW_READY:
		req.value = 1;
		result = true;
		break;
	case FL_CHARGER_READY:
		req.value = 1; /* Stock dispatcher default for this chip. */
		result = true;
		break;
	case FL_GET_PROTOCOL:
		ret = 2;
		break;
	case FL_CHIP_INFO:
		req.value = OCP_TORCH_MIN_UA;
		result = true;
		break;
	case FL_SET_DRIVER:
		if (req.value) {
			ret = ocp_power_get(chip);
			if (!ret)
				ctx->users++;
		} else if (ctx->users) {
			ctx->users--;
			ret = ocp_power_put(chip);
		}
		break;
	case FL_ONOFF:
		if (!req.value) {
			ret = ocp_disable(chip);
			break;
		}
		chip->torch_ua = OCP_TORCH_DEFAULT_UA;
		ret = ocp_torch(chip);
		break;
	case FL_TORCH_CURRENT:
		if (req.value < 0) {
			ret = -EINVAL;
			break;
		}
		chip->torch_ua = min_t(unsigned int, req.value, OCP_TORCH_MAX_UA);
		if (chip->enabled) {
			ret = ocp_program_torch(chip, chip->torch_ua);
			if (ret)
				ocp_disable(chip);
		}
		break;
	case FL_MODE:
		chip->legacy_torch_mode = req.value == V4L2_FLASH_LED_MODE_TORCH;
		if (!chip->legacy_torch_mode) {
			ret = ocp_disable(chip);
			if (!ret && req.value != V4L2_FLASH_LED_MODE_NONE)
				ret = -EOPNOTSUPP;
		}
		break;
	case FL_ENABLE:
		ret = !req.value ? ocp_disable(chip) : chip->legacy_torch_mode ?
			ocp_torch(chip) : -EOPNOTSUPP;
		break;
	case FL_SET_SCENARIO:
		/* HAL charger/scenario hint. Torch already reserves the higher vote. */
		if (req.value != 0 && req.value != 1)
			ret = -EINVAL;
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&chip->lock);
	if (!ret && result && copy_to_user(user, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

static const struct file_operations fl_fops = {
	.owner = THIS_MODULE,
	.open = fl_open,
	.release = fl_release,
	.unlocked_ioctl = fl_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static int fl_probe(struct platform_device *pdev)
{
	struct device *dev;
	int ret;

	ret = alloc_chrdev_region(&fl_devt, 0, 1, "flashlight");
	if (ret)
		return ret;
	cdev_init(&fl_cdev, &fl_fops);
	fl_cdev.owner = THIS_MODULE;
	ret = cdev_add(&fl_cdev, fl_devt, 1);
	if (ret)
		goto free_number;
	fl_class = class_create("flashlight_core");
	if (IS_ERR(fl_class)) {
		ret = PTR_ERR(fl_class);
		goto del_cdev;
	}
	dev = device_create(fl_class, NULL, fl_devt, NULL, "flashlight");
	if (!IS_ERR(dev))
		return 0;
	ret = PTR_ERR(dev);
	class_destroy(fl_class);
del_cdev:
	cdev_del(&fl_cdev);
free_number:
	unregister_chrdev_region(fl_devt, 1);
	return ret;
}

static void fl_remove(struct platform_device *pdev)
{
	device_destroy(fl_class, fl_devt);
	class_destroy(fl_class);
	cdev_del(&fl_cdev);
	unregister_chrdev_region(fl_devt, 1);
}

static const struct of_device_id fl_of_match[] = {
	{ .compatible = "mediatek,flashlight_core" },
	{ }
};
MODULE_DEVICE_TABLE(of, fl_of_match);

static struct platform_driver fl_driver = {
	.probe = fl_probe,
	.remove = fl_remove,
	.driver = {
		.name = "flashlight_core",
		.of_match_table = fl_of_match,
		.suppress_bind_attrs = true,
	},
};

static int ocp_sd_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ocp_channel *channel = v4l2_get_subdevdata(sd);
	struct ocp *chip = channel->chip;
	int ret;

	mutex_lock(&chip->lock);
	ret = ocp_power_get(chip);
	mutex_unlock(&chip->lock);
	return ret;
}

static int ocp_sd_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ocp_channel *channel = v4l2_get_subdevdata(sd);
	struct ocp *chip = channel->chip;
	int ret = 0;

	mutex_lock(&chip->lock);
	if (chip->present)
		ret = ocp_power_put(chip);
	mutex_unlock(&chip->lock);
	return ret;
}

static int ocp_sd_registered(struct v4l2_subdev *sd)
{
	struct ocp_channel *channel = v4l2_get_subdevdata(sd);

	kref_get(&channel->chip->refs);
	return 0;
}

static void ocp_sd_release(struct v4l2_subdev *sd)
{
	struct ocp_channel *channel = v4l2_get_subdevdata(sd);

	kref_put(&channel->chip->refs, ocp_free);
}

static const struct v4l2_subdev_internal_ops ocp_internal_ops = {
	.registered = ocp_sd_registered,
	.open = ocp_sd_open,
	.close = ocp_sd_close,
	.release = ocp_sd_release,
};

static int ocp_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ocp_channel *channel = container_of(ctrl->handler,
						 struct ocp_channel, controls);
	struct ocp *chip = channel->chip;
	int ret = 0;

	mutex_lock(&chip->lock);
	if (!chip->present) {
		ret = -ENODEV;
		goto out;
	}
	switch (ctrl->id) {
	case V4L2_CID_FLASH_LED_MODE:
		ret = ctrl->val == V4L2_FLASH_LED_MODE_TORCH ?
			ocp_torch(chip) : ocp_disable(chip);
		break;
	case V4L2_CID_FLASH_TORCH_INTENSITY:
		chip->torch_ua = ctrl->val;
		if (chip->enabled) {
			ret = ocp_program_torch(chip, chip->torch_ua);
			if (ret)
				ocp_disable(chip);
		}
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&chip->lock);
	return ret;
}

static int ocp_get_fault(struct v4l2_ctrl *ctrl)
{
	struct ocp_channel *channel = container_of(ctrl->handler,
						 struct ocp_channel, controls);
	struct ocp *chip = channel->chip;
	unsigned int flags;
	int ret;

	mutex_lock(&chip->lock);
	ret = chip->present && chip->powered ?
		regmap_read(chip->map, OCP_FLAGS, &flags) : -ENODEV;
	if (!ret) {
		ctrl->val = 0;
		if (flags & BIT(0))
			ctrl->val |= V4L2_FLASH_FAULT_TIMEOUT;
		if (flags & BIT(2))
			ctrl->val |= V4L2_FLASH_FAULT_OVER_TEMPERATURE;
		if (flags & (BIT(4) | BIT(5)))
			ctrl->val |= V4L2_FLASH_FAULT_SHORT_CIRCUIT;
	}
	mutex_unlock(&chip->lock);
	return ret;
}

static const struct v4l2_ctrl_ops ocp_ctrl_ops = {
	.s_ctrl = ocp_set_ctrl,
	.g_volatile_ctrl = ocp_get_fault,
};

static const struct v4l2_subdev_ops ocp_subdev_ops;
static const struct regmap_config ocp_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = OCP_CHIP_ID,
};

static int ocp_register_channel(struct device *dev, struct ocp_channel *channel,
				unsigned int id)
{
	struct v4l2_ctrl_handler *hdl = &channel->controls;
	struct v4l2_subdev *sd = &channel->sd;
	struct device_node *np;
	struct v4l2_ctrl *fault;
	int ret;

	for_each_available_child_of_node(dev->of_node, np) {
		u32 reg, type, ct, part;

		if (!of_property_read_u32(np, "reg", &reg) && reg == id &&
		    !of_property_read_u32(np, "type", &type) && type == 0 &&
		    !of_property_read_u32(np, "ct", &ct) && ct == id &&
		    !of_property_read_u32(np, "part", &part) && part == 0)
			break;
	}
	if (!np)
		return -EINVAL;
	v4l2_subdev_init(sd, &ocp_subdev_ops);
	v4l2_set_subdevdata(sd, channel);
	sd->owner = THIS_MODULE;
	sd->dev = dev;
	sd->fwnode = of_fwnode_handle(np);
	sd->internal_ops = &ocp_internal_ops;
	sd->flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(sd->name, sizeof(sd->name), "ocp81373-led%u", id);
	sd->entity.function = MEDIA_ENT_F_FLASH;
	ret = media_entity_pads_init(&sd->entity, 0, NULL);
	if (ret)
		goto put_node;
	ret = v4l2_ctrl_handler_init(hdl, 3);
	if (ret)
		goto clean_controls;
	v4l2_ctrl_new_std_menu(hdl, &ocp_ctrl_ops, V4L2_CID_FLASH_LED_MODE,
			      V4L2_FLASH_LED_MODE_TORCH,
			      BIT(V4L2_FLASH_LED_MODE_FLASH), V4L2_FLASH_LED_MODE_NONE);
	v4l2_ctrl_new_std(hdl, &ocp_ctrl_ops, V4L2_CID_FLASH_TORCH_INTENSITY,
			 OCP_TORCH_MIN_UA, OCP_TORCH_MAX_UA, OCP_TORCH_MIN_UA,
			 rounddown(OCP_TORCH_DEFAULT_UA, OCP_TORCH_MIN_UA));
	fault = v4l2_ctrl_new_std(hdl, &ocp_ctrl_ops, V4L2_CID_FLASH_FAULT,
				 0, V4L2_FLASH_FAULT_TIMEOUT | V4L2_FLASH_FAULT_OVER_TEMPERATURE |
				 V4L2_FLASH_FAULT_SHORT_CIRCUIT, 0, 0);
	if (fault)
		fault->flags |= V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY;
	ret = hdl->error;
	if (ret)
		goto clean_controls;
	sd->ctrl_handler = hdl;
	ret = v4l2_async_register_subdev(sd);
	if (!ret)
		return 0;
	sd->ctrl_handler = NULL;
clean_controls:
	v4l2_ctrl_handler_free(hdl);
	media_entity_cleanup(&sd->entity);
put_node:
	of_node_put(np);
	return ret;
}

static void ocp_unregister_channel(struct ocp_channel *channel)
{
	v4l2_async_unregister_subdev(&channel->sd);
}

static int ocp_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ocp *chip;
	int ret, off_ret, i;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	kref_init(&chip->refs);
	mutex_init(&chip->lock);
	chip->map = devm_regmap_init_i2c(client, &ocp_regmap);
	if (IS_ERR(chip->map)) {
		ret = PTR_ERR(chip->map);
		goto free_chip;
	}
	chip->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(chip->pinctrl)) {
		ret = PTR_ERR(chip->pinctrl);
		goto free_chip;
	}
	chip->high = pinctrl_lookup_state(chip->pinctrl, "hwen-high");
	chip->low = pinctrl_lookup_state(chip->pinctrl, "hwen-low");
	if (IS_ERR(chip->high) || IS_ERR(chip->low)) {
		ret = IS_ERR(chip->high) ? PTR_ERR(chip->high) : PTR_ERR(chip->low);
		goto free_chip;
	}
	ret = pinctrl_select_state(chip->pinctrl, chip->high);
	if (ret)
		goto free_chip;
	usleep_range(1000, 2000);
	ret = regmap_read(chip->map, OCP_CHIP_ID, &chip->id);
	off_ret = pinctrl_select_state(chip->pinctrl, chip->low);
	if (!ret)
		ret = off_ret;
	if (ret)
		goto free_chip;
	if (chip->id != OCP_ID && chip->id != SGM_ID) {
		dev_err(dev, "unsupported flash chip ID %#x\n", chip->id);
		ret = -ENODEV;
		goto free_chip;
	}
	chip->torch_ua = OCP_TORCH_DEFAULT_UA;
	chip->cooling = thermal_of_cooling_device_register(dev->of_node,
					    "ocp81373", chip, &ocp_cooling_ops);
	if (IS_ERR(chip->cooling)) {
		ret = PTR_ERR(chip->cooling);
		goto free_chip;
	}
	for (i = 0; i < ARRAY_SIZE(chip->channel); i++) {
		chip->channel[i].chip = chip;
		ret = ocp_register_channel(dev, &chip->channel[i], i);
		if (ret)
			goto unregister_channels;
	}
	mutex_lock(&fl_lock);
	if (fl_chip) {
		mutex_unlock(&fl_lock);
		ret = -EBUSY;
		goto unregister_channels;
	}
	mutex_lock(&chip->lock);
	chip->present = true;
	mutex_unlock(&chip->lock);
	fl_chip = chip;
	mutex_unlock(&fl_lock);
	i2c_set_clientdata(client, chip);
	dev_info(dev, "flash chip ID %#x, two torch channels\n", chip->id);
	return 0;

unregister_channels:
	while (i--)
		ocp_unregister_channel(&chip->channel[i]);
	thermal_cooling_device_unregister(chip->cooling);
free_chip:
	kref_put(&chip->refs, ocp_free);
	return dev_err_probe(dev, ret, "cannot initialize torch\n");
}

static void ocp_shutdown(struct i2c_client *client)
{
	struct ocp *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->lock);
	chip->present = false;
	ocp_power_off(chip);
	mutex_unlock(&chip->lock);
}

static void ocp_remove(struct i2c_client *client)
{
	struct ocp *chip = i2c_get_clientdata(client);
	int i;

	mutex_lock(&fl_lock);
	fl_chip = NULL;
	mutex_unlock(&fl_lock);
	ocp_shutdown(client);
	thermal_cooling_device_unregister(chip->cooling);
	for (i = 0; i < ARRAY_SIZE(chip->channel); i++)
		ocp_unregister_channel(&chip->channel[i]);
	kref_put(&chip->refs, ocp_free);
}

static const struct of_device_id ocp_of_match[] = {
	{ .compatible = "mediatek,ocp81373" },
	{ }
};
MODULE_DEVICE_TABLE(of, ocp_of_match);

static struct i2c_driver ocp_driver = {
	.probe = ocp_probe,
	.remove = ocp_remove,
	.shutdown = ocp_shutdown,
	.driver = {
		.name = "ocp81373",
		.of_match_table = ocp_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init ocp_init(void)
{
	int ret;

	ret = i2c_add_driver(&ocp_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&fl_driver);
	if (ret)
		i2c_del_driver(&ocp_driver);
	return ret;
}
device_initcall(ocp_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rothko OCP81373/SGM37ET1 torch");
