/* SPDX-License-Identifier: GPL-2.0 */
#ifndef N12_VIS_DISPLAY_H
#define N12_VIS_DISPLAY_H

#include <linux/rothko_hwid.h>
#include <linux/types.h>

bool is_mi_dev_support_nova(void);
void nvt_get_lcm_id_notify(unsigned int (*get_id)(void));
void vis_display_ops_osc_tricking_notify(int (*osc_tricking)(unsigned int setting));
int vis_fps_notify(unsigned int fps);
int vis_dsi_rate_notify(unsigned int rate);
int n12_vis_panel_power(bool enable);

/* Actual VIS SPI protocol: no register cache or fabricated READY response. */
int n12_vis_read_register(u32 address, u32 *value);
int n12_vis_write_register(u32 address, u32 value);
int n12_vis_write_memory(u32 address, const void *data, size_t length);

#endif
