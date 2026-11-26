// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 MediaTek Inc.
 * Copyright (c) 2024 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/dvfsrc.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/spinlock.h>

#include "mtk-dvfsrc-rothko-helper.h"

/* DVFSRC_LEVEL */
#define DVFSRC_V1_LEVEL_TARGET_LEVEL	GENMASK(15, 0)
#define DVFSRC_TGT_LEVEL_IDLE		0x00
#define DVFSRC_V1_LEVEL_CURRENT_LEVEL	GENMASK(31, 16)

/* DVFSRC_SW_REQ, DVFSRC_SW_REQ2 */
#define DVFSRC_V1_SW_REQ2_DRAM_LEVEL	GENMASK(1, 0)
#define DVFSRC_V1_SW_REQ2_VCORE_LEVEL	GENMASK(3, 2)

#define DVFSRC_MT6989_DRAM_REQ		GENMASK(15, 12)
#define DVFSRC_MT6989_DRAM_CUR		GENMASK(3, 0)
#define DVFSRC_V2_SW_REQ_DRAM_LEVEL	GENMASK(3, 0)
#define DVFSRC_V2_SW_REQ_VCORE_LEVEL	GENMASK(6, 4)

/* DVFSRC_VCORE */
#define DVFSRC_V2_VCORE_REQ_VSCP_LEVEL	GENMASK(14, 12)

#define DVFSRC_POLL_TIMEOUT_US		1000
#define STARTUP_TIME_US			1

#define MTK_SIP_DVFSRC_INIT		0x0
#define MTK_SIP_DVFSRC_START		0x1

static u32 dvfsrc_dram_type;

u32 dvfsrc_get_required_opp_peak_bw(struct device_node *np, int index)
{
	struct device_node *required_np;
	u32 peak_bw = 0;

	required_np = of_parse_phandle(np, "required-opps", index);
	if (!required_np) {
		pr_notice("%s: Unable to parse required-opps: %pOF, index: %d\n",
			  __func__, np, index);
		return 0;
	}

	if (of_property_read_u32_index(required_np, "opp-peak-KBps",
				       dvfsrc_dram_type, &peak_bw))
		pr_info("%s: get fail\n", __func__);

	of_node_put(required_np);
	return peak_bw;
}
EXPORT_SYMBOL(dvfsrc_get_required_opp_peak_bw);

struct dvfsrc_bw_constraints {
	u16 max_dram_nom_bw;
	u16 max_dram_peak_bw;
	u16 max_dram_hrt_bw;
};

struct dvfsrc_soc_data;
struct mtk_dvfsrc {
	struct device *dev;
	struct platform_device *icc;
	struct platform_device *regulator;
	const struct dvfsrc_soc_data *dvd;
	const struct dvfsrc_opp_desc *curr_opps;
	void __iomem *regs;
	int dram_type;
	u32 num_opp;
	struct dvfsrc_opp_desc dynamic_opps;
	spinlock_t req_lock;
	bool opp_forced;
};

struct dvfsrc_soc_data {
	const int *regs;
	const struct dvfsrc_opp_desc *opps_desc;
	u32 (*get_target_level)(struct mtk_dvfsrc *dvfsrc);
	u32 (*get_current_level)(struct mtk_dvfsrc *dvfsrc);
	u32 (*get_vcore_level)(struct mtk_dvfsrc *dvfsrc);
	u32 (*get_vscp_level)(struct mtk_dvfsrc *dvfsrc);
	void (*set_dram_bw)(struct mtk_dvfsrc *dvfsrc, u64 bw);
	void (*set_dram_peak_bw)(struct mtk_dvfsrc *dvfsrc, u64 bw);
	void (*set_dram_hrt_bw)(struct mtk_dvfsrc *dvfsrc, u64 bw);
	void (*set_opp_level)(struct mtk_dvfsrc *dvfsrc, u32 level);
	void (*set_vcore_level)(struct mtk_dvfsrc *dvfsrc, u32 level);
	void (*set_vscp_level)(struct mtk_dvfsrc *dvfsrc, u32 level);
	int (*wait_for_opp_level)(struct mtk_dvfsrc *dvfsrc, u32 level);
	int (*wait_for_vcore_level)(struct mtk_dvfsrc *dvfsrc, u32 level);
	const struct dvfsrc_bw_constraints *bw_constraints;
	bool defer_start;
};

static u32 dvfsrc_readl(struct mtk_dvfsrc *dvfs, u32 offset)
{
	return readl(dvfs->regs + dvfs->dvd->regs[offset]);
}

static void dvfsrc_writel(struct mtk_dvfsrc *dvfs, u32 offset, u32 val)
{
	writel(val, dvfs->regs + dvfs->dvd->regs[offset]);
}

enum dvfsrc_regs {
	DVFSRC_SW_REQ,
	DVFSRC_SW_REQ2,
	DVFSRC_LEVEL,
	DVFSRC_TARGET_LEVEL,
	DVFSRC_SW_BW,
	DVFSRC_SW_PEAK_BW,
	DVFSRC_SW_HRT_BW,
	DVFSRC_VCORE,
	DVFSRC_BASIC_CONTROL,
	DVFSRC_CUR_TAR_GEAR,
	DVFSRC_LEVEL_LABEL_L,
	DVFSRC_LEVEL_LABEL_H,
	DVFSRC_HALT_CONTROL,
	DVFSRC_DEFAULT_OPP_4,
	DVFSRC_DEFAULT_OPP_3,
	DVFSRC_DEFAULT_OPP_2,
	DVFSRC_DEFAULT_OPP_1,
	DVFSRC_REGS_MAX,
};

