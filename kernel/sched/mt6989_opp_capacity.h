/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_SCHED_MT6989_OPP_CAPACITY_H
#define _KERNEL_SCHED_MT6989_OPP_CAPACITY_H

#include <linux/types.h>

#define MT6989_NR_DOMAINS	3
#define MT6989_MAX_OPPS		32

struct mt6989_opp_capacity {
	u32 frequency;	/* kHz */
	u16 capacity;	/* Scheduler capacity scale */
};

struct mt6989_opp_capacity_domain {
	unsigned int nr_opps;
	struct mt6989_opp_capacity opps[MT6989_MAX_OPPS];
};

struct mt6989_opp_capacity_table {
	/* Indexed by the CPU's DT performance-domains argument. */
	struct mt6989_opp_capacity_domain domains[MT6989_NR_DOMAINS];
};

/*
 * Requires CONFIG_MT6989_OPP_CAPACITY. Call from sleepable context with
 * caller-owned, unpublished storage. The table is only filled on success.
 * OPPs are ordered by descending frequency. This is the base table, before
 * any workload-specific capacity replacement or gearless interpolation.
 */
int mt6989_opp_capacity_read(struct mt6989_opp_capacity_table *table);

#endif /* _KERNEL_SCHED_MT6989_OPP_CAPACITY_H */
