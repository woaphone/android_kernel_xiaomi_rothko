// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6989 CPU low-power firmware initialization.
 *
 * Based on MediaTek lpm_irqremain.c, lpm_resource_ctrl.c, lpm_rc.c,
 * lpm_plat_apmcu.c and lpm_plat_suspend.c (Copyright (c) 2019 MediaTek Inc.).
 * Keep runtime PSCI idle in the PSCI driver; do not replace its accounting.
 */

#define pr_fmt(fmt) "MT6989 CPU LPM: " fmt

#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/suspend.h>

#include "cpuidle-psci.h"
#include "mt6989/lpm_module.h"

#define MT6989_CPU_OFF_BLOCK_US	2
#define MT6989_SYSRAM_INFO_OFFSET	0x8
#define MT6989_SYSRAM_SIZE		0x500
#define MT6989_COMMON_SODI_DELAY_S	120
#define MT6989_COMMON_SODI_SETTLE_US	1500

struct mt6989_lpm_irq {
	struct list_head list;
	unsigned int hwirq;
	u32 wakeup_src_cat;
	u32 wakeup_src;
};

static void __iomem *mt6989_mcusys_base;
static void __iomem *mt6989_sysram_base;
static struct pm_qos_request mt6989_idle_qos;
static struct hrtimer mt6989_common_sodi_timer;
static bool mt6989_common_sodi_saved;
static bool mt6989_common_sodi_timer_paused;
static bool mt6989_lpm_present __ro_after_init;
static bool mt6989_lpm_ready __ro_after_init;
static int mt6989_init_error = -ENODEV;
static s32 mt6989_runtime_latency = PM_QOS_DEFAULT_VALUE;

void mt6989_lpm_suspend_source(unsigned int cpu, bool enter)
{
	if (!mt6989_lpm_ready)
		return;

	lpm_smc_cpu_pm_lp(SUSPEND_SRC,
			  enter ? MT_LPM_SMC_ACT_SET : MT_LPM_SMC_ACT_CLR,
			  cpu, 0);
}

static int __init mt6989_lpm_smc_result(const char *operation,
				       unsigned long result)
{
	long ret = (long)result;

	if (ret < 0) {
		pr_err("%s failed: firmware status %ld\n", operation, ret);
		return ret;
	}

	return 0;
}

static void __init mt6989_lpm_irq_result(long ret, long *error)
{
	if (ret < 0 && !*error)
		*error = ret;
}

static int __init mt6989_lpm_map(const char *compatible, size_t min_size,
				void __iomem **base)
{
	struct device_node *np;
	struct resource resource;
	int ret;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np)
		return -ENODEV;

	ret = of_address_to_resource(np, 0, &resource);
	if (!ret && resource_size(&resource) < min_size)
		ret = -EINVAL;
	if (!ret) {
		*base = of_iomap(np, 0);
		if (!*base)
			ret = -ENOMEM;
	}
	of_node_put(np);
	return ret;
}

static int __init mt6989_lpm_early_init(void)
{
	struct device_node *np;
	int ret;

	if (!of_machine_is_compatible("mediatek,MT6989"))
		return 0;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mtk-lpm");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		return -ENODEV;
	}
	of_node_put(np);
	mt6989_lpm_present = true;

	/* Firmware must not power off CPUs while its shared state is incomplete. */
	cpu_latency_qos_add_request(&mt6989_idle_qos, MT6989_CPU_OFF_BLOCK_US);

	ret = mt6989_lpm_map("mediatek,mcusys-ctrl", sizeof(u32),
			     &mt6989_mcusys_base);
	if (!ret)
		ret = mt6989_lpm_map("mediatek,cpupm-sysram", MT6989_SYSRAM_SIZE,
				     &mt6989_sysram_base);
	if (!ret) {
		/* Preserve the firmware-owned first two words, as the native driver does. */
		memset_io(mt6989_sysram_base + MT6989_SYSRAM_INFO_OFFSET, 0,
			  MT6989_SYSRAM_SIZE - MT6989_SYSRAM_INFO_OFFSET);
		mb();
	}

	mt6989_init_error = ret;
	if (ret)
		pr_err("shared state initialization failed: %d\n", ret);
	return ret;
}
subsys_initcall(mt6989_lpm_early_init);

static int __init mt6989_lpm_parse_irqs(struct device_node *parent,
				       struct list_head *irqs, unsigned int *count)
{
	struct device_node *np, *target;
	struct mt6989_lpm_irq *entry;
	struct irq_data *data;
	u32 values[4];
	unsigned int index = 0, i, j, type;
	int ret, cells, irq;

