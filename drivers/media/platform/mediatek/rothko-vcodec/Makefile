# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_VIDEO_ROTHKO_VENC) += mtk-vcodec-common.o mtk-vcodec-enc-v2.o mtk-vb2.o
mtk-vcodec-common-y := mtk_vcodec_intr.o mtk_vcodec_util.o mtk_vcodec_fence.o
mtk-vcodec-enc-v2-y := mtk_vcodec_enc_drv.o venc_drv_if.o mtk_vcodec_enc.o \
	mtk_vcodec_enc_pm.o venc/venc_vcp_if.o v2/mtk_vcodec_enc_pm_plat.o v2/vcodec_dvfs.o
mtk-vb2-y := videobuf/mtk_dma_contig.o

rothko_display := $(srctree)/drivers/gpu/drm/mediatek/mt6989
rothko_vendor := $(rothko_display)/vendor
subdir-ccflags-y += -I$(src)/include -I$(src) -I$(src)/videobuf
subdir-ccflags-y += -I$(rothko_display)/include -I$(rothko_display)/include/mt-plat
subdir-ccflags-y += -I$(rothko_vendor)/drivers/misc/mediatek/slbc
subdir-ccflags-y += -I$(rothko_vendor)/drivers/misc/mediatek/mtk-interconnect
subdir-ccflags-y += -I$(rothko_vendor)/drivers/misc/mediatek/vcp/rv
subdir-ccflags-y += -I$(rothko_vendor)/drivers/misc/mediatek/vcp/include
subdir-ccflags-y += -I$(rothko_vendor)/drivers/misc/mediatek/smi
subdir-ccflags-y += -I$(srctree)/drivers/iommu/mt6989
subdir-ccflags-y += -I$(srctree)/drivers/iommu/mt6989/arm/arm-smmu-v3
subdir-ccflags-y += -I$(srctree)/drivers/dma-buf/heaps/mt6989

subdir-ccflags-y += -iquote $(srctree)/drivers/iommu/mt6989/include
