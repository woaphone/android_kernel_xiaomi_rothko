/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MT6989_SMMU_DEBUG_H
#define MT6989_SMMU_DEBUG_H

#include <linux/types.h>

struct io_pgtable_ops;
struct mtk_smmu_ops;
struct seq_file;

typedef int (*mtk_iommu_fault_callback_t)(int port, dma_addr_t mva,
					void *cb_data);

int mtk_iommu_register_fault_callback(int port,
	mtk_iommu_fault_callback_t fn, void *cb_data, bool is_vpu);
int mtk_iommu_unregister_fault_callback(int port, bool is_vpu);

void report_custom_smmu_fault(u64 fault_iova, u64 fault_pa,
			      u32 fault_id, u32 smmu_id);

int mtk_smmu_set_ops(const struct mtk_smmu_ops *ops);
void mtk_smmu_wpreg_dump(struct seq_file *s, u32 smmu_type);
void mtk_smmu_ste_cd_info_dump(struct seq_file *s, u32 smmu_type, u32 sid);
u64 mtk_smmu_iova_to_iopte(struct io_pgtable_ops *ops, u64 iova);

#endif
