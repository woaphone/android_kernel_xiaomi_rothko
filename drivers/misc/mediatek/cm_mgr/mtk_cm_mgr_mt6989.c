// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

/* system includes */
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/interconnect.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/time.h>
#include <linux/topology.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/soc/mediatek/dvfsrc.h>
#include "mtk_cm_ipi.h"
#include "mtk_cm_mgr_common.h"
#include "mtk_cm_mgr_mt6989.h"

#define CM_MGR_PERF_TIMEOUT_MS msecs_to_jiffies(100)

/*****************************************************************************
 *  Variables
 *****************************************************************************/
spinlock_t cm_mgr_lock;
static int cm_mgr_idx;
static int cm_mgr_dram_opp = -1;
static int pm_qos_update_request_status;
static int sw_ver;
static unsigned int prev_freq_idx[CM_MGR_CPU_CLUSTER];

static struct cm_mgr_hook local_hk;
static ktime_t perf_now;

struct timer_list cm_mgr_perf_timeout_timer;
static struct delayed_work cm_mgr_timeout_work;

u32 *cm_mgr_perfs;


/*****************************************************************************
 *  Platform functions
 *****************************************************************************/
static int cm_get_chipid(void)
{
	struct device_node *dn = of_find_node_by_path("/chosen");
	const struct tag_chipid *chipid;
	int len;

	if (!dn)
		return -ENODEV;
	chipid = of_get_property(dn, "atag,chipid", &len);
	if (!chipid || len < sizeof(*chipid)) {
		of_node_put(dn);
		return -EINVAL;
	}
	/* The bootloader ATAG is stored as raw little-endian bytes. */
	sw_ver = le32_to_cpu(chipid->sw_ver);
	of_node_put(dn);
	return 0;
}

static u32 cm_mgr_get_perfs_mt6989(int num)
{
	if (num < 0 || num >= cm_mgr_get_num_perf())
		return 0;
	return cm_mgr_perfs[num];
}

static int cm_mgr_check_dram_type(void)
{
	unsigned int ddr_type = mtk_dramc_get_ddr_type();

	if (!ddr_type)
		return -EPROBE_DEFER;
	if (ddr_type == TYPE_LPDDR5)
		cm_mgr_idx = CM_MGR_LP5;
	else if (ddr_type == TYPE_LPDDR5X)
		cm_mgr_idx = CM_MGR_LP5X;
	else
		return -EINVAL;

	return cm_mgr_to_sspm_command(IPI_CM_MGR_DRAM_TYPE, cm_mgr_idx | (sw_ver << 4));
}

static void cm_mgr_timeout_process(struct work_struct *work)
{
	icc_set_bw(cm_mgr_get_bw_path(), 0, 0);
}

static void cm_mgr_perf_timeout_timer_fn(struct timer_list *timer)
{
	if (pm_qos_update_request_status) {
		cm_mgr_dram_opp = -1;
		cm_mgr_set_dram_opp_base(cm_mgr_dram_opp);
		schedule_delayed_work(&cm_mgr_timeout_work, 1);

		pm_qos_update_request_status = 0;
		debounce_times_perf_down_local_set(-1);
	}
}

static void cm_mgr_perf_platform_set_status_mt6989(int enable)
{
	unsigned long expires;
	int down_local;

	if (enable || pm_qos_update_request_status) {
		expires = jiffies + CM_MGR_PERF_TIMEOUT_MS;
		mod_timer(&cm_mgr_perf_timeout_timer, expires);
	}

	if (enable) {
		if (!cm_mgr_get_perf_enable())
			return;

		debounce_times_perf_down_local_set(0);

		perf_now = ktime_get();

		if (cm_mgr_get_dram_opp_base() == -1) {
			cm_mgr_dram_opp = 0;
			cm_mgr_set_dram_opp_base(cm_mgr_get_num_perf());
		} else {
			if (cm_mgr_dram_opp > 0)
				cm_mgr_dram_opp--;
		}

		cm_mgr_dram_opp = cm_mgr_judge_perfs_dram_opp(cm_mgr_dram_opp);

		icc_set_bw(cm_mgr_get_bw_path(), 0,
			   cm_mgr_get_perfs_mt6989(cm_mgr_dram_opp));

		pm_qos_update_request_status = enable;
	} else {
		down_local = debounce_times_perf_down_local_get();
		if (down_local < 0)
			return;

		++down_local;
		debounce_times_perf_down_local_set(down_local);
		if (down_local < debounce_times_perf_down_get()) {
			if (cm_mgr_get_dram_opp_base() < 0) {
				icc_set_bw(cm_mgr_get_bw_path(), 0, 0);
				pm_qos_update_request_status = enable;
				debounce_times_perf_down_local_set(-1);
				return;
			}
			if (ktime_ms_delta(ktime_get(), perf_now) < PERF_TIME)
				return;
			cm_mgr_set_dram_opp_base(-1);
		}

		if ((cm_mgr_dram_opp < cm_mgr_get_dram_opp_base()) &&
		    (debounce_times_perf_down_get() > 0)) {
			cm_mgr_dram_opp = cm_mgr_get_dram_opp_base() *
					  debounce_times_perf_down_local_get() /
					  debounce_times_perf_down_get();

			icc_set_bw(cm_mgr_get_bw_path(), 0,
				   cm_mgr_get_perfs_mt6989(cm_mgr_dram_opp));
		} else {
			cm_mgr_dram_opp = -1;
			cm_mgr_set_dram_opp_base(cm_mgr_dram_opp);
			icc_set_bw(cm_mgr_get_bw_path(), 0, 0);
			pm_qos_update_request_status = enable;
			debounce_times_perf_down_local_set(-1);
		}
	}
}

