// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 * SMMU diagnostics from the MT6989 donor iommu_debug.c.
 */
#define pr_fmt(fmt) "mtk_iommu: debug " fmt

#include <linux/bitfield.h>
#include <linux/export.h>
#include <linux/io-pgtable.h>
#include <linux/log2.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <asm/memory.h>

#include "mtk-smmu-v3.h"
#include "smmu_debug.h"
#include "smmu_reg.h"
#include "smmu_mt6989_ports.h"

#define iommu_dump(file, fmt, args...) \
	do { \
		if (file) \
			seq_printf(file, fmt, ##args); \
		else \
			pr_info(fmt, ##args); \
	} while (0)

static const struct mtk_smmu_ops *smmu_ops;

int mtk_smmu_set_ops(const struct mtk_smmu_ops *ops)
{
	if (smmu_ops == NULL)
		smmu_ops = ops;

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_smmu_set_ops);

static void dump_wrapper_register(struct seq_file *s,
				  struct arm_smmu_device *smmu)
{
	struct mtk_smmu_data *data = to_mtk_smmu_data(smmu);
	void __iomem *wp_base = smmu->wp_base;
	unsigned int smmuwp_reg_nr, i;

	iommu_dump(s, "wp reg for smmu:%d, base:0x%llx, wp_base:0x%llx\n",
		   data->plat_data->smmu_type,
		   (unsigned long long) smmu->base,
		   (unsigned long long) smmu->wp_base);

	smmuwp_reg_nr = ARRAY_SIZE(smmuwp_regs);

	/* SOC has one less TBU than the others */
	if (data->plat_data->smmu_type == SOC_SMMU)
		smmuwp_reg_nr -= SMMU_TBU_REG_NUM;

	for (i = 0; i < smmuwp_reg_nr; i++) {
		if (i + 4 < smmuwp_reg_nr) {
			iommu_dump(s,
				   "%s:0x%x=0x%x\t%s:0x%x=0x%x\t%s:0x%x=0x%x\t%s:0x%x=0x%x\t%s:0x%x=0x%x\n",
				   smmuwp_regs[i + 0].name, smmuwp_regs[i + 0].offset,
				   readl_relaxed(wp_base + smmuwp_regs[i + 0].offset),
				   smmuwp_regs[i + 1].name, smmuwp_regs[i + 1].offset,
				   readl_relaxed(wp_base + smmuwp_regs[i + 1].offset),
				   smmuwp_regs[i + 2].name, smmuwp_regs[i + 2].offset,
				   readl_relaxed(wp_base + smmuwp_regs[i + 2].offset),
				   smmuwp_regs[i + 3].name, smmuwp_regs[i + 3].offset,
				   readl_relaxed(wp_base + smmuwp_regs[i + 3].offset),
				   smmuwp_regs[i + 4].name, smmuwp_regs[i + 4].offset,
				   readl_relaxed(wp_base + smmuwp_regs[i + 4].offset));
			i = i + 4;
		} else {
			iommu_dump(s, "%s:0x%x=0x%x\n",
				   smmuwp_regs[i].name, smmuwp_regs[i].offset,
				   readl_relaxed(wp_base + smmuwp_regs[i].offset));
		}
	}
}

void mtk_smmu_wpreg_dump(struct seq_file *s, u32 smmu_type)
{
	struct mtk_smmu_data *data;

	if (smmu_ops && smmu_ops->get_smmu_data) {
		data = smmu_ops->get_smmu_data(smmu_type);
		if (data != NULL && data->hw_init_flag == 1)
			dump_wrapper_register(s, &data->smmu);
	}
}
EXPORT_SYMBOL_GPL(mtk_smmu_wpreg_dump);

#define ARM_LPAE_MAX_LEVELS		4

/* Struct accessors */
#define io_pgtable_to_data(x)						\
	container_of((x), struct arm_lpae_io_pgtable, iop)

#define io_pgtable_ops_to_data(x)					\
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))

/*
 * Calculate the right shift amount to get to the portion describing level l
 * in a virtual address mapped by the pagetable in d.
 */
#define ARM_LPAE_LVL_SHIFT(l, d)					\
	(((ARM_LPAE_MAX_LEVELS - (l)) * (d)->bits_per_level) +		\
	ilog2(sizeof(arm_lpae_iopte)))

