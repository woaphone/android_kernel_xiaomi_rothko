/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */
#ifndef MTK_TYPEC_MUX_SWITCH_H
#define MTK_TYPEC_MUX_SWITCH_H

#include <linux/usb/typec_mux.h>

struct typec_switch_dev *mtk_typec_switch_register(struct device *dev,
			const struct typec_switch_desc *desc);
void mtk_typec_switch_unregister(struct typec_switch_dev *sw);
struct typec_mux_dev *mtk_typec_mux_register(struct device *dev,
			const struct typec_mux_desc *desc);
void mtk_typec_mux_unregister(struct typec_mux_dev *mux);

#if IS_REACHABLE(CONFIG_DRM_MEDIATEK_MT6989)
void mtk_dp_SWInterruptSet(int bstatus);
void mtk_dp_aux_swap_enable(bool enable);
void mtk_dp_set_pin_assign(u8 type);
#endif

#endif /* MTK_TYPEC_MUX_SWITCH_H */
