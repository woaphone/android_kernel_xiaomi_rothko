// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6989 MCUPM platform Alive and timesync client for Xiaomi rothko.
 * Timesync protocol based on the MediaTek MCUPM v2 driver.
 * Copyright (c) 2019 MediaTek Inc.
 */
#include <linux/bitfield.h>
#include <linux/capability.h>
#include <linux/clocksource.h>
#include <linux/completion.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/sched/clock.h>
#include <linux/soc/mediatek/mtk-mbox.h>
#include <linux/timecounter.h>
#include <linux/workqueue.h>

#include <asm/arch_timer.h>

#define MCUPM_MBOX0_PHYS	0x0c0df600
#define MCUPM_MBOX_COUNT	16
#define MCUPM_MBOX_WORDS	40
#define MCUPM_PLT_MBOX		0
#define MCUPM_PLT_PIN		0
#define MCUPM_ALIVE_CMD		0xdead
#define MCUPM_ALIVE_ACK_SLOT	20
#define MCUPM_ALIVE_ACK		1
#define MCUPM_ALIVE_TIMEOUT_MS	2000

#define MCUPM_PTP3_CMD		0x2457
#define UV32_CMD		0x245f
#define UV32_API		0x00093201
#define UV32_TIMEOUT_MS		500

enum uv32_command { UV32_QUERY, UV32_SET, UV32_RESET, UV32_CORE_DOMAIN };

struct rothko_uv_dir {
	struct kobject kobj;
	struct rothko_mcupm *mc;
};

#define MCUPM_TS_MBOX		4
#define MCUPM_TS_PIN		4
#define MCUPM_TS_TICK_H		12
#define MCUPM_TS_TICK_L		13
#define MCUPM_TS_TIME_H		14
#define MCUPM_TS_TIME_L		15
#define MCUPM_TS_VERSION	GENMASK(30, 28)
#define MCUPM_TS_FREEZE		BIT(31)
#define MCUPM_TS_VALUE_H	GENMASK(27, 0)

struct rothko_mcupm {
	struct device *dev;
	struct mtk_mbox_device mbox;
	struct mtk_mbox_info info[MCUPM_TS_MBOX + 1];
	struct completion alive_done;
	s32 alive_ack;
	int alive_error;
	bool alive_drained;
	struct mutex platform_lock;
	int platform_irq;
	void __iomem *ptp3;
	struct rothko_uv_dir *uv_dir;
	bool suspended;

	struct cyclecounter cc;
	struct timecounter tc;
	struct hrtimer refresh_timer;
	struct work_struct refresh_work;
	ktime_t refresh_period;
	u8 version;
};

/* Probe claims this one DT mailbox's MMIO and IRQ exclusively. */
static bool unanswered_alive;

static bool rothko_mcupm_is_board(void)
{
	struct device_node *np;
	const char *firmware;

	if (!of_machine_is_compatible("mediatek,MT6989"))
		return false;

	/* Stock uses a generic SoC root compatible; identify the board via DT. */
	for_each_compatible_node(np, NULL, "xiaomi,touch-spi") {
		if (!of_property_read_string(np, "goodix,firmware-name", &firmware) &&
		    !strcmp(firmware, "goodix_firmware_rothko")) {
			of_node_put(np);
			return true;
		}
	}

	return false;
}

