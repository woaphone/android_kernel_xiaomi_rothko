// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Samuel Hsieh <samuel.hsieh@mediatek.com>
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "../rothko_modem/include/mt-plat/mtk_ccci_common.h"
#include "../rothko_modem/eccci/inc/ccci_fsm.h"
#include "mtk_mdpm.h"
#include "mtk_mdpm_platform.h"
#include "mtk_mdpm_platform_data.h"
#include "mtk_mdpm_platform_table.h"

static_assert(MDPM_DBM_SIZE ==
	      (SHARE_MEM_BLOCK_NUM + 2 * MDPM_DBM_GUARD_WORDS) * sizeof(u32));
static_assert(M_MD_SCENARIO_1 == 114 && M_MD_SCENARIO_2 == 111);
static_assert(M_VERSION_CHECK == 118 && M_RF_HW == 121);
static_assert(SECTION_LEVEL_2_START == 41 && SECTION_LEVEL_2_END == 81);
static_assert(SECTION_LEVEL_START == 104 && SECTION_LEVEL_END == 123);

/* Each DBM word contains six five-bit section counters. */
static const int section_shift[DBM_SECTION_NUM] = {
	DBM_SECTION_1, DBM_SECTION_2, DBM_SECTION_3, DBM_SECTION_4,
	DBM_SECTION_5, DBM_SECTION_6, DBM_SECTION_7, DBM_SECTION_8,
	DBM_SECTION_9, DBM_SECTION_10, DBM_SECTION_11, DBM_SECTION_12,
};

static DEFINE_MUTEX(mdpm_lock);
static u32 __iomem *dbm;
static u32 previous[SHARE_MEM_BLOCK_NUM];

/* The stock PBM samples at 200 ms intervals. */
#define MDPM_SAMPLE_INTERVAL_NS (200 * NSEC_PER_MSEC)

static struct mdpm_sample {
	ktime_t expires;
	enum md_scenario scenario;
	int base_power;
	int tx_power;
	int section;
	int rat;
	int rf_hw;
} mdpm_cached_sample;

void mtk_mdpm_reset_dbm(void)
{
	mutex_lock(&mdpm_lock);
	dbm = NULL;
	memset(previous, 0, sizeof(previous));
	mdpm_cached_sample.expires = 0;
	mutex_unlock(&mdpm_lock);
}

int mtk_mdpm_init_dbm(void __iomem *base, size_t size)
{
	u32 section[SHARE_MEM_BLOCK_NUM] = {};
	u32 __iomem *payload;
	int i, j;

	if (!base || size != MDPM_DBM_SIZE)
		return -EINVAL;

	payload = (u32 __iomem *)base + MDPM_DBM_GUARD_WORDS;
	for (i = 0; i < ARRAY_SIZE(mt6989_mdpm_tx_pwr); i++) {
		const struct tx_power *tx = &mt6989_mdpm_tx_pwr[i];

		for (j = 0; j < ARRAY_SIZE(section_shift); j++) {
			int shift = section_shift[j];

			section[tx->shm_sec_idx[shift / 32]] |=
				(u32)tx->rfhw->section[j] << (shift % 32);
		}
	}

	mutex_lock(&mdpm_lock);
	/*
	 * The stock HS1 initialization writes exactly these two spans.
	 * The upper span also clears the scenario and RF HW slots before
	 * the modem takes ownership. Leave DBM counters and guards alone.
	 */
	for (i = SECTION_LEVEL_START; i <= SECTION_LEVEL_END; i++)
		writel_relaxed(section[i], payload + i);
	for (i = SECTION_LEVEL_2_START; i <= SECTION_LEVEL_2_END; i++)
		writel_relaxed(section[i], payload + i);
	/* The first span cleared this word, including the modem valid bits. */
	writel(AP_MD_MDPM_VERSION, payload + M_VERSION_CHECK);
	/* Publish the section table before CCCI signals H2D_SRAM. */
	wmb();
	memset(previous, 0, sizeof(previous));
	mdpm_cached_sample.expires = 0;
	dbm = payload;
	mutex_unlock(&mdpm_lock);

	return 0;
}

static enum md_scenario mdpm_scenario(u64 status)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt6989_scen_priority); i++) {
		enum md_scenario id = mt6989_scen_priority[i];
		u64 mask = mt6989_mdpm_scen[id].scenario_reg;

		if ((status & mask) == mask)
			return id;
	}

	return S_STANDBY;
}

static int mdpm_tx_table(const u32 *sample, enum tx_power_table id,
			 int *section)
{
	const struct tx_power *tx = &mt6989_mdpm_tx_pwr[id];
	int i;

	for (i = 0; i < ARRAY_SIZE(section_shift); i++) {
		int shift = section_shift[i];
		int word = tx->shm_dbm_idx[shift / 32];

		if (((sample[word] ^ previous[word]) >> (shift % 32)) &
		    DBM_SECTION_MASK) {
			previous[tx->shm_dbm_idx[0]] = sample[tx->shm_dbm_idx[0]];
			previous[tx->shm_dbm_idx[1]] = sample[tx->shm_dbm_idx[1]];
			*section = i + 1;
			return tx->rfhw->pa_power.max[i] + tx->rfhw->rf_power.max[i];
		}
	}

	return 0;
}