static const int dvfsrc_mt8183_regs[] = {
	[DVFSRC_SW_REQ] = 0x4,
	[DVFSRC_SW_REQ2] = 0x8,
	[DVFSRC_LEVEL] = 0xDC,
	[DVFSRC_SW_BW] = 0x160,
};

static const int dvfsrc_mt8195_regs[] = {
	[DVFSRC_SW_REQ] = 0xc,
	[DVFSRC_VCORE] = 0x6c,
	[DVFSRC_SW_PEAK_BW] = 0x278,
	[DVFSRC_SW_BW] = 0x26c,
	[DVFSRC_SW_HRT_BW] = 0x290,
	[DVFSRC_LEVEL] = 0xd44,
	[DVFSRC_TARGET_LEVEL] = 0xd48,
};

static const int dvfsrc_mt6989_regs[] = {
	[DVFSRC_SW_REQ] = 0x18,
	[DVFSRC_SW_REQ2] = 0x604,
	[DVFSRC_LEVEL] = 0x5f0,
	[DVFSRC_TARGET_LEVEL] = 0x5f0,
	[DVFSRC_SW_BW] = 0x1e8,
	[DVFSRC_SW_PEAK_BW] = 0x1f4,
	[DVFSRC_SW_HRT_BW] = 0x20c,
	[DVFSRC_VCORE] = 0x80,
	[DVFSRC_BASIC_CONTROL] = 0x00,
	[DVFSRC_CUR_TAR_GEAR] = 0x6ac,
	[DVFSRC_LEVEL_LABEL_L] = 0xfc,
	[DVFSRC_LEVEL_LABEL_H] = 0x6b0,
	[DVFSRC_HALT_CONTROL] = 0xc4,
	[DVFSRC_DEFAULT_OPP_4] = 0x744,
	[DVFSRC_DEFAULT_OPP_3] = 0x740,
	[DVFSRC_DEFAULT_OPP_2] = 0x230,
	[DVFSRC_DEFAULT_OPP_1] = 0x22c,
};

static const struct dvfsrc_opp *dvfsrc_get_current_opp(struct mtk_dvfsrc *dvfsrc)
{
	u32 level = dvfsrc->dvd->get_current_level(dvfsrc);

	return &dvfsrc->curr_opps->opps[level];
}

static bool dvfsrc_is_idle(struct mtk_dvfsrc *dvfsrc)
{
	if (!dvfsrc->dvd->get_target_level)
		return true;

	return dvfsrc->dvd->get_target_level(dvfsrc) == DVFSRC_TGT_LEVEL_IDLE;
}

static int dvfsrc_wait_for_vcore_level_v1(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	const struct dvfsrc_opp *curr;

	return readx_poll_timeout_atomic(dvfsrc_get_current_opp, dvfsrc, curr,
					 curr->vcore_opp >= level, STARTUP_TIME_US,
					 DVFSRC_POLL_TIMEOUT_US);
}

static int dvfsrc_wait_for_opp_level_v1(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	const struct dvfsrc_opp *target, *curr;
	int ret;

	target = &dvfsrc->curr_opps->opps[level];
	ret = readx_poll_timeout_atomic(dvfsrc_get_current_opp, dvfsrc, curr,
					curr->dram_opp >= target->dram_opp &&
					curr->vcore_opp >= target->vcore_opp,
					STARTUP_TIME_US, DVFSRC_POLL_TIMEOUT_US);
	if (ret < 0) {
		dev_warn(dvfsrc->dev,
			 "timeout! target OPP: %u, dram: %d, vcore: %d\n", level,
			 curr->dram_opp, curr->vcore_opp);
		return ret;
	}

	return 0;
}

static int dvfsrc_wait_for_opp_level_v2(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	const struct dvfsrc_opp *target, *curr;
	int ret;

	target = &dvfsrc->curr_opps->opps[level];
	ret = readx_poll_timeout_atomic(dvfsrc_get_current_opp, dvfsrc, curr,
					curr->dram_opp >= target->dram_opp &&
					curr->vcore_opp >= target->vcore_opp,
					STARTUP_TIME_US, DVFSRC_POLL_TIMEOUT_US);
	if (ret < 0) {
		dev_warn(dvfsrc->dev,
			 "timeout! target OPP: %u, dram: %d\n", level, curr->dram_opp);
		return ret;
	}

	return 0;
}

static u32 dvfsrc_get_target_level_v1(struct mtk_dvfsrc *dvfsrc)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_LEVEL);

	return FIELD_GET(DVFSRC_V1_LEVEL_TARGET_LEVEL, val);
}

static u32 dvfsrc_get_current_level_v1(struct mtk_dvfsrc *dvfsrc)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_LEVEL);
	u32 current_level = FIELD_GET(DVFSRC_V1_LEVEL_CURRENT_LEVEL, val);

	return ffs(current_level) - 1;
}

