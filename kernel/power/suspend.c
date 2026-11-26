// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/power/suspend.c - Suspend to RAM and standby functionality.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 * Copyright (c) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 */

#define pr_fmt(fmt) "PM: " fmt

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/suspend.h>
#include <linux/rothko_pstore.h>
#include <linux/syscore_ops.h>
#include <linux/swait.h>
#include <linux/ftrace.h>
#include <trace/events/power.h>
#include <linux/compiler.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/wakeup_reason.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/stacktrace.h>
#include <linux/workqueue.h>
#include <trace/hooks/suspend.h>

#include "power.h"

const char * const pm_labels[] = {
	[PM_SUSPEND_TO_IDLE] = "freeze",
	[PM_SUSPEND_STANDBY] = "standby",
	[PM_SUSPEND_MEM] = "mem",
};
const char *pm_states[PM_SUSPEND_MAX];
static const char * const mem_sleep_labels[] = {
	[PM_SUSPEND_TO_IDLE] = "s2idle",
	[PM_SUSPEND_STANDBY] = "shallow",
	[PM_SUSPEND_MEM] = "deep",
};
const char *mem_sleep_states[PM_SUSPEND_MAX];

suspend_state_t mem_sleep_current = PM_SUSPEND_TO_IDLE;
suspend_state_t mem_sleep_default = PM_SUSPEND_MAX;
suspend_state_t pm_suspend_target_state;
EXPORT_SYMBOL_GPL(pm_suspend_target_state);

unsigned int pm_suspend_global_flags;
EXPORT_SYMBOL_GPL(pm_suspend_global_flags);

static const struct platform_suspend_ops *suspend_ops;
static const struct platform_s2idle_ops *s2idle_ops;
static DECLARE_SWAIT_QUEUE_HEAD(s2idle_wait_head);

enum s2idle_states __read_mostly s2idle_state;
static DEFINE_RAW_SPINLOCK(s2idle_lock);

static unsigned int s2idle_attempt;
static bool s2idle_recording;
static unsigned int s2idle_record_round; /* protected by s2idle_lock */

#if defined(CONFIG_DRM_ROTHKO_N12) && defined(CONFIG_PSTORE_CONSOLE) && \
    defined(CONFIG_PSTORE_RAM)
unsigned int rothko_s2idle_attempt(void)
{
	return READ_ONCE(s2idle_recording) ? READ_ONCE(s2idle_attempt) : 0;
}
#endif

static void s2idle_record(const char *stage, int status)
{
	if (s2idle_recording)
		rothko_pstore_suspend_stage(s2idle_attempt, stage, status);
}

#if defined(CONFIG_DRM_ROTHKO_N12) && defined(CONFIG_PM_SLEEP_DEBUG)
/* The owner cancels the sample before dropping system_transition_mutex. */
static struct task_struct *s2idle_resume_owner;
static const char *s2idle_resume_origin;
static const char *s2idle_resume_stage;
static bool s2idle_resume_sampled;

static void s2idle_persist_task(struct task_struct *task, unsigned int attempt)
{
#ifdef CONFIG_STACKTRACE
	unsigned long entries[12];
	unsigned int count, i;
	char comm[TASK_COMM_LEN], line[72];
	pid_t pid = task_pid_nr(task);

	get_task_comm(comm, task);
	count = task_is_running(task) ? 0 :
		stack_trace_save_tsk(task, entries, ARRAY_SIZE(entries), 0);
	scnprintf(line, sizeof(line), "blocked sample pid=%d comm=%s frames=%u",
		  pid, comm, count);
	rothko_pstore_suspend_stage(attempt, line, 0);
	for (i = 0; i < count; i++) {
		scnprintf(line, sizeof(line), "stack pid=%d %pS", pid, (void *)entries[i]);
		rothko_pstore_suspend_stage(attempt, line, 0);
	}
#endif
}

