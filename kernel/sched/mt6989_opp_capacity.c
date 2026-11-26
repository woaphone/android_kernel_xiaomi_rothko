// SPDX-License-Identifier: GPL-2.0

#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sched.h>

#include "mt6989_opp_capacity.h"

#define MT6989_CSRAM_LUT_OFFSET		0x10
#define MT6989_CSRAM_DOMAIN_STRIDE	0x120
#define MT6989_CSRAM_LUT_FREQ		GENMASK(11, 0)
#define MT6989_CSRAM_CAP_OFFSET		0xfa0
#define MT6989_CSRAM_CAP_SIZE		0x100
#define MT6989_CPU_DVFS_MAGIC		0x55aa55aa
#define MT6989_CPU_DVFS_STATE		0x9c

static void __iomem *mt6989_map_sram(struct device_node *node, int index,
				     unsigned int min_size)
{
	struct resource res;
	void __iomem *base;
	int ret;

	ret = of_address_to_resource(node, index, &res);
	if (ret)
		return ERR_PTR(ret);
	if (resource_size(&res) < min_size)
		return ERR_PTR(-EINVAL);

	/* CSRAM contains the CPUFreq driver's reserved performance domains. */
	base = of_iomap(node, index);
	return base ?: ERR_PTR(-ENOMEM);
}

static int mt6989_capacity_check_firmware(struct device_node *hvfs)
{
	struct device_node *node;
	void __iomem *base;
	bool ready;

	node = of_parse_phandle(hvfs, "csram-sys-base", 0);
	if (!node)
		return -ENODEV;

	base = mt6989_map_sram(node, 0, MT6989_CPU_DVFS_STATE + sizeof(u32));
	of_node_put(node);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* Use the same firmware readiness check as mtk-cpufreq-hw. */
	ready = readl(base) == MT6989_CPU_DVFS_MAGIC &&
		readl(base + MT6989_CPU_DVFS_STATE);
	iounmap(base);

	return ready ? 0 : -EAGAIN;
}

static int mt6989_capacity_read_domain(struct mt6989_opp_capacity_domain *domain,
				       void __iomem *lut, void __iomem *caps,
				       unsigned int *offset)
{
	unsigned int i, prev_freq = 0;
	u16 prev_cap = SCHED_CAPACITY_SCALE;

	for (i = 0; i < MT6989_MAX_OPPS; i++) {
		u32 row = readl_relaxed(lut + i * sizeof(row));
		u32 freq = FIELD_GET(MT6989_CSRAM_LUT_FREQ, row) * 1000;
		u16 cap;

		/* The hardware LUT ends at the first adjacent duplicate. */
		if (freq == prev_freq)
			break;
		if (!freq || (i && freq > prev_freq))
			return -EINVAL;

		/* Leave room after each OPP for the cluster's end marker. */
		if (*offset + 2 * sizeof(cap) > MT6989_CSRAM_CAP_SIZE)
			return -EINVAL;
		cap = ioread16(caps + *offset);
		if (!cap || cap > prev_cap)
			return -EINVAL;

		domain->opps[i].frequency = freq;
		domain->opps[i].capacity = cap;
		prev_freq = freq;
		prev_cap = cap;
		*offset += sizeof(cap);
	}

	/* Firmware repeats the last capacity once before the next domain. */
	if (!i || ioread16(caps + *offset) != prev_cap)
		return -EINVAL;

	*offset += sizeof(prev_cap);
	domain->nr_opps = i;
	return 0;
}

int mt6989_opp_capacity_read(struct mt6989_opp_capacity_table *table)
{
	struct mt6989_opp_capacity_table snapshot = {};
	struct device_node *hvfs;
	void __iomem *base;
	unsigned int domain, offset = 0;
	int index, ret;

	if (!of_machine_is_compatible("mediatek,MT6989"))
		return -ENODEV;

	hvfs = of_find_compatible_node(NULL, NULL, "mediatek,cpufreq-hybrid");
	if (!hvfs)
		return -ENODEV;

	ret = mt6989_capacity_check_firmware(hvfs);
	if (ret)
		goto put_node;

	index = of_property_match_string(hvfs, "reg-names", "CSRAM");
	if (index < 0) {
		ret = index;
		goto put_node;
	}

	base = mt6989_map_sram(hvfs, index,
			       MT6989_CSRAM_CAP_OFFSET + MT6989_CSRAM_CAP_SIZE);
	if (IS_ERR(base)) {
		ret = PTR_ERR(base);
		goto put_node;
	}

	/* Read both halves from CSRAM without acquiring transient policies. */
	for (domain = 0; domain < MT6989_NR_DOMAINS; domain++) {
		void __iomem *lut = base + MT6989_CSRAM_LUT_OFFSET +
				    domain * MT6989_CSRAM_DOMAIN_STRIDE;

		ret = mt6989_capacity_read_domain(&snapshot.domains[domain], lut,
						  base + MT6989_CSRAM_CAP_OFFSET,
						  &offset);
		if (ret)
			break;
	}

	iounmap(base);
	if (!ret)
		*table = snapshot;

put_node:
	of_node_put(hvfs);
	return ret;
}
