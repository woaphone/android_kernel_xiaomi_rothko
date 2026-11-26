// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/cpumask.h>
#include <linux/interconnect.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/mod_devicetable.h>
#include <linux/sched/cputime.h>
#include <sched/sched.h>
#include <uapi/linux/sched/types.h>
#include <mt-plat/fpsgo_common.h>
#include <linux/soc/mediatek/dvfsrc.h>
#include "fpsgo_base.h"
#include "fbt_cpu_platform.h"
#if IS_REACHABLE(CONFIG_MTK_SCHEDULER) && IS_ENABLED(CONFIG_MTK_SCHED_VIP_TASK)
#include <common.h>
#endif

static int mask_int[FPSGO_PREFER_TOTAL];
static struct cpumask mask[FPSGO_PREFER_TOTAL];
static int mask_done;
static DEFINE_MUTEX(bw_lock);
static struct icc_path *bw_path;
static unsigned int peak_bw;
static bool frame_vote;
static bool applied_vote;
static LIST_HEAD(ux_dram_votes);
static struct hrtimer ux_dram_timer;
static void fbt_ux_dram_expire(struct work_struct *work);
static DECLARE_WORK(ux_dram_work, fbt_ux_dram_expire);

static enum hrtimer_restart fbt_ux_dram_timeout(struct hrtimer *timer)
{
	schedule_work(&ux_dram_work);
	return HRTIMER_NORESTART;
}
static int plat_gcc_enable;
static int plat_sbe_rescue_enable;
static int plat_ux_scroll_policy_type = 1;
static int plat_cpu_limit;

void fbt_notify_CM_limit(int reach_limit)
{
#if IS_REACHABLE(CONFIG_MTK_CM_MGR)
	cm_mgr_perf_set_status(reach_limit);
#endif
	fpsgo_systrace_c_fbt_debug(-100, 0, reach_limit, "notify_cm");
}

static int generate_cpu_mask(struct device_node *node);
static int generate_sbe_rescue_enable(struct device_node *node);
static int generate_ux_scroll_policy_type(struct device_node *node);

static int platform_fpsgo_probe(struct platform_device *pdev)
{
	struct icc_path *path;
	unsigned int peak = 0;

	path = devm_of_icc_get(&pdev->dev, "fpsgo-perf-bw");
	if (IS_ERR(path))
		return dev_err_probe(&pdev->dev, PTR_ERR(path), "acquiring fpsgo-perf-bw\n");

#if IS_REACHABLE(CONFIG_MTK_DVFSRC)
	if (path)
		peak = dvfsrc_get_required_opp_peak_bw(pdev->dev.of_node, 0);
#endif

	mutex_lock(&bw_lock);
	bw_path = path;
	peak_bw = peak;
	mutex_unlock(&bw_lock);
	return 0;
}

/* bw_lock protects both list membership and each render's deadline. */
static void fbt_ux_drop_all_votes(void)
{
	struct render_info *thr, *next;

	list_for_each_entry_safe(thr, next, &ux_dram_votes, ux_ddr_vote)
		list_del_init(&thr->ux_ddr_vote);
}

static void platform_fpsgo_remove(struct platform_device *pdev)
{
	/* Drain outstanding votes before devres releases the path and its vote. */
	mutex_lock(&bw_lock);
	bw_path = NULL;
	peak_bw = 0;
	frame_vote = false;
	applied_vote = false;
	fbt_ux_drop_all_votes();
	hrtimer_cancel(&ux_dram_timer);
	mutex_unlock(&bw_lock);
	cancel_work_sync(&ux_dram_work);
}

static const struct of_device_id platform_fpsgo_of_match[] = {
	{ .compatible = "mediatek,fpsgo", },
	{},
};

static const struct platform_device_id platform_fpsgo_id_table[] = {
	{ "fpsgo", 0},
	{ },
};

static struct platform_driver mtk_platform_fpsgo_driver = {
	.probe = platform_fpsgo_probe,
	.remove	= platform_fpsgo_remove,
	.driver = {
		.name = "fpsgo",
		.owner = THIS_MODULE,
		.of_match_table = platform_fpsgo_of_match,
	},
	.id_table = platform_fpsgo_id_table,
};

