// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 * Rothko layouts from Xiaomi malachite-u-oss, checked against the stock
 * camera_eeprom_v4l2 module and the Rothko running device tree.
 */

#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include "../imgsensor/inc/kd_imgsensor.h"
#include "cam_cal_format.h"

#define CAM_CALIOC_S_SENSOR_INFO _IOW('i', 10, __u32)
#define CAM_CALIOC_G_GKI_READ _IOWR('i', 15, struct STRUCT_CAM_CAL_DATA_STRUCT)
#define CAM_CALIOC_G_GKI_QUERY _IOR('i', 20, struct STRUCT_CAM_CAL_DATA_STRUCT)
#define CAM_CALIOC_G_GKI_NEED_POWER_ON _IOWR('i', 25, struct STRUCT_CAM_CAL_NEED_POWER_ON)

static_assert(CAM_CALIOC_S_SENSOR_INFO == 0x4004690a);
static_assert(CAM_CALIOC_G_GKI_READ == 0xea88690f);
static_assert(CAM_CALIOC_G_GKI_QUERY == 0xaa886914);
static_assert(CAM_CALIOC_G_GKI_NEED_POWER_ON == 0xc0106919);
static_assert(offsetof(struct STRUCT_CAM_CAL_DATA_STRUCT, sensorID) == 32);
static_assert(offsetof(struct STRUCT_CAM_CAL_DATA_STRUCT, Single2A) == 2164);
static_assert(offsetof(struct STRUCT_CAM_CAL_DATA_STRUCT, PDAF) == 2340);

#define CAM_CAL_RAW_MAX		0x4000
#define CAM_CAL_PRELOAD_SIZE	0x2000
#define CAM_CAL_SEEK_END	0x3f40
#define CAM_CAL_HEADER_OFFSET	0x08
#define CAM_CAL_HEADER_MASK	GENMASK(23, 0)
#define CAM_CAL_DEVICE_IDS	GENMASK(6, 0)
#define CAM_CAL_I2C_CHUNK	1024

/* Module version, part number, LSC and 2A are present on every Rothko module. */
#define CAM_CAL_BASE_COMMANDS GENMASK(CAMERA_CAM_CAL_DATA_3A_GAIN, CAMERA_CAM_CAL_DATA_MODULE_VERSION)
#define CAM_CAL_PDAF_COMMAND BIT(CAMERA_CAM_CAL_DATA_PDAF)

static const char * const eeprom_nodes[] = {
	"camera-eeprom0", "camera-eeprom1", "camera-eeprom2", "camera-eeprom3",
};

struct cam_cal_layout {
	u32 sensor_id;
	u32 header;
	u16 size;
	u16 pdaf_offset;
	u16 commands;
	u8 node;
	u8 addr;
	u8 lsc_rotation;
};

