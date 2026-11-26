// SPDX-License-Identifier: GPL-2.0
/* MT6989 camera HAL transport and fixed per-thread uclamp policy. */
#include <linux/build_bug.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <trace/hooks/sched.h>
#include <uapi/linux/sched/types.h>

#include "c2ps_abi.h"

static_assert(C2PS_ACTIVATE == 0x4010671b);
static_assert(C2PS_ADD_TASK == 0x4024671c);
static_assert(C2PS_TASK_START == 0x4010671d);
static_assert(C2PS_TASK_END == 0x4010671e);
static_assert(C2PS_TASK_CHANGE == 0x4010671f);
static_assert(C2PS_DESTROY == 0x40046720);
static_assert(C2PS_NOTIFY_VSYNC == 0x40086721);
static_assert(C2PS_NOTIFY_CAMFPS == 0x40086722);
static_assert(C2PS_TASK_SINGLE_SHOT == 0x408c6723);

#define C2PS_MAX_TASKS	4096
#define C2PS_MAX_THREADS	256
#define C2PS_MAX_VOTES	32

struct c2ps_session {
	struct pid *tgid;
	struct mm_struct *mm;
	struct xarray tasks;
	struct list_head threads;
	unsigned int nr_tasks;
	unsigned int nr_threads;
	bool active;
};

struct c2ps_thread {
	struct list_head link;
	struct hlist_node hash;
	struct c2ps_session *session;
	struct task_struct *task;
	struct uclamp_se saved[UCLAMP_CNT];
	unsigned int applied;
	unsigned int nr_votes;
	u32 votes[C2PS_MAX_VOTES];
};

/* The mutex serializes ioctl/release; the fork hook only takes the hash lock. */
static DEFINE_MUTEX(c2ps_lock);
static DEFINE_RAW_SPINLOCK(c2ps_threads_lock);
static DEFINE_HASHTABLE(c2ps_threads, 6);

static struct c2ps_thread *c2ps_find_thread(struct task_struct *task)
{
	struct c2ps_thread *thread;

	hash_for_each_possible(c2ps_threads, thread, hash, (unsigned long)task)
		if (thread->task == task)
			return thread;
	return NULL;
}

static bool c2ps_clamps_match(const struct uclamp_se *req, unsigned int value)
{
	return req[UCLAMP_MIN].user_defined && req[UCLAMP_MAX].user_defined &&
	       req[UCLAMP_MIN].value == value && req[UCLAMP_MAX].value == value;
}

static void c2ps_fork(void *unused, struct task_struct *child,
		      struct task_struct *parent)
{
	struct c2ps_thread *thread;
	const struct uclamp_se *inherit;
	unsigned long flags;

	/* C2PS only installs equal, nonzero, explicitly requested bounds. */
	if (!child->uclamp_req[UCLAMP_MIN].value ||
	    !c2ps_clamps_match(child->uclamp_req, child->uclamp_req[UCLAMP_MIN].value))
		return;

	raw_spin_lock_irqsave(&c2ps_threads_lock, flags);
	raw_spin_lock(&parent->pi_lock);
	thread = c2ps_find_thread(parent);
	/* Refresh the fork snapshot if close restored the parent during fork. */
	inherit = parent->uclamp_req;
	if (thread && c2ps_clamps_match(inherit, thread->applied))
		inherit = thread->saved;
	child->uclamp_req[UCLAMP_MIN] = inherit[UCLAMP_MIN];
	child->uclamp_req[UCLAMP_MAX] = inherit[UCLAMP_MAX];
	raw_spin_unlock(&parent->pi_lock);
	raw_spin_unlock_irqrestore(&c2ps_threads_lock, flags);
}

static void c2ps_drop_thread(struct c2ps_thread *thread)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&c2ps_threads_lock, flags);
	hash_del(&thread->hash);
	raw_spin_unlock_irqrestore(&c2ps_threads_lock, flags);
	list_del(&thread->link);
	thread->session->nr_threads--;
	put_task_struct(thread->task);
	kfree(thread);
}

