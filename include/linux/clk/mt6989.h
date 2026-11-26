/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CLK_MT6989_H
#define _LINUX_CLK_MT6989_H

#include <linux/types.h>

struct clk_ops;

struct mtk_mux_user {
	int id;
	const char *name;
	const char *target_name;
	unsigned long rate;
	const struct clk_ops *ops;
	unsigned int flags;
	u8 target_id;
	unsigned long undo_rate;
};

#endif /* _LINUX_CLK_MT6989_H */
