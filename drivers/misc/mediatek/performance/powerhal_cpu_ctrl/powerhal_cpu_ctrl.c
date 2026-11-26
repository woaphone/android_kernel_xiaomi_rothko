// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 MediaTek Inc. */
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "powerhal_cpu_ctrl.h"

#define CREATE_TRACE_POINTS
#include "powerhal_trace_event.h"

#define ADPF_MAX_CALLBACK 64

struct session_slot {
	struct _SESSION state;
	struct _SESSION_WORK_DURATION durations[ADPF_MAX_THREAD];
};

static struct session_slot sessionList[ADPF_MAX_SESSION];
static adpfCallback adpfCallbackList[ADPF_MAX_CALLBACK];
static DEFINE_MUTEX(adpf_mutex);
static int adpf_enable = 1;

int adpf_register_callback(adpfCallback callback)
{
	int i;

	if (!callback)
		return -EINVAL;
	mutex_lock(&adpf_mutex);
	for (i = 0; i < ADPF_MAX_CALLBACK; i++) {
		if (!adpfCallbackList[i]) {
			adpfCallbackList[i] = callback;
			break;
		}
	}
	mutex_unlock(&adpf_mutex);
	return i == ADPF_MAX_CALLBACK ? -ENOSPC : i;
}
EXPORT_SYMBOL(adpf_register_callback);

int adpf_unregister_callback(int idx)
{
	int ret = idx;

	if (idx < 0 || idx >= ADPF_MAX_CALLBACK)
		return -EINVAL;
	mutex_lock(&adpf_mutex);
	if (!adpfCallbackList[idx])
		ret = -ENOENT;
	adpfCallbackList[idx] = NULL;
	mutex_unlock(&adpf_mutex);
	return ret;
}
EXPORT_SYMBOL(adpf_unregister_callback);

static void adpf_trace(unsigned int tgid, long val, const char *name)
{
	char buf[96];

	if (trace_powerhal_adpf_enabled()) {
		scnprintf(buf, sizeof(buf), "C|%u|%s|%ld\n", tgid, name, val);
		trace_powerhal_adpf(buf);
	}
}

/* Serialize the state change and callback: a concurrent close/create may reuse sid. */
static int adpf_session_cmd(const struct _ADPF_PACKAGE *msg)
{
	struct _SESSION_WORK_DURATION reported[ADPF_MAX_THREAD];
	struct _SESSION next;
	struct session_slot *slot;
	bool delivered = false;
	int i, ret = 0;

	if (msg->sid >= ARRAY_SIZE(sessionList))
		return -EINVAL;
	mutex_lock(&adpf_mutex);
	if (!adpf_enable) {
		ret = -EACCES;
		goto out;
	}
	slot = &sessionList[msg->sid];
	if (msg->cmd == ADPF_CREATE_HINT_SESSION) {
		if (slot->state.used) {
			ret = -EEXIST;
			goto out;
		}
		next = (struct _SESSION) { .sid = msg->sid, .tgid = msg->tgid,
			.uid = msg->uid, .used = SESSION_USED };
	} else {
		if (!slot->state.used) {
			ret = -ENOENT;
			goto out;
		}
		next = slot->state;
	}
	next.cmd = msg->cmd;
	switch (msg->cmd) {
	case ADPF_CREATE_HINT_SESSION:
		if (!msg->tgid || msg->tgid > INT_MAX || msg->durationNanos <= 0) {
			ret = -EINVAL;
			goto out;
		}
		next.durationNanos = msg->durationNanos;
		fallthrough;
	case ADPF_SET_THREADS:
		if (!msg->threadIds || msg->threadIds_size <= 0 ||
		    msg->threadIds_size > ADPF_MAX_THREAD) {
			ret = -EINVAL;
			goto out;
		}
		for (i = 0; i < msg->threadIds_size; i++) {
			if (msg->threadIds[i] <= 0) {
				ret = -EINVAL;
				goto out;
			}
		}
		next.threadIds_size = msg->threadIds_size;
		memcpy(next.threadIds, msg->threadIds, sizeof(*msg->threadIds) * next.threadIds_size);
		break;
	case ADPF_UPDATE_TARGET_WORK_DURATION:
		if (msg->targetDurationNanos <= 0) {
			ret = -EINVAL;
			goto out;
		}
		next.targetDurationNanos = msg->targetDurationNanos;
		break;
	case ADPF_REPORT_ACTUAL_WORK_DURATION:
		if (!msg->workDuration || msg->work_duration_size <= 0 ||
		    msg->work_duration_size > ADPF_MAX_THREAD) {
			ret = -EINVAL;
			goto out;
		}
		next.work_duration_size = msg->work_duration_size;
		for (i = 0; i < next.work_duration_size; i++) {
			if (msg->workDuration[i].durationNanos <= 0 ||
			    msg->workDuration[i].timeStampNanos < 0) {
				ret = -EINVAL;
				goto out;
			}
			reported[i].durationNanos = msg->workDuration[i].durationNanos;
			reported[i].timeStampNanos = msg->workDuration[i].timeStampNanos;
			next.workDuration[i] = &reported[i];
		}
		break;
	case ADPF_SENT_HINT:
		next.hint = msg->hint;
		break;
	case ADPF_CLOSE:
		next.used = SESSION_UNUSED;
		break;
	}

	for (i = 0; i < ADPF_MAX_CALLBACK; i++) {
		int err;

		if (!adpfCallbackList[i])
			continue;
		err = adpfCallbackList[i](&next);
		if (!err)
			delivered = true;
		else if (!ret)
			ret = err;
	}
	if (!delivered) {
		if (!ret)
			ret = -EAGAIN;
		goto out;
	}
	if (msg->cmd == ADPF_REPORT_ACTUAL_WORK_DURATION) {
		for (i = 0; i < next.work_duration_size; i++) {
			slot->durations[i] = reported[i];
			next.workDuration[i] = &slot->durations[i];
		}
		adpf_trace(next.tgid, reported[i - 1].timeStampNanos, "adpf_timeStamp");
		adpf_trace(next.tgid, reported[i - 1].durationNanos, "adpf_workDuration");
	} else {
		adpf_trace(next.tgid, next.cmd, "adpf_status");
		if (msg->cmd == ADPF_CREATE_HINT_SESSION || msg->cmd == ADPF_UPDATE_TARGET_WORK_DURATION)
			adpf_trace(next.tgid, next.durationNanos, "adpf_target");
		if (msg->cmd == ADPF_SENT_HINT)
			adpf_trace(next.tgid, next.hint, "adpf_hint");
	}
	slot->state = next;
out:
	mutex_unlock(&adpf_mutex);
	return ret;
}