static int rothko_mcupm_map(struct platform_device *pdev, struct rothko_mcupm *mc)
{
	struct mtk_mbox_info *plt = &mc->info[MCUPM_PLT_MBOX];
	struct mtk_mbox_info *ts = &mc->info[MCUPM_TS_MBOX];
	struct device *dev = &pdev->dev;
	struct resource *res;
	resource_size_t stride, ts_phys;
	u32 banks;
	int ret;

	ret = of_property_read_u32(dev->of_node, "mbox-extend", &banks);
	if (ret)
		return ret;
	if (banks != MCUPM_MBOX_COUNT)
		return -EINVAL;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mbox0_base");
	if (!res)
		return -ENODEV;
	stride = resource_size(res);
	if (res->start != MCUPM_MBOX0_PHYS || stride != MCUPM_MBOX_WORDS * MBOX_SLOT_SIZE)
		return -EINVAL;
	ts_phys = res->start + MCUPM_TS_MBOX * stride;

	plt->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(plt->base))
		return PTR_ERR(plt->base);

	plt->set_irq_reg = devm_platform_ioremap_resource_byname(pdev, "mbox0_set");
	if (IS_ERR(plt->set_irq_reg))
		return PTR_ERR(plt->set_irq_reg);
	plt->clr_irq_reg = devm_platform_ioremap_resource_byname(pdev, "mbox0_clr");
	if (IS_ERR(plt->clr_irq_reg))
		return PTR_ERR(plt->clr_irq_reg);
	plt->send_status_reg = devm_platform_ioremap_resource_byname(pdev, "mbox0_send");
	if (IS_ERR(plt->send_status_reg))
		return PTR_ERR(plt->send_status_reg);
	plt->recv_status_reg = devm_platform_ioremap_resource_byname(pdev, "mbox0_recv");
	if (IS_ERR(plt->recv_status_reg))
		return PTR_ERR(plt->recv_status_reg);

	/* Stock extended SRAM layout; the GPU's read-only MCDI mapping overlaps. */
	ts->base = devm_ioremap(dev, ts_phys, stride);
	if (!ts->base)
		return -ENOMEM;
	ts->send_status_reg = plt->send_status_reg;
	plt->slot = MCUPM_MBOX_WORDS;
	ts->slot = MCUPM_MBOX_WORDS;
	spin_lock_init(&plt->mbox_lock);
	spin_lock_init(&ts->mbox_lock);
	mc->mbox.info_table = mc->info;
	mc->mbox.count = ARRAY_SIZE(mc->info);

	return 0;
}

static irqreturn_t rothko_mcupm_alive_irq(int irq, void *data)
{
	struct rothko_mcupm *mc = data;
	unsigned int bit = BIT(MCUPM_PLT_PIN);
	int ret;

	if (!(mtk_mbox_read_recv_irq(&mc->mbox, MCUPM_PLT_MBOX) & bit))
		return IRQ_NONE;

	mc->alive_error = mtk_mbox_read(&mc->mbox, MCUPM_PLT_MBOX, MCUPM_ALIVE_ACK_SLOT,
				       &mc->alive_ack, sizeof(mc->alive_ack));
	ret = mtk_mbox_clr_irq(&mc->mbox, MCUPM_PLT_MBOX, bit);
	if (!mc->alive_error)
		mc->alive_error = ret;
	/* Retire the firmware IRQ before handing the reply to the caller. */
	mb();
	mc->alive_drained = ret == MBOX_DONE;
	complete(&mc->alive_done);

	return IRQ_HANDLED;
}

static int rothko_mcupm_platform_xfer(struct rothko_mcupm *mc, const u32 *request,
				      size_t bytes, const u32 *inner, u32 *result)
{
	unsigned int bit = BIT(MCUPM_PLT_PIN);
	u32 ack;
	bool pending;
	int ret;

	lockdep_assert_held(&mc->platform_lock);
	if (mc->suspended)
		return -EBUSY;

	if (mtk_mbox_check_send_irq(&mc->mbox, MCUPM_PLT_MBOX, MCUPM_PLT_PIN))
		return -EBUSY;
	pending = mtk_mbox_read_recv_irq(&mc->mbox, MCUPM_PLT_MBOX) & bit;
	if (unanswered_alive) {
		/* This late reply belongs to our previous probe, never the new one. */
		if (!pending)
			return -EBUSY;
		ret = mtk_mbox_clr_irq(&mc->mbox, MCUPM_PLT_MBOX, bit);
		if (ret)
			goto mbox_error;
		/* Complete the stale IRQ clear before submitting another request. */
		mb();
		unanswered_alive = false;
	} else if (pending) {
		return -EBUSY;
	}

	if (inner && readl(mc->ptp3))
		return -EBUSY;
	reinit_completion(&mc->alive_done);
	mc->alive_drained = false;
	ret = mtk_mbox_write(&mc->mbox, MCUPM_PLT_MBOX, 0, (void *)request, bytes);
	if (ret)
		goto mbox_error;
	if (inner) {
		writel(0, mc->ptp3 + 0x04);
		writel(UV32_CMD, mc->ptp3 + 0x08);
		writel(inner[0], mc->ptp3 + 0x0c);
		writel(inner[1], mc->ptp3 + 0x10);
		writel(inner[2], mc->ptp3 + 0x14);
		wmb();
		writel(1, mc->ptp3);
	}
	/* Commit the payload before ringing the MCUPM doorbell. */
	mb();
	ret = mtk_mbox_trigger_irq(&mc->mbox, MCUPM_PLT_MBOX, bit);
	if (ret)
		goto mbox_error;

	enable_irq(mc->platform_irq);
	pending = wait_for_completion_timeout(&mc->alive_done,
		msecs_to_jiffies(inner ? UV32_TIMEOUT_MS : MCUPM_ALIVE_TIMEOUT_MS));
	/* Synchronize the ISR even if the wait expired at the ACK boundary. */
	disable_irq(mc->platform_irq);
	unanswered_alive = !mc->alive_drained;
	if (!pending) {
		dev_err(mc->dev, "platform IPI %#x outer ACK timeout\n", request[0]);
		return -ETIMEDOUT;
	}
	ret = mc->alive_error;
	if (ret)
		goto mbox_error;
	if (mc->alive_ack != (inner ? 0 : MCUPM_ALIVE_ACK))
		return dev_err_probe(mc->dev, -EREMOTEIO, "platform IPI %#x ACK %d\n",
				     request[0], mc->alive_ack);
	if (inner) {
		ret = readl_poll_timeout(mc->ptp3 + 0x04, ack, ack == 1 && !readl(mc->ptp3),
					 100, UV32_TIMEOUT_MS * 1000);
		if (ret)
			return dev_err_probe(mc->dev, ret, "UV32 inner ACK timeout\n");
		*result = readl(mc->ptp3 + 0x18);
		if (*result == U32_MAX)
			return -EREMOTEIO;
	} else {
		dev_info(mc->dev, "MCUPM Alive ACK %d\n", mc->alive_ack);
	}
	return 0;

mbox_error:
	return dev_err_probe(mc->dev, -EIO, "MCUPM platform mailbox status %d\n", ret);
}