/* The DT reg is the default raw address. Some modules on that bus differ. */
static const struct cam_cal_layout rothko_layouts[] = {
	{
		.sensor_id = ROTHKOIMX906WIDE_SENSOR_ID, .header = 0xff54d420,
		.size = 0x4000, .pdaf_offset = 0x1078, .node = 0, .addr = 0x51,
		.commands = CAM_CAL_BASE_COMMANDS | CAM_CAL_PDAF_COMMAND |
			BIT(CAMERA_CAM_CAL_DATA_STEREO_DATA) | BIT(CAMERA_CAM_CAL_DATA_DUMP),
	}, {
		.sensor_id = ROTHKOOV50H40WIDE_SENSOR_ID, .header = 0xff4cd036,
		.size = 0x4000, .pdaf_offset = 0x0ef1, .node = 0, .addr = 0x51,
		.commands = CAM_CAL_BASE_COMMANDS | CAM_CAL_PDAF_COMMAND,
		.lsc_rotation = 1,
	}, {
		.sensor_id = ROTHKOOV20BFRONT_SENSOR_ID, .header = 0xffffffa3,
		.size = 0x2000, .node = 1, .addr = 0x51,
		.commands = CAM_CAL_BASE_COMMANDS,
	}, {
		.sensor_id = ROTHKOS5KKD1FRONT_SENSOR_ID, .header = 0xffffff35,
		.size = 0x2000, .node = 1, .addr = 0x51,
		.commands = CAM_CAL_BASE_COMMANDS,
	}, {
		.sensor_id = ROTHKOIMX355ULTRA_SENSOR_ID, .header = 0xffffffc1,
		.size = 0x2000, .node = 2, .addr = 0x50,
		.commands = CAM_CAL_BASE_COMMANDS,
	}, {
		.sensor_id = ROTHKOS5KJN1TELE_SENSOR_ID, .header = 0xff11d137,
		.size = 0x2000, .pdaf_offset = 0x136c, .node = 2, .addr = 0x54,
		.commands = CAM_CAL_BASE_COMMANDS | CAM_CAL_PDAF_COMMAND,
	}, {
		.sensor_id = ROTHKOGC02M1MACRO_SENSOR_ID, .header = 0xffffff1f,
		.size = 0x2000, .node = 3, .addr = 0x52,
		.commands = CAM_CAL_BASE_COMMANDS,
	}, {
		.sensor_id = ROTHKOOV13B10ULTRA_SENSOR_ID, .header = 0xffffff1d,
		.size = 0x2000, .node = 3, .addr = 0x50,
		.commands = CAM_CAL_BASE_COMMANDS,
		.lsc_rotation = 1,
	},
};

struct cam_cal_eeprom {
	struct cdev cdev;
	struct device dev;
	struct mutex lock;
	struct i2c_client *client;
	const struct cam_cal_layout *layout;
	u8 *preload;
	unsigned int index;
	char class_name[sizeof("camera_eepromdrv3")];
};

struct cam_cal_file {
	struct cam_cal_eeprom *eeprom;
	u32 sensor_id;
};

static const struct cam_cal_layout *cam_cal_find_layout(unsigned int index, u32 sensor_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rothko_layouts); i++)
		if (rothko_layouts[i].node == index && rothko_layouts[i].sensor_id == sensor_id)
			return &rothko_layouts[i];

	return NULL;
}

static int cam_cal_i2c_read(struct i2c_client *client, u16 addr, u16 offset,
			    void *buf, size_t len)
{
	u8 reg[2];
	struct i2c_msg msgs[] = {
		{ .addr = addr, .len = sizeof(reg), .buf = reg },
		{ .addr = addr, .flags = I2C_M_RD, .buf = buf },
	};
	int ret;

	while (len) {
		put_unaligned_be16(offset, reg);
		msgs[1].len = min_t(size_t, len, CAM_CAL_I2C_CHUNK);
		ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
		if (ret != ARRAY_SIZE(msgs))
			return ret < 0 ? ret : -EIO;

		offset += msgs[1].len;
		msgs[1].buf += msgs[1].len;
		len -= msgs[1].len;
	}

	return 0;
}

static bool cam_cal_select_layout(struct cam_cal_eeprom *eeprom, u32 sensor_id)
{
	const struct cam_cal_layout *layout;
	__le32 header;

	layout = cam_cal_find_layout(eeprom->index, sensor_id);
	if (!layout)
		return false;
	if (eeprom->layout == layout)
		return true;

	if (cam_cal_i2c_read(eeprom->client, layout->addr, CAM_CAL_HEADER_OFFSET,
			     &header, sizeof(header)) ||
	    ((le32_to_cpu(header) ^ layout->header) & CAM_CAL_HEADER_MASK))
		return false;

	kfree(eeprom->preload);
	eeprom->preload = NULL;
	eeprom->layout = layout;
	return true;
}

static void cam_cal_preload(struct cam_cal_eeprom *eeprom)
{
	u8 *buf;

	if (eeprom->preload)
		return;

	buf = kmalloc(CAM_CAL_PRELOAD_SIZE, GFP_KERNEL);
	if (!buf)
		return;
	if (cam_cal_i2c_read(eeprom->client, eeprom->layout->addr, 0,
			     buf, CAM_CAL_PRELOAD_SIZE)) {
		kfree(buf);
		return;
	}

	eeprom->preload = buf;
}