static u32 c2ps_original_clamp(const struct c2ps_thread *thread,
			      enum uclamp_id id)
{
	return thread->saved[id].user_defined ? thread->saved[id].value : -1U;
}

/* sched_setattr() takes the rq lock and updates effective runqueue clamps. */
static int c2ps_set_clamp(struct c2ps_thread *thread, unsigned int value,
			  bool closing)
{
	struct task_struct *task = thread->task;
	struct sched_attr attr = {
		.size = sizeof(attr),
		/* The internal API takes -1 for a parameter-only change. */
		.sched_policy = -1,
		.sched_flags = SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP,
	};
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&task->pi_lock, flags);
	if (!value && (READ_ONCE(task->flags) & PF_EXITING)) {
		raw_spin_unlock_irqrestore(&task->pi_lock, flags);
		return 0;
	}
	if (thread->applied &&
	    !c2ps_clamps_match(task->uclamp_req, thread->applied)) {
		raw_spin_unlock_irqrestore(&task->pi_lock, flags);
		/* The scheduler request now belongs to another writer. */
		return value ? -EBUSY : 0;
	}
	attr.sched_nice = task_nice(task);
	attr.sched_priority = task->rt_priority;
	if (task->policy == SCHED_DEADLINE) {
		if (value) {
			raw_spin_unlock_irqrestore(&task->pi_lock, flags);
			return -EOPNOTSUPP;
		}
		attr.sched_runtime = task->dl.dl_runtime;
		attr.sched_deadline = task->dl.dl_deadline;
		attr.sched_period = task->dl.dl_period;
		attr.sched_flags |= task->dl.flags;
	}
	raw_spin_unlock_irqrestore(&task->pi_lock, flags);

	attr.sched_util_min = value ?: c2ps_original_clamp(thread, UCLAMP_MIN);
	attr.sched_util_max = value ?: c2ps_original_clamp(thread, UCLAMP_MAX);
	/* A final close can run with unrelated credentials (e.g. delayed fput). */
	if (closing)
		return sched_setattr_nocheck(task, &attr);

	ret = sched_setattr(task, &attr);
	if (!ret && value) {
		raw_spin_lock_irqsave(&c2ps_threads_lock, flags);
		thread->applied = value;
		raw_spin_unlock_irqrestore(&c2ps_threads_lock, flags);
	}
	return ret;
}

static unsigned int c2ps_vote_value(struct c2ps_session *session, u32 id)
{
	return xa_to_value(xa_load(&session->tasks, id));
}

static int c2ps_start(struct c2ps_session *session, u32 id)
{
	struct c2ps_thread *thread, *tmp;
	unsigned int value, i;
	unsigned long flags;
	bool fresh;
	int ret;

	value = c2ps_vote_value(session, id);
	if (!value)
		return -ENOENT;

	thread = c2ps_find_thread(current);
	fresh = !thread;
	if (fresh) {
		list_for_each_entry_safe(thread, tmp, &session->threads, link)
			if (READ_ONCE(thread->task->flags) & PF_EXITING)
				c2ps_drop_thread(thread);
		if (session->nr_threads == C2PS_MAX_THREADS)
			return -ENOSPC;
		thread = kzalloc(sizeof(*thread), GFP_KERNEL);
		if (!thread)
			return -ENOMEM;

		thread->task = current;
		thread->session = session;
		get_task_struct(current);
		raw_spin_lock_irqsave(&current->pi_lock, flags);
		memcpy(thread->saved, current->uclamp_req, sizeof(thread->saved));
		raw_spin_unlock_irqrestore(&current->pi_lock, flags);

		raw_spin_lock_irqsave(&c2ps_threads_lock, flags);
		hash_add(c2ps_threads, &thread->hash, (unsigned long)current);
		raw_spin_unlock_irqrestore(&c2ps_threads_lock, flags);
		list_add(&thread->link, &session->threads);
		session->nr_threads++;
	} else {
		if (thread->session != session)
			return -EBUSY;
		for (i = 0; i < thread->nr_votes; i++)
			if (thread->votes[i] == id)
				return -EALREADY;
		if (thread->nr_votes == C2PS_MAX_VOTES)
			return -ENOSPC;
	}

	ret = c2ps_set_clamp(thread, max(value, thread->applied), false);
	if (ret) {
		if (fresh)
			c2ps_drop_thread(thread);
		return ret;
	}
	thread->votes[thread->nr_votes++] = id;
	return 0;
}