static u32 dvfsrc_get_target_level_v2(struct mtk_dvfsrc *dvfsrc)
{
	return dvfsrc_readl(dvfsrc, DVFSRC_TARGET_LEVEL);
}

static u32 dvfsrc_get_current_level_v2(struct mtk_dvfsrc *dvfsrc)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_LEVEL);
	u32 level = ffs(val);

	/* Valid levels */
	if (level < dvfsrc->curr_opps->num_opp)
		return dvfsrc->curr_opps->num_opp - level;

	/* Zero for level 0 or invalid level */
	return 0;
}

static u32 dvfsrc_get_target_level_mt6989(struct mtk_dvfsrc *dvfsrc)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_TARGET_LEVEL);

	if (val & BIT(16))
		return FIELD_GET(GENMASK(15, 8), val) + 1;

	return 0;
}

static u32 dvfsrc_get_current_level_mt6989(struct mtk_dvfsrc *dvfsrc)
{
	u32 level = FIELD_GET(GENMASK(7, 0),
			      dvfsrc_readl(dvfsrc, DVFSRC_LEVEL)) + 1;

	return level <= dvfsrc->num_opp ? dvfsrc->num_opp - level : U32_MAX;
}

#if IS_ENABLED(CONFIG_MTK_ROTHKO_DDR_DEVFREQ)
const struct dvfsrc_opp_desc *mt6989_dvfsrc_get_opps(struct device *dev)
{
	struct mtk_dvfsrc *dvfsrc = dev_get_drvdata(dev);

	return dvfsrc->curr_opps;
}

static void mt6989_dvfsrc_set_halt(struct mtk_dvfsrc *dvfsrc, bool halt)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_HALT_CONTROL);

	dvfsrc_writel(dvfsrc, DVFSRC_HALT_CONTROL,
		      halt ? val | BIT(1) : val & ~BIT(1));
}

static int mt6989_dvfsrc_halt_and_wait(struct mtk_dvfsrc *dvfsrc)
{
	bool idle;

	mt6989_dvfsrc_set_halt(dvfsrc, true);
	udelay(STARTUP_TIME_US);
	return readx_poll_timeout_atomic(dvfsrc_is_idle, dvfsrc, idle, idle,
					 STARTUP_TIME_US, DVFSRC_POLL_TIMEOUT_US);
}

/* All four banks form one bitmap. Publish the new bit before clearing others. */
static void mt6989_dvfsrc_set_bitmap(struct mtk_dvfsrc *dvfsrc, u32 index)
{
	unsigned int bank = DVFSRC_DEFAULT_OPP_4 + index / 32;
	unsigned int reg;

	dvfsrc_writel(dvfsrc, bank, BIT(index % 32));
	for (reg = DVFSRC_DEFAULT_OPP_4; reg <= DVFSRC_DEFAULT_OPP_1; reg++)
		if (reg != bank)
			dvfsrc_writel(dvfsrc, reg, 0);
}

/* Called under the helper mutex, including from its sleep and teardown paths. */
int mt6989_dvfsrc_force_opp(struct device *dev, u32 index)
{
	struct mtk_dvfsrc *dvfsrc = dev_get_drvdata(dev);
	bool release = index == MT6989_FORCE_RELEASE;
	u32 val;
	int ret;

	if (!release && index >= dvfsrc->num_opp)
		return -EINVAL;
	if (release && !dvfsrc->opp_forced)
		return 0;

	ret = mt6989_dvfsrc_halt_and_wait(dvfsrc);
	if (ret || release)
		goto restore;

	WRITE_ONCE(dvfsrc->opp_forced, true);
	dvfsrc_writel(dvfsrc, DVFSRC_SW_REQ2, U32_MAX);
	mt6989_dvfsrc_set_bitmap(dvfsrc, index);
	mt6989_dvfsrc_set_halt(dvfsrc, false);
	ret = readl_poll_timeout_atomic(dvfsrc->regs + dvfsrc->dvd->regs[DVFSRC_LEVEL],
					val, (val & GENMASK(6, 0)) == index,
					STARTUP_TIME_US, DVFSRC_POLL_TIMEOUT_US);
	if (!ret)
		return 0;

	/* Even a stalled transition must leave the ordinary request path enabled. */
	mt6989_dvfsrc_halt_and_wait(dvfsrc);
restore:
	if (dvfsrc->opp_forced) {
		mt6989_dvfsrc_set_bitmap(dvfsrc, 0);
		dvfsrc_writel(dvfsrc, DVFSRC_SW_REQ2, 0);
	}
	mt6989_dvfsrc_set_halt(dvfsrc, false);
	WRITE_ONCE(dvfsrc->opp_forced, false);
	return ret;
}
#endif

static int dvfsrc_wait_for_vcore_level_mt6989(struct mtk_dvfsrc *dvfsrc,
					    u32 level)
{
	u32 val;

	return readl_poll_timeout_atomic(dvfsrc->regs +
				dvfsrc->dvd->regs[DVFSRC_CUR_TAR_GEAR], val,
				READ_ONCE(dvfsrc->opp_forced) ||
				FIELD_GET(GENMASK(10, 8), val) >= level,
				STARTUP_TIME_US, DVFSRC_POLL_TIMEOUT_US);
}