static void s2idle_resume_dump(struct work_struct *work)
{
	struct task_struct *owner = READ_ONCE(s2idle_resume_owner);
	struct task_struct *p, *t;
	unsigned int attempt = READ_ONCE(s2idle_attempt);
	unsigned int count = 0;
	char line[72];

	WRITE_ONCE(s2idle_resume_sampled, true);
	scnprintf(line, sizeof(line), "stalled at %s", READ_ONCE(s2idle_resume_stage));
	rothko_pstore_suspend_stage(attempt, line, 0);
	s2idle_persist_task(owner, attempt);
	pr_warn("s2idle resume stalled: origin=%s stage=%s pid=%d\n",
		s2idle_resume_origin, READ_ONCE(s2idle_resume_stage),
		task_pid_nr(owner));
	sched_show_task(owner);

	rcu_read_lock();
	for_each_process_thread(p, t) {
		unsigned int state = READ_ONCE(t->__state);

		if (t == owner || !(READ_ONCE(t->flags) & PF_WQ_WORKER) ||
		    !(state & TASK_UNINTERRUPTIBLE) || (state & TASK_NOLOAD))
			continue;
		s2idle_persist_task(t, attempt);
		sched_show_task(t);
		if (++count == 8)
			goto unlock;
	}
unlock:
	rcu_read_unlock();
	pr_warn("s2idle resume stack dump complete: workers=%u\n", count);
}

static DECLARE_DELAYED_WORK(s2idle_resume_work, s2idle_resume_dump);

void rothko_s2idle_vcp_wait(const char *operation, int feature, unsigned long caller)
{
	char stage[70];

	if (!READ_ONCE(s2idle_recording))
		return;

	scnprintf(stage, sizeof(stage), "VCP %s feature=%d pid=%d %ps",
		  operation, feature, task_pid_nr(current), (void *)caller);
	rothko_pstore_suspend_stage(READ_ONCE(s2idle_attempt), stage, 0);
}
EXPORT_SYMBOL_GPL(rothko_s2idle_vcp_wait);

void rothko_s2idle_resume_stage(const char *stage)
{
	s2idle_record(stage, 0);
	if (READ_ONCE(s2idle_resume_owner) == current &&
	    READ_ONCE(s2idle_resume_stage))
		WRITE_ONCE(s2idle_resume_stage, stage);
}

static void s2idle_resume_arm(const char *origin, const char *stage)
{
	if (READ_ONCE(s2idle_resume_owner) != current ||
	    READ_ONCE(s2idle_resume_sampled) || READ_ONCE(s2idle_resume_stage))
		return;

	s2idle_resume_origin = origin;
	WRITE_ONCE(s2idle_resume_stage, stage);
	queue_delayed_work(system_dfl_wq, &s2idle_resume_work, 10 * HZ);
}

static void s2idle_sample_before_wait(void)
{
	if (READ_ONCE(s2idle_resume_owner) != current ||
	    !READ_ONCE(s2idle_resume_stage))
		return;

	cancel_delayed_work_sync(&s2idle_resume_work);
	WRITE_ONCE(s2idle_resume_stage, NULL);
}

static void s2idle_resume_track(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && READ_ONCE(pm_debug_messages_on) &&
	    !READ_ONCE(s2idle_resume_sampled))
		WRITE_ONCE(s2idle_resume_owner, current);
}

static void s2idle_resume_stop(void)
{
	if (READ_ONCE(s2idle_resume_owner) != current)
		return;
	s2idle_sample_before_wait();
	if (READ_ONCE(s2idle_resume_sampled))
		pr_info("s2idle transition completed after diagnostic sample\n");
	WRITE_ONCE(s2idle_resume_owner, NULL);
}
#else
static inline void s2idle_resume_arm(const char *origin, const char *stage) { }
static inline void s2idle_resume_track(suspend_state_t state) { }
static inline void s2idle_sample_before_wait(void) { }
static inline void s2idle_resume_stop(void) { }
#endif

/**
 * pm_suspend_default_s2idle - Check if suspend-to-idle is the default suspend.
 *
 * Return 'true' if suspend-to-idle has been selected as the default system
 * suspend method.
 */
bool pm_suspend_default_s2idle(void)
{
	return mem_sleep_current == PM_SUSPEND_TO_IDLE;
}
EXPORT_SYMBOL_GPL(pm_suspend_default_s2idle);

void s2idle_set_ops(const struct platform_s2idle_ops *ops)
{
	unsigned int sleep_flags;

	sleep_flags = lock_system_sleep();
	s2idle_ops = ops;
	unlock_system_sleep(sleep_flags);
}