static u64 rothko_mcupm_read_tick(struct cyclecounter *cc)
{
	return arch_timer_read_counter();
}

static int uv32_xfer(struct rothko_mcupm *mc, enum uv32_command cmd, u32 domain,
		     u32 scale, u32 *result)
{
	u32 request[] = { MCUPM_PTP3_CMD, 0, 0, 0 };
	u32 inner[] = { cmd, domain, scale };

	return rothko_mcupm_platform_xfer(mc, request, sizeof(request), inner, result);
}

static const u8 uv32_topology[] = { 1, 1, 1, 1, 4, 4, 4, 3, 2 };

static int uv32_scale(struct rothko_mcupm *mc, unsigned int domain, u32 *scale)
{
	int ret = uv32_xfer(mc, UV32_QUERY, domain, 0, scale);

	if (!ret && *scale > 32)
		return -EPROTO;
	return ret;
}

static ssize_t uv32_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct rothko_mcupm *mc = container_of(kobj, struct rothko_uv_dir, kobj)->mc;
	unsigned int domain;
	u32 scale;
	int ret, pos = 0;

	mutex_lock(&mc->platform_lock);
	if (!strcmp(attr->attr.name, "status")) {
		pos = sysfs_emit(buf, "api=0x%08x entries=%zu domains=4 topology=1,1,1,1,4,4,4,3,2\n",
				 UV32_API, ARRAY_SIZE(uv32_topology));
		for (domain = 1; domain <= 4; domain++) {
			ret = uv32_scale(mc, domain, &scale);
			if (ret)
				break;
			pos += sysfs_emit_at(buf, pos, "domain_%u=%u/32\n", domain, scale);
		}
	} else {
		domain = attr->attr.name[sizeof("domain_") - 1] - '0';
		ret = uv32_scale(mc, domain, &scale);
		if (!ret)
			pos = sysfs_emit(buf, "%u/32\n", scale);
	}
	mutex_unlock(&mc->platform_lock);
	return ret ? ret : pos;
}

static ssize_t uv32_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	struct rothko_mcupm *mc = container_of(kobj, struct rothko_uv_dir, kobj)->mc;
	u32 domain = 0, scale = 0, reply, expected;
	char args[48], *domain_text, *scale_text;
	enum uv32_command cmd;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!strcmp(attr->attr.name, "reset")) {
		if (!sysfs_streq(buf, "1"))
			return -EINVAL;
		cmd = UV32_RESET;
		expected = 0x20000;
	} else {
		if (!strcmp(attr->attr.name, "set")) {
			if (count >= sizeof(args))
				return -EINVAL;
			strscpy(args, buf, sizeof(args));
			domain_text = strim(args);
			scale_text = strpbrk(domain_text, " \t");
			if (!scale_text)
				return -EINVAL;
			*scale_text++ = '\0';
			if (kstrtou32(domain_text, 10, &domain) ||
			    kstrtou32(skip_spaces(scale_text), 10, &scale))
				return -EINVAL;
		} else {
			domain = attr->attr.name[sizeof("domain_") - 1] - '0';
			if (kstrtou32(buf, 10, &scale))
				return -EINVAL;
		}
		if (domain < 1 || domain > 4 || scale > 32)
			return -ERANGE;
		cmd = UV32_SET;
		expected = 0x10000 | (domain << 8) | scale;
	}
	mutex_lock(&mc->platform_lock);
	ret = uv32_xfer(mc, cmd, domain, scale, &reply);
	if (!ret && reply != expected)
		ret = -EPROTO;
	if (!ret && cmd == UV32_SET) {
		ret = uv32_scale(mc, domain, &reply);
		if (!ret && reply != scale)
			ret = -EIO;
	}
	mutex_unlock(&mc->platform_lock);
	return ret ? ret : count;
}