static int adpf_create(unsigned int sid, unsigned int tgid, unsigned int uid,
		       int *threads, int n, long duration)
{
	return adpf_session_cmd(&(struct _ADPF_PACKAGE){ .cmd = ADPF_CREATE_HINT_SESSION,
		.sid = sid, .tgid = tgid, .uid = uid, .threadIds = threads,
		.threadIds_size = n, .durationNanos = duration });
}

static int adpf_target(unsigned int sid, long duration)
{
	return adpf_session_cmd(&(struct _ADPF_PACKAGE){ .cmd = ADPF_UPDATE_TARGET_WORK_DURATION,
		.sid = sid, .targetDurationNanos = duration });
}

static int adpf_report(unsigned int sid, struct _ADPF_WORK_DURATION *work, int n)
{
	return adpf_session_cmd(&(struct _ADPF_PACKAGE){ .cmd = ADPF_REPORT_ACTUAL_WORK_DURATION,
		.sid = sid, .workDuration = work, .work_duration_size = n });
}

static int adpf_set_threads(unsigned int sid, int *threads, int n)
{
	return adpf_session_cmd(&(struct _ADPF_PACKAGE){ .cmd = ADPF_SET_THREADS,
		.sid = sid, .threadIds = threads, .threadIds_size = n });
}

static int adpf_hint(unsigned int sid, int hint)
{
	return adpf_session_cmd(&(struct _ADPF_PACKAGE){ .cmd = ADPF_SENT_HINT,
		.sid = sid, .hint = hint });
}

static int adpf_pause(unsigned int sid)
{
	return adpf_session_cmd(&(struct _ADPF_PACKAGE){ .cmd = ADPF_PAUSE, .sid = sid });
}

static int adpf_resume(unsigned int sid)
{
	return adpf_session_cmd(&(struct _ADPF_PACKAGE){ .cmd = ADPF_RESUME, .sid = sid });
}

static int adpf_close(unsigned int sid)
{
	return adpf_session_cmd(&(struct _ADPF_PACKAGE){ .cmd = ADPF_CLOSE, .sid = sid });
}

typedef void (*fpsgo_notify_is_boost_cb)(int);
int (*register_get_fpsgo_is_boosting_fp)(fpsgo_notify_is_boost_cb);
EXPORT_SYMBOL_GPL(register_get_fpsgo_is_boosting_fp);
int (*unregister_get_fpsgo_is_boosting_fp)(fpsgo_notify_is_boost_cb);
EXPORT_SYMBOL_GPL(unregister_get_fpsgo_is_boosting_fp);