static void s2idle_begin(void)
{
	s2idle_state = S2IDLE_STATE_NONE;
}

static void s2idle_wait_record(unsigned int attempt, unsigned int round,
			       const char *stage, int status)
{
	char line[48];

	if (!attempt || !round || round > 3)
		return;
	scnprintf(line, sizeof(line), "wait loop=%u %s", round, stage);
	rothko_pstore_suspend_stage(attempt, line, status);
}

static void s2idle_enter(unsigned int round)
{
	unsigned int attempt = s2idle_recording ? s2idle_attempt : 0;
	bool entered = false;
	trace_suspend_resume(TPS("machine_suspend"), PM_SUSPEND_TO_IDLE, true);

	/*
	 * The correctness of the code below depends on the number of online
	 * CPUs being stable, but CPUs cannot be taken offline or put online
	 * while it is running.
	 *
	 * The s2idle_lock must be acquired before the pending wakeup check to
	 * prevent pm_system_wakeup() from running as a whole between that check
	 * and the subsequent s2idle_state update in which case a wakeup event
	 * would get lost.
	 */
	raw_spin_lock_irq(&s2idle_lock);
	if (pm_wakeup_pending())
		goto out;

	s2idle_state = S2IDLE_STATE_ENTER;
	s2idle_record_round = attempt && round <= 3 ? round : 0;
	entered = true;
	raw_spin_unlock_irq(&s2idle_lock);
	s2idle_wait_record(attempt, round, "enter armed", 0);

	/* Push all the CPUs into the idle loop. */
	wake_up_all_idle_cpus();
	/* Make the current CPU wait so it can enter the idle loop too. */
	swait_event_exclusive(s2idle_wait_head,
		    s2idle_state == S2IDLE_STATE_WAKE);
	s2idle_wait_record(attempt, round, "wait returned irq", pm_wakeup_irq());

	/*
	 * Kick all CPUs to ensure that they resume their timers and restore
	 * consistent system state.
	 */
	wake_up_all_idle_cpus();

	raw_spin_lock_irq(&s2idle_lock);

 out:
	s2idle_state = S2IDLE_STATE_NONE;
	s2idle_record_round = 0;
	raw_spin_unlock_irq(&s2idle_lock);
	if (!entered)
		s2idle_wait_record(attempt, round, "locked pending irq", pm_wakeup_irq());

	trace_suspend_resume(TPS("machine_suspend"), PM_SUSPEND_TO_IDLE, false);
}

static void s2idle_loop(void)
{
	unsigned int round = 0;

	pm_pr_dbg("suspend-to-idle\n");

	/*
	 * Suspend-to-idle equals:
	 * frozen processes + suspended devices + idle processors.
	 * Thus s2idle_enter() should be called right after all devices have
	 * been suspended.
	 *
	 * Wakeups during the noirq suspend of devices may be spurious, so try
	 * to avoid them upfront.
	 */
	for (;;) {
		if (round < 4)
			round++;
		if (s2idle_ops && s2idle_ops->wake) {
			if (s2idle_ops->wake())
				break;
		} else if (pm_wakeup_pending()) {
			s2idle_record("wait loop pending irq", pm_wakeup_irq());
			break;
		}

		clear_wakeup_reasons();

		if (s2idle_ops && s2idle_ops->check)
			s2idle_ops->check();

		s2idle_enter(round);
	}

	pm_pr_dbg("resume from suspend-to-idle\n");
}

void s2idle_wake(void)
{
	unsigned int attempt = 0, round = 0, irq = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&s2idle_lock, flags);
	if (s2idle_state > S2IDLE_STATE_NONE) {
		if (s2idle_state == S2IDLE_STATE_ENTER && s2idle_record_round) {
			attempt = READ_ONCE(s2idle_attempt);
			round = s2idle_record_round;
			irq = pm_wakeup_irq();
		}
		s2idle_state = S2IDLE_STATE_WAKE;
		swake_up_one(&s2idle_wait_head);
	}
	raw_spin_unlock_irqrestore(&s2idle_lock, flags);
	s2idle_wait_record(attempt, round, "wake notify irq", irq);
}
EXPORT_SYMBOL_GPL(s2idle_wake);

