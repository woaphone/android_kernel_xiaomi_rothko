// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2020 MediaTek Inc. */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* Both MT6363 and MT6373 use the byte-wide V2 OTP controller. */
#define EFUSE_CK_PDN		0x10f
#define EFUSE_CK_HWEN		0x121
#define EFUSE_OTP_CLK		0x38a
#define EFUSE_OTP_ADDR		0x38b
#define EFUSE_OTP_TRIGGER	0x395
#define EFUSE_OTP_SW_READ	0x398
#define EFUSE_OTP_DATA		0x399
#define EFUSE_OTP_BUSY		0x39b

#define EFUSE_SET		1
#define EFUSE_CLR		2
#define EFUSE_CK_PDN_MASK	BIT(4)
#define EFUSE_CK_HWEN_MASK	BIT(2)
#define EFUSE_OTP_CLK_MASK	GENMASK(1, 0)
#define EFUSE_OTP_BUSY_MASK	BIT(0)
#define EFUSE_ROW_BYTES		2
#define EFUSE_SIZE		(128 * EFUSE_ROW_BYTES)

struct mt635x_efuse {
	struct regmap *regmap;
	struct mutex lock;
};

static void mt635x_efuse_restore(struct regmap *map, unsigned int reg,
				 unsigned int val, int *ret)
{
	int err = regmap_write(map, reg, val);

	if (!*ret)
		*ret = err;
}

static int mt635x_efuse_read(void *ctx, unsigned int offset, void *val,
			    size_t bytes)
{
	struct mt635x_efuse *efuse = ctx;
	struct regmap *map = efuse->regmap;
	unsigned int trigger, busy;
	u8 *buf = val;
	int ret;

	if (!IS_ALIGNED(offset, EFUSE_ROW_BYTES) ||
	    !IS_ALIGNED(bytes, EFUSE_ROW_BYTES))
		return -EINVAL;

	mutex_lock(&efuse->lock);
	ret = regmap_write(map, EFUSE_CK_HWEN + EFUSE_CLR, EFUSE_CK_HWEN_MASK);
	if (ret)
		goto unlock;
	ret = regmap_write(map, EFUSE_CK_PDN + EFUSE_CLR, EFUSE_CK_PDN_MASK);
	if (ret)
		goto disable_hwen;
	ret = regmap_write(map, EFUSE_OTP_CLK, EFUSE_OTP_CLK_MASK);
	if (ret)
		goto disable_clock;
	ret = regmap_write(map, EFUSE_OTP_SW_READ, 1);
	if (ret)
		goto disable_read;
	ret = regmap_read(map, EFUSE_OTP_TRIGGER, &trigger);
	if (ret)
		goto disable_read;

	for (; bytes; bytes -= EFUSE_ROW_BYTES) {
		ret = regmap_write(map, EFUSE_OTP_ADDR, offset);
		if (ret)
			break;
		/* Reading each row requires an edge on the trigger bit. */
		trigger = !(trigger & BIT(0));
		udelay(40);
		ret = regmap_write(map, EFUSE_OTP_TRIGGER, trigger);
		if (ret)
			break;
		udelay(50);
		ret = regmap_read_poll_timeout(map, EFUSE_OTP_BUSY, busy,
					       !(busy & EFUSE_OTP_BUSY_MASK),
					       50, 30000);
		if (ret)
			break;
		udelay(30);
		ret = regmap_bulk_read(map, EFUSE_OTP_DATA, buf, EFUSE_ROW_BYTES);
		if (ret)
			break;
		offset += EFUSE_ROW_BYTES;
		buf += EFUSE_ROW_BYTES;
	}

disable_read:
	mt635x_efuse_restore(map, EFUSE_OTP_SW_READ, 0, &ret);
	mt635x_efuse_restore(map, EFUSE_OTP_CLK, 0, &ret);
disable_clock:
	mt635x_efuse_restore(map, EFUSE_CK_PDN + EFUSE_SET, EFUSE_CK_PDN_MASK, &ret);
disable_hwen:
	mt635x_efuse_restore(map, EFUSE_CK_HWEN + EFUSE_SET, EFUSE_CK_HWEN_MASK, &ret);
unlock:
	mutex_unlock(&efuse->lock);

	return ret;
}

static int mt635x_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_config config = {
		.dev = dev,
		.name = dev_name(dev),
		.id = NVMEM_DEVID_NONE,
		.type = NVMEM_TYPE_OTP,
		.size = EFUSE_SIZE,
		.word_size = EFUSE_ROW_BYTES,
		.stride = EFUSE_ROW_BYTES,
		.read_only = true,
		.add_legacy_fixed_of_cells = true,
		.reg_read = mt635x_efuse_read,
	};
	struct mt635x_efuse *efuse;

	efuse = devm_kzalloc(dev, sizeof(*efuse), GFP_KERNEL);
	if (!efuse)
		return -ENOMEM;

	efuse->regmap = dev_get_regmap(dev->parent, NULL);
	if (!efuse->regmap)
		return dev_err_probe(dev, -ENODEV, "missing parent PMIC regmap\n");

	mutex_init(&efuse->lock);
	config.priv = efuse;

	return PTR_ERR_OR_ZERO(devm_nvmem_register(dev, &config));
}

static const struct of_device_id mt635x_efuse_of_match[] = {
	{ .compatible = "mediatek,mt6363-efuse" },
	{ .compatible = "mediatek,mt6373-efuse" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt635x_efuse_of_match);

static struct platform_driver mt635x_efuse_driver = {
	.probe = mt635x_efuse_probe,
	.driver = {
		.name = "mt635x-efuse",
		.of_match_table = mt635x_efuse_of_match,
	},
};
module_platform_driver(mt635x_efuse_driver);

MODULE_SOFTDEP("pre: mtk_spmi_pmic");
MODULE_AUTHOR("Jeter Chen <Jeter.Chen@mediatek.com>");
MODULE_DESCRIPTION("MediaTek MT6363/MT6373 PMIC eFuse driver");
MODULE_LICENSE("GPL");
