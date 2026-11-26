/* SPDX-License-Identifier: GPL-2.0 */
/* MT6989 camera HAL ioctl layout, based on the MediaTek C2PS transport. */
#ifndef _MTK_C2PS_ABI_H
#define _MTK_C2PS_ABI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct c2ps_activate {
	__u32 camfps;
	__u32 max_uclamp[3];
};

struct c2ps_add_task {
	__u32 task_id;
	__u32 task_group_head;
	__u32 default_uclamp;
	__u32 task_target_time;
	__u32 task_group_target_time;
	__u32 perf_prefer_ratio;
	__u8 is_vip_task;
	__u8 is_dynamic_tid;
	char task_name[10];
};

struct c2ps_task_event {
	__u32 tid;
	__u32 task_id;
	__u32 serial_no;
	__u32 mode_change_hint;
};

struct c2ps_notify {
	__u32 serial_no;
	__u32 cur_camfps;
};

/* The shipping ABI uses eight entries, independently of CONFIG_NR_CPUS. */
#define C2PS_ABI_CPUS 8
struct c2ps_single_shot {
	__s32 uclamp_max[C2PS_ABI_CPUS];
	__s32 idle_rate_alert;
	__s32 timeout;
	__s32 uclamp_max_placeholder[3][C2PS_ABI_CPUS];
	__u8 reset_param;
	__u8 padding[3];
};

#define C2PS_ACTIVATE		_IOW('g', 27, struct c2ps_activate)
#define C2PS_ADD_TASK		_IOW('g', 28, struct c2ps_add_task)
#define C2PS_TASK_START		_IOW('g', 29, struct c2ps_task_event)
#define C2PS_TASK_END		_IOW('g', 30, struct c2ps_task_event)
#define C2PS_TASK_CHANGE	_IOW('g', 31, struct c2ps_task_event)
#define C2PS_DESTROY		_IOW('g', 32, __u32)
#define C2PS_NOTIFY_VSYNC	_IOW('g', 33, struct c2ps_notify)
#define C2PS_NOTIFY_CAMFPS	_IOW('g', 34, struct c2ps_notify)
#define C2PS_TASK_SINGLE_SHOT	_IOW('g', 35, struct c2ps_single_shot)

#endif /* _MTK_C2PS_ABI_H */
