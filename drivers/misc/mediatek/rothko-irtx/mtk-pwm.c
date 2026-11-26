// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 * MT6989 memory-mode port of the MediaTek pwm_v2 HAL.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "mtk-pwm.h"

#define PWM_ENABLE		0x00
#define PWM_CHANNEL(n)		(0x100 * ((n) + 1))
#define PWM_CON			0x00
#define PWM_HDURATION		0x04
#define PWM_LDURATION		0x08
#define PWM_GDURATION		0x0c
#define PWM_BUF0_BASE		0x10
#define PWM_BUF0_SIZE		0x14
#define PWM_WAVE_NUM		0x28
#define PWM_BUF_BASE_HIGH	0x3c

#define PWM_CON_MEMORY		BIT(5)
#define PWM_CON_STOP_BITS	GENMASK(14, 9)
#define PWM_BUF0_HIGH_MASK	GENMASK(3, 0)
#define PWM_DMA_BITS		36
#define PWM_VERSION_MT6989	3

struct mtk_rothko_pwm {
	void __iomem *base;
	struct regmap *src;
	struct clk_bulk_data common[2];
	struct clk *clk[MTK_PWM_CHANNELS];
	u32 src_reg;
	u32 bus_shift;
	u32 chan_shift[MTK_PWM_CHANNELS];
};

/* Serializes transfers with provider removal, including failed rebinds. */
static DEFINE_MUTEX(pwm_lock);

static void mtk_rothko_pwm_put(void *data)
{
	put_device(data);
}

struct device *mtk_rothko_pwm_get(struct device *consumer)
{
	struct platform_device *pdev;
	struct device_node *node;
	struct device *dev;
	int ret;