static int cam_cal_read_region(struct cam_cal_eeprom *eeprom, u16 offset,
			       void *buf, size_t len)
{
	if (offset > eeprom->layout->size || len > eeprom->layout->size - offset)
		return -EINVAL;

	if (eeprom->preload && offset <= CAM_CAL_PRELOAD_SIZE &&
	    len <= CAM_CAL_PRELOAD_SIZE - offset) {
		memcpy(buf, eeprom->preload + offset, len);
		return 0;
	}

	return cam_cal_i2c_read(eeprom->client, eeprom->layout->addr, offset, buf, len);
}

static bool cam_cal_read_2a(struct cam_cal_eeprom *eeprom,
			    struct STRUCT_CAM_CAL_SINGLE_2A_STRUCT *data)
{
	struct STRUCT_CAM_CAL_PREGAIN_STRUCT *awb = &data->S2aAwb;
	u16 unit_r, unit_gr, unit_b, gold_r, gold_gr, gold_b;
	u8 awb_buf[16], af_buf[8], flag;

	memset(data, 0, sizeof(*data));
	if (cam_cal_read_region(eeprom, 0x750, &flag, 1))
		return false;
	if (flag == 1) {
		if (cam_cal_read_region(eeprom, 0x754, awb_buf, sizeof(awb_buf)))
			return false;

		unit_r = get_unaligned_be16(awb_buf);
		unit_gr = get_unaligned_be16(awb_buf + 2);
		unit_b = get_unaligned_be16(awb_buf + 6);
		gold_r = get_unaligned_be16(awb_buf + 8);
		gold_gr = get_unaligned_be16(awb_buf + 10);
		gold_b = get_unaligned_be16(awb_buf + 14);
		if (!unit_r || !unit_b || !gold_r || !gold_b)
			return false;

		awb->rUnitGainu4R = 1023 * unit_gr / unit_r;
		awb->rUnitGainu4G = 1023;
		awb->rUnitGainu4B = 1023 * unit_gr / unit_b;
		awb->rGoldGainu4R = 1023 * gold_gr / gold_r;
		awb->rGoldGainu4G = 1023;
		awb->rGoldGainu4B = 1023 * gold_gr / gold_b;
		data->S2aBitEn |= CAM_CAL_AWB_BITEN;
	}

	if (cam_cal_read_region(eeprom, 0x27, &flag, 1))
		return false;
	if (flag == 1) {
		/* Macro distance/DAC, then infinity distance/DAC. */
		if (cam_cal_read_region(eeprom, 0x2b, af_buf, sizeof(af_buf)))
			return false;

		data->S2aAf[0] = get_unaligned_be16(af_buf + 6);
		data->S2aAf[1] = get_unaligned_be16(af_buf + 2);
		data->S2aAF_t.AF_infinite_pattern_distance = get_unaligned_be16(af_buf + 4) * 10;
		data->S2aAF_t.AF_Macro_pattern_distance = get_unaligned_be16(af_buf) * 10;
		data->S2aBitEn |= CAM_CAL_AF_BITEN;
	}

	data->S2aVer = !!data->S2aBitEn;
	return true;
}

static bool cam_cal_read_pdaf(struct cam_cal_eeprom *eeprom,
			      struct STRUCT_CAM_CAL_PDAF_STRUCT *data)
{
	const struct cam_cal_layout *layout = eeprom->layout;
	u16 offset = layout->pdaf_offset;
	unsigned int first = 496, second = 1004;
	unsigned int size;

	if (layout->sensor_id == ROTHKOOV50H40WIDE_SENSOR_ID) {
		first = 976;
		second = 1624;
	}

	/* Each pair skips two bytes between its blocks. */
	if (cam_cal_read_region(eeprom, offset, data->Data, first) ||
	    cam_cal_read_region(eeprom, offset + first + 2, data->Data + first, second))
		return false;
	size = first + second;