	while ((np = of_parse_phandle(parent, "irq-remain", index++))) {
		target = of_parse_phandle(np, "target", 0);
		cells = of_property_count_u32_elems(np, "value");
		if (!target || cells <= 0 || cells % ARRAY_SIZE(values)) {
			ret = -EINVAL;
			goto put_nodes;
		}

		for (i = 0; i < cells; i += ARRAY_SIZE(values)) {
			for (j = 0; j < ARRAY_SIZE(values); j++) {
				ret = of_property_read_u32_index(np, "value", i + j, &values[j]);
				if (ret)
					goto put_nodes;
			}

			irq = of_irq_get(target, values[1]);
			if (irq <= 0) {
				ret = irq ? irq : -EINVAL;
				goto put_nodes;
			}
			type = irq_get_trigger_type(irq);
			if (values[0]) {
				ret = irq_set_irq_type(irq, type);
				if (ret)
					goto put_nodes;
			}
			data = irq_get_irq_data(irq);
			if (!data) {
				ret = -EINVAL;
				goto put_nodes;
			}

			entry = kzalloc(sizeof(*entry), GFP_KERNEL);
			if (!entry) {
				ret = -ENOMEM;
				goto put_nodes;
			}
			entry->hwirq = irqd_to_hwirq(data);
			entry->wakeup_src_cat = values[2];
			entry->wakeup_src = values[3];
			/* Retain the native driver's submission order. */
			list_add(&entry->list, irqs);
			(*count)++;
		}

		of_node_put(target);
		of_node_put(np);
	}

	return *count ? 0 : -ENODEV;

put_nodes:
	of_node_put(target);
	of_node_put(np);
	return ret;
}

static int __init mt6989_lpm_submit_irqs(struct list_head *irqs,
					unsigned int count)
{
	struct mt6989_lpm_irq *entry;
	long legacy_error = 0, lp_error = 0;

	/* Both interfaces are present in the native MT6989 initialization chain. */
	mt6989_lpm_irq_result(lpm_smc_cpu_pm(IRQ_REMAIN_LIST_ALLOC, count, 0, 0),
			      &legacy_error);
	mt6989_lpm_irq_result(lpm_smc_cpu_pm_lp(IRQS_REMAIN_ALLOC,
			      MT_LPM_SMC_ACT_SET, count, 0), &lp_error);

	list_for_each_entry(entry, irqs, list) {
		mt6989_lpm_irq_result(lpm_smc_cpu_pm(IRQ_REMAIN_IRQ_ADD,
				      entry->hwirq, entry->wakeup_src_cat,
				      entry->wakeup_src), &legacy_error);
		mt6989_lpm_irq_result(lpm_smc_cpu_pm_lp(IRQS_REMAIN_IRQ,
				      MT_LPM_SMC_ACT_SET, entry->hwirq, 0), &lp_error);
		mt6989_lpm_irq_result(lpm_smc_cpu_pm_lp(IRQS_REMAIN_WAKEUP_CAT,
				      MT_LPM_SMC_ACT_SET, entry->wakeup_src_cat, 0),
				      &lp_error);
		mt6989_lpm_irq_result(lpm_smc_cpu_pm_lp(IRQS_REMAIN_WAKEUP_SRC,
				      MT_LPM_SMC_ACT_SET, entry->wakeup_src, 0), &lp_error);
		mt6989_lpm_irq_result(lpm_smc_cpu_pm_lp(IRQS_REMAIN_CTRL,
				      MT_LPM_SMC_ACT_PUSH, 0, 0), &lp_error);
	}

	mt6989_lpm_irq_result(lpm_smc_cpu_pm(IRQ_REMAIN_IRQ_SUBMIT, 0, 0, 0),
			      &legacy_error);
	mt6989_lpm_irq_result(lpm_smc_cpu_pm_lp(IRQS_REMAIN_CTRL,
			      MT_LPM_SMC_ACT_SUBMIT, 0, 0), &lp_error);
	pr_info("IRQ remain count=%u, legacy status=%ld, LP status=%ld\n",
		count, legacy_error, lp_error);

	/* A firmware may implement only one of the two native interfaces. */
	return legacy_error && lp_error ? legacy_error : 0;
}

static int __init mt6989_lpm_parse_controls(struct device_node *parent,
					   const char *property, bool constraints)
{
	struct device_node *np;
	u32 id, value;
	unsigned int index = 0;
	int ret;

	while ((np = of_parse_phandle(parent, property, index++))) {
		ret = of_property_read_u32(np, "id", &id);
		if (!ret)
			ret = of_property_read_u32(np, "value", &value);
		of_node_put(np);
		if (ret)
			return ret;

		if (constraints)
			ret = mt6989_lpm_smc_result("constraint",
				lpm_smc_spm_dbg(MT_SPM_DBG_SMC_UID_DOE_RC,
					value ? MT_LPM_SMC_ACT_SET : MT_LPM_SMC_ACT_CLR,
					id, 0));
		else
			ret = mt6989_lpm_smc_result("resource control",
				lpm_smc_spm_dbg(MT_SPM_DBG_SMC_UID_DOE_RESOURCE_CTRL,
					MT_LPM_SMC_ACT_SET, id, value));
		if (ret)
			return ret;
	}

	return 0;
}

