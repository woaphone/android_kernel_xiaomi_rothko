/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 MediaTek Inc.
 */


int get_dump_buf_usage(char buf[], int size);
int ccci_get_plat_ft_inf(char buf[], int size);
extern void inject_pin_status_event(int pin_value, const char pin_name[]);

