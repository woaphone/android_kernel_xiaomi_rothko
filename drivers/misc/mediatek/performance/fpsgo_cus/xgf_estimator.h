/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FPSGO_XGF_ESTIMATOR_H
#define _FPSGO_XGF_ESTIMATOR_H

#include <linux/types.h>

void xgf_cus_set_boost(bool enable);
void xgf_cus_delete_render_info(int pid, unsigned long long bufid);
int xgf_cus_est_runtime(int pid, unsigned long long bufid, int tgid, int spid,
	int max_paths, int max_tasks, int ema2, int dep_frames, int dividend, int extra_sub,
	int *raw_dep, int *raw_count, int raw_cap, int *dep, int *dep_count, int dep_cap,
	int *magt, int magt_count, unsigned long long *raw, unsigned long long *ema,
	unsigned long long *deq, unsigned long long *enq,
	unsigned long long deq_start, unsigned long long deq_end,
	unsigned long long enq_start, unsigned long long enq_end,
	unsigned long long start, unsigned long long end);

#endif