	/* IMX906 also supplies an HV-bin pair after the partial OTP pair. */
	if (layout->sensor_id == ROTHKOIMX906WIDE_SENSOR_ID) {
		if (cam_cal_read_region(eeprom, offset + size + 11, data->Data + size, 976) ||
		    cam_cal_read_region(eeprom, offset + size + 976 + 13,
					data->Data + size + 976, 1624))
			return false;
		size += 976 + 1624;
	}

	data->Size_of_PDAF = size;
	return true;
}

static bool cam_cal_parse(struct cam_cal_eeprom *eeprom,
			  struct STRUCT_CAM_CAL_DATA_STRUCT *data)
{
	struct STRUCT_CAM_CAL_LSC_MTK_TYPE *lsc = &data->SingleLsc.LscTable.MtkLcsData;
	u8 version[15];

	switch (data->Command) {
	case CAMERA_CAM_CAL_DATA_MODULE_VERSION:
		/* Stock validates that the module-version bytes can be read. */
		return !cam_cal_read_region(eeprom, 0x01, version, sizeof(version));
	case CAMERA_CAM_CAL_DATA_PART_NUMBER:
		memset(data->PartNumber, 0, sizeof(data->PartNumber));
		if (cam_cal_read_region(eeprom, 0x10, data->PartNumber, 16) ||
		    cam_cal_read_region(eeprom, 0x01, data->PartNumber, 1))
			return false;
		data->PartNumber[1] = 0;
		return true;
	case CAMERA_CAM_CAL_DATA_SHADING_TABLE:
		data->SingleLsc.TableRotation = eeprom->layout->lsc_rotation;
		lsc->MtkLscType = 2;
		lsc->PixId = 8;
		lsc->TableSize = CAM_CAL_SINGLE_LSC_SIZE;
		/* The payload starts at SlimLscType and spans the following fields. */
		return !cam_cal_read_region(eeprom, 0x78c,
				 data->SingleLsc.LscTable.Data +
				 offsetof(struct STRUCT_CAM_CAL_LSC_MTK_TYPE, SlimLscType),
				 CAM_CAL_SINGLE_LSC_SIZE);
	case CAMERA_CAM_CAL_DATA_3A_GAIN:
		return cam_cal_read_2a(eeprom, &data->Single2A);
	case CAMERA_CAM_CAL_DATA_PDAF:
		return cam_cal_read_pdaf(eeprom, &data->PDAF);
	default:
		return false;
	}
}

static unsigned int cam_cal_get_data(struct cam_cal_eeprom *eeprom,
				     struct STRUCT_CAM_CAL_DATA_STRUCT *data)
{
	unsigned int cmd = data->Command;

	if (cmd >= CAMERA_CAM_CAL_DATA_LIST)
		return CAM_CAL_ERR_NO_CMD;
	if (!eeprom->client || !(data->deviceID & CAM_CAL_DEVICE_IDS))
		return CAM_CAL_ERR_NO_DEVICE;
	if (!cam_cal_select_layout(eeprom, data->sensorID) ||
	    !(eeprom->layout->commands & BIT(cmd)))
		return CamCalReturnErr[cmd];

	data->DataVer = CAM_CAL_SINGLE_EEPROM_DATA;
	/* Stock IMX906 DUMP is a status-only command. Raw bytes use read(). */
	if (cmd == CAMERA_CAM_CAL_DATA_DUMP)
		return CAM_CAL_ERR_NO_ERR;
	if (cmd == CAMERA_CAM_CAL_DATA_STEREO_DATA)
		return CamCalReturnErr[cmd]; /* Stock has no stereo handler. */

	cam_cal_preload(eeprom);
	return cam_cal_parse(eeprom, data) ? CAM_CAL_ERR_NO_ERR : CamCalReturnErr[cmd];
}

static unsigned int cam_cal_need_power(struct cam_cal_eeprom *eeprom,
				       struct STRUCT_CAM_CAL_NEED_POWER_ON *query)
{
	unsigned int cmd = query->Command;