static int dvfsrc_wait_for_opp_level_mt6989(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	const struct dvfsrc_opp *target = &dvfsrc->curr_opps->opps[level];
	u32 val;

	return readl_poll_timeout_atomic(dvfsrc->regs +
				dvfsrc->dvd->regs[DVFSRC_CUR_TAR_GEAR], val,
				READ_ONCE(dvfsrc->opp_forced) ||
				(FIELD_GET(GENMASK(10, 8), val) >= target->vcore_opp &&
				 FIELD_GET(GENMASK(3, 0), val) >= target->dram_opp),
				STARTUP_TIME_US, DVFSRC_POLL_TIMEOUT_US);
}

static u32 dvfsrc_get_vcore_level_v1(struct mtk_dvfsrc *dvfsrc)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_SW_REQ2);

	return FIELD_GET(DVFSRC_V1_SW_REQ2_VCORE_LEVEL, val);
}

static void dvfsrc_set_vcore_level_v1(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_SW_REQ2);

	val &= ~DVFSRC_V1_SW_REQ2_VCORE_LEVEL;
	val |= FIELD_PREP(DVFSRC_V1_SW_REQ2_VCORE_LEVEL, level);

	dvfsrc_writel(dvfsrc, DVFSRC_SW_REQ2, val);
}

static u32 dvfsrc_get_vcore_level_v2(struct mtk_dvfsrc *dvfsrc)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_SW_REQ);

	return FIELD_GET(DVFSRC_V2_SW_REQ_VCORE_LEVEL, val);
}

static void dvfsrc_set_vcore_level_v2(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_SW_REQ);

	val &= ~DVFSRC_V2_SW_REQ_VCORE_LEVEL;
	val |= FIELD_PREP(DVFSRC_V2_SW_REQ_VCORE_LEVEL, level);

	dvfsrc_writel(dvfsrc, DVFSRC_SW_REQ, val);
}

static void dvfsrc_set_vcore_level_mt6989(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	unsigned long flags;

	spin_lock_irqsave(&dvfsrc->req_lock, flags);
	dvfsrc_set_vcore_level_v2(dvfsrc, level);
	spin_unlock_irqrestore(&dvfsrc->req_lock, flags);
}

static void dvfsrc_set_opp_level_mt6989(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	const struct dvfsrc_opp *opp = &dvfsrc->curr_opps->opps[level];
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&dvfsrc->req_lock, flags);
	val = dvfsrc_readl(dvfsrc, DVFSRC_SW_REQ);
	val &= ~(GENMASK(15, 12) | DVFSRC_V2_SW_REQ_VCORE_LEVEL);
	val |= FIELD_PREP(GENMASK(15, 12), opp->dram_opp) |
	       FIELD_PREP(DVFSRC_V2_SW_REQ_VCORE_LEVEL, opp->vcore_opp);
	dvfsrc_writel(dvfsrc, DVFSRC_SW_REQ, val);
	spin_unlock_irqrestore(&dvfsrc->req_lock, flags);
}

static u32 dvfsrc_get_vscp_level_v2(struct mtk_dvfsrc *dvfsrc)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_VCORE);

	return FIELD_GET(DVFSRC_V2_VCORE_REQ_VSCP_LEVEL, val);
}

static void dvfsrc_set_vscp_level_v2(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	u32 val = dvfsrc_readl(dvfsrc, DVFSRC_VCORE);

	val &= ~DVFSRC_V2_VCORE_REQ_VSCP_LEVEL;
	val |= FIELD_PREP(DVFSRC_V2_VCORE_REQ_VSCP_LEVEL, level);

	dvfsrc_writel(dvfsrc, DVFSRC_VCORE, val);
}

static void __dvfsrc_set_dram_bw_v1(struct mtk_dvfsrc *dvfsrc, u32 reg,
				    u16 max_bw, u16 min_bw, u64 bw)
{
	u32 new_bw = (u32)div_u64(bw, 100 * 1000);

	/* If bw constraints (in mbps) are defined make sure to respect them */
	if (max_bw)
		new_bw = min(new_bw, max_bw);
	if (min_bw && new_bw > 0)
		new_bw = max(new_bw, min_bw);

	dvfsrc_writel(dvfsrc, reg, new_bw);
}

static void dvfsrc_set_dram_bw_v1(struct mtk_dvfsrc *dvfsrc, u64 bw)
{
	u64 max_bw = dvfsrc->dvd->bw_constraints->max_dram_nom_bw;

	__dvfsrc_set_dram_bw_v1(dvfsrc, DVFSRC_SW_BW, max_bw, 0, bw);
};

static void dvfsrc_set_dram_peak_bw_v1(struct mtk_dvfsrc *dvfsrc, u64 bw)
{
	u64 max_bw = dvfsrc->dvd->bw_constraints->max_dram_peak_bw;

	__dvfsrc_set_dram_bw_v1(dvfsrc, DVFSRC_SW_PEAK_BW, max_bw, 0, bw);
}