static int c2ps_end(struct c2ps_session *session, u32 id)
{
	struct c2ps_thread *thread = c2ps_find_thread(current);
	unsigned int value = 0, index = C2PS_MAX_VOTES, i;
	int ret;

	if (!thread || thread->session != session)
		return -ENOENT;
	for (i = 0; i < thread->nr_votes; i++) {
		if (thread->votes[i] == id)
			index = i;
		else
			value = max(value, c2ps_vote_value(session, thread->votes[i]));
	}
	if (index == C2PS_MAX_VOTES)
		return -ENOENT;

	ret = c2ps_set_clamp(thread, value, false);
	if (ret)
		return ret;
	thread->votes[index] = thread->votes[--thread->nr_votes];
	if (!thread->nr_votes)
		c2ps_drop_thread(thread);
	return 0;
}

static int c2ps_teardown(struct c2ps_session *session, bool closing)
{
	struct c2ps_thread *thread, *tmp;
	int ret = 0;

	list_for_each_entry_safe(thread, tmp, &session->threads, link) {
		int err = c2ps_set_clamp(thread, 0, closing);

		if (err) {
			if (!ret)
				ret = err;
			if (!closing)
				continue;
			pr_warn_ratelimited("c2ps: clamp restore on close failed: %d\n",
					    err);
		}
		c2ps_drop_thread(thread);
	}
	if (!ret || closing) {
		xa_destroy(&session->tasks);
		session->nr_tasks = 0;
		session->active = false;
	}
	return ret;
}

static int c2ps_add(struct c2ps_session *session, const struct c2ps_add_task *p)
{
	unsigned int old, value;
	int ret;

	if (p->default_uclamp > SCHED_CAPACITY_SCALE)
		return -EINVAL;

	/* The vendor fixed policy clamps zero to one and sets both bounds. */
	value = max(p->default_uclamp, 1U);
	old = c2ps_vote_value(session, p->task_id);
	if (old)
		return old == value ? 0 : -EEXIST;
	if (session->nr_tasks == C2PS_MAX_TASKS)
		return -ENOSPC;
	ret = xa_insert(&session->tasks, p->task_id, xa_mk_value(value), GFP_KERNEL);
	if (!ret)
		session->nr_tasks++;
	return ret;
}