static struct kobj_attribute uv32_status = __ATTR(status, 0444, uv32_show, NULL);
static struct kobj_attribute uv32_set = __ATTR(set, 0200, NULL, uv32_store);
static struct kobj_attribute uv32_reset = __ATTR(reset, 0200, NULL, uv32_store);
static struct kobj_attribute uv32_domains[] = {
	__ATTR(domain_1, 0644, uv32_show, uv32_store),
	__ATTR(domain_2, 0644, uv32_show, uv32_store),
	__ATTR(domain_3, 0644, uv32_show, uv32_store),
	__ATTR(domain_4, 0644, uv32_show, uv32_store),
};
static struct attribute *uv32_attrs[] = {
	&uv32_status.attr, &uv32_set.attr, &uv32_reset.attr,
	&uv32_domains[0].attr, &uv32_domains[1].attr,
	&uv32_domains[2].attr, &uv32_domains[3].attr, NULL,
};
ATTRIBUTE_GROUPS(uv32);

static void uv32_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct rothko_uv_dir, kobj));
}

static const struct kobj_type uv32_ktype = {
	.release = uv32_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = uv32_groups,
};

static int rothko_uv32_init(struct platform_device *pdev, struct rothko_mcupm *mc)
{
	struct resource *mbox = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mbox0_base");
	struct resource window = DEFINE_RES_MEM_NAMED(mbox->start - (0x5f600 - 0x5f350),
						      7 * sizeof(u32), "mcupm-ptp3");
	struct rothko_uv_dir *dir;
	unsigned int entry;
	u32 reply;
	int ret;

	mc->ptp3 = devm_ioremap_resource(mc->dev, &window);
	if (IS_ERR(mc->ptp3))
		return PTR_ERR(mc->ptp3);
	mutex_lock(&mc->platform_lock);
	ret = uv32_xfer(mc, UV32_QUERY, 0xff, 0, &reply);
	if (!ret && reply != UV32_API) {
		dev_info(mc->dev, "UV32 API mismatch: %#x\n", reply);
		ret = -EPROTONOSUPPORT;
	}
	for (entry = 0; !ret && entry < ARRAY_SIZE(uv32_topology); entry++) {
		ret = uv32_xfer(mc, UV32_CORE_DOMAIN, entry, 0, &reply);
		if (!ret && reply != uv32_topology[entry]) {
			dev_warn(mc->dev, "UV32 topology mismatch at %u: %u\n", entry, reply);
			ret = -EPROTO;
		}
	}
	mutex_unlock(&mc->platform_lock);
	if (ret)
		return ret;
	dir = kzalloc(sizeof(*dir), GFP_KERNEL);
	if (!dir)
		return -ENOMEM;
	dir->mc = mc;
	ret = kobject_init_and_add(&dir->kobj, &uv32_ktype, kernel_kobj, "mtk_hwloop_uv");
	if (ret)
		kobject_put(&dir->kobj);
	else
		mc->uv_dir = dir;
	return ret;
}