static irqreturn_t mt6989_spm_irq(int irq, void *data)
{
	/* The SPM firmware handles acknowledgement, as in the native driver. */
	return IRQ_HANDLED;
}

static int __init mt6989_lpm_spm_init(void)
{
	struct device_node *np;
	struct irq_data *data;
	int irq, ret;

	np = of_find_compatible_node(NULL, NULL, "mediatek,sleep");
	if (!np)
		return -ENODEV;
	irq = of_irq_get(np, 0);
	of_node_put(np);
	if (irq <= 0)
		return irq ? irq : -EINVAL;
	data = irq_get_irq_data(irq);
	if (!data)
		return -EINVAL;

	ret = request_irq(irq, mt6989_spm_irq, 0, "spm-irq", NULL);
	if (ret)
		return ret;
	ret = enable_irq_wake(irq);
	if (ret)
		goto free_spm_irq;

	ret = mt6989_lpm_smc_result("SPM pending IRQ",
		lpm_smc_spm(MT_SPM_SMC_UID_SET_PENDING_IRQ_INIT,
			   MT_LPM_SMC_ACT_SET, irqd_to_hwirq(data), 0));
	if (ret) {
		disable_irq_wake(irq);
		goto free_spm_irq;
	}
	pr_info("SPM wake IRQ hwirq=%lu\n", irqd_to_hwirq(data));
	return 0;

free_spm_irq:
	free_irq(irq, NULL);
	return ret;
}

static int mt6989_lpm_cpu_block(unsigned int cpu)
{
	cpu_latency_qos_update_request(&mt6989_idle_qos, MT6989_CPU_OFF_BLOCK_US);
	return 0;
}

static int mt6989_lpm_cpu_allow(unsigned int cpu)
{
	if (mt6989_lpm_ready)
		cpu_latency_qos_update_request(&mt6989_idle_qos, mt6989_runtime_latency);
	return 0;
}

static int __init mt6989_lpm_idle_init(void)
{
	struct cpuidle_device *dev;
	struct cpuidle_driver *drv;
	int cpu, index, bp_state, ret;
	s32 latency;

	/* Keep the DT's suspend-only state out of ordinary governor selection. */
	for_each_online_cpu(cpu) {
		dev = per_cpu(cpuidle_devices, cpu);
		if (!dev)
			return -ENODEV;
		drv = cpuidle_get_cpu_driver(dev);
		if (!drv || drv->state_count < 2)
			return -ENODEV;
		index = drv->state_count - 1;
		if (strcmp(drv->states[index].name, "s2idle"))
			continue;
		latency = (drv->states[index].exit_latency +
			   drv->states[index - 1].exit_latency) / 2;
		if (mt6989_runtime_latency == PM_QOS_DEFAULT_VALUE ||
		    latency < mt6989_runtime_latency)
			mt6989_runtime_latency = latency;
	}

	/* Bracket CPU hotplug with the same CPU-off QoS gate as the native driver. */
	bp_state = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN,
		"cpuidle/mt6989:prepare", mt6989_lpm_cpu_block, mt6989_lpm_cpu_allow);
	if (bp_state < 0)
		return bp_state;
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
		"cpuidle/mt6989:online", mt6989_lpm_cpu_allow, mt6989_lpm_cpu_block);
	if (ret < 0) {
		cpuhp_remove_state_nocalls(bp_state);
		return ret;
	}
	return 0;
}

static enum hrtimer_restart mt6989_lpm_enable_common_sodi(struct hrtimer *timer)
{
	int ret;

	ret = lpm_smc_spm_dbg(MT_SPM_DBG_SMC_COMMON_SODI5_CTRL,
			      MT_LPM_SMC_ACT_SET, 1, 0);
	if (ret < 0)
		pr_err("Common SODI5 enable failed: firmware status %d\n", ret);
	else
		pr_info("Common SODI5 enable returned firmware status %d\n", ret);

	return HRTIMER_NORESTART;
}

static int mt6989_lpm_get_common_sodi(void)
{
	return lpm_smc_spm_dbg(MT_SPM_DBG_SMC_COMMON_SODI5_CTRL,
			       MT_LPM_SMC_ACT_GET, 0, 0);
}

