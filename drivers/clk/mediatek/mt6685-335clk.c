// SPDX-License-Identifier: GPL-2.0
/*
 * Reconstructed from the mt6685-335clk.ko collected read-only from rothko
 * OS2.0.207.0.VNNTWXM (6.1.115-android14-11-gc09015130d76).
 * Source binary SHA256:
 * 79924719af5631bc04d20c69b8177d89aae5ac002c29fe73237c63533badc488
 * This is a reconstruction, not an original MediaTek source release.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mt6685-335clk.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define MT6685_DCXO_EXTBUF3_CW0	0x79c

struct mt6685_335clk {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
};

static struct mt6685_335clk *clk;

bool mt6685_335clk_ready(void)
{
	return clk != NULL;
}
EXPORT_SYMBOL(mt6685_335clk_ready);

void mt6685_set_335_dcxo_mode(unsigned int mode)
{
	unsigned int val = 0;

	if (!clk || !clk->regmap)
		return;

	regmap_read(clk->regmap, MT6685_DCXO_EXTBUF3_CW0, &val);
	dev_info(clk->dev, "%s() MT6685_DCXO_EXTBUF3_CW0 = 0x%x\n",
		 __func__, val);
	regmap_update_bits(clk->regmap, MT6685_DCXO_EXTBUF3_CW0, 3, mode);
	regmap_read(clk->regmap, MT6685_DCXO_EXTBUF3_CW0, &val);
	dev_info(clk->dev, "%s() MT6685_DCXO_EXTBUF3_CW0 = 0x%x\n",
		 __func__, val);
}
EXPORT_SYMBOL(mt6685_set_335_dcxo_mode);

void mt6685_set_335_dcxo(bool enable)
{
	unsigned int val = 0;

	if (!clk || !clk->regmap)
		return;

	regmap_read(clk->regmap, MT6685_DCXO_EXTBUF3_CW0, &val);
	dev_info(clk->dev, "%s() MT6685_DCXO_EXTBUF3_CW0 = 0x%x\n",
		 __func__, val);
	if (enable) {
		regmap_update_bits(clk->regmap, MT6685_DCXO_EXTBUF3_CW0, 4, 4);
		usleep_range(400, 420);
	} else {
		regmap_update_bits(clk->regmap, MT6685_DCXO_EXTBUF3_CW0, 4, 0);
	}
	regmap_read(clk->regmap, MT6685_DCXO_EXTBUF3_CW0, &val);
	dev_info(clk->dev, "%s() MT6685_DCXO_EXTBUF3_CW0 = 0x%x\n",
		 __func__, val);
}
EXPORT_SYMBOL(mt6685_set_335_dcxo);

static int mt6685_335clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6685_335clk *ctx;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->regmap = dev_get_regmap(dev->parent, NULL);
	if (!ctx->regmap) {
		dev_info(dev, "failed to get efuse regmap.\n");
		return -ENODEV;
	}

	mutex_init(&ctx->lock);
	ctx->dev = dev;
	clk = ctx;
	dev_info(dev, "%s done.\n", __func__);

	return 0;
}

static const struct of_device_id mt6685_335clk_of_match[] = {
	{ .compatible = "mediatek,mt6685-335clk" },
	{ }
};

static struct platform_driver mt6685_335clk_driver = {
	.probe = mt6685_335clk_probe,
	.driver = {
		.name = "mt6685-335clk",
		.of_match_table = mt6685_335clk_of_match,
	},
};
module_platform_driver(mt6685_335clk_driver);

MODULE_DESCRIPTION("MediaTek PMIC 335CLK Driver for MT6685 PMIC");
MODULE_LICENSE("GPL v2");
