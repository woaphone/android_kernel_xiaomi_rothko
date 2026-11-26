/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef __MTK_CM_MGR_H__
#define __MTK_CM_MGR_H__

#include <linux/kernel.h>

struct device;
struct platform_device;
struct work_struct;
#define CM_DBG_PRINT(fmt, ...) \
	do { \
		if (cm_dbg_info) \
			pr_info(pr_fmt(fmt), ##__VA_ARGS__); \
	} while (0)
enum {
	CM_MGR_ARCH_V1,
	CM_MGR_ARCH_V1P,
};

struct mtk_cm_mgr {
	struct device *dev;
	int num_perf;
	int idx;
	void __iomem *base;
	int *buf;
};


struct cm_mgr_hook {
	u32 (*cm_mgr_get_perfs)(int enable);
	void (*cm_mgr_perf_set_force_status)(int enable);
	void (*check_cm_mgr_status)(unsigned int cluster, unsigned int freq,
				    unsigned int idx);
	void (*cm_mgr_perf_platform_set_status)(int enable);
	void (*cm_mgr_perf_set_status)(int enable);
	int (*cm_mgr_get_latency_awareness_model_indexes)(unsigned int *buf);
};


/* common api */
extern int get_cm_step_num(void);
extern int cm_mgr_judge_perfs_dram_opp(int dram_opp);
extern void cm_mgr_update_dram_by_cpu_opp(int cpu_opp);
extern int cm_mgr_check_dts_setting(struct platform_device *pdev);
extern int cm_mgr_common_init(struct device *dev);
extern void cm_mgr_common_exit(void);
extern void cm_mgr_process(struct work_struct *work);

extern int cm_mgr_get_enable(void);
extern void cm_mgr_set_pdev(struct platform_device *pdev);
extern int cm_mgr_get_num_array(void);
extern void cm_mgr_set_num_array(int num);
extern int cm_mgr_get_dram_opp_base(void);
extern void cm_mgr_set_dram_opp_base(int num);
extern int cm_mgr_get_sspm_version(void);
extern int cm_mgr_get_num_perf(void);
extern void cm_mgr_set_num_perf(int num);
extern int cm_mgr_get_perf_enable(void);
extern int cm_mgr_get_perf_force_enable(void);
extern void cm_mgr_set_perf_force_enable(int enable);
extern int debounce_times_perf_down_get(void);
extern int debounce_times_perf_force_down_get(void);
extern int debounce_times_perf_down_local_get(void);
extern void debounce_times_perf_down_local_set(int num);
extern int debounce_times_perf_down_force_local_get(void);
extern void debounce_times_perf_down_force_local_set(int num);
extern int cm_mgr_get_blank_status(void);
extern int cm_mgr_get_disable_fb(void);
extern void cm_mgr_set_disable_fb(int disable_fb);
extern struct icc_path *cm_mgr_get_bw_path(void);
extern struct icc_path *cm_mgr_set_bw_path(struct icc_path *bw_path);
extern void cm_mgr_perf_set_status(int enable);
extern void cm_mgr_register_hook(struct cm_mgr_hook *hook);
extern void cm_mgr_unregister_hook(struct cm_mgr_hook *hook);
extern void cm_mgr_set_dram_opp_ceiling(int opp);
extern void cm_mgr_set_dram_opp_floor(int opp);
extern int cm_mgr_get_latency_awareness_model_info(unsigned int *buf);
extern void cm_mgr_set_perf_mode_enable(int enable);
extern int cm_mgr_get_perf_mode_enable(void);
extern void cm_mgr_set_perf_mode_ceiling_opp(int opp);
extern int cm_mgr_get_perf_mode_ceiling_opp(void);
extern void cm_mgr_set_perf_mode_thd(int thd);
extern int cm_mgr_get_perf_mode_thd(void);

#endif /* __MTK_CM_MGR_H__ */