static DEFINE_MUTEX(hint_lock);
static int sf_hint_low_power_enabled;
static DEFINE_MUTEX(boost_lock);
static DECLARE_WAIT_QUEUE_HEAD(boost_queue);
struct k_list {
	struct list_head queue_list;
	int _cmd;
	int _value;
};
static LIST_HEAD(boost_events);
static bool lost_event;

static void notify_fpsgo_is_boost(int value)
{
	struct k_list *event = kmalloc(sizeof(*event), GFP_KERNEL_ACCOUNT);

	mutex_lock(&boost_lock);
	if (event) {
		event->_cmd = FPSGO_BOOST;
		event->_value = value;
		list_add_tail(&event->queue_list, &boost_events);
	} else {
		WRITE_ONCE(lost_event, true);
	}
	mutex_unlock(&boost_lock);
	wake_up_interruptible(&boost_queue);
}

static void boost_get_cmd(int *cmd, int *value)
{
	struct k_list *event;
	int ret;

	for (;;) {
		mutex_lock(&boost_lock);
		if (lost_event) {
			WRITE_ONCE(lost_event, false);
			*cmd = UNKNOWN;
			*value = -ENOMEM;
			mutex_unlock(&boost_lock);
			return;
		}
		event = list_first_entry_or_null(&boost_events, struct k_list, queue_list);
		if (event) {
			list_del(&event->queue_list);
			*cmd = event->_cmd;
			*value = event->_value;
			mutex_unlock(&boost_lock);
			kfree(event);
			return;
		}
		mutex_unlock(&boost_lock);
		ret = wait_event_interruptible(boost_queue,
				!list_empty(&boost_events) || READ_ONCE(lost_event));
		if (ret) {
			*cmd = UNKNOWN;
			*value = ret;
			return;
		}
	}
}

static ssize_t setting_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	int *setting = pde_data(file_inode(file));
	char text[16];
	int n = scnprintf(text, sizeof(text), "%d\n", READ_ONCE(*setting));

	return simple_read_from_buffer(buf, len, pos, text, n);
}

static ssize_t setting_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	int *setting = pde_data(file_inode(file));
	int value, ret;

	if (len >= 32)
		return -EINVAL;
	ret = kstrtoint_from_user(buf, len, 10, &value);
	if (ret)
		return ret;
	if (setting == &adpf_enable) {
		mutex_lock(&adpf_mutex);
		WRITE_ONCE(adpf_enable, value);
		mutex_unlock(&adpf_mutex);
		return len;
	}
	mutex_lock(&hint_lock);
	if (!!value != !!sf_hint_low_power_enabled) {
		if (value) {
			ret = register_get_fpsgo_is_boosting_fp ?
				register_get_fpsgo_is_boosting_fp(notify_fpsgo_is_boost) : -EAGAIN;
		} else {
			ret = unregister_get_fpsgo_is_boosting_fp ?
				unregister_get_fpsgo_is_boosting_fp(notify_fpsgo_is_boost) : -EAGAIN;
		}
		if (ret > 0)
			ret = value ? -ENOSPC : -ENOENT;
	}
	if (!ret)
		WRITE_ONCE(sf_hint_low_power_enabled, value);
	mutex_unlock(&hint_lock);
	return ret ? ret : (ssize_t)len;
}

static const struct proc_ops setting_ops = {
	.proc_read = setting_read,
	.proc_write = setting_write,
};

static int __init powerhal_cpu_ctrl_init(void)
{
	struct proc_dir_entry *dir = proc_mkdir("powerhal_cpu_ctrl", NULL);

	if (!dir)
		return -ENOMEM;
	if (!proc_create_data("sf_hint_low_power_enabled", 0644, dir, &setting_ops,
			      &sf_hint_low_power_enabled) ||
	    !proc_create_data("adpf_enable", 0644, dir, &setting_ops, &adpf_enable)) {
		proc_remove(dir);
		return -ENOMEM;
	}
	powerhal_adpf_create_session_hint_fp = adpf_create;
	powerhal_adpf_update_work_duration_fp = adpf_target;
	powerhal_adpf_report_actual_work_duration_fp = adpf_report;
	powerhal_adpf_pause_fp = adpf_pause;
	powerhal_adpf_resume_fp = adpf_resume;
	powerhal_adpf_close_fp = adpf_close;
	powerhal_adpf_sent_hint_fp = adpf_hint;
	powerhal_adpf_set_threads_fp = adpf_set_threads;
	boost_get_cmd_fp = boost_get_cmd;
	return 0;
}
/* FPSGO and the ioctl transport retain the installed callbacks for system lifetime. */
module_init(powerhal_cpu_ctrl_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek PowerHAL ADPF session manager");