static void cm_mgr_perf_set_status_mt6989(int enable)
{
	cm_mgr_perf_platform_set_status_mt6989(enable);
}

static void check_cm_mgr_status_mt6989(unsigned int cluster, unsigned int freq,
				       unsigned int idx)
{
	unsigned int bcpu_opp_max;
	unsigned long spinlock_save_flag;

	if (cluster < CM_MGR_B || cluster >= CM_MGR_CPU_CLUSTER || !cm_mgr_get_enable())
		return;

	spin_lock_irqsave(&cm_mgr_lock, spinlock_save_flag);

	prev_freq_idx[cluster] = idx;

	if (prev_freq_idx[CM_MGR_B] < prev_freq_idx[CM_MGR_BB])
		bcpu_opp_max = prev_freq_idx[CM_MGR_B];
	else
		bcpu_opp_max = prev_freq_idx[CM_MGR_BB];

	cm_mgr_update_dram_by_cpu_opp(bcpu_opp_max);
	spin_unlock_irqrestore(&cm_mgr_lock, spinlock_save_flag);
}

static int cm_mgr_check_dts_setting_mt6989(struct platform_device *pdev)
{
	int i = 0;
	int ret = 0;
	int temp = 0;
	struct device_node *node = pdev->dev.of_node;
	struct icc_path *bw_path;

	ret = of_count_phandle_with_args(node, "required-opps", NULL);
	if (ret > 0) {
		cm_mgr_set_num_perf(ret);
		pr_info("%s(%d): required_opps count %d\n", __func__, __LINE__,
			ret);
	} else {
		ret = -1;
		pr_info("%s(%d): fail to get required_opps count from dts.\n",
			__func__, __LINE__);
		goto ERROR;
	}

	cm_mgr_perfs = devm_kzalloc(&pdev->dev, ret * sizeof(u32), GFP_KERNEL);
	if (!cm_mgr_perfs) {
		ret = -ENOMEM;
		pr_info("%s(%d): fail to kzalloc cm_mgr_perfs.\n", __func__,
			__LINE__);
		goto ERROR;
	}

	/*
	 * The EMI ICC supplier is a child of DVFSRC, which selects the
	 * DRAM type used to index each required OPP's bandwidth table.
	 */
	bw_path = devm_of_icc_get(&pdev->dev, "cm-perf-bw");
	if (IS_ERR(bw_path))
		return PTR_ERR(bw_path);
	if (!bw_path)
		return -ENODEV;

	for (i = 0; i < ret; i++)
		cm_mgr_perfs[i] = dvfsrc_get_required_opp_peak_bw(node, i);

	ret = of_property_read_u32(node, "cm-mgr-num-array", &temp);
	if (ret) {
		pr_info("%s(%d): fail to get cm_mgr_num_array from dts. ret %d\n",
			__func__, __LINE__, ret);
		goto ERROR;
	} else {
		cm_mgr_set_num_array(temp);
		pr_info("%s(%d): cm_mgr_num_array %d\n", __func__, __LINE__,
			cm_mgr_get_num_array());
	}

	ret = of_property_read_u32(node, "cm-perf-mode-enable", &temp);
	if (ret) {
		pr_info("%s(%d): fail to get cm_perf_mode_enable from dts. ret %d\n",
			__func__, __LINE__, ret);
		goto ERROR;
	} else {
		cm_mgr_set_perf_mode_enable(temp);
		pr_info("%s(%d): cm_perf_mode_enable %d\n", __func__, __LINE__,
			cm_mgr_get_perf_mode_enable());
	}

	ret = of_property_read_u32(node, "cm-perf-mode-ceiling-opp", &temp);
	if (ret) {
		pr_info("%s(%d): fail to get cm_perf_mode_ceiling_opp from dts. ret %d\n",
			__func__, __LINE__, ret);
		goto ERROR;
	} else {
		cm_mgr_set_perf_mode_ceiling_opp(temp);
		pr_info("%s(%d): cm_perf_mode_ceiling_opp %d\n", __func__, __LINE__,
			cm_mgr_get_perf_mode_ceiling_opp());
	}

	ret = of_property_read_u32(node, "cm-perf-mode-thd", &temp);
	if (ret) {
		pr_info("%s(%d): fail to get cm_perf_mode_thd from dts. ret %d\n",
			__func__, __LINE__, ret);
		goto ERROR;
	} else {
		cm_mgr_set_perf_mode_thd(temp);
		pr_info("%s(%d): cm_perf_mode_thd %d\n", __func__, __LINE__,
			cm_mgr_get_perf_mode_thd());
	}

	cm_mgr_set_bw_path(bw_path);
	return 0;

ERROR:
	return ret;
}

