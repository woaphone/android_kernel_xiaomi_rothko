# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(srctree)/drivers/dma-buf/heaps/mt6989
ccflags-y += -I$(srctree)/drivers/iommu/mt6989
ccflags-y += -I$(srctree)/drivers/iommu/mt6989/arm/arm-smmu-v3
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/trusted_mem
mtk_system_heaps-y := mtk_system_heap.o mtk_page_pool.o mtk_cache_pool.o mtk_dmabuf_monitor.o mtk-deferred-free-helper.o
obj-$(CONFIG_DEVICE_MODULES_DMABUF_HEAPS_SYSTEM) += mtk_system_heaps.o
obj-$(CONFIG_DEVICE_MODULES_DMABUF_HEAPS_REFILL) += mtk_heap_refill.o
obj-$(CONFIG_MTK_TRUSTED_MEMORY_SUBSYSTEM) += mtk_sec_heap.o page_pool.o
obj-$(CONFIG_DMABUF_HEAPS_DEBUG) += mtk_heap_debug.o