#define ARM_LPAE_GRANULE(d)						\
	(sizeof(arm_lpae_iopte) << (d)->bits_per_level)
#define ARM_LPAE_PGD_SIZE(d)						\
	(sizeof(arm_lpae_iopte) << (d)->pgd_bits)

/*
 * Calculate the index at level l used to map virtual address a using the
 * pagetable in d.
 */
#define ARM_LPAE_PGD_IDX(l, d)						\
	((l) == (d)->start_level ? (d)->pgd_bits - (d)->bits_per_level : 0)

#define ARM_LPAE_LVL_IDX(a, l, d)					\
	(((u64)(a) >> ARM_LPAE_LVL_SHIFT(l, d)) &			\
	 ((1 << ((d)->bits_per_level + ARM_LPAE_PGD_IDX(l, d))) - 1))

/* Calculate the block/page mapping size at level l for pagetable in d. */
#define ARM_LPAE_BLOCK_SIZE(l,d)	(1ULL << ARM_LPAE_LVL_SHIFT(l,d))

/* Page table bits */
#define ARM_LPAE_PTE_TYPE_SHIFT		0
#define ARM_LPAE_PTE_TYPE_MASK		0x3

#define ARM_LPAE_PTE_TYPE_BLOCK		1
#define ARM_LPAE_PTE_TYPE_TABLE		3
#define ARM_LPAE_PTE_TYPE_PAGE		3

#define ARM_LPAE_PTE_ADDR_MASK		GENMASK_ULL(47, 12)

/* IOPTE accessors */
#define iopte_deref(pte, d) __va(iopte_to_paddr(pte, d))

#define iopte_type(pte)					\
	(((pte) >> ARM_LPAE_PTE_TYPE_SHIFT) & ARM_LPAE_PTE_TYPE_MASK)

struct arm_lpae_io_pgtable {
	struct io_pgtable	iop;

	int			pgd_bits;
	int			start_level;
	int			bits_per_level;

	void			*pgd;
};

typedef u64 arm_lpae_iopte;

static inline bool iopte_leaf(arm_lpae_iopte pte, int lvl,
			      enum io_pgtable_fmt fmt)
{
	if (lvl == (ARM_LPAE_MAX_LEVELS - 1) && fmt != ARM_MALI_LPAE)
		return iopte_type(pte) == ARM_LPAE_PTE_TYPE_PAGE;

	return iopte_type(pte) == ARM_LPAE_PTE_TYPE_BLOCK;
}

static phys_addr_t iopte_to_paddr(arm_lpae_iopte pte,
				  struct arm_lpae_io_pgtable *data)
{
	u64 paddr = pte & ARM_LPAE_PTE_ADDR_MASK;

	if (ARM_LPAE_GRANULE(data) < SZ_64K)
		return paddr;

	/* Rotate the packed high-order bits back to the top */
	return (paddr | (paddr << (48 - 12))) & (ARM_LPAE_PTE_ADDR_MASK << 4);
}

static u64 arm_lpae_iova_to_iopte(struct io_pgtable_ops *ops, unsigned long iova)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	arm_lpae_iopte pte, *ptep = data->pgd;
	int lvl = data->start_level;

	do {
		/* Valid IOPTE pointer? */
		if (!ptep)
			return 0;

		/* Grab the IOPTE we're interested in */
		ptep += ARM_LPAE_LVL_IDX(iova, lvl, data);
		pte = READ_ONCE(*ptep);

		/* Valid entry? */
		if (!pte)
			return 0;

		/* Leaf entry? */
		if (iopte_leaf(pte, lvl, data->iop.fmt))
			goto found_translation;

		/* Take it to the next level */
		ptep = iopte_deref(pte, data);
	} while (++lvl < ARM_LPAE_MAX_LEVELS);

	/* Ran out of page tables to walk */
	return 0;

found_translation:
	pr_info("%s, iova:0x%lx, pte:0x%llx, lvl:%d, iopte_type:%llu, fmt:%u\n",
		__func__, iova, pte, lvl, iopte_type(pte), data->iop.fmt);
	return pte;
}

static inline bool is_valid_ste(__le64 *ste)
{
	return (ste && (le64_to_cpu(ste[0]) & STRTAB_STE_0_V));
}