	if (cmd >= CAMERA_CAM_CAL_DATA_LIST)
		return CAM_CAL_ERR_NO_CMD;
	if (!eeprom->client || !(query->deviceID & CAM_CAL_DEVICE_IDS))
		return CAM_CAL_ERR_NO_DEVICE;
	if (!cam_cal_select_layout(eeprom, query->sensorID))
		return CamCalReturnErr[cmd];

	query->needPowerOn = !!(eeprom->layout->commands & BIT(cmd));
	return CAM_CAL_ERR_NO_ERR;
}

static long eeprom_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cam_cal_file *ctx = file->private_data;
	struct cam_cal_eeprom *eeprom = ctx->eeprom;
	struct STRUCT_CAM_CAL_NEED_POWER_ON query;
	struct STRUCT_CAM_CAL_DATA_STRUCT *data;
	void __user *argp = (void __user *)arg;
	unsigned int result;
	u32 sensor_id;

	switch (cmd) {
	case CAM_CALIOC_S_SENSOR_INFO:
		if (get_user(sensor_id, (u32 __user *)argp))
			return -EFAULT;
		mutex_lock(&eeprom->lock);
		ctx->sensor_id = sensor_id;
		mutex_unlock(&eeprom->lock);
		return 0;
	case CAM_CALIOC_G_GKI_QUERY:
		return 0; /* Presence query, with no output in the stock ABI. */
	case CAM_CALIOC_G_GKI_READ:
		data = memdup_user(argp, sizeof(*data));
		if (IS_ERR(data))
			return PTR_ERR(data);
		mutex_lock(&eeprom->lock);
		result = cam_cal_get_data(eeprom, data);
		mutex_unlock(&eeprom->lock);
		if (!result && copy_to_user(argp, data, sizeof(*data)))
			result = CAM_CAL_ERR_NO_DEVICE;
		kfree(data);
		return result;
	case CAM_CALIOC_G_GKI_NEED_POWER_ON:
		if (copy_from_user(&query, argp, sizeof(query)))
			return -EFAULT;
		mutex_lock(&eeprom->lock);
		result = cam_cal_need_power(eeprom, &query);
		mutex_unlock(&eeprom->lock);
		if (!result && copy_to_user(argp, &query, sizeof(query)))
			result = CAM_CAL_ERR_NO_DEVICE;
		return result;
	default:
		return _IOC_DIR(cmd) == _IOC_NONE ? -EFAULT : -EPERM;
	}
}

static ssize_t eeprom_read(struct file *file, char __user *user_buf,
			   size_t size, loff_t *offset)
{
	struct cam_cal_file *ctx = file->private_data;
	struct cam_cal_eeprom *eeprom = ctx->eeprom;
	const struct cam_cal_layout *layout;
	ssize_t ret;
	u8 *buf;

	if (!size)
		return 0;
	if (*offset < 0 || *offset > CAM_CAL_RAW_MAX || size > CAM_CAL_RAW_MAX - *offset)
		return -EFAULT;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&eeprom->lock);
	layout = cam_cal_find_layout(eeprom->index, ctx->sensor_id);
	if (!eeprom->client)
		ret = -ENODEV;
	else if (cam_cal_i2c_read(eeprom->client, layout ? layout->addr : eeprom->client->addr,
				  *offset, buf, size))
		ret = -EFAULT;
	else
		ret = size;
	mutex_unlock(&eeprom->lock);

	if (ret > 0) {
		if (copy_to_user(user_buf, buf, size))
			ret = -EFAULT;
		else
			*offset += size;
	}
	kfree(buf);
	return ret;
}

static loff_t eeprom_seek(struct file *file, loff_t offset, int whence)
{
	if (whence > SEEK_END)
		return -EINVAL;

	return generic_file_llseek_size(file, offset, whence, S64_MAX, CAM_CAL_SEEK_END);
}

static int eeprom_open(struct inode *inode, struct file *file)
{
	struct cam_cal_eeprom *eeprom = container_of(inode->i_cdev, struct cam_cal_eeprom, cdev);
	struct cam_cal_file *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	get_device(&eeprom->dev);
	ctx->eeprom = eeprom;
	file->private_data = ctx;
	return 0;
}

