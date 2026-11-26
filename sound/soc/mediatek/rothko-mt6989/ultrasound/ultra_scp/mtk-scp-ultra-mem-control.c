// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2018 MediaTek Inc.

#include "mtk-scp-ultra-mem-control.h"
#include "mtk-scp-ultra-common.h"
#include "mtk-base-afe.h"
#include "mtk-base-scp-ultra.h"
#include "mtk-afe-fe-dai.h"
#include <sound/soc.h>
#include <linux/device.h>
#include <linux/compat.h>
#include <linux/io.h>
#include "mtk-sram-manager.h"
#include "audio_ultra_msg_id.h"
#if IS_ENABLED(CONFIG_MTK_TINYSYS_SCP_SUPPORT)
#include "scp.h"
#endif
#include "mtk-scp-ultra.h"


int mtk_scp_ultra_reserved_dram_init(struct mtk_base_scp_ultra *scp_ultra)
{
#if IS_ENABLED(CONFIG_MTK_TINYSYS_SCP_SUPPORT)
	struct audio_ultra_dram *ultra_resv_mem = &scp_ultra->ultra_reserve_dram;
	struct audio_ultra_dram *dump_resv_mem =
		&scp_ultra->ultra_dump.dump_resv_mem;

	if (!scp_ipidev.ipi_inited)
		return -EPROBE_DEFER;

	ultra_resv_mem->phy_addr =
		scp_get_reserve_mem_phys(ULTRA_MEM_ID);
	ultra_resv_mem->vir_addr =
		(unsigned char *)scp_get_reserve_mem_virt(ULTRA_MEM_ID);
	ultra_resv_mem->size =
		scp_get_reserve_mem_size(ULTRA_MEM_ID);

	if (!ultra_resv_mem->phy_addr) {
		pr_info("%s(), scp ultra_resv_mem phy_addr error\n", __func__);
		return -EPROBE_DEFER;
	}

	if (!ultra_resv_mem->vir_addr) {
		pr_info("%s(), scp ultra_resv_mem phy_addr vir_addr error\n", __func__);
		return -EPROBE_DEFER;
	}

	if (!ultra_resv_mem->size) {
		pr_info("%s(), scp ultra_resv_mem phy_addr size error\n", __func__);
		return -EPROBE_DEFER;
	}

	memset_io((void *)ultra_resv_mem->vir_addr, 0, ultra_resv_mem->size);

	dev_info(scp_ultra->dev,
		 "%s(), sce reserve mem pa=0x%llx, va=0x%lx, size=0x%llx\n",
		 __func__,
		 ultra_resv_mem->phy_addr,
		 (unsigned long)ultra_resv_mem->vir_addr,
		 ultra_resv_mem->size);

	dump_resv_mem->phy_addr =
		scp_get_reserve_mem_phys(ULTRA_MEM_ID);
	dump_resv_mem->vir_addr =
		(unsigned char *)scp_get_reserve_mem_virt
				 (ULTRA_MEM_ID);
	dump_resv_mem->size =
		scp_get_reserve_mem_size(ULTRA_MEM_ID);

	if (!dump_resv_mem->phy_addr) {
		pr_err("%s(), dump_resv_mem phy_addr error\n", __func__);
		return -EPROBE_DEFER;
	}
	if (!dump_resv_mem->vir_addr) {
		pr_err("%s(), dump_resv_mem vir_addr error\n", __func__);
		return -EPROBE_DEFER;
	}
	if (!dump_resv_mem->size) {
		pr_err("%s(), dump_resv_mem size error\n", __func__);
		return -EPROBE_DEFER;
	}
	dev_info(scp_ultra->dev,
		 "%s(), dump pa=0x%llx, va=0x%lx, size=0x%llx\n",
		 __func__,
		 dump_resv_mem->phy_addr,
		 (unsigned long)dump_resv_mem->vir_addr,
		 dump_resv_mem->size);

	memset_io((void *)dump_resv_mem->vir_addr, 0, dump_resv_mem->size);
#else
	return -ENODEV;
#endif

	return 0;
}