static bool valid_state(suspend_state_t state)
{
	/*
	 * The PM_SUSPEND_STANDBY and PM_SUSPEND_MEM states require low-level
	 * support and need to be valid to the low-level implementation.
	 *
	 * No ->valid() or ->enter() callback implies that none are valid.
	 */
	return suspend_ops && suspend_ops->valid && suspend_ops->valid(state) &&
		suspend_ops->enter;
}

void __init pm_states_init(void)
{
	/* "mem" and "freeze" are always present in /sys/power/state. */
	pm_states[PM_SUSPEND_MEM] = pm_labels[PM_SUSPEND_MEM];
	pm_states[PM_SUSPEND_TO_IDLE] = pm_labels[PM_SUSPEND_TO_IDLE];
	/*
	 * Suspend-to-idle should be supported even without any suspend_ops,
	 * initialize mem_sleep_states[] accordingly here.
	 */
	mem_sleep_states[PM_SUSPEND_TO_IDLE] = mem_sleep_labels[PM_SUSPEND_TO_IDLE];
}

static int __init mem_sleep_default_setup(char *str)
{
	suspend_state_t state;

	for (state = PM_SUSPEND_TO_IDLE; state <= PM_SUSPEND_MEM; state++)
		if (mem_sleep_labels[state] &&
		    !strcmp(str, mem_sleep_labels[state])) {
			mem_sleep_default = state;
			mem_sleep_current = state;
			break;
		}

	return 1;
}
__setup("mem_sleep_default=", mem_sleep_default_setup);

/**
 * suspend_set_ops - Set the global suspend method table.
 * @ops: Suspend operations to use.
 */
void suspend_set_ops(const struct platform_suspend_ops *ops)
{
	unsigned int sleep_flags;

	sleep_flags = lock_system_sleep();

	suspend_ops = ops;

	if (valid_state(PM_SUSPEND_STANDBY)) {
		mem_sleep_states[PM_SUSPEND_STANDBY] = mem_sleep_labels[PM_SUSPEND_STANDBY];
		pm_states[PM_SUSPEND_STANDBY] = pm_labels[PM_SUSPEND_STANDBY];
		if (mem_sleep_default == PM_SUSPEND_STANDBY)
			mem_sleep_current = PM_SUSPEND_STANDBY;
	}
	if (valid_state(PM_SUSPEND_MEM)) {
		mem_sleep_states[PM_SUSPEND_MEM] = mem_sleep_labels[PM_SUSPEND_MEM];
		if (mem_sleep_default >= PM_SUSPEND_MEM)
			mem_sleep_current = PM_SUSPEND_MEM;
	}

	unlock_system_sleep(sleep_flags);
}
EXPORT_SYMBOL_GPL(suspend_set_ops);

/**
 * suspend_valid_only_mem - Generic memory-only valid callback.
 * @state: Target system sleep state.
 *
 * Platform drivers that implement mem suspend only and only need to check for
 * that in their .valid() callback can use this instead of rolling their own
 * .valid() callback.
 */
int suspend_valid_only_mem(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM;
}
EXPORT_SYMBOL_GPL(suspend_valid_only_mem);

static bool sleep_state_supported(suspend_state_t state)
{
	return state == PM_SUSPEND_TO_IDLE ||
	       (valid_state(state) && !cxl_mem_active());
}

static int platform_suspend_prepare(suspend_state_t state)
{
	return state != PM_SUSPEND_TO_IDLE && suspend_ops->prepare ?
		suspend_ops->prepare() : 0;
}

static int platform_suspend_prepare_late(suspend_state_t state)
{
	return state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->prepare ?
		s2idle_ops->prepare() : 0;
}

static int platform_suspend_prepare_noirq(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE)
		return s2idle_ops && s2idle_ops->prepare_late ?
			s2idle_ops->prepare_late() : 0;

	return suspend_ops->prepare_late ? suspend_ops->prepare_late() : 0;
}

static void platform_resume_noirq(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE) {
		if (s2idle_ops && s2idle_ops->restore_early)
			s2idle_ops->restore_early();
	} else if (suspend_ops->wake) {
		suspend_ops->wake();
	}
}

static void platform_resume_early(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->restore)
		s2idle_ops->restore();
}

