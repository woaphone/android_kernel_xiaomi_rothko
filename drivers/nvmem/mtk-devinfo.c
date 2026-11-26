// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek device information from the bootloader's chosen ATAG.
 * Based on MediaTek mtk-devinfo.c, Copyright (c) 2020 MediaTek Inc.
 * Author: Mac Lu <mac.lu@mediatek.com>
 */
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

struct mtk_devinfo {
	const u8 *data;
};

static int mtk_devinfo_read(void *context, unsigned int offset,
			    void *value, size_t bytes)
{
	struct mtk_devinfo *priv = context;

	memcpy(value, priv->data + offset, bytes);
	return 0;
}

static int mtk_devinfo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *chosen;
	struct nvmem_config config = {};
	struct mtk_devinfo *priv;
	const u8 *tag;
	u32 words;
	int length;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		chosen = of_find_node_by_path("/chosen@0");
	if (!chosen)
		return dev_err_probe(dev, -ENXIO, "chosen node is missing\n");
	tag = of_get_property(chosen, "atag,devinfo", &length);
	of_node_put(chosen);
	if (!tag || length < sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "atag,devinfo is missing or short\n");
	words = get_unaligned_le32(tag);
	if (!words || words > (length - sizeof(u32)) / sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "atag,devinfo word count is invalid\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->data = devm_kmemdup(dev, tag + sizeof(u32), words * sizeof(u32), GFP_KERNEL);
	if (!priv->data)
		return -ENOMEM;
	config.dev = dev;
	config.name = "mtk-devinfo";
	config.add_legacy_fixed_of_cells = true;
	config.read_only = true;
	config.stride = sizeof(u32);
	config.word_size = sizeof(u32);
	config.size = words * sizeof(u32);
	config.reg_read = mtk_devinfo_read;
	config.priv = priv;
	return PTR_ERR_OR_ZERO(devm_nvmem_register(dev, &config));
}

static const struct of_device_id mtk_devinfo_of_match[] = {
	{ .compatible = "mediatek,devinfo" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_devinfo_of_match);

static struct platform_driver mtk_devinfo_driver = {
	.probe = mtk_devinfo_probe,
	.driver = {
		.name = "mtk-devinfo",
		.of_match_table = mtk_devinfo_of_match,
	},
};
module_platform_driver(mtk_devinfo_driver);

MODULE_AUTHOR("Mac Lu <mac.lu@mediatek.com>");
MODULE_DESCRIPTION("MediaTek bootloader device information NVMEM");
MODULE_LICENSE("GPL");
