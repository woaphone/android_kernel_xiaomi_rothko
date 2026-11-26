# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_VIDEO_ROTHKO_JPEG) += rothko-jpeg.o
rothko-jpeg-y := jpeg_drv.o jpeg_dma_buf.o

rothko_display := $(srctree)/drivers/gpu/drm/mediatek/mt6989
rothko_vendor := $(rothko_display)/vendor
ccflags-y += -I$(rothko_display)/include -I$(rothko_display)/include/mt-plat
ccflags-y += -I$(rothko_vendor)/drivers/misc/mediatek/vcp/rv
ccflags-y += -I$(rothko_vendor)/drivers/misc/mediatek/vcp/include
ccflags-y += -I$(srctree)/drivers/iommu/mt6989/arm/arm-smmu-v3
ccflags-y += -I$(srctree)/drivers/dma-buf/heaps/mt6989
ccflags-y += -iquote $(srctree)/drivers/iommu/mt6989/include
