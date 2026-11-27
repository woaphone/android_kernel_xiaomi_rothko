/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SC858X_CHARGER_H_
#define _SC858X_CHARGER_H_

#include <linux/bits.h>
#include <linux/types.h>
#include "charger_class.h"

struct i2c_client;

#define SC858X_MODE_4_1		0
#define SC858X_MODE_2_1		1
#define SC858X_MODE_1_1		2

#define SC858X_FAULT_MISC_MASK	BIT(0)
#define SC858X_FAULT_TSBAT_BIT	1
#define SC858X_FAULT_IBUS_UCP_FALL_BIT	2
#define SC858X_FAULT_IBUS_OCP	BIT(4)
#define SC858X_FAULT_OVP	BIT(5)
#define SC858X_FAULT_IBAT_OCP	BIT(6)
#define SC858X_FAULT_VBAT_OVP	BIT(7)

#define SC858X_WORK_STANDALONE	0
#define SC858X_WORK_MASTER	1
#define SC858X_WORK_SLAVE	2

int sc858x_read_legacy(struct i2c_client *client, u8 reg);
int sc858x_cp_set_mode(struct charger_device *chgdev, int mode);
int sc858x_cp_prepare_pps(struct charger_device *chgdev, bool after_detach);
int sc858x_cp_enable_adc(struct charger_device *chgdev, bool enable);
int sc858x_cp_get_vbat(struct charger_device *chgdev, u32 *vbat_mv);
int sc858x_cp_get_vbus_mv(struct charger_device *chgdev, u32 *vbus_mv);
int sc858x_cp_get_ibus_ma(struct charger_device *chgdev, u32 *ibus_ma);
int sc858x_cp_get_ibat_ma(struct charger_device *chgdev, u32 *ibat_ma);
int sc858x_cp_get_tdie_c(struct charger_device *chgdev, int *tdie_c);
int sc858x_cp_get_adc_enabled(struct charger_device *chgdev, bool *enabled);
int sc858x_cp_get_fault_type(struct charger_device *chgdev, u32 *fault);
int sc858x_cp_clear_fault_type(struct charger_device *chgdev);
int sc858x_cp_get_chip_ok(struct charger_device *chgdev, bool *ok);
int sc858x_cp_set_usb_gate_en(struct charger_device *chgdev, bool enable);
int sc858x_cp_enable_acdrv_manual(struct charger_device *chgdev, bool manual);

#endif
