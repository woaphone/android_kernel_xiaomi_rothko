/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ROTHKO_MI_DISP_DDIC_H__
#define __ROTHKO_MI_DISP_DDIC_H__

#include <linux/bits.h>
#include <linux/types.h>

/* Xiaomi table layout recovered from the Android 15 N12/DRM modules. */
struct mi_ddic_cmd {
	u32 command;
	u8 count;
	u8 para_list[128];
};

#define MI_DDIC_WAIT_TE		BIT(0)
#define MI_DDIC_BLOCKING		BIT(1)
#define MI_DDIC_USE_LPM		BIT(2)
#define MI_DDIC_SKIP_LOCK	BIT(3)
#define MI_DDIC_MAX_CMDS		40

/* With WAIT_TE, stock bit 3 is passed as blocking, not as skip_lock. */
int mi_disp_panel_ddic_send_cmd(const struct mi_ddic_cmd *table,
		unsigned int count, unsigned int flags);

#endif
