/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_MEDIATEK_MTK_PCIE_H
#define __LINUX_SOC_MEDIATEK_MTK_PCIE_H

#include <linux/types.h>

struct irq_data;

int mtk_pcie_probe_port(int port);
int mtk_pcie_remove_port(int port);
int mtk_pcie_hw_control_vote(int port, bool hw_mode_en, u8 who);
u32 mtk_pcie_dump_link_info(int port);
int mtk_pcie_wait_link_ready(int port);
int mtk_pcie_disable_data_trans(int port);
int mtk_msi_unmask_to_other_mcu(struct irq_data *data, u32 group);

#endif /* __LINUX_SOC_MEDIATEK_MTK_PCIE_H */