static void platform_resume_finish(suspend_state_t state)
{
	if (state != PM_SUSPEND_TO_IDLE && suspend_ops->finish)
		suspend_ops->finish();
}

static int platform_suspend_begin(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->begin)
		return s2idle_ops->begin();
	else if (suspend_ops && suspend_ops->begin)
		return suspend_ops->begin(state);
	else
		return 0;
}

static void platform_resume_end(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->end)
		s2idle_ops->end();
	else if (suspend_ops && suspend_ops->end)
		suspend_ops->end();
}

static void platform_recover(suspend_state_t state)
{
	if (state != PM_SUSPEND_TO_IDLE && suspend_ops->recover)
		suspend_ops->recover();
}

static bool platform_suspend_again(suspend_state_t state)
{
	return state != PM_SUSPEND_TO_IDLE && suspend_ops->suspend_again ?
		suspend_ops->suspend_again() : false;
}

#ifdef CONFIG_PM_DEBUG
static unsigned int pm_test_delay = 5;
module_param(pm_test_delay, uint, 0644);
MODULE_PARM_DESC(pm_test_delay,
		 "Number of seconds to wait before resuming from suspend test");
#endif

static int suspend_test(int level)
{
#ifdef CONFIG_PM_DEBUG
	if (pm_test_level == level) {
		pr_info("suspend debug: Waiting for %d second(s).\n",
				pm_test_delay);
		mdelay(pm_test_delay * 1000);
		return 1;
	}
#endif /* !CONFIG_PM_DEBUG */
	return 0;
}

/**
 * suspend_prepare - Prepare for entering system sleep state.
 * @state: Target system sleep state.
 *
 * Common code run for every system sleep state that can be entered (except for
 * hibernation).  Run suspend notifiers, allocate the "suspend" console and
 * freeze processes.
 */
static int suspend_prepare(suspend_state_t state)
{
	int error;

	if (!sleep_state_supported(state))
		return -EPERM;

	pm_prepare_console();

	error = pm_notifier_call_chain_robust(PM_SUSPEND_PREPARE, PM_POST_SUSPEND);
	if (error)
		goto Restore;

	filesystems_freeze(filesystem_freeze_enabled);
	trace_suspend_resume(TPS("freeze_processes"), 0, true);
	error = suspend_freeze_processes();
	trace_suspend_resume(TPS("freeze_processes"), 0, false);
	if (!error)
		return 0;

	log_suspend_abort_reason("One or more tasks refusing to freeze");
	dpm_save_failed_step(SUSPEND_FREEZE);
	filesystems_thaw();
	pm_notifier_call_chain(PM_POST_SUSPEND);
 Restore:
	pm_restore_console();
	return error;
}

/* default implementation */
void __weak arch_suspend_disable_irqs(void)
{
	local_irq_disable();
}

/* default implementation */
void __weak arch_suspend_enable_irqs(void)
{
	local_irq_enable();
}

/**
 * suspend_enter - Make the system enter the given sleep state.
 * @state: System sleep state to enter.
 * @wakeup: Returns information that the sleep state should not be re-entered.
 *
 * This function should be called after devices have been suspended.
 */
