// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __GPU_BM_H__
#define __GPU_BM_H__

#include <linux/types.h>

#define GPU_BW_DEFAULT_MODE             (0)
#define GPU_BW_SPORT_MODE               (1)
#define GPU_BW_NO_PRED_MODE             (2)
#define GPU_BW_LP_MODE                  (3)
#define GPU_BM_PEAK_PERF_MODE           (4)
#define GPU_BM_PEAK_PERF_MODE_LIMIT     (5)

#define GPU_BW_RATIO_CEIL               (300)
#define GPU_BW_RATIO_FLOOR              (10)

#define GPU_BW_NO_PRED_RATIO_CEIL       (2300)
#define GPU_BW_NO_PRED_RATIO_FLOOR      (2010)

#define GPU_BM_PEAK_INDEX_TOP_LIMIT      (1)

struct v1_data {
	u32 version;
	u32 ctx;
	u32 frame;
	u32 job;
	u32 freq;
};

void MTKGPUQoS_mode(int seg_flag);
void MTKGPUQoS_update_info(u32 pid, u32 frame, u32 job, int seg_flag);
int MTKGPUQoS_is_inited(void);

#endif