static long c2ps_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct c2ps_session *session = file->private_data;
	void __user *argp = (void __user *)arg;
	union {
		struct c2ps_activate activate;
		struct c2ps_add_task add;
		struct c2ps_task_event event;
		struct c2ps_notify notify;
		struct c2ps_single_shot shot;
		__u32 destroy;
	} data;
	int ret;

	if (task_tgid(current) != session->tgid || current->mm != session->mm ||
	    !capable(CAP_SYS_NICE))
		return -EPERM;
	/* The HAL opens O_RDONLY. Recheck the node's read access for ioctl. */
	ret = file_permission(file, MAY_READ);
	if (ret)
		return ret;

	switch (cmd) {
	case C2PS_ACTIVATE:
	case C2PS_ADD_TASK:
	case C2PS_TASK_START:
	case C2PS_TASK_END:
	case C2PS_DESTROY:
	case C2PS_TASK_CHANGE:
	case C2PS_NOTIFY_VSYNC:
	case C2PS_NOTIFY_CAMFPS:
	case C2PS_TASK_SINGLE_SHOT:
		break;
	default:
		return -ENOTTY;
	}
	if (copy_from_user(&data, argp, _IOC_SIZE(cmd)))
		return -EFAULT;

	switch (cmd) {
	case C2PS_TASK_CHANGE: /* No adaptive history to reset. */
	case C2PS_NOTIFY_VSYNC:
	case C2PS_NOTIFY_CAMFPS: /* Inputs to the vendor background regulator. */
	case C2PS_TASK_SINGLE_SHOT: /* Vendor gear limits and timed boosts. */
		return -EOPNOTSUPP;
	case C2PS_TASK_START:
	case C2PS_TASK_END:
		/* The shipping library encodes gettid() for both operations. */
		if (data.event.tid != task_pid_vnr(current))
			return -EPERM;
		break;
	}

	mutex_lock(&c2ps_lock);
	if (cmd != C2PS_ACTIVATE && !session->active) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case C2PS_ACTIVATE:
		if (!data.activate.camfps && !data.activate.max_uclamp[0] &&
		    !data.activate.max_uclamp[1] && !data.activate.max_uclamp[2]) {
			/* Stock reserves the all-zero request for heavy-load EAS. */
			ret = -EOPNOTSUPP;
			break;
		}
		/* Activate the fixed per-task policy only; no gear state is set. */
		session->active = true;
		ret = 0;
		break;
	case C2PS_ADD_TASK:
		/* Group, VIP and timing fields are outside the fixed policy. */
		ret = c2ps_add(session, &data.add);
		break;
	case C2PS_TASK_START:
		ret = c2ps_start(session, data.event.task_id);
		break;
	case C2PS_TASK_END:
		ret = c2ps_end(session, data.event.task_id);
		break;
	case C2PS_DESTROY:
		ret = c2ps_teardown(session, false);
		break;
	}

out:
	mutex_unlock(&c2ps_lock);
	return ret;
}

#ifdef CONFIG_COMPAT
static long c2ps_compat_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	return c2ps_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static int c2ps_open(struct inode *inode, struct file *file)
{
	struct c2ps_session *session;

	if (!capable(CAP_SYS_NICE) || !current->mm)
		return -EPERM;
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		return -ENOMEM;
	session->tgid = get_task_pid(current, PIDTYPE_TGID);
	session->mm = current->mm;
	mmgrab(session->mm);
	xa_init(&session->tasks);
	INIT_LIST_HEAD(&session->threads);
	file->private_data = session;
	return 0;
}

static int c2ps_release(struct inode *inode, struct file *file)
{
	struct c2ps_session *session = file->private_data;

	mutex_lock(&c2ps_lock);
	c2ps_teardown(session, true);
	mutex_unlock(&c2ps_lock);
	mmdrop(session->mm);
	put_pid(session->tgid);
	kfree(session);
	return 0;
}

static const struct proc_ops c2ps_proc_ops = {
	.proc_open = c2ps_open,
	.proc_ioctl = c2ps_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = c2ps_compat_ioctl,
#endif
	.proc_release = c2ps_release,
};

static int __init c2ps_init(void)
{
	struct proc_dir_entry *root;
	int ret;

	ret = register_trace_android_vh_dup_task_struct(c2ps_fork, NULL);
	if (ret)
		return ret;

	root = proc_mkdir("c2ps", NULL);
	if (!root)
		goto unregister;
	/* Stock init sets system:camera ownership on this 0660 node. */
	if (!proc_create("c2ps_ioctl", 0660, root, &c2ps_proc_ops)) {
		proc_remove(root);
		goto unregister;
	}
	return 0;

unregister:
	unregister_trace_android_vh_dup_task_struct(c2ps_fork, NULL);
	tracepoint_synchronize_unregister();
	return -ENOMEM;
}
device_initcall(c2ps_init);