static int mdpm_tx_rat(const u32 *sample, enum tx_rat_type rat, int *section)
{
	int power;

	switch (rat) {
	case RAT_2G:
		return mdpm_tx_table(sample, TX_2G_DBM, section);
	case RAT_3G:
		return mdpm_tx_table(sample, TX_3G_DBM, section);
	case RAT_3GTDD:
		return mdpm_tx_table(sample, TX_3GTDD_DBM, section);
	case RAT_4G:
		power = mdpm_tx_table(sample, TX_4G_CC0_DBM, section);
		return power + mdpm_tx_table(sample, TX_4G_CC1_DBM, section);
	case RAT_C2K:
		return mdpm_tx_table(sample, TX_C2K_DBM, section);
	case RAT_5G:
		power = mdpm_tx_table(sample, TX_NR_CC0_DBM, section);
		return power + mdpm_tx_table(sample, TX_NR_CC1_DBM, section);
	case RAT_MMW:
		power = mdpm_tx_table(sample, TX_MMW_TX2_DBM, section);
		return power ?: mdpm_tx_table(sample, TX_MMW_TX1_DBM, section);
	default:
		return 0;
	}
}

static unsigned int mdpm_version_status(void)
{
	return (readl(dbm + M_VERSION_CHECK) >> VERSION_CHECK_VALID_SHIFT) &
		VERSION_CHECK_VALID_MASK;
}

static int mdpm_power_show(struct seq_file *m, void *unused)
{
	u32 sample[SHARE_MEM_BLOCK_NUM];
	const struct mdpm_scenario *scenario;
	int i, tx_power = 0, rat = 0, section = 0, rf_hw = -1;
	unsigned int version_status;
	enum md_scenario id;
	u32 low, high, rf;
	int base_power;
	struct mdpm_sample result;

	mutex_lock(&mdpm_lock);
	if (!dbm || ccci_fsm_get_md_state() != READY) {
		mdpm_cached_sample.expires = 0;
		seq_puts(m, "unavailable: modem DBM not ready\n");
		goto out;
	}

	version_status = mdpm_version_status();
	if (version_status != VERSION_VALID) {
		mdpm_cached_sample.expires = 0;
		if (version_status == VERSION_INVALID)
			seq_puts(m, "unavailable: incompatible modem DBM version\n");
		else
			seq_puts(m, "unavailable: waiting for modem DBM version\n");
		goto out;
	}

	if (ktime_before(ktime_get(), mdpm_cached_sample.expires))
		goto report;

	low = readl_relaxed(dbm + M_MD_SCENARIO_1);
	high = readl_relaxed(dbm + M_MD_SCENARIO_2);
	rf = readl_relaxed(dbm + M_RF_HW);
	for (i = DBM_TABLE_START; i <= DBM_TABLE_END; i++)
		sample[i] = readl_relaxed(dbm + i);
	if (mdpm_version_status() != VERSION_VALID ||
	    ccci_fsm_get_md_state() != READY) {
		mdpm_cached_sample.expires = 0;
		seq_puts(m, "unavailable: modem DBM restarting\n");
		goto out;
	}

	/* MT6989 provides one table, regardless of the reported RF HW ID. */
	if (((rf >> RF_HW_VALID_SHIFT) & RF_HW_VALID_MASK) == RF_HW_VALID &&
	    (rf & RF_HW_VERSION_MASK) < RF_HW_NUM)
		rf_hw = rf & RF_HW_VERSION_MASK;

	id = mdpm_scenario((u64)high << 32 | low);
	scenario = &mt6989_mdpm_scen[id];
	base_power = scenario->scenario_power->max;
	for (i = 0; i < ARRAY_SIZE(scenario->tx_power_rat); i++) {
		int rat_section = 0;
		int power;

		if (!scenario->tx_power_rat[i])
			break;
		power = mdpm_tx_rat(sample, scenario->tx_power_rat[i], &rat_section);
		if (!power)
			continue;
		if (scenario->tx_power_rat_sum) {
			tx_power += power;
			rat |= scenario->tx_power_rat[i];
		} else if (power > tx_power) {
			tx_power = power;
			rat = scenario->tx_power_rat[i];
		} else {
			continue;
		}
		section = rat_section;
	}

	mdpm_cached_sample = (struct mdpm_sample) {
		.expires = ktime_add_ns(ktime_get(), MDPM_SAMPLE_INTERVAL_NS),
		.scenario = id,
		.base_power = base_power,
		.tx_power = tx_power,
		.section = section,
		.rat = rat,
		.rf_hw = rf_hw,
	};
report:
	result = mdpm_cached_sample;
	mutex_unlock(&mdpm_lock);
	seq_printf(m, "MAX power: scenario=%dmW dbm=%dmW total=%dmW\n",
		   result.base_power, result.tx_power, result.base_power + result.tx_power);
	seq_printf(m, " scenario name=%s(%d) section=%d, rat=%d, power_type=0, rf_hw=%d\n",
		   mt6989_mdpm_scen[result.scenario].scenario_name, result.scenario,
		   result.section, result.rat, result.rf_hw);
	return 0;
out:
	mutex_unlock(&mdpm_lock);
	return 0;
}

static int __init mtk_mdpm_init(void)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir("mdpm", NULL);
	if (!dir)
		return -ENOMEM;
	if (!proc_create_single("mdpm_power", 0440, dir, mdpm_power_show)) {
		proc_remove(dir);
		return -ENOMEM;
	}

	return 0;
}
late_initcall(mtk_mdpm_init);
