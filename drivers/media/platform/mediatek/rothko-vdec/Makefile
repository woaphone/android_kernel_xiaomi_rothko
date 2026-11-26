# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_VIDEO_ROTHKO_VDEC) += mtk-vcodec-dec-v2.o
mtk-vcodec-dec-v2-y := mtk_vcodec_dec_drv.o vdec_drv_if.o mtk_vcodec_dec.o \
	mtk_vcodec_dec_pm.o mtk_vcodec_dec_slc.o vdec/vdec_vcp_if.o \
	v2/mtk_vcodec_dec_pm_plat.o

# The frozen rothko-vcodec common and DVFS objects serve both codec devices.
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