static int suspend_enter(suspend_state_t state, bool *wakeup)
{
	int error;

	error = platform_suspend_prepare(state);
	if (error)
		goto Platform_finish;

	rothko_s2idle_resume_stage("device suspend late");
	error = dpm_suspend_late(PMSG_SUSPEND);
	s2idle_record("devices late done", error);
	if (error) {
		pr_err("late suspend of devices failed: %d\n", error);
		s2idle_resume_arm("late suspend abort", "late abort reason");
		log_suspend_abort_reason("late suspend aborted: %d", error);
		goto Platform_finish;
	}
	error = platform_suspend_prepare_late(state);
	if (error)
		goto Devices_early_resume;

	rothko_s2idle_resume_stage("device suspend noirq");
	error = dpm_suspend_noirq(PMSG_SUSPEND);
	s2idle_record("devices noirq done", error);
	if (error) {
		pr_err("noirq suspend of devices failed: %d\n", error);
		log_suspend_abort_reason("noirq suspend aborted: %d", error);
		goto Platform_early_resume;
	}
	error = platform_suspend_prepare_noirq(state);
	if (error)
		goto Platform_wake;

	if (suspend_test(TEST_PLATFORM))
		goto Platform_wake;

	if (state == PM_SUSPEND_TO_IDLE) {
		s2idle_sample_before_wait();
		s2idle_record("platform wait start", 0);
		s2idle_loop();
		s2idle_record("platform wait done", 0);
		s2idle_resume_arm("s2idle loop returned", "wakeup irq report");
		pm_print_wakeup_irq();
		goto Platform_wake;
	}

	error = pm_sleep_disable_secondary_cpus();
	if (error || suspend_test(TEST_CPUS)) {
		log_suspend_abort_reason("Disabling non-boot cpus failed");
		goto Enable_cpus;
	}

	arch_suspend_disable_irqs();
	BUG_ON(!irqs_disabled());

	system_state = SYSTEM_SUSPEND;

	error = syscore_suspend();
	if (!error) {
		*wakeup = pm_wakeup_pending();
		if (!(suspend_test(TEST_CORE) || *wakeup)) {
			trace_suspend_resume(TPS("machine_suspend"),
				state, true);
			error = suspend_ops->enter(state);
			trace_suspend_resume(TPS("machine_suspend"),
				state, false);
			trace_android_vh_early_resume_begin(NULL);
		} else if (*wakeup) {
			error = -EBUSY;
		}
		syscore_resume();
	}

	system_state = SYSTEM_RUNNING;

	arch_suspend_enable_irqs();
	BUG_ON(irqs_disabled());

	pm_print_wakeup_irq();

 Enable_cpus:
	pm_sleep_enable_secondary_cpus();

 Platform_wake:
	rothko_s2idle_resume_stage("platform resume noirq");
	platform_resume_noirq(state);
	rothko_s2idle_resume_stage("device resume noirq");
	dpm_resume_noirq(PMSG_RESUME);
	s2idle_record("resume noirq done", 0);

 Platform_early_resume:
	rothko_s2idle_resume_stage("platform resume early");
	platform_resume_early(state);

 Devices_early_resume:
	rothko_s2idle_resume_stage("device resume early");
	dpm_resume_early(PMSG_RESUME);
	s2idle_record("resume early done", 0);

 Platform_finish:
	rothko_s2idle_resume_stage("platform resume finish");
	platform_resume_finish(state);
	rothko_s2idle_resume_stage("wakeup irq report");
	pm_print_wakeup_irq();
	return error;
}

/**
 * suspend_devices_and_enter - Suspend devices and enter system sleep state.
 * @state: System sleep state to enter.
 */
int suspend_devices_and_enter(suspend_state_t state)
{
	int error;
	bool wakeup = false;

	if (!sleep_state_supported(state))
		return -ENOSYS;

	pm_suspend_target_state = state;

	if (state == PM_SUSPEND_TO_IDLE)
		pm_set_suspend_no_platform();

	error = platform_suspend_begin(state);
	if (error)
		goto Close;

	s2idle_resume_arm("s2idle device suspend", "console suspend");
	s2idle_record("console suspend start", 0);
	console_suspend_all();
	s2idle_record("console suspend done", 0);
	rothko_s2idle_resume_stage("device suspend dispatch");
	suspend_test_start();
	error = dpm_suspend_start(PMSG_SUSPEND);
	s2idle_record("devices suspend done", error);
	if (error) {
		pr_err("Some devices failed to suspend, or early wake event detected\n");
		log_suspend_abort_reason(
				"Some devices failed to suspend, or early wake event detected");
		goto Recover_platform;
	}
	suspend_test_finish("suspend devices");
	if (suspend_test(TEST_DEVICES))
		goto Recover_platform;

	do {
		error = suspend_enter(state, &wakeup);
	} while (!error && !wakeup && platform_suspend_again(state));

 Resume_devices:
	rothko_s2idle_resume_stage("resume begin hook");
	trace_android_vh_resume_begin(NULL);
	suspend_test_start();
	rothko_s2idle_resume_stage("device resume dispatch");
	dpm_resume_end(PMSG_RESUME);
	s2idle_record("resume devices done", 0);
	suspend_test_finish("resume devices");
	trace_suspend_resume(TPS("console_resume_all"), state, true);
	rothko_s2idle_resume_stage("console resume");
	console_resume_all();
	trace_suspend_resume(TPS("console_resume_all"), state, false);

 Close:
	rothko_s2idle_resume_stage("platform resume end");
	platform_resume_end(state);
	pm_suspend_target_state = PM_SUSPEND_ON;
	rothko_s2idle_resume_stage("resume end hook");
	trace_android_vh_resume_end(NULL);
	return error;

 Recover_platform:
	platform_recover(state);
	goto Resume_devices;
}

