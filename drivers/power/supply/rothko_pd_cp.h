/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROTHKO_PD_CP_H
#define _ROTHKO_PD_CP_H

#include <linux/types.h>

struct charger_device;
struct mtk_charger;

#ifdef CONFIG_ROTHKO_SC858X
int mt6375_pps_hold(struct charger_device *chgdev, bool hold, bool restore_buck);
void mt6375_report_pd_tier(struct mtk_charger *info, int watts, int type);
void mt6375_report_pd_auth(struct mtk_charger *info, bool auth, bool verifying,
			   bool verify_done, int pd_type);
void rothko_pd_cp_init(struct mtk_charger *info);
bool rothko_pd_cp_poll(struct mtk_charger *info, bool allow);
bool rothko_pd_cp_active(struct mtk_charger *info);
bool rothko_pd_cp_attached(struct mtk_charger *info);
bool rothko_pd_cp_switching(struct mtk_charger *info);
bool rothko_pd_cp_verifying(struct mtk_charger *info);
#else
static inline void rothko_pd_cp_init(struct mtk_charger *info) { }
static inline bool rothko_pd_cp_poll(struct mtk_charger *info, bool allow) { return false; }
static inline bool rothko_pd_cp_active(struct mtk_charger *info) { return false; }
static inline bool rothko_pd_cp_attached(struct mtk_charger *info) { return false; }
static inline bool rothko_pd_cp_switching(struct mtk_charger *info) { return false; }
static inline bool rothko_pd_cp_verifying(struct mtk_charger *info) { return false; }
#endif

#endif