	/* The original DT describes the controller and transmitter as siblings. */
	node = of_get_compatible_child(consumer->of_node->parent, "mediatek,pwm");
	if (!node)
		return ERR_PTR(-ENODEV);
	if (!of_device_is_available(node)) {
		of_node_put(node);
		return ERR_PTR(-ENODEV);
	}
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);

	dev = &pdev->dev;
	ret = devm_add_action_or_reset(consumer, mtk_rothko_pwm_put, dev);
	if (ret)
		return ERR_PTR(ret);
	if (!device_link_add(consumer, dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return ERR_PTR(-EINVAL);

	mutex_lock(&pwm_lock);
	ret = dev_get_drvdata(dev) ? 0 : -EPROBE_DEFER;
	mutex_unlock(&pwm_lock);
	return ret ? ERR_PTR(ret) : dev;
}
EXPORT_SYMBOL_GPL(mtk_rothko_pwm_get);

int mtk_rothko_pwm_send(struct device *dev, u32 channel, u32 ticks,
			const void *wave, u32 words, unsigned int duration_us)
{
	struct mtk_rothko_pwm *pwm;
	void __iomem *regs;
	dma_addr_t addr;
	size_t len;
	u32 ctrl, mask, select;
	void *buf;
	int ret;

	if (channel >= MTK_PWM_CHANNELS || !ticks || ticks > MTK_PWM_MAX_TICKS ||
	    !words || words > MTK_PWM_MAX_WORDS)
		return -EINVAL;
	mutex_lock(&pwm_lock);
	pwm = dev_get_drvdata(dev);
	if (!pwm) {
		ret = -ENODEV;
		goto out_unlock;
	}
	regs = pwm->base + PWM_CHANNEL(channel);
	len = words * sizeof(u32);
	buf = dma_alloc_coherent(dev, len, &addr, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	memcpy(buf, wave, len);

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(pwm->common), pwm->common);
	if (ret)
		goto out_free;
	ret = clk_prepare_enable(pwm->clk[channel]);
	if (ret)
		goto out_common;

	ctrl = readl(pwm->base + PWM_ENABLE);
	if (ctrl & BIT(channel)) {
		ret = -EBUSY;
		goto out_channel;
	}
	mask = (3U << pwm->chan_shift[channel]) | (3U << pwm->bus_shift);
	select = BIT(pwm->chan_shift[channel]) | BIT(pwm->bus_shift);
	ret = regmap_update_bits(pwm->src, pwm->src_reg, mask, select);
	if (ret)
		goto out_channel;

	/* One memory-mode waveform, bit 31 last, 26 MHz undivided. */
	writel(PWM_CON_MEMORY | FIELD_PREP(PWM_CON_STOP_BITS, 31), regs + PWM_CON);
	writel(ticks - 1, regs + PWM_HDURATION);
	writel(ticks - 1, regs + PWM_LDURATION);
	writel(0, regs + PWM_GDURATION);
	writel(lower_32_bits(addr), regs + PWM_BUF0_BASE);
	writel((readl(regs + PWM_BUF_BASE_HIGH) & ~PWM_BUF0_HIGH_MASK) |
	       FIELD_PREP(PWM_BUF0_HIGH_MASK, upper_32_bits(addr)),
	       regs + PWM_BUF_BASE_HIGH);
	writel(words - 1, regs + PWM_BUF0_SIZE);
	writel(1, regs + PWM_WAVE_NUM);
	dma_wmb();
	writel(ctrl | BIT(channel), pwm->base + PWM_ENABLE);
	readl(pwm->base + PWM_ENABLE);

	fsleep(duration_us);
	writel(ctrl, pwm->base + PWM_ENABLE);
	readl(pwm->base + PWM_ENABLE);
	/* Give outstanding reads the native HAL stop interval before freeing DMA. */
	usleep_range(1000, 1100);

out_channel:
	clk_disable_unprepare(pwm->clk[channel]);
out_common:
	clk_bulk_disable_unprepare(ARRAY_SIZE(pwm->common), pwm->common);
out_free:
	dma_free_coherent(dev, len, buf, addr);
out_unlock:
	mutex_unlock(&pwm_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_rothko_pwm_send);

static int mtk_rothko_pwm_probe(struct platform_device *pdev)
{
	static const char * const chan_clk[] = {
		"PWM1-main", "PWM2-main", "PWM3-main", "PWM4-main",
	};
	static const char * const chan_shift[] = {
		"mediatek,pwm1-bclk-sw-ctrl-offset", "mediatek,pwm2-bclk-sw-ctrl-offset",
		"mediatek,pwm3-bclk-sw-ctrl-offset", "mediatek,pwm4-bclk-sw-ctrl-offset",
	};
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct mtk_rothko_pwm *pwm;
	u32 version;
	int ret, i;

	if (of_property_read_u32(np, "mediatek,pwm-version", &version) ||
	    version != PWM_VERSION_MT6989)
		return -ENODEV;
	pwm = devm_kzalloc(dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm)
		return -ENOMEM;
	if (of_property_read_u32(np, "mediatek,pwm-topclk-ctl-reg", &pwm->src_reg) ||
	    of_property_read_u32(np, "mediatek,pwm-bclk-sw-ctrl-offset", &pwm->bus_shift) ||
	    pwm->bus_shift > 30)
		return -EINVAL;

	pwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pwm->base))
		return PTR_ERR(pwm->base);
	pwm->src = syscon_regmap_lookup_by_phandle(np, "pwmsrcclk");
	if (IS_ERR(pwm->src))
		return dev_err_probe(dev, PTR_ERR(pwm->src), "PWM source syscon\n");

	pwm->common[0].id = "PWM-main";
	pwm->common[1].id = "PWM-HCLK-main";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(pwm->common), pwm->common);
	if (ret)
		return dev_err_probe(dev, ret, "PWM bus clocks\n");
	for (i = 0; i < MTK_PWM_CHANNELS; i++) {
		if (of_property_read_u32(np, chan_shift[i], &pwm->chan_shift[i]) ||
		    pwm->chan_shift[i] > 30)
			return -EINVAL;
		pwm->clk[i] = devm_clk_get(dev, chan_clk[i]);
		if (IS_ERR(pwm->clk[i]))
			return dev_err_probe(dev, PTR_ERR(pwm->clk[i]), "%s\n", chan_clk[i]);
	}
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(PWM_DMA_BITS));
	if (ret)
		return dev_err_probe(dev, ret, "36-bit PWM DMA unavailable\n");

	mutex_lock(&pwm_lock);
	platform_set_drvdata(pdev, pwm);
	mutex_unlock(&pwm_lock);
	return 0;
}

static void mtk_rothko_pwm_remove(struct platform_device *pdev)
{
	mutex_lock(&pwm_lock);
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&pwm_lock);
}

static const struct of_device_id mtk_rothko_pwm_of_match[] = {
	{ .compatible = "mediatek,pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_rothko_pwm_of_match);

static struct platform_driver mtk_rothko_pwm_driver = {
	.probe = mtk_rothko_pwm_probe,
	.remove = mtk_rothko_pwm_remove,
	.driver = {
		.name = "mt-pwm",
		.of_match_table = mtk_rothko_pwm_of_match,
	},
};
module_platform_driver(mtk_rothko_pwm_driver);

MODULE_AUTHOR("Chun-Hung Wu");
MODULE_DESCRIPTION("MediaTek MT6989 memory-mode PWM controller");
MODULE_LICENSE("GPL");