static int platform_cm_mgr_probe(struct platform_device *pdev)
{
	int ret = 0;

	spin_lock_init(&cm_mgr_lock);
	ret = cm_mgr_check_dts_setting_mt6989(pdev);
	if (ret) {
		pr_info("%s(%d): fail to get platform data from dts. ret %d\n",
			__func__, __LINE__, ret);
		goto ERROR;
	}

	ret = cm_mgr_check_dts_setting(pdev);
	if (ret) {
		pr_info("%s(%d): fail to get common data from dts. ret %d\n",
			__func__, __LINE__, ret);
		goto ERROR;
	}

	ret = cm_get_chipid();
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "invalid chipid ATAG\n");

	ret = cm_ipi_init(&pdev->dev);
	if (ret) {
		pr_info("%s(%d): fail to common init. ret %d\n", __func__,
			__LINE__, ret);
		goto ERROR;
	}

	ret = cm_mgr_check_dram_type();
	if (ret)
		goto ERROR;

	INIT_DELAYED_WORK(&cm_mgr_timeout_work, cm_mgr_timeout_process);
	timer_setup(&cm_mgr_perf_timeout_timer, cm_mgr_perf_timeout_timer_fn,
		    0);

	local_hk.cm_mgr_get_perfs = cm_mgr_get_perfs_mt6989;
	local_hk.check_cm_mgr_status = check_cm_mgr_status_mt6989;
	local_hk.cm_mgr_perf_set_status = cm_mgr_perf_set_status_mt6989;

	ret = cm_mgr_get_sspm_version();
	if (ret)
		goto ERROR;

	ret = cm_mgr_to_sspm_command(IPI_CM_MGR_PERF_MODE_ENABLE, cm_mgr_get_perf_mode_enable());
	if (ret)
		goto ERROR;
	ret = cm_mgr_to_sspm_command(IPI_CM_MGR_PERF_MODE_CEILING_OPP, cm_mgr_get_perf_mode_ceiling_opp());
	if (ret)
		goto ERROR;
	ret = cm_mgr_to_sspm_command(IPI_CM_MGR_PERF_MODE_THD, cm_mgr_get_perf_mode_thd());
	if (ret)
		goto ERROR;
	ret = cm_mgr_to_sspm_command(IPI_CM_MGR_ENABLE, cm_mgr_get_enable());
	if (ret)
		goto ERROR;

	/* An unobserved cluster must not masquerade as CPU OPP 0. */
	memset(prev_freq_idx, 0xff, sizeof(prev_freq_idx));
	cm_mgr_register_hook(&local_hk);
	ret = cm_mgr_common_init(&pdev->dev);
	if (ret) {
		cm_mgr_unregister_hook(&local_hk);
		goto ERROR;
	}
	pr_info("%s(%d): platform-cm_mgr_probe Done.\n", __func__, __LINE__);

	return 0;

ERROR:
	cm_mgr_set_bw_path(NULL);
	return dev_err_probe(&pdev->dev, ret, "CM initialization failed\n");
}

static void platform_cm_mgr_remove(struct platform_device *pdev)
{
	cm_mgr_common_exit();
	cm_mgr_unregister_hook(&local_hk);
	timer_shutdown_sync(&cm_mgr_perf_timeout_timer);
	cancel_delayed_work_sync(&cm_mgr_timeout_work);
	icc_set_bw(cm_mgr_get_bw_path(), 0, 0);
	cm_mgr_set_bw_path(NULL);
}

static const struct of_device_id platform_cm_mgr_of_match[] = {
	{
		.compatible = "mediatek,mt6989-cm_mgr",
	},
	{},
};

MODULE_DEVICE_TABLE(of, platform_cm_mgr_of_match);

static const struct platform_device_id platform_cm_mgr_id_table[] = {
	{ "mt6989-cm_mgr", 0 },
	{},
};

static struct platform_driver mtk_platform_cm_mgr_driver = {
	.probe = platform_cm_mgr_probe,
	.remove = platform_cm_mgr_remove,
	.driver = {
		.name = "mt6989-cm_mgr",
		.owner = THIS_MODULE,
		.of_match_table = platform_cm_mgr_of_match,
	},
	.id_table = platform_cm_mgr_id_table,
};

/*
 * driver initialization entry point
 */
static int __init platform_cm_mgr_init(void)
{
	return platform_driver_register(&mtk_platform_cm_mgr_driver);
}

static void __exit platform_cm_mgr_exit(void)
{
	platform_driver_unregister(&mtk_platform_cm_mgr_driver);
	pr_info("%s(%d): platform-cm_mgr Exit.\n", __func__, __LINE__);
}

subsys_initcall(platform_cm_mgr_init);
module_exit(platform_cm_mgr_exit);

MODULE_DESCRIPTION("Mediatek cm_mgr driver");
MODULE_AUTHOR("Morven-CF Yeh<morven-cf.yeh@mediatek.com>");
MODULE_LICENSE("GPL");