static int eeprom_release(struct inode *inode, struct file *file)
{
	struct cam_cal_file *ctx = file->private_data;

	put_device(&ctx->eeprom->dev);
	kfree(ctx);
	return 0;
}

static const struct file_operations eeprom_fops = {
	.owner = THIS_MODULE,
	.open = eeprom_open,
	.release = eeprom_release,
	.read = eeprom_read,
	.llseek = eeprom_seek,
	.unlocked_ioctl = eeprom_ioctl,
#ifdef CONFIG_COMPAT
	/* The supported payloads contain no native pointers or longs. */
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static void eeprom_dev_release(struct device *dev)
{
	struct cam_cal_eeprom *eeprom = container_of(dev, struct cam_cal_eeprom, dev);

	kfree(eeprom->preload);
	kfree(eeprom);
}

static int eeprom_probe(struct i2c_client *client)
{
	struct cam_cal_eeprom *eeprom;
	struct class *class;
	unsigned int index;
	int ret;

	for (index = 0; index < ARRAY_SIZE(eeprom_nodes); index++)
		if (of_node_name_eq(client->dev.of_node, eeprom_nodes[index]))
			break;
	if (index == ARRAY_SIZE(eeprom_nodes))
		return -EINVAL;

	eeprom = kzalloc(sizeof(*eeprom), GFP_KERNEL);
	if (!eeprom)
		return -ENOMEM;

	device_initialize(&eeprom->dev);
	eeprom->dev.release = eeprom_dev_release;
	eeprom->dev.parent = &client->dev;
	eeprom->index = index;
	eeprom->client = client;
	mutex_init(&eeprom->lock);
	ret = dev_set_name(&eeprom->dev, "camera_eeprom%u", index);
	if (ret)
		goto put_dev;

	ret = alloc_chrdev_region(&eeprom->dev.devt, 0, 1, dev_name(&eeprom->dev));
	if (ret)
		goto put_dev;

	snprintf(eeprom->class_name, sizeof(eeprom->class_name), "camera_eepromdrv%u", index);
	class = class_create(eeprom->class_name);
	if (IS_ERR(class)) {
		ret = PTR_ERR(class);
		goto unregister_region;
	}

	eeprom->dev.class = class;
	cdev_init(&eeprom->cdev, &eeprom_fops);
	eeprom->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&eeprom->cdev, &eeprom->dev);
	if (ret) {
		mutex_lock(&eeprom->lock);
		eeprom->client = NULL;
		mutex_unlock(&eeprom->lock);
		class_destroy(class);
		goto unregister_region;
	}

	i2c_set_clientdata(client, eeprom);
	return 0;

unregister_region:
	unregister_chrdev_region(eeprom->dev.devt, 1);
put_dev:
	put_device(&eeprom->dev);
	return ret;
}

static void eeprom_remove(struct i2c_client *client)
{
	struct cam_cal_eeprom *eeprom = i2c_get_clientdata(client);

	cdev_device_del(&eeprom->cdev, &eeprom->dev);
	mutex_lock(&eeprom->lock);
	eeprom->client = NULL;
	mutex_unlock(&eeprom->lock);
	class_destroy(eeprom->dev.class);
	unregister_chrdev_region(eeprom->dev.devt, 1);
	put_device(&eeprom->dev);
}

static const struct of_device_id eeprom_of_match[] = {
	{ .compatible = "mediatek,camera_eeprom" },
	{ }
};
MODULE_DEVICE_TABLE(of, eeprom_of_match);

static struct i2c_driver eeprom_driver = {
	.driver = {
		.name = "mediatek,camera_eeprom",
		.of_match_table = eeprom_of_match,
	},
	.probe = eeprom_probe,
	.remove = eeprom_remove,
};
module_i2c_driver(eeprom_driver);

MODULE_DESCRIPTION("Rothko camera EEPROM calibration driver");
MODULE_LICENSE("GPL v2");