static int mt6989_lpm_set_common_sodi(bool enable)
{
	int ret;

	/* Quiesce idle while the SPM firmware switches its software flag. */
	cpuidle_pause_and_lock();
	udelay(MT6989_COMMON_SODI_SETTLE_US);
	ret = lpm_smc_spm_dbg(MT_SPM_DBG_SMC_COMMON_SODI5_CTRL,
			      MT_LPM_SMC_ACT_SET, enable, 0);
	udelay(MT6989_COMMON_SODI_SETTLE_US);
	cpuidle_resume_and_unlock();

	return ret;
}

static void mt6989_lpm_resume_sodi_timer(void)
{
	if (mt6989_common_sodi_timer_paused) {
		mt6989_common_sodi_timer_paused = false;
		hrtimer_restart(&mt6989_common_sodi_timer);
	}
}

static int mt6989_lpm_common_sodi_pm(struct notifier_block *nb,
				    unsigned long event, void *unused)
{
	int ret, state;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		/* Preserve the one-shot deadline across suspend and early abort. */
		mt6989_common_sodi_timer_paused = hrtimer_cancel(&mt6989_common_sodi_timer);
		state = mt6989_lpm_get_common_sodi();
		ret = state < 0 ? state : 0;
		if (!ret) {
			mt6989_common_sodi_saved = state;
			ret = mt6989_lpm_set_common_sodi(false);
		}
		if (ret < 0) {
			mt6989_lpm_resume_sodi_timer();
			return notifier_from_errno(ret);
		}
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
		/* GET reports the firmware's cached flag, not the SPM state. */
		ret = mt6989_lpm_set_common_sodi(mt6989_common_sodi_saved);
		if (ret < 0)
			pr_err("Common SODI5 restore failed: firmware status %d\n", ret);
		mt6989_lpm_resume_sodi_timer();
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block mt6989_common_sodi_nb = {
	.notifier_call = mt6989_lpm_common_sodi_pm,
};

static void __init mt6989_lpm_schedule_common_sodi(struct device_node *np)
{
	u32 enabled;
	int ret;

	if (of_property_read_u32(np, "common-sodi", &enabled) || !enabled)
		return;

	hrtimer_setup(&mt6989_common_sodi_timer, mt6989_lpm_enable_common_sodi,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	/* The native LPM driver enables Common SODI5 after a 120 s boot delay. */
	hrtimer_start(&mt6989_common_sodi_timer,
		      ktime_set(MT6989_COMMON_SODI_DELAY_S, 0), HRTIMER_MODE_REL);
	ret = register_pm_notifier(&mt6989_common_sodi_nb);
	if (ret) {
		hrtimer_cancel(&mt6989_common_sodi_timer);
		pr_err("Common SODI5 PM notifier registration failed: %d\n", ret);
	}
}

static int __init mt6989_lpm_init(void)
{
	struct device_node *np;
	struct mt6989_lpm_irq *entry, *next;
	unsigned int count = 0;
	LIST_HEAD(irqs);
	int ret;

	if (!mt6989_lpm_present)
		return 0;
	if (mt6989_init_error)
		return mt6989_init_error;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mtk-lpm");
	if (!np)
		return -ENODEV;

	cpuidle_pause_and_lock();
	ret = mt6989_lpm_parse_irqs(np, &irqs, &count);
	if (!ret)
		ret = mt6989_lpm_submit_irqs(&irqs, count);
	if (!ret)
		ret = mt6989_lpm_parse_controls(np, "resource-ctrl", false);
	if (!ret)
		ret = mt6989_lpm_parse_controls(np, "constraints", true);
	if (!ret)
		ret = mt6989_lpm_spm_init();
	if (!ret)
		ret = mt6989_lpm_idle_init();
	if (!ret)
		ret = mt6989_lpm_smc_result("power state validation",
			lpm_smc_cpu_pm(VALIDATE_PWR_STATE_CTRL, MT_LPM_SMC_ACT_SET,
				       PSCI_E_SUCCESS, 0));
	if (!ret) {
		mt6989_lpm_ready = true;
		cpu_latency_qos_update_request(&mt6989_idle_qos, mt6989_runtime_latency);
		pr_info("runtime initialization complete, latency limit=%d us\n",
			mt6989_runtime_latency);
	} else {
		pr_err("initialization failed: %d; CPU-off gate remains closed\n", ret);
	}
	cpuidle_resume_and_unlock();
	if (!ret)
		mt6989_lpm_schedule_common_sodi(np);
	of_node_put(np);

	list_for_each_entry_safe(entry, next, &irqs, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	return ret;
}
late_initcall_sync(mt6989_lpm_init);
