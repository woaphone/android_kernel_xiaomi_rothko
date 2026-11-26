/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ROTHKO_MTK_PWM_H
#define ROTHKO_MTK_PWM_H

#include <linux/types.h>

#define MTK_PWM_CHANNELS		4
#define MTK_PWM_TICKS_PER_US		26
#define MTK_PWM_MAX_TICKS		65536U
#define MTK_PWM_MAX_WORDS		65536U

struct device;

struct device *mtk_rothko_pwm_get(struct device *consumer);
int mtk_rothko_pwm_send(struct device *dev, u32 channel, u32 ticks,
			const void *wave, u32 words, unsigned int duration_us);

#endif
