/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MT6685_335CLK_H
#define _LINUX_MT6685_335CLK_H

#include <linux/types.h>

/* True after the PMIC regmap and clock provider have been initialized. */
bool mt6685_335clk_ready(void);

/* Signatures verified against the stock module's KCFI type IDs. */
void mt6685_set_335_dcxo_mode(unsigned int mode);
void mt6685_set_335_dcxo(bool enable);

#endif
