// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2021 MediaTek Inc. */
#include <linux/arm-smccc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

#define MTK_SIP_TINYSYS_SSPM_CONTROL MTK_SIP_SMC_CMD(0x53c)
#define INTR_SET_OFS 0x0
#define INTR_CLR_OFS 0x4
#define MBOX_CHANS 2

struct mhu_link {
	int irq;
	void __iomem *base;
	u32 mbox_irq;
};

struct mtk_mbu {
	struct mhu_link mlink[MBOX_CHANS];
	struct mbox_chan chan[MBOX_CHANS];
	struct mbox_controller mbox;
	bool secure_mbox_clr;
};

static irqreturn_t tinysys_mbox_rx_interrupt(int irq, void *p)
{
	struct mbox_chan *chan = p;
	struct mhu_link *mlink = chan->con_priv;
	struct mtk_mbu *mbu = container_of(chan->mbox, struct mtk_mbu, mbox);
	struct arm_smccc_res res;
	u32 val = readl_relaxed(mlink->base + INTR_CLR_OFS);

	if (!val)
		return IRQ_NONE;

	if (mbu->secure_mbox_clr) {
		/* SSPM operation 0 clears the actual channel 0/1 interrupt. */
		arm_smccc_smc(MTK_SIP_TINYSYS_SSPM_CONTROL, 0, mlink->mbox_irq,
			      0, 0, 0, 0, 0, &res);
		if (res.a0) {
			dev_err_ratelimited(chan->mbox->dev,
					    "SSPM IRQ clear failed: %ld\n",
					    (long)res.a0);
			return IRQ_NONE;
		}
	} else {
		writel_relaxed(1, mlink->base + INTR_CLR_OFS);
	}

	mbox_chan_received_data(chan, &val);
	return IRQ_HANDLED;
}

static bool tinysys_mbox_last_tx_done(struct mbox_chan *chan)
{
	struct mhu_link *mlink = chan->con_priv;

	return !readl_relaxed(mlink->base + INTR_SET_OFS);
}

static int tinysys_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct mhu_link *mlink = chan->con_priv;

	/* Publish the SCMI shared-memory message before ringing the doorbell. */
	smp_mb();
	writel_relaxed(1, mlink->base + INTR_SET_OFS);
	return 0;
}

static int tinysys_mbox_startup(struct mbox_chan *chan)
{
	struct mhu_link *mlink = chan->con_priv;

	return request_irq(mlink->irq, tinysys_mbox_rx_interrupt,
			   IRQF_NO_SUSPEND, "mtk_tinysys_mbox", chan);
}

static void tinysys_mbox_shutdown(struct mbox_chan *chan)
{
	struct mhu_link *mlink = chan->con_priv;

	free_irq(mlink->irq, chan);
}

static const struct mbox_chan_ops tinysys_mbox_chan_ops = {
	.send_data = tinysys_mbox_send_data,
	.startup = tinysys_mbox_startup,
	.shutdown = tinysys_mbox_shutdown,
	.last_tx_done = tinysys_mbox_last_tx_done,
};

static int tinysys_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_mbu *mbu;
	u32 secure = 0;
	int i, ret;

	mbu = devm_kzalloc(dev, sizeof(*mbu), GFP_KERNEL);
	if (!mbu)
		return -ENOMEM;

	if (of_find_property(dev->of_node, "secure-sspm-mbox-clr", NULL)) {
		ret = of_property_read_u32(dev->of_node,
					   "secure-sspm-mbox-clr", &secure);
		if (ret)
			return ret;
	}
	mbu->secure_mbox_clr = !!secure;

	for (i = 0; i < MBOX_CHANS; i++) {
		mbu->mlink[i].base = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(mbu->mlink[i].base))
			return PTR_ERR(mbu->mlink[i].base);
		mbu->mlink[i].irq = platform_get_irq(pdev, i);
		if (mbu->mlink[i].irq < 0)
			return mbu->mlink[i].irq;
		mbu->mlink[i].mbox_irq = i;
		mbu->chan[i].con_priv = &mbu->mlink[i];
	}

	mbu->mbox.dev = dev;
	mbu->mbox.chans = mbu->chan;
	mbu->mbox.num_chans = MBOX_CHANS;
	mbu->mbox.ops = &tinysys_mbox_chan_ops;
	mbu->mbox.txdone_poll = true;
	mbu->mbox.txpoll_period = 1;

	return devm_mbox_controller_register(dev, &mbu->mbox);
}

static const struct of_device_id tinysys_mbox_of_ids[] = {
	{ .compatible = "mediatek,tinysys_mbox" },
	{ }
};
MODULE_DEVICE_TABLE(of, tinysys_mbox_of_ids);

static struct platform_driver tinysys_mbox_drv = {
	.probe = tinysys_mbox_probe,
	.driver = {
		.name = "mtk_tinysys_mbox",
		.of_match_table = tinysys_mbox_of_ids,
	},
};
module_platform_driver(tinysys_mbox_drv);
MODULE_DESCRIPTION("MediaTek Tinysys SCMI mailbox");
MODULE_LICENSE("GPL v2");