static inline bool is_valid_cd(__le64 *cd)
{
	return (cd && (le64_to_cpu(cd[0]) & CTXDESC_CD_0_V));
}

static void ste_dump(struct seq_file *s, u32 sid, __le64 *ste)
{
	int i;

	if (ste == NULL)
		return;

	if (!is_valid_ste(ste)) {
		iommu_dump(s, "Failed to valid ste(sid:%u)\n", sid);
		return;
	}

	iommu_dump(s, "SMMU STE values:\n");
	for (i = 0; i < STRTAB_STE_DWORDS; i++) {
		if (i + 3 < STRTAB_STE_DWORDS) {
			iommu_dump(s, "u64[%d~%d]:0x%016llx|0x%016llx|0x%016llx|0x%016llx\n",
				   i, i + 3, ste[i + 0], ste[i + 1], ste[i + 2], ste[i + 3]);
			i = i + 3;
		} else {
			iommu_dump(s, "u64[%d]:0x%016llx\n", i, ste[i]);
		}
	}
}

static void cd_dump(struct seq_file *s, u32 ssid, __le64 *cd)
{
	int i;

	if (cd == NULL)
		return;

	if (!is_valid_cd(cd)) {
		iommu_dump(s, "Failed to valid cd(ssid:%u)\n", ssid);
		return;
	}

	iommu_dump(s, "SMMU CD values:\n");
	for (i = 0; i < CTXDESC_CD_DWORDS; i++) {
		if (i + 3 < CTXDESC_CD_DWORDS) {
			iommu_dump(s, "u64[%d~%d]:0x%016llx|0x%016llx|0x%016llx|0x%016llx\n",
				   i, i + 3, cd[i + 0], cd[i + 1], cd[i + 2], cd[i + 3]);
			i = i + 3;
		} else {
			iommu_dump(s, "u64[%d]:0x%016llx\n", i, cd[i]);
		}
	}
}

static void dump_ste_cd_info(struct seq_file *s,
			     struct arm_smmu_master *master)
{
	struct arm_smmu_domain *domain;
	struct arm_smmu_device *smmu;
	__le64 *steptr = NULL, *cdptr = NULL;
	u64 asid = 0, ttbr = 0;
	u32 sid, ssid;

	if (!master || !master->streams) {
		pr_info("%s, ERROR", __func__);
		return;
	}

	/* currently only support one master one sid */
	sid = master->streams[0].id;
	ssid = 0;
	smmu = master->smmu;
	domain = master->domain;

	if (smmu_ops && smmu_ops->get_step_ptr)
		steptr = smmu_ops->get_step_ptr(smmu, sid);

	if (smmu_ops && smmu_ops->get_cd_ptr)
		cdptr = smmu_ops->get_cd_ptr(domain, ssid);

	if (is_valid_cd(cdptr)) {
		asid = FIELD_GET(CTXDESC_CD_0_ASID, le64_to_cpu(cdptr[0]));
		ttbr = FIELD_GET(CTXDESC_CD_1_TTB0_MASK,  le64_to_cpu(cdptr[1]));
	}

	iommu_dump(s, "%s strtab base:0x%llx, cfg:0x%x\n", __func__,
		   smmu->strtab_cfg.strtab_base, smmu->strtab_cfg.strtab_base_cfg);
	iommu_dump(s, "%s sid=0x%x asid=0x%llx ttbr=0x%llx dev(%s) [cd:%d ste:%d]\n",
		   __func__, sid, asid, ttbr, dev_name(master->dev),
		   (cdptr == NULL), (steptr == NULL));

	ste_dump(s, sid, steptr);
	cd_dump(s, ssid, cdptr);
}

void mtk_smmu_ste_cd_info_dump(struct seq_file *s, u32 smmu_type, u32 sid)
{
	struct arm_smmu_device *smmu = NULL;
	struct arm_smmu_stream *stream;
	struct mtk_smmu_data *data;
	struct rb_node *n;

	if (smmu_ops && smmu_ops->get_smmu_data) {
		data = smmu_ops->get_smmu_data(smmu_type);
		if (data != NULL && data->hw_init_flag == 1)
			smmu = &data->smmu;
	}

	if (smmu == NULL) {
		pr_info("%s, ERROR\n", __func__);
		return;
	}

	for (n = rb_first(&smmu->streams); n; n = rb_next(n)) {
		stream = rb_entry(n, struct arm_smmu_stream, node);
		if (stream != NULL && stream->master != NULL &&
		    stream->master->streams != NULL)
			/* currently only support one master one sid */
			if (sid == stream->master->streams[0].id) {
				dump_ste_cd_info(s, stream->master);
				return;
			}
	}
}
EXPORT_SYMBOL_GPL(mtk_smmu_ste_cd_info_dump);

