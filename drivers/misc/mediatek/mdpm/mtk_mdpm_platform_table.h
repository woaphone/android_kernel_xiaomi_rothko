/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Minghao Chou <minghao.chou@mediatek.com>
 */

#ifndef _MTK_MDPM_PLATFORM_TABLE_H_
#define _MTK_MDPM_PLATFORM_TABLE_H_

static struct tx_power mt6989_mdpm_tx_pwr[TX_DBM_NUM] = {
	[TX_2G_DBM] = {
		.dbm_name = "2G",
		.shm_dbm_idx = {M_2G_DBM_TABLE, M_2G_DBM_1_TABLE},
		.shm_sec_idx = {M_2G_SECTION_LEVEL, M_2G_SECTION_1_LEVEL},
		.rfhw = &rfhw_6989[TX_2G_DBM],
	},

	[TX_3G_DBM] = {
		.dbm_name = "3G",
		.shm_dbm_idx = {M_3G_DBM_TABLE, M_3G_DBM_1_TABLE},
		.shm_sec_idx = {M_3G_SECTION_LEVEL, M_3G_SECTION_1_LEVEL},
		.rfhw = &rfhw_6989[TX_3G_DBM],
	},

	[TX_3GTDD_DBM] = {
		.dbm_name = "3GTDD",
		.shm_dbm_idx = {M_TDD_DBM_TABLE, M_TDD_DBM_1_TABLE},
		.shm_sec_idx = {M_TDD_SECTION_LEVEL, M_TDD_SECTION_1_LEVEL},
		.rfhw = &rfhw_6989[TX_3GTDD_DBM],
	},

	[TX_4G_CC0_DBM] = {
		.dbm_name = "4G_CC0",
		.shm_dbm_idx = {M_4G_DBM_TABLE, M_4G_DBM_2_TABLE},
		.shm_sec_idx = {M_4G_SECTION_LEVEL, M_4G_SECTION_9_LEVEL},
		.rfhw = &rfhw_6989[TX_4G_CC0_DBM],
	},

	[TX_4G_CC1_DBM] = {
		.dbm_name = "4G_CC1",
		.shm_dbm_idx = {M_4G_DBM_1_TABLE, M_4G_DBM_3_TABLE},
		.shm_sec_idx = {M_4G_SECTION_LEVEL, M_4G_SECTION_9_LEVEL},
		.rfhw = &rfhw_6989[TX_4G_CC1_DBM],
	},

	[TX_C2K_DBM] = {
		.dbm_name = "C2K",
		.shm_dbm_idx = {M_C2K_DBM_1_TABLE, M_C2K_DBM_2_TABLE},
		.shm_sec_idx = {M_C2K_SECTION_1_LEVEL, M_C2K_SECTION_2_LEVEL},
		.rfhw = &rfhw_6989[TX_C2K_DBM],
	},

	[TX_NR_CC0_DBM] = {
		.dbm_name = "NR_CC0",
		.shm_dbm_idx = {M_NR_DBM_TABLE, M_NR_DBM_1_TABLE},
		.shm_sec_idx = {M_NR_SECTION_LEVEL, M_NR_SECTION_1_LEVEL},
		.rfhw = &rfhw_6989[TX_NR_CC0_DBM],
	},

	[TX_NR_CC1_DBM] = {
		.dbm_name = "NR_CC1",
		.shm_dbm_idx = {M_NR_DBM_2_TABLE, M_NR_DBM_3_TABLE},
		.shm_sec_idx = {M_NR_SECTION_2_LEVEL, M_NR_SECTION_3_LEVEL},
		.rfhw = &rfhw_6989[TX_NR_CC1_DBM],
	},

	[TX_MMW_TX1_DBM] = {
		.dbm_name = "MMW_TX1",
		.shm_dbm_idx = {M_MMW_DBM_TABLE, M_MMW_DBM_1_TABLE},
		.shm_sec_idx = {M_MMW_SECTION_LEVEL, M_MMW_SECTION_1_LEVEL},
		.rfhw = &rfhw_6989[TX_MMW_TX1_DBM],
	},

	[TX_MMW_TX2_DBM] = {
		.dbm_name = "MMW_TX2",
		.shm_dbm_idx = {M_MMW_DBM_2_TABLE, M_MMW_DBM_3_TABLE},
		.shm_sec_idx = {M_MMW_SECTION_2_LEVEL, M_MMW_SECTION_3_LEVEL},
		.rfhw = &rfhw_6989[TX_MMW_TX2_DBM],
	}
};

