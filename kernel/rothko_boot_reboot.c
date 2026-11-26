// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>

#define ROTHKO_REBOOT_PROBE_MS 50000

static bool reboot_probe_enabled = true;
static struct delayed_work reboot_probe_work;

static int __init rothko_reboot_probe_setup(char *value)
{
	return kstrtobool(value, &reboot_probe_enabled);
}
early_param("rothko_reboot_probe", rothko_reboot_probe_setup);

static void rothko_reboot_probe_workfn(struct work_struct *work)
{
	if (!READ_ONCE(reboot_probe_enabled))
		return;

	pr_emerg("rothko diagnostic: 50-second reboot probe fired\n");
	kernel_restart("rothko-diagnostic-probe");
}

static int __init rothko_reboot_probe_init(void)
{
	s64 elapsed_ms, remaining_ms;

	if (!reboot_probe_enabled)
		return 0;

	INIT_DELAYED_WORK(&reboot_probe_work, rothko_reboot_probe_workfn);
	elapsed_ms = ktime_to_ms(ktime_get_boottime());
	remaining_ms = max_t(s64, 0, ROTHKO_REBOOT_PROBE_MS - elapsed_ms);
	pr_warn("rothko diagnostic: reboot probe armed, uptime=%lld ms, delay=%lld ms\n",
		elapsed_ms, remaining_ms);
	schedule_delayed_work(&reboot_probe_work,
			      msecs_to_jiffies((unsigned int)remaining_ms));
	return 0;
}
late_initcall(rothko_reboot_probe_init);