static void dvfsrc_set_dram_hrt_bw_v1(struct mtk_dvfsrc *dvfsrc, u64 bw)
{
	u64 max_bw = dvfsrc->dvd->bw_constraints->max_dram_hrt_bw;

	__dvfsrc_set_dram_bw_v1(dvfsrc, DVFSRC_SW_HRT_BW, max_bw, 0, bw);
}

static void dvfsrc_set_dram_hrt_bw_mt6989(struct mtk_dvfsrc *dvfsrc, u64 bw)
{
	u64 hrt_bw = div_u64(div_u64(bw, 1000) + 29, 30);

	dvfsrc_writel(dvfsrc, DVFSRC_SW_HRT_BW, min_t(u64, hrt_bw, 1023));
}

static void dvfsrc_set_opp_level_v1(struct mtk_dvfsrc *dvfsrc, u32 level)
{
	const struct dvfsrc_opp *opp = &dvfsrc->curr_opps->opps[level];
	u32 val;

	/* Translate Pstate to DVFSRC level and set it to DVFSRC HW */
	val = FIELD_PREP(DVFSRC_V1_SW_REQ2_DRAM_LEVEL, opp->dram_opp);
	val |= FIELD_PREP(DVFSRC_V1_SW_REQ2_VCORE_LEVEL, opp->vcore_opp);

	dev_dbg(dvfsrc->dev, "vcore_opp: %d, dram_opp: %d\n", opp->vcore_opp, opp->dram_opp);
	dvfsrc_writel(dvfsrc, DVFSRC_SW_REQ, val);
}

static int dvfsrc_request_mt6989_dram_gear(struct mtk_dvfsrc *dvfsrc, u64 gear)
{
	unsigned long flags;
	u32 val;

	if (dvfsrc->dvd->regs != dvfsrc_mt6989_regs)
		return -EOPNOTSUPP;
	if (gear > FIELD_MAX(DVFSRC_MT6989_DRAM_REQ))
		return -EINVAL;

	spin_lock_irqsave(&dvfsrc->req_lock, flags);
	val = dvfsrc_readl(dvfsrc, DVFSRC_SW_REQ);
	val &= ~DVFSRC_MT6989_DRAM_REQ;
	val |= FIELD_PREP(DVFSRC_MT6989_DRAM_REQ, gear);
	dvfsrc_writel(dvfsrc, DVFSRC_SW_REQ, val);
	spin_unlock_irqrestore(&dvfsrc->req_lock, flags);

	return readl_poll_timeout_atomic(dvfsrc->regs +
				dvfsrc->dvd->regs[DVFSRC_CUR_TAR_GEAR], val,
				READ_ONCE(dvfsrc->opp_forced) ||
				FIELD_GET(DVFSRC_MT6989_DRAM_CUR, val) >= gear,
				STARTUP_TIME_US, DVFSRC_POLL_TIMEOUT_US);
}