static int rothko_mcupm_publish(struct rothko_mcupm *mc, bool freeze)
{
	static const u8 slots[] = {
		MCUPM_TS_TICK_H, MCUPM_TS_TICK_L, MCUPM_TS_TIME_L, MCUPM_TS_TIME_H,
	};
	unsigned long flags;
	u32 header, words[ARRAY_SIZE(slots)];
	u64 ns, tick;
	u8 version;
	int i, ret = 0;

	/* Probe, suspend, resume and refresh are serialized by the timer lifecycle. */
	local_irq_save(flags);
	ns = timecounter_read(&mc->tc);
	tick = mc->tc.cycle_last;
	version = (mc->version + 1) & FIELD_MAX(MCUPM_TS_VERSION);
	header = FIELD_PREP(MCUPM_TS_VERSION, version);
	if (freeze)
		header |= MCUPM_TS_FREEZE;

	words[0] = header | (upper_32_bits(tick) & MCUPM_TS_VALUE_H);
	words[1] = lower_32_bits(tick);
	words[2] = lower_32_bits(ns);
	words[3] = header | (upper_32_bits(ns) & MCUPM_TS_VALUE_H);

	/* Firmware ABI publishes the timestamp header last. */
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		if (mtk_mbox_check_send_irq(&mc->mbox, MCUPM_TS_MBOX, MCUPM_TS_PIN)) {
			ret = MBOX_PIN_BUSY;
			break;
		}
		ret = mtk_mbox_write(&mc->mbox, MCUPM_TS_MBOX, slots[i],
				     &words[i], MBOX_SLOT_SIZE);
		if (ret)
			break;
		/* Keep the producer word order visible to the firmware reader. */
		mb();
	}
	if (!ret)
		mc->version = version;
	local_irq_restore(flags);

	if (ret) {
		dev_err_ratelimited(mc->dev, "MCUPM timesync word %u, freeze %u: mailbox status %d\n",
				    slots[i], freeze, ret);
		return ret == MBOX_PIN_BUSY ? -EBUSY : -EIO;
	}

	dev_dbg(mc->dev, "MCUPM timesync published tick %#llx ns %llu freeze %u version %u\n",
		tick, ns, freeze, mc->version);
	return 0;
}

static void rothko_mcupm_refresh_work(struct work_struct *work)
{
	struct rothko_mcupm *mc = container_of(work, struct rothko_mcupm, refresh_work);

	/* There is no PM caller to return a periodic refresh error to. */
	rothko_mcupm_publish(mc, false);
}

static enum hrtimer_restart rothko_mcupm_refresh_timer(struct hrtimer *timer)
{
	struct rothko_mcupm *mc = container_of(timer, struct rothko_mcupm, refresh_timer);

	hrtimer_forward_now(timer, mc->refresh_period);
	schedule_work(&mc->refresh_work);
	return HRTIMER_RESTART;
}

static void rothko_mcupm_start_refresh(struct rothko_mcupm *mc)
{
	hrtimer_start(&mc->refresh_timer, mc->refresh_period, HRTIMER_MODE_REL);
}

static void rothko_mcupm_stop_refresh(struct rothko_mcupm *mc)
{
	hrtimer_cancel(&mc->refresh_timer);
	cancel_work_sync(&mc->refresh_work);
}

static int rothko_mcupm_suspend(struct device *dev)
{
	struct rothko_mcupm *mc = dev_get_drvdata(dev);
	int ret;

	rothko_mcupm_stop_refresh(mc);
	ret = rothko_mcupm_publish(mc, true);
	if (ret) {
		/* Suspend failed; this device need not get a resume callback. */
		rothko_mcupm_publish(mc, false);
		rothko_mcupm_start_refresh(mc);
	} else {
		mutex_lock(&mc->platform_lock);
		mc->suspended = true;
		mutex_unlock(&mc->platform_lock);
	}

	return ret;
}

static int rothko_mcupm_resume(struct device *dev)
{
	struct rothko_mcupm *mc = dev_get_drvdata(dev);
	int ret;

	/* sched_clock stops across suspend, unlike the architectural counter. */
	timecounter_init(&mc->tc, &mc->cc, sched_clock());
	ret = rothko_mcupm_publish(mc, false);
	rothko_mcupm_start_refresh(mc);
	mutex_lock(&mc->platform_lock);
	mc->suspended = false;
	mutex_unlock(&mc->platform_lock);
	return ret;
}

static const struct dev_pm_ops rothko_mcupm_pm_ops = {
	.suspend = pm_sleep_ptr(rothko_mcupm_suspend),
	.resume = pm_sleep_ptr(rothko_mcupm_resume),
};

