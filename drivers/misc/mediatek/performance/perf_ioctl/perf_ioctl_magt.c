// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 MediaTek Inc. */
#include <linux/cpufreq.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/uaccess.h>

#include "perf_ioctl_magt.h"

static int thermal_aware_threshold = -1;
static int fpsdrop_aware_threshold = -1;
static int advice_bat_avg_current = -1;
static int advice_bat_max_current = -1;
static int targetfps_throttling_temp = -1;
static int thermal_aware_light_threshold = -1;
module_param(thermal_aware_threshold, int, 0644);
module_param(fpsdrop_aware_threshold, int, 0644);
module_param(advice_bat_avg_current, int, 0644);
module_param(advice_bat_max_current, int, 0644);
module_param(targetfps_throttling_temp, int, 0644);
module_param(thermal_aware_light_threshold, int, 0644);

static DEFINE_MUTEX(cpu_lock);
static struct cpu_time prev_idle[max_cpus], prev_wall[max_cpus];
static struct cpu_info ci;
static struct proc_dir_entry *magt_dir;

static void sample_loading(struct cpu_info *info)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		u64 wall, idle, dw, di;

		if (cpu >= ARRAY_SIZE(info->cpu_loading))
			break;
		idle = get_cpu_idle_time(cpu, &wall, 1);
		dw = wall - prev_wall[cpu].time;
		di = idle - prev_idle[cpu].time;
		prev_wall[cpu].time = wall;
		prev_idle[cpu].time = idle;
		info->cpu_loading[cpu] = cpu_active(cpu) && dw > di ?
			div64_u64(100 * (dw - di), dw) : 0;
	}
}

int get_cpu_loading(struct cpu_info *info)
{
	mutex_lock(&cpu_lock);
	memset(info->cpu_loading, 0, sizeof(info->cpu_loading));
	sample_loading(info);
	mutex_unlock(&cpu_lock);
	return 0;
}
EXPORT_SYMBOL(get_cpu_loading);

int get_perf_index(struct cpu_info *info)
{
	unsigned long seen = 0;
	int cpu;

	memset(info->perf_index, 0, sizeof(info->perf_index));
	for_each_possible_cpu(cpu) {
		int cluster = topology_cluster_id(cpu);
		unsigned long capacity;

		if (cluster < 0)
			return -ENODATA;
		if (cluster >= ARRAY_SIZE(info->perf_index))
			return -EOVERFLOW;
		if (test_and_set_bit(cluster, &seen))
			continue;
		capacity = arch_scale_cpu_capacity(cpu) * arch_scale_freq_capacity(cpu);
		capacity >>= SCHED_CAPACITY_SHIFT;
		info->perf_index[cluster] = (100 * capacity) >> SCHED_CAPACITY_SHIFT;
	}
	return 0;
}
EXPORT_SYMBOL(get_perf_index);

int (*magt2fpsgo_notify_dep_list_fp)(int, int *, int);
EXPORT_SYMBOL(magt2fpsgo_notify_dep_list_fp);
int (*magt2fpsgo_notify_target_fps_fp)(int *, int *, int *, int);
EXPORT_SYMBOL(magt2fpsgo_notify_target_fps_fp);

static long magt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct target_fps_info fps;
	struct dep_list_info deps;
	struct cpu_info snapshot;
	void __user *up = (void __user *)arg;
	int ret = 0;

	switch (cmd) {
	case MAGT_GET_CPU_LOADING:
	case MAGT_GET_PERF_INDEX:
		mutex_lock(&cpu_lock);
		if (cmd == MAGT_GET_CPU_LOADING)
			sample_loading(&ci);
		else
			ret = get_perf_index(&ci);
		snapshot = ci;
		mutex_unlock(&cpu_lock);
		if (ret)
			return ret;
		return copy_to_user(up, &snapshot, sizeof(snapshot)) ? -EFAULT : 0;
	case MAGT_SET_TARGET_FPS:
		if (copy_from_user(&fps, up, sizeof(fps)))
			return -EFAULT;
		if (fps.num > MAX_MAGT_TARGET_FPS_NUM)
			return -EINVAL;
		if (!magt2fpsgo_notify_target_fps_fp)
			return -EAGAIN;
		return magt2fpsgo_notify_target_fps_fp(fps.pid_arr, fps.tid_arr, fps.tfps_arr, fps.num);
	case MAGT_SET_DEP_LIST:
		if (copy_from_user(&deps, up, sizeof(deps)))
			return -EFAULT;
		if (deps.user_dep_num > MAGT_DEP_LIST_NUM)
			return -EINVAL;
		if (!magt2fpsgo_notify_dep_list_fp)
			return -EAGAIN;
		return magt2fpsgo_notify_dep_list_fp(deps.pid, deps.user_dep_arr, deps.user_dep_num);
	default:
		return -ENOTTY;
	}
}

static int device_show(struct seq_file *m, void *v)
{
	return 0;
}
static int device_open(struct inode *inode, struct file *file)
{
	return single_open(file, device_show, NULL);
}

static const struct proc_ops magt_ops = {
	.proc_open = device_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_ioctl = magt_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = magt_ioctl,
#endif
};

static int __init init_magt_perfctl(void)
{
	magt_dir = proc_mkdir("perfmgr_magt", NULL);
	if (!magt_dir)
		return -ENOMEM;
	if (!proc_create("magt_ioctl", 0660, magt_dir, &magt_ops)) {
		proc_remove(magt_dir);
		return -ENOMEM;
	}
	return 0;
}
module_init(init_magt_perfctl);

static void __exit exit_magt_perfctl(void)
{
	proc_remove(magt_dir);
}
module_exit(exit_magt_perfctl);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MAGT ioctl");
