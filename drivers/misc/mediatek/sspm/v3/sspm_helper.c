// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2011-2015, 2021 MediaTek Inc. */

#include <linux/clocksource.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>
#include <linux/scmi_protocol.h>
#include <linux/soc/mediatek/tinysys-scmi.h>
#include <linux/timecounter.h>
#include <linux/workqueue.h>
#include <asm/arch_timer.h>

#define _SSPM_INTERNAL_
#include "sspm_reservedmem_define.h"

#define SSPM_TS_TICK_H  (20 * sizeof(u32))
#define SSPM_TS_TICK_L  (21 * sizeof(u32))
#define SSPM_TS_NS_H    (22 * sizeof(u32))
#define SSPM_TS_NS_L    (23 * sizeof(u32))
#define SSPM_TS_PERIOD  msecs_to_jiffies(4000000)

struct sspm_v3 {
	const struct scmi_protocol_handle *ph;
	u32 plt_id;
	void __iomem *sram;
	void __iomem *mbox;
	u32 share_base;
	u32 share_size;
	struct cyclecounter cycles;
	struct timecounter time;
	struct delayed_work refresh;
	u32 version;
};

static struct sspm_v3 *sspm;

phys_addr_t sspm_reserve_mem_get_phys(unsigned int id)
{
	return smp_load_acquire(&sspm) && id < NUMS_MEM_ID ?
		sspm_reserve_mblock[id].start_phys : 0;
}
EXPORT_SYMBOL_GPL(sspm_reserve_mem_get_phys);

phys_addr_t sspm_reserve_mem_get_virt(unsigned int id)
{
	return smp_load_acquire(&sspm) && id < NUMS_MEM_ID ?
		sspm_reserve_mblock[id].start_virt : 0;
}
EXPORT_SYMBOL_GPL(sspm_reserve_mem_get_virt);

phys_addr_t sspm_reserve_mem_get_size(unsigned int id)
{
	return smp_load_acquire(&sspm) && id < NUMS_MEM_ID ?
		sspm_reserve_mblock[id].size : 0;
}
EXPORT_SYMBOL_GPL(sspm_reserve_mem_get_size);

phys_addr_t sspm_sbuf_get(unsigned int offset)
{
	struct sspm_v3 *data = smp_load_acquire(&sspm);

	if (!data || offset < data->share_base ||
	    offset - data->share_base >= data->share_size)
		return 0;
	return (phys_addr_t)(uintptr_t)(data->sram + offset);
}
EXPORT_SYMBOL_GPL(sspm_sbuf_get);

static u64 sspm_cycles_read(struct cyclecounter *cc)
{
	return arch_timer_read_counter();
}

static void sspm_timesync(struct sspm_v3 *data, bool freeze)
{
	u64 ns = timecounter_read(&data->time);
	u64 tick = data->time.cycle_last;
	u32 header;

	data->version = (data->version + 1) & 7;
	header = (data->version << 28) | (freeze ? BIT(31) : 0);
	/* The firmware samples the header at both ends of this update. */
	writel(upper_32_bits(tick) | header, data->mbox + SSPM_TS_TICK_H);
	mb();
	writel(lower_32_bits(tick), data->mbox + SSPM_TS_TICK_L);
	mb();
	writel(lower_32_bits(ns), data->mbox + SSPM_TS_NS_L);
	mb();
	writel(upper_32_bits(ns) | header, data->mbox + SSPM_TS_NS_H);
	mb();
}

static void sspm_refresh(struct work_struct *work)
{
	struct sspm_v3 *data = container_of(to_delayed_work(work),
					   struct sspm_v3, refresh);

	sspm_timesync(data, false);
	schedule_delayed_work(&data->refresh, SSPM_TS_PERIOD);
}

static int sspm_suspend(struct device *dev)
{
	struct sspm_v3 *data = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&data->refresh);
	sspm_timesync(data, true);
	return 0;
}

static int sspm_resume(struct device *dev)
{
	struct sspm_v3 *data = dev_get_drvdata(dev);

	timecounter_init(&data->time, &data->cycles, sched_clock());
	sspm_timesync(data, false);
	schedule_delayed_work(&data->refresh, SSPM_TS_PERIOD);
	return 0;
}
static DEFINE_SIMPLE_DEV_PM_OPS(sspm_pm_ops, sspm_suspend, sspm_resume);