int mtk_dvfsrc_send_request(const struct device *dev, u32 cmd, u64 data)
{
	struct mtk_dvfsrc *dvfsrc = dev_get_drvdata(dev);
	bool state;
	int ret;

	dev_dbg(dvfsrc->dev, "cmd: %d, data: %llu\n", cmd, data);

	switch (cmd) {
	case MTK_DVFSRC_CMD_BW:
		dvfsrc->dvd->set_dram_bw(dvfsrc, data);
		return 0;
	case MTK_DVFSRC_CMD_HRT_BW:
		if (dvfsrc->dvd->set_dram_hrt_bw)
			dvfsrc->dvd->set_dram_hrt_bw(dvfsrc, data);
		return 0;
	case MTK_DVFSRC_CMD_PEAK_BW:
		if (dvfsrc->dvd->set_dram_peak_bw)
			dvfsrc->dvd->set_dram_peak_bw(dvfsrc, data);
		return 0;
	case MTK_DVFSRC_CMD_DRAM_GEAR:
		return dvfsrc_request_mt6989_dram_gear(dvfsrc, data);
	case MTK_DVFSRC_CMD_OPP:
		if (dvfsrc->num_opp && data >= dvfsrc->num_opp)
			return -EINVAL;

		if (!dvfsrc->dvd->set_opp_level)
			return 0;

		dvfsrc->dvd->set_opp_level(dvfsrc, data);
		break;
	case MTK_DVFSRC_CMD_VCORE_LEVEL:
		dvfsrc->dvd->set_vcore_level(dvfsrc, data);
		break;
	case MTK_DVFSRC_CMD_VSCP_LEVEL:
		if (!dvfsrc->dvd->set_vscp_level)
			return 0;

		dvfsrc->dvd->set_vscp_level(dvfsrc, data);
		break;
	default:
		dev_err(dvfsrc->dev, "unknown command: %d\n", cmd);
		return -EOPNOTSUPP;
	}

	/* DVFSRC needs at least 2T(~196ns) to handle a request */
	udelay(STARTUP_TIME_US);

	/* A forced combo overrides the votes, but keep recording the votes in HW. */
	ret = readx_poll_timeout_atomic(dvfsrc_is_idle, dvfsrc, state,
					state || READ_ONCE(dvfsrc->opp_forced),
					STARTUP_TIME_US, DVFSRC_POLL_TIMEOUT_US);
	if (ret < 0) {
		dev_warn(dvfsrc->dev,
			 "%d: idle timeout, data: %llu, last: %d -> %d\n", cmd, data,
			 dvfsrc->dvd->get_current_level(dvfsrc),
			 dvfsrc->dvd->get_target_level(dvfsrc));
		return ret;
	}

	if (cmd == MTK_DVFSRC_CMD_OPP)
		ret = dvfsrc->dvd->wait_for_opp_level(dvfsrc, data);
	else
		ret = dvfsrc->dvd->wait_for_vcore_level(dvfsrc, data);

	if (ret < 0) {
		dev_warn(dvfsrc->dev,
			 "%d: wait timeout, data: %llu, last: %d -> %d\n",
			 cmd, data,
			 dvfsrc->dvd->get_current_level(dvfsrc),
			 dvfsrc->dvd->get_target_level(dvfsrc));
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(mtk_dvfsrc_send_request);

int mtk_dvfsrc_query_info(const struct device *dev, u32 cmd, int *data)
{
	struct mtk_dvfsrc *dvfsrc = dev_get_drvdata(dev);

	switch (cmd) {
	case MTK_DVFSRC_CMD_VCORE_LEVEL:
		*data = dvfsrc->dvd->get_vcore_level(dvfsrc);
		break;
	case MTK_DVFSRC_CMD_VSCP_LEVEL:
		*data = dvfsrc->dvd->get_vscp_level(dvfsrc);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}
EXPORT_SYMBOL(mtk_dvfsrc_query_info);

static void mtk_dvfsrc_unregister_child(void *data)
{
	platform_device_unregister(data);
}

static int mtk_dvfsrc_register_mt6989_children(struct mtk_dvfsrc *dvfsrc)
{
	struct platform_device *start, *emi, *ddr;
	int ret;

	dvfsrc->regulator = platform_device_register_data(dvfsrc->dev,
					"mtk-dvfsrc-regulator", PLATFORM_DEVID_AUTO,
					NULL, 0);
	if (IS_ERR(dvfsrc->regulator))
		return PTR_ERR(dvfsrc->regulator);

	ret = devm_add_action_or_reset(dvfsrc->dev,
				      mtk_dvfsrc_unregister_child, dvfsrc->regulator);
	if (ret)
		return ret;

	start = platform_device_register_data(dvfsrc->dev, "mtk-dvfsrc-start",
					     PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(start))
		return PTR_ERR(start);

	ret = devm_add_action_or_reset(dvfsrc->dev,
				      mtk_dvfsrc_unregister_child, start);
	if (ret)
		return ret;

	if (IS_ENABLED(CONFIG_MTK_ROTHKO_DDR_DEVFREQ)) {
		ddr = platform_device_register_data(dvfsrc->dev, "mtk-dvfsrc-devfreq",
						    PLATFORM_DEVID_NONE, NULL, 0);
		if (IS_ERR(ddr))
			return PTR_ERR(ddr);
		ret = devm_add_action_or_reset(dvfsrc->dev,
					      mtk_dvfsrc_unregister_child, ddr);
		if (ret)
			return ret;
	}

	if (!IS_ENABLED(CONFIG_INTERCONNECT_MTK_EMI))
		return 0;

	emi = platform_device_register_data(dvfsrc->dev, "mediatek-emi-icc",
					    PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(emi))
		return PTR_ERR(emi);

	return devm_add_action_or_reset(dvfsrc->dev,
				       mtk_dvfsrc_unregister_child, emi);
}

static int mtk_dvfsrc_init_mt6989_opps(struct mtk_dvfsrc *dvfsrc)
{
	struct dvfsrc_opp *opps;
	u32 max_opp, i;

	max_opp = FIELD_GET(GENMASK(26, 20),
			    dvfsrc_readl(dvfsrc, DVFSRC_BASIC_CONTROL));
	if (!max_opp)
		return dev_err_probe(dvfsrc->dev, -EINVAL,
				     "DVFSRC firmware OPP table is not initialized\n");

	dvfsrc->num_opp = max_opp + 1;
	opps = devm_kcalloc(dvfsrc->dev, dvfsrc->num_opp, sizeof(*opps), GFP_KERNEL);
	if (!opps)
		return -ENOMEM;

	for (i = 0; i < dvfsrc->num_opp; i++) {
		u32 index = max_opp - i;
		u32 offset, val;

		if (index < 64)
			offset = dvfsrc->dvd->regs[DVFSRC_LEVEL_LABEL_L];
		else
			offset = dvfsrc->dvd->regs[DVFSRC_LEVEL_LABEL_H];

		val = readl(dvfsrc->regs + offset + 4 * ((index % 64) / 2));
		val >>= 16 * (index % 2);
		opps[i].vcore_opp = FIELD_GET(GENMASK(2, 0), val);
		opps[i].dram_opp = FIELD_GET(GENMASK(15, 12), val);
	}

	dvfsrc->dynamic_opps.opps = opps;
	dvfsrc->dynamic_opps.num_opp = dvfsrc->num_opp;
	dvfsrc->curr_opps = &dvfsrc->dynamic_opps;

	return 0;
}

static int mtk_dvfsrc_probe(struct platform_device *pdev)
{
	struct arm_smccc_res ares;
	struct mtk_dvfsrc *dvfsrc;
	int ret;

	dvfsrc = devm_kzalloc(&pdev->dev, sizeof(*dvfsrc), GFP_KERNEL);
	if (!dvfsrc)
		return -ENOMEM;

	dvfsrc->dvd = of_device_get_match_data(&pdev->dev);
	dvfsrc->dev = &pdev->dev;
	spin_lock_init(&dvfsrc->req_lock);

	dvfsrc->regs = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(dvfsrc->regs))
		return PTR_ERR(dvfsrc->regs);

	arm_smccc_smc(MTK_SIP_DVFSRC_VCOREFS_CONTROL, MTK_SIP_DVFSRC_INIT,
		      0, 0, 0, 0, 0, 0, &ares);
	if (ares.a0)
		return dev_err_probe(&pdev->dev, -EINVAL, "DVFSRC init failed: %lu\n", ares.a0);

	dvfsrc->dram_type = ares.a1;
	dvfsrc_dram_type = dvfsrc->dram_type;
	dev_dbg(&pdev->dev, "DRAM Type: %d\n", dvfsrc->dram_type);

	if (dvfsrc->dvd->opps_desc)
		dvfsrc->curr_opps = &dvfsrc->dvd->opps_desc[dvfsrc->dram_type];
	else {
		ret = mtk_dvfsrc_init_mt6989_opps(dvfsrc);
		if (ret)
			return ret;
	}
	platform_set_drvdata(pdev, dvfsrc);

	if (dvfsrc->dvd->defer_start) {
		ret = mtk_dvfsrc_register_mt6989_children(dvfsrc);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "Failed to register DVFSRC child devices\n");
	}

	ret = devm_of_platform_populate(&pdev->dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to populate child devices\n");

	if (dvfsrc->dvd->defer_start)
		return 0;

	/* Everything is set up - make it run! */
	arm_smccc_smc(MTK_SIP_DVFSRC_VCOREFS_CONTROL, MTK_SIP_DVFSRC_START,
		      0, 0, 0, 0, 0, 0, &ares);
	if (ares.a0)
		return dev_err_probe(&pdev->dev, -EINVAL, "Cannot start DVFSRC: %lu\n", ares.a0);

	return 0;
}

static const struct dvfsrc_bw_constraints dvfsrc_bw_constr_v1 = { 0, 0, 0 };
static const struct dvfsrc_bw_constraints dvfsrc_bw_constr_v2 = {
	.max_dram_nom_bw = 255,
	.max_dram_peak_bw = 255,
	.max_dram_hrt_bw = 1023,
};

static const struct dvfsrc_opp dvfsrc_opp_mt6893_lp4[] = {
	{ 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 },
	{ 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 },
	{ 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 2 },
	{ 0, 3 }, { 1, 3 }, { 2, 3 }, { 3, 3 },
	{ 1, 4 }, { 2, 4 }, { 3, 4 }, { 2, 5 },
	{ 3, 5 }, { 3, 6 }, { 4, 6 }, { 4, 7 },
};

static const struct dvfsrc_opp_desc dvfsrc_opp_mt6893_desc[] = {
	[0] = {
		.opps = dvfsrc_opp_mt6893_lp4,
		.num_opp = ARRAY_SIZE(dvfsrc_opp_mt6893_lp4),
	}
};

static const struct dvfsrc_soc_data mt6893_data = {
	.opps_desc = dvfsrc_opp_mt6893_desc,
	.regs = dvfsrc_mt8195_regs,
	.get_target_level = dvfsrc_get_target_level_v2,
	.get_current_level = dvfsrc_get_current_level_v2,
	.get_vcore_level = dvfsrc_get_vcore_level_v2,
	.get_vscp_level = dvfsrc_get_vscp_level_v2,
	.set_dram_bw = dvfsrc_set_dram_bw_v1,
	.set_dram_peak_bw = dvfsrc_set_dram_peak_bw_v1,
	.set_dram_hrt_bw = dvfsrc_set_dram_hrt_bw_v1,
	.set_vcore_level = dvfsrc_set_vcore_level_v2,
	.set_vscp_level = dvfsrc_set_vscp_level_v2,
	.wait_for_opp_level = dvfsrc_wait_for_opp_level_v2,
	.wait_for_vcore_level = dvfsrc_wait_for_vcore_level_v1,
	.bw_constraints = &dvfsrc_bw_constr_v2,
};

static const struct dvfsrc_opp dvfsrc_opp_mt8183_lp4[] = {
	{ 0, 0 }, { 0, 1 }, { 0, 2 }, { 1, 2 },
};

static const struct dvfsrc_opp dvfsrc_opp_mt8183_lp3[] = {
	{ 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 2 },
};

static const struct dvfsrc_opp_desc dvfsrc_opp_mt8183_desc[] = {
	[0] = {
		.opps = dvfsrc_opp_mt8183_lp4,
		.num_opp = ARRAY_SIZE(dvfsrc_opp_mt8183_lp4),
	},
	[1] = {
		.opps = dvfsrc_opp_mt8183_lp3,
		.num_opp = ARRAY_SIZE(dvfsrc_opp_mt8183_lp3),
	},
	[2] = {
		.opps = dvfsrc_opp_mt8183_lp3,
		.num_opp = ARRAY_SIZE(dvfsrc_opp_mt8183_lp3),
	}
};

static const struct dvfsrc_soc_data mt8183_data = {
	.opps_desc = dvfsrc_opp_mt8183_desc,
	.regs = dvfsrc_mt8183_regs,
	.get_target_level = dvfsrc_get_target_level_v1,
	.get_current_level = dvfsrc_get_current_level_v1,
	.get_vcore_level = dvfsrc_get_vcore_level_v1,
	.set_dram_bw = dvfsrc_set_dram_bw_v1,
	.set_opp_level = dvfsrc_set_opp_level_v1,
	.set_vcore_level = dvfsrc_set_vcore_level_v1,
	.wait_for_opp_level = dvfsrc_wait_for_opp_level_v1,
	.wait_for_vcore_level = dvfsrc_wait_for_vcore_level_v1,
	.bw_constraints = &dvfsrc_bw_constr_v1,
};

static const struct dvfsrc_opp dvfsrc_opp_mt8195_lp4[] = {
	{ 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 },
	{ 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 },
	{ 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 2 },
	{ 1, 3 }, { 2, 3 }, { 3, 3 }, { 1, 4 },
	{ 2, 4 }, { 3, 4 }, { 2, 5 }, { 3, 5 },
	{ 3, 6 },
};

static const struct dvfsrc_opp_desc dvfsrc_opp_mt8195_desc[] = {
	[0] = {
		.opps = dvfsrc_opp_mt8195_lp4,
		.num_opp = ARRAY_SIZE(dvfsrc_opp_mt8195_lp4),
	}
};

static const struct dvfsrc_soc_data mt8195_data = {
	.opps_desc = dvfsrc_opp_mt8195_desc,
	.regs = dvfsrc_mt8195_regs,
	.get_target_level = dvfsrc_get_target_level_v2,
	.get_current_level = dvfsrc_get_current_level_v2,
	.get_vcore_level = dvfsrc_get_vcore_level_v2,
	.get_vscp_level = dvfsrc_get_vscp_level_v2,
	.set_dram_bw = dvfsrc_set_dram_bw_v1,
	.set_dram_peak_bw = dvfsrc_set_dram_peak_bw_v1,
	.set_dram_hrt_bw = dvfsrc_set_dram_hrt_bw_v1,
	.set_vcore_level = dvfsrc_set_vcore_level_v2,
	.set_vscp_level = dvfsrc_set_vscp_level_v2,
	.wait_for_opp_level = dvfsrc_wait_for_opp_level_v2,
	.wait_for_vcore_level = dvfsrc_wait_for_vcore_level_v1,
	.bw_constraints = &dvfsrc_bw_constr_v2,
};

static const struct dvfsrc_bw_constraints dvfsrc_bw_constr_mt6989 = {
	.max_dram_nom_bw = 1023,
	.max_dram_peak_bw = 1023,
	.max_dram_hrt_bw = 1023,
};

static const struct dvfsrc_soc_data mt6989_data = {
	.regs = dvfsrc_mt6989_regs,
	.get_target_level = dvfsrc_get_target_level_mt6989,
	.get_current_level = dvfsrc_get_current_level_mt6989,
	.get_vcore_level = dvfsrc_get_vcore_level_v2,
	.get_vscp_level = dvfsrc_get_vscp_level_v2,
	.set_dram_bw = dvfsrc_set_dram_bw_v1,
	.set_dram_peak_bw = dvfsrc_set_dram_peak_bw_v1,
	.set_dram_hrt_bw = dvfsrc_set_dram_hrt_bw_mt6989,
	.set_opp_level = dvfsrc_set_opp_level_mt6989,
	.set_vcore_level = dvfsrc_set_vcore_level_mt6989,
	.set_vscp_level = dvfsrc_set_vscp_level_v2,
	.wait_for_vcore_level = dvfsrc_wait_for_vcore_level_mt6989,
	.wait_for_opp_level = dvfsrc_wait_for_opp_level_mt6989,
	.bw_constraints = &dvfsrc_bw_constr_mt6989,
	.defer_start = true,
};

static const struct of_device_id mtk_dvfsrc_of_match[] = {
	{ .compatible = "mediatek,mt6989-dvfsrc", .data = &mt6989_data },
	{ .compatible = "mediatek,mt6893-dvfsrc", .data = &mt6893_data },
	{ .compatible = "mediatek,mt8183-dvfsrc", .data = &mt8183_data },
	{ .compatible = "mediatek,mt8195-dvfsrc", .data = &mt8195_data },
	{ /* sentinel */ }
};

static struct platform_driver mtk_dvfsrc_driver = {
	.probe	= mtk_dvfsrc_probe,
	.driver = {
		.name = "mtk-dvfsrc",
		.of_match_table = mtk_dvfsrc_of_match,
	},
};
module_platform_driver(mtk_dvfsrc_driver);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_AUTHOR("Dawei Chien <dawei.chien@mediatek.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek DVFSRC driver");