u64 mtk_smmu_iova_to_iopte(struct io_pgtable_ops *ops, u64 iova)
{
	return arm_lpae_iova_to_iopte(ops, iova);
}
EXPORT_SYMBOL_GPL(mtk_smmu_iova_to_iopte);



/* The callback indices and fault AXI decoding follow the MT6989 donor. */
struct mtk_iommu_cb {
	int port;
	mtk_iommu_fault_callback_t fault_fn;
	void *fault_data;
};

static struct mtk_iommu_cb mm_callbacks[ARRAY_SIZE(mm_port_mt6989)];
static struct mtk_iommu_cb apu_callbacks[ARRAY_SIZE(apu_port_mt6989)];

#define F_MMU_INT_TF_MSK			GENMASK(12, 2)
#define F_MMU_INT_TF_LARB(id)		FIELD_GET(GENMASK(13, 7), id)
#define F_MMU_INT_TF_PORT(id)		FIELD_GET(GENMASK(6, 2), id)
#define F_MMU_INT_TF_SPEC_MSK(port_s_b)	GENMASK(12, port_s_b)

static int mtk_iommu_port_idx(int port, bool is_vpu)
{
	const struct mtk_iommu_port *ports = is_vpu ?
		apu_port_mt6989 : mm_port_mt6989;
	unsigned int nr = is_vpu ? ARRAY_SIZE(apu_port_mt6989) :
		ARRAY_SIZE(mm_port_mt6989);
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (ports[i].larb_id == MTK_M4U_TO_LARB(port) &&
		    ports[i].port_id == MTK_M4U_TO_PORT(port))
			return i;
	}
	return -1;
}

int mtk_iommu_register_fault_callback(int port,
	mtk_iommu_fault_callback_t fn, void *cb_data, bool is_vpu)
{
	struct mtk_iommu_cb *callbacks = is_vpu ? apu_callbacks : mm_callbacks;
	int idx = mtk_iommu_port_idx(port, is_vpu);

	if (idx < 0) {
		pr_info("%s fail, port=%d\n", __func__, port);
		return -1;
	}
	callbacks[idx].port = port;
	callbacks[idx].fault_fn = fn;
	callbacks[idx].fault_data = cb_data;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_iommu_register_fault_callback);

int mtk_iommu_unregister_fault_callback(int port, bool is_vpu)
{
	struct mtk_iommu_cb *callbacks = is_vpu ? apu_callbacks : mm_callbacks;
	int idx = mtk_iommu_port_idx(port, is_vpu);

	if (idx < 0) {
		pr_info("%s fail, port=%d\n", __func__, port);
		return -1;
	}
	callbacks[idx].port = -1;
	callbacks[idx].fault_fn = NULL;
	callbacks[idx].fault_data = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_iommu_unregister_fault_callback);

static int mt6989_tf_is_gce_videoup(u32 port_tf, u32 vld_tf)
{
	return F_MMU_INT_TF_LARB(port_tf) ==
	       FIELD_GET(GENMASK(12, 8), vld_tf) &&
	       F_MMU_INT_TF_PORT(port_tf) ==
	       FIELD_GET(GENMASK(1, 0), vld_tf);
}

static u32 mt6989_get_valid_tf_id(int tf_id, u32 type, int id)
{
	u32 vld_id = 0;

	if (type == APU_SMMU)
		vld_id = FIELD_GET(GENMASK(12, 8), tf_id);
	else
		vld_id = tf_id & F_MMU_INT_TF_MSK;

	return vld_id;
}

static bool mt6989_tf_id_is_match(int tf_id, u32 type, int id,
				  struct mtk_iommu_port port)
{
	int vld_id = -1;

	if (port.id != id)
		return false;