static int rothko_mcupm_probe(struct platform_device *pdev)
{
	u32 alive[] = { MCUPM_ALIVE_CMD, 0, 0 };
	struct device *dev = &pdev->dev;
	struct device_node *rmem;
	struct rothko_mcupm *mc;
	u64 max_cycles, wrap;
	u32 rate, header;
	int ret;

	if (!rothko_mcupm_is_board())
		return -ENODEV;

	/* The observed rothko ABI selects Alive (not PLT_INIT with a logger). */
	rmem = of_find_compatible_node(NULL, NULL, "mediatek,reserve-memory-mcupm_share");
	if (rmem) {
		of_node_put(rmem);
		return dev_err_probe(dev, -EOPNOTSUPP, "MCUPM log shared memory not supported\n");
	}

	rate = arch_timer_get_cntfrq();
	if (!rate)
		return dev_err_probe(dev, -EINVAL, "MCUPM timesync counter rate is zero\n");

	mc = devm_kzalloc(dev, sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;
	mc->dev = dev;

	ret = rothko_mcupm_map(pdev, mc);
	if (ret)
		return dev_err_probe(dev, ret, "MCUPM mailbox resources\n");
	mc->platform_irq = platform_get_irq_byname(pdev, "mbox0");
	if (mc->platform_irq < 0)
		return mc->platform_irq;
	init_completion(&mc->alive_done);
	mutex_init(&mc->platform_lock);
	ret = devm_request_irq(dev, mc->platform_irq, rothko_mcupm_alive_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), mc);
	if (ret)
		return dev_err_probe(dev, ret, "MCUPM platform IRQ\n");
	mutex_lock(&mc->platform_lock);
	ret = rothko_mcupm_platform_xfer(mc, alive, sizeof(alive), NULL, NULL);
	mutex_unlock(&mc->platform_lock);
	if (ret)
		return dev_err_probe(dev, ret, "MCUPM Alive transaction failed\n");

	/* Preserve the version protocol when rebinding to a running MCUPM. */
	ret = mtk_mbox_read(&mc->mbox, MCUPM_TS_MBOX, MCUPM_TS_TIME_H,
			    &header, sizeof(header));
	if (ret)
		return dev_err_probe(dev, -EIO, "MCUPM timesync header: mailbox status %d\n", ret);
	mc->version = FIELD_GET(MCUPM_TS_VERSION, header);
	mc->cc.read = rothko_mcupm_read_tick;
	mc->cc.mask = CLOCKSOURCE_MASK(56);
	clocks_calc_mult_shift(&mc->cc.mult, &mc->cc.shift, rate, NSEC_PER_SEC, 3600);
	max_cycles = min(div_u64(U64_MAX, mc->cc.mult), mc->cc.mask);
	wrap = clocksource_cyc2ns(max_cycles, mc->cc.mult, mc->cc.shift) / 2;
	mc->refresh_period = ns_to_ktime(wrap);

	INIT_WORK(&mc->refresh_work, rothko_mcupm_refresh_work);
	hrtimer_setup(&mc->refresh_timer, rothko_mcupm_refresh_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timecounter_init(&mc->tc, &mc->cc, sched_clock());
	ret = rothko_mcupm_publish(mc, false);
	if (ret)
		return dev_err_probe(dev, ret, "MCUPM initial timesync failed\n");

	platform_set_drvdata(pdev, mc);
	rothko_mcupm_start_refresh(mc);
	ret = rothko_uv32_init(pdev, mc);
	if (ret)
		dev_info(dev, "UV32 unavailable: %d\n", ret);
	dev_info(dev, "MCUPM timesync base published tick %#llx ns %llu version %u\n",
		 mc->tc.cycle_last, mc->tc.nsec, mc->version);
	return 0;
}

static void rothko_mcupm_stop(struct platform_device *pdev)
{
	struct rothko_mcupm *mc = platform_get_drvdata(pdev);

	if (mc->uv_dir) {
		kobject_del(&mc->uv_dir->kobj);
		kobject_put(&mc->uv_dir->kobj);
		mc->uv_dir = NULL;
	}
	rothko_mcupm_stop_refresh(mc);
}

static const struct of_device_id rothko_mcupm_of_match[] = {
	{ .compatible = "mediatek,mcupm" },
	{ }
};
MODULE_DEVICE_TABLE(of, rothko_mcupm_of_match);

static struct platform_driver rothko_mcupm_driver = {
	.probe = rothko_mcupm_probe,
	.remove = rothko_mcupm_stop,
	.shutdown = rothko_mcupm_stop,
	.driver = {
		.name = "mtk-rothko-mcupm",
		.of_match_table = rothko_mcupm_of_match,
		.pm = pm_sleep_ptr(&rothko_mcupm_pm_ops),
	},
};
module_platform_driver(rothko_mcupm_driver);

MODULE_DESCRIPTION("MediaTek MCUPM timesync client for Xiaomi rothko");
MODULE_LICENSE("GPL");
