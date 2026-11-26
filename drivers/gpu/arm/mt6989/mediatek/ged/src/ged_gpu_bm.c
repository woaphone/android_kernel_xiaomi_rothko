// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/module.h>

#include <ged_gpu_bm.h>
#include <ged_base.h>
#include <ged_type.h>

#if defined(MTK_GPU_BM_2)
#include <gpu_bm.h>

static int sf_pid;
static unsigned int qos_frame_nr;

static int add_check_ovf(int v1, int v2)
{
	if (v1 > INT_MAX - v2)
		return 1;
	else
		return v1 + v2;
}

int mtk_bandwidth_resource_init(void)
{
	/* The GPU QoS DT driver owns the resource and firmware handshake. */
	if (!IS_REACHABLE(CONFIG_MTK_TINYSYS_SCMI))
		return -EOPNOTSUPP;

	return MTKGPUQoS_is_inited() ? 0 : -ENODEV;
}
EXPORT_SYMBOL(mtk_bandwidth_resource_init);

void mtk_bandwidth_update_info(int pid, int frame_nr, int job_id)
{
	if (pid == READ_ONCE(sf_pid) || !MTKGPUQoS_is_inited())
		return;

	MTKGPUQoS_update_info(pid, frame_nr, job_id,
		ged_get_segment_id() == MT6985W_TCZA_SEGMENT);
}

void mtk_bandwidth_check_SF(int pid, int isSF)
{
	if (isSF == 1)
		WRITE_ONCE(sf_pid, pid);
}

u32 qos_inc_frame_nr(void)
{
	qos_frame_nr = add_check_ovf(qos_frame_nr, 1);
	return qos_frame_nr;
}

u32 qos_get_frame_nr(void)
{
	return qos_frame_nr;
}
#endif /* MTK_GPU_BM_2 */
