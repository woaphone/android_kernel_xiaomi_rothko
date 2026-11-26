/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ROTHKO_XIAOMI_TOUCH_COMMON_H
#define _ROTHKO_XIAOMI_TOUCH_COMMON_H

#include <linux/types.h>

struct device;

/* OS2 xiaomi_touch_common.ko's v3 request; sizeof is 520 on both ABIs. */
struct xiaomi_touch_request {
	__u8 touch_id;
	__u8 command;
	__u16 mode;
	__u16 length;
	__u16 reserved;
	__s32 values[128];
};

struct xiaomi_touch_ops {
	int (*read_palm)(void *data, u8 *value);
	int (*write_palm)(void *data, bool enable);
	int (*request)(void *data, struct xiaomi_touch_request *request);
};

int devm_xiaomi_touch_register(struct device *dev,
			      const struct xiaomi_touch_ops *ops,
			      void *data, u32 resolution_factor);

void xiaomi_touch_notify_palm(struct device *dev);

#endif
