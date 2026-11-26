/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_DVFSRC_ROTHKO_HELPER_H
#define __MTK_DVFSRC_ROTHKO_HELPER_H

#include <linux/types.h>

struct device;

struct dvfsrc_opp {
	u32 vcore_opp;
	u32 dram_opp;
};

struct dvfsrc_opp_desc {
	const struct dvfsrc_opp *opps;
	u32 num_opp;
};

#define MT6989_FORCE_RELEASE	999

const struct dvfsrc_opp_desc *mt6989_dvfsrc_get_opps(struct device *dev);
int mt6989_dvfsrc_force_opp(struct device *dev, u32 index);

#endif