	if (port.port_type == SPECIAL)
		vld_id = tf_id & F_MMU_INT_TF_SPEC_MSK(port.port_start);
	else if (port.port_type == NORMAL)
		vld_id = mt6989_get_valid_tf_id(tf_id, type, id);

	if (port.tf_id == vld_id)
		return true;

	return false;
}

static const u32 mt6989_smmu_common_ids[SMMU_TYPE_NUM][SMMU_TBU_CNT_MAX] = {
	[MM_SMMU] = {
		MM_SMMU_MDP,
		MM_SMMU_MDP,
		MM_SMMU_DISP,
		MM_SMMU_DISP,
	},
	[APU_SMMU] = {
		APU_SMMU_M0,
		APU_SMMU_M0,
		APU_SMMU_M0,
		APU_SMMU_M0,
	},
	[SOC_SMMU] = {
		SOC_SMMU_M4,
		SOC_SMMU_M6,
		SOC_SMMU_M7,
	},
};

static int mt6989_smmu_common_id(u32 smmu_type, u32 tbu_id)
{
	if (smmu_type >= SMMU_TYPE_NUM || tbu_id >= SMMU_TBU_CNT(smmu_type))
		return -1;

	return mt6989_smmu_common_ids[smmu_type][tbu_id];
}

static char *mt6989_smmu_soc_port_name(u32 type, int id, int tf_id)
{
	if (type != SOC_SMMU) {
		pr_info("%s is not support type:%u\n", __func__, type);
		return NULL;
	}

	switch (id) {
	case SOC_SMMU_M4:
		return mt6989_soc_m4_port_name(tf_id);
	case SOC_SMMU_M6:
		return mt6989_soc_m6_port_name(tf_id);
	case SOC_SMMU_M7:
		return mt6989_soc_m7_port_name(tf_id);
	default:
		return "SOC_UNKNOWN";
	}
}


static int mtk_iommu_get_tf_port_idx(u32 tf_id, u32 type, int id)
{
	const struct mtk_iommu_port *ports = type == APU_SMMU ?
		apu_port_mt6989 : mm_port_mt6989;
	unsigned int nr = type == APU_SMMU ? ARRAY_SIZE(apu_port_mt6989) :
		ARRAY_SIZE(mm_port_mt6989);
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (mt6989_tf_id_is_match(tf_id, type, id, ports[i]))
			return i;
	}
	if (type == MM_SMMU) {
		for (i = 0; i < nr; i++) {
			if (ports[i].port_type == GCE_VIDEOUP_FAKE &&
			    mt6989_tf_is_gce_videoup(ports[i].tf_id, tf_id) &&
			    ports[i].id == id)
				return i;
		}
	}
	return -1;
}

void report_custom_smmu_fault(u64 fault_iova, u64 fault_pa,
			      u32 fault_id, u32 smmu_id)
{
	const struct mtk_iommu_port *ports;
	const char *port_name;
	u32 tbu_id = SMMUWP_TF_TBU_VAL(fault_id);
	int idx, id = mt6989_smmu_common_id(smmu_id, tbu_id);

	if (id < 0)
		return;
	if (smmu_id == SOC_SMMU) {
		port_name = mt6989_smmu_soc_port_name(smmu_id, id, fault_id);
	} else if (smmu_id == GPU_SMMU) {
		port_name = "GPUSYS";
	} else {
		idx = mtk_iommu_get_tf_port_idx(fault_id, smmu_id, id);
		if (idx < 0) {
			pr_warn("fail,iova:0x%llx, port:0x%x\n", fault_iova, fault_id);
			return;
		}
		ports = smmu_id == APU_SMMU ? apu_port_mt6989 : mm_port_mt6989;
		port_name = ports[idx].name;
		/* The donor dispatches translation-fault callbacks for MM only. */
		if (smmu_id == MM_SMMU && ports[idx].enable_tf &&
		    mm_callbacks[idx].fault_fn)
			mm_callbacks[idx].fault_fn(mm_callbacks[idx].port,
				fault_iova, mm_callbacks[idx].fault_data);
	}
	pr_err("[M4U] error:CRDISPATCH_KEY:SMMU_%s\ntranslation fault:port=%s,mva=0x%llx,pa=0x%llx\n",
		port_name, port_name, fault_iova, fault_pa);
}
EXPORT_SYMBOL_GPL(report_custom_smmu_fault);
