// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 MediaTek Inc. */
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "../powerhal_cpu_ctrl/powerhal_cpu_ctrl.h"

int (*powerhal_adpf_create_session_hint_fp)(unsigned int, unsigned int, unsigned int,
					  int *, int, long);
EXPORT_SYMBOL_GPL(powerhal_adpf_create_session_hint_fp);
int (*powerhal_adpf_get_hint_session_preferred_rate_fp)(long long *);
EXPORT_SYMBOL_GPL(powerhal_adpf_get_hint_session_preferred_rate_fp);
int (*powerhal_adpf_update_work_duration_fp)(unsigned int, long);
EXPORT_SYMBOL_GPL(powerhal_adpf_update_work_duration_fp);
int (*powerhal_adpf_report_actual_work_duration_fp)(unsigned int,
					struct _ADPF_WORK_DURATION *, int);
EXPORT_SYMBOL_GPL(powerhal_adpf_report_actual_work_duration_fp);
int (*powerhal_adpf_pause_fp)(unsigned int);
EXPORT_SYMBOL_GPL(powerhal_adpf_pause_fp);
int (*powerhal_adpf_resume_fp)(unsigned int);
EXPORT_SYMBOL_GPL(powerhal_adpf_resume_fp);
int (*powerhal_adpf_close_fp)(unsigned int);
EXPORT_SYMBOL_GPL(powerhal_adpf_close_fp);
int (*powerhal_adpf_sent_hint_fp)(unsigned int, int);
EXPORT_SYMBOL_GPL(powerhal_adpf_sent_hint_fp);
int (*powerhal_adpf_set_threads_fp)(unsigned int, int *, int);
EXPORT_SYMBOL_GPL(powerhal_adpf_set_threads_fp);
void (*boost_get_cmd_fp)(int *, int *);
EXPORT_SYMBOL_GPL(boost_get_cmd_fp);

static struct proc_dir_entry *powerhal_dir;

static long adpf_device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct _ADPF_WORK_DURATION durations[ADPF_MAX_THREAD];
	int threads[ADPF_MAX_THREAD];
	struct _ADPF_PACKAGE msg;
	void __user *up = (void __user *)arg;
	int ret;

	if (cmd != POWERHAL_SET_ADPF_DATA && cmd != POWERHAL_GET_ADPF_DATA)
		return -ENOTTY;
	if (copy_from_user(&msg, up, sizeof(msg)))
		return -EFAULT;
	if (cmd == POWERHAL_GET_ADPF_DATA) {
		if (msg.cmd != ADPF_GET_HINT_SESSION_PREFERED_RATE)
			return -EINVAL;
		/* The donor implements the preferred rate in the native HAL. */
		if (!powerhal_adpf_get_hint_session_preferred_rate_fp)
			return -EOPNOTSUPP;
		ret = powerhal_adpf_get_hint_session_preferred_rate_fp(&msg.preferredRate);
		if (ret)
			return ret;
		return copy_to_user(up, &msg, sizeof(msg)) ? -EFAULT : 0;
	}

	if (msg.sid >= ADPF_MAX_SESSION)
		return -EINVAL;
	switch (msg.cmd) {
	case ADPF_CREATE_HINT_SESSION:
	case ADPF_SET_THREADS:
		if (msg.threadIds_size <= 0 || msg.threadIds_size > ARRAY_SIZE(threads))
			return -EINVAL;
		if (copy_from_user(threads, msg.threadIds, sizeof(*threads) * msg.threadIds_size))
			return -EFAULT;
		if (msg.cmd == ADPF_SET_THREADS)
			return powerhal_adpf_set_threads_fp ?
				powerhal_adpf_set_threads_fp(msg.sid, threads, msg.threadIds_size) : -EAGAIN;
		return powerhal_adpf_create_session_hint_fp ?
			powerhal_adpf_create_session_hint_fp(msg.sid, msg.tgid, msg.uid,
				threads, msg.threadIds_size, msg.durationNanos) : -EAGAIN;
	case ADPF_REPORT_ACTUAL_WORK_DURATION:
		if (msg.work_duration_size <= 0 || msg.work_duration_size > ARRAY_SIZE(durations))
			return -EINVAL;
		if (copy_from_user(durations, msg.workDuration,
				   sizeof(*durations) * msg.work_duration_size))
			return -EFAULT;
		return powerhal_adpf_report_actual_work_duration_fp ?
			powerhal_adpf_report_actual_work_duration_fp(msg.sid, durations,
				msg.work_duration_size) : -EAGAIN;
	case ADPF_UPDATE_TARGET_WORK_DURATION:
		return powerhal_adpf_update_work_duration_fp ?
			powerhal_adpf_update_work_duration_fp(msg.sid, msg.targetDurationNanos) : -EAGAIN;
	case ADPF_PAUSE:
		return powerhal_adpf_pause_fp ? powerhal_adpf_pause_fp(msg.sid) : -EAGAIN;
	case ADPF_RESUME:
		return powerhal_adpf_resume_fp ? powerhal_adpf_resume_fp(msg.sid) : -EAGAIN;
	case ADPF_CLOSE:
		return powerhal_adpf_close_fp ? powerhal_adpf_close_fp(msg.sid) : -EAGAIN;
	case ADPF_SENT_HINT:
		return powerhal_adpf_sent_hint_fp ? powerhal_adpf_sent_hint_fp(msg.sid, msg.hint) : -EAGAIN;
	default:
		return -EINVAL;
	}
}

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct _CPU_CTRL_PACKAGE msg = { .cmd = UNKNOWN };

	if (cmd != NOTIFY_BOOST)
		return -ENOTTY;
	if (!boost_get_cmd_fp)
		return -EAGAIN;
	boost_get_cmd_fp(&msg.cmd, &msg.value);
	if (msg.cmd == UNKNOWN)
		return msg.value;
	return copy_to_user((void __user *)arg, &msg, sizeof(msg)) ? -EFAULT : 0;
}

static int device_show(struct seq_file *m, void *v)
{
	return 0;
}
static int device_open(struct inode *inode, struct file *file)
{
	return single_open(file, device_show, NULL);
}

static const struct proc_ops adpf_fops = {
	.proc_open = device_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_ioctl = adpf_device_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = adpf_device_ioctl,
#endif
};

static const struct proc_ops boost_fops = {
	.proc_open = device_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_ioctl = device_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = device_ioctl,
#endif
};

static int __init init_powerhal_perfctl(void)
{
	powerhal_dir = proc_mkdir("perfmgr_powerhal", NULL);
	if (!powerhal_dir)
		return -ENOMEM;
	if (!proc_create("ioctl_powerhal_adpf", 0440, powerhal_dir, &adpf_fops) ||
	    !proc_create("ioctl_powerhal", 0440, powerhal_dir, &boost_fops)) {
		proc_remove(powerhal_dir);
		return -ENOMEM;
	}
	return 0;
}
module_init(init_powerhal_perfctl);

static void __exit exit_powerhal_perfctl(void)
{
	proc_remove(powerhal_dir);
}
module_exit(exit_powerhal_perfctl);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek POWERHAL perf_ioctl");