static struct mdpm_scenario mt6989_mdpm_scen[SCENARIO_NUM] = {
	[S_STANDBY] = {
		.scenario_reg = 0,
		.scenario_name = "S_STANDBY",
		.scenario_power = &md_scen_power_6989[S_STANDBY],
		.tx_power_rat = {0, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_2G_IDLE] = {
		.scenario_reg = (u64)1 << 2,
		.scenario_name = "S_2G_IDLE",
		.scenario_power = &md_scen_power_6989[S_2G_IDLE],
		.tx_power_rat = {RAT_2G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_C2K_DATALINK] = {
		.scenario_reg = (u64)1 << 11,
		.scenario_name = "S_C2K_DATALINK",
		.scenario_power = &md_scen_power_6989[S_C2K_DATALINK],
		.tx_power_rat = {RAT_C2K, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_3G_IDLE] = {
		.scenario_reg = (u64)1 << 3,
		.scenario_name = "S_3G_IDLE",
		.scenario_power = &md_scen_power_6989[S_3G_IDLE],
		.tx_power_rat = {RAT_3G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_4G_1CC] = {
		.scenario_reg = (u64)1 << 4,
		.scenario_name = "S_4G_1CC",
		.scenario_power = &md_scen_power_6989[S_4G_1CC],
		.tx_power_rat = {RAT_4G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_4G_1CC_H] = {
		.scenario_reg = (u64)1 << 7,
		.scenario_name = "S_4G_1CC_H",
		.scenario_power = &md_scen_power_6989[S_4G_1CC_H],
		.tx_power_rat = {RAT_4G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_4G_2CC] = {
		.scenario_reg = (u64)1 << 12,
		.scenario_name = "S_4G_2CC",
		.scenario_power = &md_scen_power_6989[S_4G_2CC],
		.tx_power_rat = {RAT_4G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_4G_3CC] = {
		.scenario_reg = (u64)1 << 13,
		.scenario_name = "S_4G_3CC_4CC_5CC",
		.scenario_power = &md_scen_power_6989[S_4G_3CC],
		.tx_power_rat = {RAT_4G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_NR_0CC_1TG] = {
		.scenario_reg = (u64)1 << 16,
		.scenario_name = "S_5G_NR_0CC_1TG",
		.scenario_power = &md_scen_power_6989[S_5G_NR_0CC_1TG],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_NR_0CC_2TG] = {
		.scenario_reg = (u64)1 << 17,
		.scenario_name = "S_5G_NR_0CC_2TG",
		.scenario_power = &md_scen_power_6989[S_5G_NR_0CC_2TG],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_NR_0CC_3TG] = {
		.scenario_reg = (u64)1 << 18,
		.scenario_name = "S_5G_NR_0CC_3TG",
		.scenario_power = &md_scen_power_6989[S_5G_NR_0CC_3TG],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR1_1CC_1TG] = {
		.scenario_reg = (u64)1 << 5,
		.scenario_name = "S_5G_FR1_1CC_1TG",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_1CC_1TG],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR1_1CC_1TG_H] = {
		.scenario_reg = (u64)1 << 8,
		.scenario_name = "S_5G_FR1_1CC_1TG_H",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_1CC_1TG_H],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR1_2CC_1TG] = {
		.scenario_reg = (u64)1 << 19,
		.scenario_name = "S_5G_FR1_2CC_1TG",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_2CC_1TG],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR1_3_4CC_1TG] = {
		.scenario_reg = (u64)1 << 20,
		.scenario_name = "S_5G_FR1_3_4CC_1TG",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_3_4CC_1TG],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR1_1_4CC_2TG] = {
		.scenario_reg = (u64)1 << 21,
		.scenario_name = "S_5G_FR1_1_4CC_2TG",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_1_4CC_2TG],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR1_1_4CC_3TG] = {
		.scenario_reg = (u64)1 << 22,
		.scenario_name = "S_5G_FR1_1_4CC_3TG",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_1_4CC_3TG],
		.tx_power_rat = {RAT_5G, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR2_1CC_1TG] = {
		.scenario_reg = (u64)1 << 24,
		.scenario_name = "S_5G_FR2_1CC_1TG",
		.scenario_power = &md_scen_power_6989[S_5G_FR2_1CC_1TG],
		.tx_power_rat = {RAT_MMW, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR2_2_4CC_1TG] = {
		.scenario_reg = (u64)1 << 25,
		.scenario_name = "S_5G_FR2_2_4CC_1TG",
		.scenario_power = &md_scen_power_6989[S_5G_FR2_2_4CC_1TG],
		.tx_power_rat = {RAT_MMW, 0, 0, 0, 0},
		.tx_power_rat_sum = false,
	},

	[S_5G_FR1_FR2] = {
		.scenario_reg = (u64)1 << 26,
		.scenario_name = "S_5G_NNDC",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_FR2],
		.tx_power_rat = {RAT_MMW, RAT_5G, 0, 0, 0},
		.tx_power_rat_sum = true,
	},

	[S_5G_FR1_1CC_1TG_4G_1CC] = {
		.scenario_reg = (u64)1 << 6,
		.scenario_name = "S_5G_FR1_1CC_1TG_4G_1CC",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_1CC_1TG_4G_1CC],
		.tx_power_rat = {RAT_5G, RAT_4G, 0, 0, 0},
		.tx_power_rat_sum = true,
	},

	[S_5G_FR1_1CC_1TG_4G_1CC_H] = {
		.scenario_reg = (u64)1 << 9,
		.scenario_name = "S_5G_FR1_1CC_1TG_4G_1CC_H",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_1CC_1TG_4G_1CC_H],
		.tx_power_rat = {RAT_5G, RAT_4G, 0, 0, 0},
		.tx_power_rat_sum = true,
	},

	[S_5G_FR2_1CC_1TG_4G_1CC_NR_0CC] = {
		.scenario_reg = (u64)1 << 27,
		.scenario_name = "S_5G_FR2_1CC_1TG_4G_1CC_NR_0CC",
		.scenario_power = &md_scen_power_6989[S_5G_FR2_1CC_1TG_4G_1CC_NR_0CC],
		.tx_power_rat = {RAT_MMW, RAT_5G, RAT_4G, 0, 0},
		.tx_power_rat_sum = true,
	},

	[S_5G_FR1_FR2_4G] = {
		.scenario_reg = (u64)1 << 28,
		.scenario_name = "S_5G_ENDC_OTHERS",
		.scenario_power = &md_scen_power_6989[S_5G_FR1_FR2_4G],
		.tx_power_rat = {RAT_MMW, RAT_5G, RAT_4G, 0, 0},
		.tx_power_rat_sum = true,
	},

	[S_4G_5G_POS_URGENT] = {
		.scenario_reg = (u64)1 << 31,
		.scenario_name = "S_4G_5G_POS_URGENT",
		.scenario_power = &md_scen_power_6989[S_4G_5G_POS_URGENT],
		.tx_power_rat = {RAT_5G, RAT_4G, 0, 0, 0},
		.tx_power_rat_sum = true,
	}

};

static int mt6989_scen_priority[SCENARIO_NUM] = {
	S_5G_FR1_FR2_4G,
	S_5G_FR1_FR2,
	S_5G_FR2_1CC_1TG_4G_1CC_NR_0CC,
	S_5G_FR1_1CC_1TG_4G_1CC_H,
	S_5G_FR1_1CC_1TG_4G_1CC,
	S_5G_FR2_2_4CC_1TG,
	S_5G_FR1_1_4CC_3TG,
	S_5G_FR1_1_4CC_2TG,
	S_5G_FR1_3_4CC_1TG,
	S_4G_3CC,
	S_5G_FR2_1CC_1TG,
	S_5G_FR1_2CC_1TG,
	S_5G_FR1_1CC_1TG_H,
	S_5G_FR1_1CC_1TG,
	S_4G_2CC,
	S_4G_1CC_H,
	S_4G_1CC,
	S_5G_NR_0CC_3TG,
	S_5G_NR_0CC_2TG,
	S_5G_NR_0CC_1TG,
	S_C2K_DATALINK,
	S_3G_IDLE,
	S_2G_IDLE,
	S_STANDBY,
	S_4G_5G_POS_URGENT
};

#endif /* _MTK_MDPM_PLATFORM_TABLE_H_ */