static int sspm_init_shared_memory(struct device *dev, struct sspm_v3 *data)
{
	struct device_node *node;
	struct reserved_mem *rmem;
	void __iomem *base;
	u32 phys, size;
	phys_addr_t offset = 0;
	unsigned int i;
	int ret;

	ret = of_property_read_u32(dev->of_node, "sspm-res-ram-start", &phys);
	if (ret)
		return ret;
	ret = of_property_read_u32(dev->of_node, "sspm-res-ram-size", &size);
	if (ret)
		return ret;

	node = of_find_compatible_node(NULL, NULL, "mediatek,sspm_ap-shared");
	if (!node)
		return -ENODEV;
	rmem = of_reserved_mem_lookup(node);
	ret = !rmem || !of_property_read_bool(node, "no-map") ||
	      rmem->base != phys || rmem->size != size ? -EINVAL : 0;
	of_node_put(node);
	if (ret)
		return ret;

	for (i = 0; i < NUMS_MEM_ID; i++)
		offset += sspm_reserve_mblock[i].size;
	if (offset > size)
		return -EINVAL;
	base = devm_ioremap_wc(dev, phys, size);
	if (!base)
		return -ENOMEM;
	offset = 0;
	for (i = 0; i < NUMS_MEM_ID; i++) {
		sspm_reserve_mblock[i].start_phys = phys + offset;
		sspm_reserve_mblock[i].start_virt = (phys_addr_t)(uintptr_t)(base + offset);
		offset += sspm_reserve_mblock[i].size;
	}

	/* v3 PLT control block without the optional logger. */
	size = sspm_reserve_mblock[SSPM_MEM_ID].size;
	writel(PLT_INIT, base);
	writel(3 * sizeof(u32), base + 4);
	writel(size, base + 8);
	writel(PLT_INIT, base + size - sizeof(u32));
	wmb();

	return scmi_tinysys_common_set(data->ph, data->plt_id, PLT_INIT,
				       phys, size, 0, 0);
}

static ssize_t sspm_alive_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct sspm_v3 *data = dev_get_drvdata(dev);
	int ret;

	ret = scmi_tinysys_common_set(data->ph, data->plt_id, 0xdead, 0, 0, 0, 0);
	return sysfs_emit(buf, "%s\n", ret ? "Dead" : "Alive");
}
static DEVICE_ATTR_RO(sspm_alive);

static struct attribute *sspm_attrs[] = {
	&dev_attr_sspm_alive.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sspm);

static int sspm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct scmi_tinysys_info_st *info = get_scmi_tinysys_info();
	struct resource *res;
	struct sspm_v3 *data;
	int ret;

	if (!info)
		return -EPROBE_DEFER;
	if (READ_ONCE(sspm))
		return -EBUSY;
	if (!device_link_add(dev, &info->sdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->ph = info->ph;
	ret = of_property_read_u32(info->sdev->dev.of_node, "scmi-plt", &data->plt_id);
	if (ret)
		return dev_err_probe(dev, ret, "missing Tinysys PLT feature\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sspm_base");
	data->sram = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->sram))
		return PTR_ERR(data->sram);
	ret = of_property_read_u32(dev->of_node, "sspm-share-region-base", &data->share_base);
	if (ret)
		return ret;
	ret = of_property_read_u32(dev->of_node, "sspm-share-region-size", &data->share_size);
	if (ret)
		return ret;
	if (!data->share_size || data->share_base >= resource_size(res) ||
	    data->share_size > resource_size(res) - data->share_base)
		return -EINVAL;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mbox_share");
	if (!res || resource_size(res) < SSPM_TS_NS_L + sizeof(u32))
		return -EINVAL;
	data->mbox = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->mbox))
		return PTR_ERR(data->mbox);

	ret = sspm_init_shared_memory(dev, data);
	if (ret)
		return dev_err_probe(dev, ret, "SSPM platform handshake failed\n");

	data->cycles.read = sspm_cycles_read;
	data->cycles.mask = CLOCKSOURCE_MASK(56);
	clocks_calc_mult_shift(&data->cycles.mult, &data->cycles.shift,
			      arch_timer_get_cntfrq(), NSEC_PER_SEC, 3600);
	INIT_DELAYED_WORK(&data->refresh, sspm_refresh);
	platform_set_drvdata(pdev, data);
	sspm_resume(dev);
	smp_store_release(&sspm, data);
	dev_info(dev, "SSPM v3 platform service ready\n");
	return 0;
}

static void sspm_remove(struct platform_device *pdev)
{
	WRITE_ONCE(sspm, NULL);
	sspm_suspend(&pdev->dev);
}

static const struct of_device_id sspm_of_match[] = {
	{ .compatible = "mediatek,sspm" },
	{ }
};
MODULE_DEVICE_TABLE(of, sspm_of_match);

static struct platform_driver sspm_driver = {
	.probe = sspm_probe,
	.remove = sspm_remove,
	.driver = {
		.name = "sspm",
		.of_match_table = sspm_of_match,
		.pm = pm_sleep_ptr(&sspm_pm_ops),
		.dev_groups = sspm_groups,
	},
};
module_platform_driver(sspm_driver);

MODULE_DESCRIPTION("MediaTek SCMI SSPM v3 platform service");
MODULE_LICENSE("GPL v2");