void init_fbt_platform(void)
{
	struct device_node *node;
	u32 value;

	/* FBT snapshots these settings before a deferred ICC probe can run. */
	node = of_find_compatible_node(NULL, NULL, "mediatek,fpsgo");
	while (node && !of_device_is_available(node))
		node = of_find_compatible_node(node, NULL, "mediatek,fpsgo");
	if (node) {
		if (!of_property_read_u32(node, "gcc-enable", &value))
			plat_gcc_enable = value;
		if (!of_property_read_u32(node, "cpu-limit", &value))
			plat_cpu_limit = value;
		generate_cpu_mask(node);
		generate_sbe_rescue_enable(node);
		generate_ux_scroll_policy_type(node);
		of_node_put(node);
	}

	hrtimer_setup(&ux_dram_timer, fbt_ux_dram_timeout, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	platform_driver_register(&mtk_platform_fpsgo_driver);
}

void exit_fbt_platform(void)
{
	platform_driver_unregister(&mtk_platform_fpsgo_driver);
}

/* Callers serialize the regular frame vote and the UX deadline under bw_lock. */
static void fbt_update_dram_vote(void)
{
	bool boost = frame_vote || !list_empty(&ux_dram_votes);
	int ret;

	if (!bw_path || !peak_bw || boost == applied_vote)
		return;

	ret = icc_set_bw(bw_path, 0, boost ? peak_bw : 0);
	if (ret) {
		pr_warn_ratelimited("fpsgo: DRAM bandwidth vote %d failed: %d\n", boost, ret);
		fpsgo_systrace_c_fbt_debug(-100, 0, ret, "dram_boost_err");
	} else {
		applied_vote = boost;
		fpsgo_systrace_c_fbt_debug(-100, 0, boost, "dram_boost");
	}
}

void fbt_boost_dram(int boost)
{
	mutex_lock(&bw_lock);
	frame_vote = boost;
	fbt_update_dram_vote();
	mutex_unlock(&bw_lock);
}

static void fbt_ux_update_dram_votes(void)
{
	struct render_info *thr, *next;
	ktime_t now = ktime_get(), earliest = KTIME_MAX;

	list_for_each_entry_safe(thr, next, &ux_dram_votes, ux_ddr_vote) {
		ktime_t deadline = max(thr->ux_ddr_deadline, thr->sbe_ddr_deadline);

		if (!ktime_before(now, deadline))
			list_del_init(&thr->ux_ddr_vote);
		else if (ktime_before(deadline, earliest))
			earliest = deadline;
	}

	fbt_update_dram_vote();
	if (earliest == KTIME_MAX)
		hrtimer_cancel(&ux_dram_timer);
	else
		hrtimer_start(&ux_dram_timer, earliest, HRTIMER_MODE_ABS);
}

static void fbt_ux_dram_expire(struct work_struct *work)
{
	mutex_lock(&bw_lock);
	fbt_ux_update_dram_votes();
	mutex_unlock(&bw_lock);
}

void fbt_ux_vote_dram(struct render_info *thr, u64 duration_ns, bool sbe)
{
	ktime_t deadline = ktime_add_ns(ktime_get(), duration_ns);
	ktime_t *expires = sbe ? &thr->sbe_ddr_deadline : &thr->ux_ddr_deadline;

	mutex_lock(&bw_lock);
	if (bw_path && peak_bw) {
		if (list_empty(&thr->ux_ddr_vote)) {
			thr->ux_ddr_deadline = 0;
			thr->sbe_ddr_deadline = 0;
			list_add_tail(&thr->ux_ddr_vote, &ux_dram_votes);
		}
		*expires = max(*expires, deadline);
		fbt_ux_update_dram_votes();
	}
	mutex_unlock(&bw_lock);
}

void fbt_ux_clear_sbe_dram(struct render_info *thr)
{
	mutex_lock(&bw_lock);
	if (!list_empty(&thr->ux_ddr_vote) && thr->sbe_ddr_deadline) {
		thr->sbe_ddr_deadline = 0;
		fbt_ux_update_dram_votes();
	}
	mutex_unlock(&bw_lock);
}

void fbt_ux_clear_dram(struct render_info *thr)
{
	mutex_lock(&bw_lock);
	if (!thr) {
		fbt_ux_drop_all_votes();
		fbt_ux_update_dram_votes();
	} else if (!list_empty(&thr->ux_ddr_vote)) {
		list_del_init(&thr->ux_ddr_vote);
		fbt_ux_update_dram_votes();
	}
	mutex_unlock(&bw_lock);
}

void fbt_set_boost_value(unsigned int base_blc)
{
	base_blc = clamp(base_blc, 1U, 100U);
	fpsgo_sentcmd(FPSGO_SET_BOOST_TA, base_blc, -1);
	fpsgo_systrace_c_fbt_debug(-100, 0, base_blc, "TA_cap");
}

void fbt_clear_boost_value(void)
{
	fpsgo_sentcmd(FPSGO_SET_BOOST_TA, -1, -1);
	fpsgo_systrace_c_fbt_debug(-100, 0, 0, "TA_cap");

	fbt_notify_CM_limit(0);
	fbt_boost_dram(0);
}

void fbt_set_per_task_cap(int pid, unsigned int min_blc,
			unsigned int max_blc, unsigned int max_util)
{
	int ret = -1;
	unsigned int min_blc_1024;
	unsigned int max_blc_1024;
	struct task_struct *p;
	struct sched_attr attr = {};
	unsigned long cur_min = 0, cur_max = 0;

	if (!pid)
		return;

	max_blc_1024 = (max_util != 1024U) ? max_util : ((max_blc << 10) / 100U);
	max_blc_1024 = clamp(max_blc_1024, 1U, 1024U);

	min_blc_1024 = (min_blc << 10) / 100U;
	min_blc_1024 = clamp(min_blc_1024, 1U, max_blc_1024);

	attr.sched_policy = -1;
	attr.sched_flags =
		SCHED_FLAG_KEEP_ALL |
		SCHED_FLAG_UTIL_CLAMP |
		SCHED_FLAG_RESET_ON_FORK;

	if (min_blc == 0 && max_blc == 100) {
		attr.sched_util_min = -1;
		attr.sched_util_max = -1;
	} else {
		attr.sched_util_min = (min_blc_1024 << 10) / 1280;
		attr.sched_util_max = (max_blc_1024 << 10) / 1280;
	}

	if (pid < 0)
		goto out;

	rcu_read_lock();
	p = find_task_by_vpid(pid);

	if (likely(p))
		get_task_struct(p);

	rcu_read_unlock();

	if (likely(p)) {
		cur_min = uclamp_eff_value(p, UCLAMP_MIN);
		cur_max = uclamp_eff_value(p, UCLAMP_MAX);
		if (cur_min != attr.sched_util_min || cur_max != attr.sched_util_max) {
			attr.sched_policy = p->policy;
			if (rt_policy(p->policy))
				attr.sched_priority = p->rt_priority;
			ret = sched_setattr_nocheck(p, &attr);
		} else {
			ret = 0;
		}
		put_task_struct(p);
	}

out:
	if (ret != 0) {
		fpsgo_systrace_c_fbt(pid, 0, ret, "uclamp fail");
		fpsgo_systrace_c_fbt(pid, 0, 0, "uclamp fail");
		return;
	}

	fpsgo_systrace_c_fbt_debug(pid, 0, attr.sched_util_min, "min_cap");
	fpsgo_systrace_c_fbt_debug(pid, 0, attr.sched_util_max, "max_cap");
}

static int generate_cpu_mask(struct device_node *node)
{
	int i, ret, cpu;
	int temp_mask = 0;

	ret = of_property_read_u32_array(node, "fbt-cpu-mask",
			mask_int, FPSGO_PREFER_TOTAL);

	for (i = 0; i < FPSGO_PREFER_TOTAL; i++) {
		cpumask_clear(&mask[i]);
		temp_mask = mask_int[i];
		for_each_possible_cpu(cpu) {
			if (temp_mask & (1 << cpu))
				cpumask_set_cpu(cpu, &mask[i]);
		}
		FPSGO_LOGE("%s i:%d mask:%d %*pbl\n",
			__func__, i, mask_int[i], cpumask_pr_args(&mask[i]));
	}

	if (!ret)
		mask_done = 1;

	return ret;
}

struct cpumask fbt_generate_user_cpu_mask(int mask_int)
{
	unsigned long cpumask_ulval = mask_int;
	struct cpumask cpumask_setting;
	int cpu;

	cpumask_clear(&cpumask_setting);
	for_each_possible_cpu(cpu) {
		if (cpumask_ulval & (1 << cpu))
			cpumask_set_cpu(cpu, &cpumask_setting);
	}
	return cpumask_setting;
}

static int generate_sbe_rescue_enable(struct device_node *node)
{
	int ret = 0, retval = 0;

	ret = of_property_read_u32(node,
		 "sbe-resceue-enable", &retval);
	if (!ret)
		plat_sbe_rescue_enable = retval;
	else
		FPSGO_LOGE("%s unable to get plat_sbe_rescue_enable\n", __func__);

	return ret;
}

static int generate_ux_scroll_policy_type(struct device_node *node)
{
	int ret = 0, retval = 0;

	ret = of_property_read_u32(node,
		 "ux-scrol-policy-type", &retval);
	if (!ret)
		plat_ux_scroll_policy_type = retval;
	else
		FPSGO_LOGE("%s unable to get plat_ux_scroll_policy_type\n", __func__);

	return ret;
}

#if IS_REACHABLE(CONFIG_MTK_SCHEDULER)
int fbt_check_ls(int pid)
{
	struct task_struct *tsk;
	int ls = 0;

	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (!tsk)
		goto EXIT;
	get_task_struct(tsk);
	ls = is_task_latency_sensitive(tsk);
	put_task_struct(tsk);

EXIT:
	rcu_read_unlock();
	return ls;
}
#endif

#if IS_REACHABLE(CONFIG_MTK_SCHEDULER) && IS_ENABLED(CONFIG_MTK_SCHED_VIP_TASK)
int fbt_check_vip(int pid)
{
	struct task_struct *tsk;
	struct vip_task_struct *vts;
	int is_vip = 0;

	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (!tsk)
		goto EXIT;
	get_task_struct(tsk);
	vts = &((struct mtk_task *) tsk->android_vendor_data1)->vip_task;
	is_vip = vts->basic_vip;
	put_task_struct(tsk);

EXIT:
	rcu_read_unlock();
	return is_vip;
}

int fbt_check_vvip(int pid)
{
	struct task_struct *tsk;
	struct vip_task_struct *vts;
	int is_vvip = 0;

	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (!tsk)
		goto EXIT;
	get_task_struct(tsk);
	vts = &((struct mtk_task *) tsk->android_vendor_data1)->vip_task;
	is_vvip = vts->vvip;
	put_task_struct(tsk);

EXIT:
	rcu_read_unlock();
	return is_vvip;
}
#endif

int fbt_set_affinity(pid_t pid, unsigned int prefer_type)
{
	int ret = 0;

	if (!mask_done) {
		ret = -100;
		goto out;
	}

	ret = fpsgo_sched_setaffinity(pid, &mask[prefer_type]);

out:
	if (ret != 0) {
		fpsgo_systrace_c_fbt(pid, 0, ret, "setaffinity fail");
		fpsgo_systrace_c_fbt(pid, 0, 0, "setaffinity fail");
		return ret;
	}
	fpsgo_systrace_c_fbt(pid, 0, prefer_type, "set_affinity");
	return ret;
}

#if IS_REACHABLE(CONFIG_MTK_SCHEDULER)
int fbt_set_soft_affinity(int pid, int set, unsigned int prefer_type)
{
	int ret = 0;

	if (!mask_done) {
		ret = -100;
		goto out;
	}

	if (set)
		set_task_ls_prefer_cpus(pid, mask_int[prefer_type]);
	else
		unset_task_ls_prefer_cpus(pid);

out:
	if (ret != 0) {
		fpsgo_systrace_c_fbt(pid, 0, ret, "softaffinity fail");
		fpsgo_systrace_c_fbt(pid, 0, 0, "softaffinity fail");
		return ret;
	}
	fpsgo_systrace_c_fbt(pid, 0, prefer_type, "soft_affinity");
	return ret;
}
#endif

int fbt_get_L_min_ceiling(void)
{
	return 0;
}

int fbt_get_default_boost_ta(void)
{
	return 0;
}

int fbt_get_default_adj_loading(void)
{
	return 0;
}

int fbt_get_default_adj_count(void)
{
	return 10;
}

int fbt_get_default_adj_tdiff(void)
{
	return 1000000;
}

int fbt_get_cluster_limit(int *cluster, int *freq, int *r_freq, int *cpu)
{
/*
 * Use return value to specify limit on frequency or core
 * 1. when return value is FPSGO_LIMIT_NO_LIMIT -> no limit
 * 2. when return value is FPSGO_LIMIT_FREQ -> frequency limit
 * 2.1 when cluster is set and freq is set -> ceiling limit
 * 2.2 when cluster is set and r_freq is set -> rescue ceiling limit
 * 3. when return value is FPSGO_LIMIT_CPU -> cpu limit
 * 3.1 when cpu is set valid -> cpu isolation
 */
	int limit = plat_cpu_limit;

	if (limit == FPSGO_LIMIT_CPU)
		*cpu = 7;
	else if (limit == FPSGO_LIMIT_FREQ) {
		*cluster = 2;
		*freq = 2600000;
	}

	return limit;
}

int fbt_get_default_qr_enable(void)
{
	return 1;
}

int fbt_get_default_gcc_enable(void)
{
	return plat_gcc_enable;
}

int fbt_get_default_sbe_rescue_enable(void)
{
	return plat_sbe_rescue_enable;
}

int fbt_get_ux_scroll_policy_type(void)
{
	return plat_ux_scroll_policy_type;
}

int fbt_get_l_min_bhropp(void)
{
	return 0;
}