/**
 * suspend_finish - Clean up before finishing the suspend sequence.
 *
 * Call platform code to clean up, restart processes, and free the console that
 * we've allocated. This routine is not called for hibernation.
 */
static void suspend_finish(void)
{
	rothko_s2idle_resume_stage("thaw processes");
	suspend_thaw_processes();
	s2idle_record("thaw done", 0);
	rothko_s2idle_resume_stage("thaw filesystems");
	filesystems_thaw();
	rothko_s2idle_resume_stage("post-suspend notifiers");
	s2idle_record("POST done", pm_notifier_call_chain(PM_POST_SUSPEND));
	rothko_s2idle_resume_stage("restore console");
	pm_restore_console();
}

/**
 * enter_state - Do common work needed to enter system sleep state.
 * @state: System sleep state to enter.
 *
 * Make sure that no one else is trying to put the system into a sleep state.
 * Fail if that's not the case.  Otherwise, prepare for system suspend, make the
 * system enter the given sleep state and clean up after wakeup.
 */
static int enter_state(suspend_state_t state)
{
	int error;

	trace_suspend_resume(TPS("suspend_enter"), state, true);
	if (state == PM_SUSPEND_TO_IDLE) {
#ifdef CONFIG_PM_DEBUG
		if (pm_test_level != TEST_NONE && pm_test_level <= TEST_CPUS) {
			pr_warn("Unsupported test mode for suspend to idle, please choose none/freezer/devices/platform.\n");
			return -EAGAIN;
		}
#endif
	} else if (!valid_state(state)) {
		return -EINVAL;
	}
	if (!mutex_trylock(&system_transition_mutex))
		return -EBUSY;

	if (state == PM_SUSPEND_TO_IDLE) {
		WRITE_ONCE(s2idle_attempt, s2idle_attempt + 1);
		WRITE_ONCE(s2idle_recording, true);
		s2idle_begin();
	}

	if (sync_on_suspend_enabled) {
		trace_suspend_resume(TPS("sync_filesystems"), 0, true);

		error = pm_sleep_fs_sync();
		if (error)
			goto Unlock;

		trace_suspend_resume(TPS("sync_filesystems"), 0, false);
	}

	pm_pr_dbg("Preparing system for sleep (%s)\n", mem_sleep_labels[state]);
	pm_suspend_clear_flags();
	error = suspend_prepare(state);
	if (error)
		goto Unlock;

	if (suspend_test(TEST_FREEZER))
		goto Finish;

	trace_suspend_resume(TPS("suspend_enter"), state, false);
	pm_pr_dbg("Suspending system (%s)\n", mem_sleep_labels[state]);
	s2idle_resume_track(state);
	error = suspend_devices_and_enter(state);

 Finish:
	events_check_enabled = false;
	pm_pr_dbg("Finishing wakeup.\n");
	suspend_finish();
	s2idle_resume_stop();
 Unlock:
	WRITE_ONCE(s2idle_recording, false);
	mutex_unlock(&system_transition_mutex);
	return error;
}

/**
 * pm_suspend - Externally visible function for suspending the system.
 * @state: System sleep state to enter.
 *
 * Check if the value of @state represents one of the supported states,
 * execute enter_state() and update system suspend statistics.
 */
int pm_suspend(suspend_state_t state)
{
	int error;

	if (state <= PM_SUSPEND_ON || state >= PM_SUSPEND_MAX)
		return -EINVAL;

	pr_info("suspend entry (%s)\n", mem_sleep_labels[state]);
	error = enter_state(state);
	if (error) {
		suspend_stats.fail++;
		dpm_save_failed_errno(error);
	} else {
		suspend_stats.success++;
	}
	pr_info("suspend exit\n");
	return error;
}
EXPORT_SYMBOL(pm_suspend);
