# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src) -I$(srctree)/drivers/interconnect
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/mtk-interconnect
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/mmdvfs
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/vcp/include
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/vcp/rv
ccflags-y += -I$(srctree)/drivers/iommu/mt6989
ccflags-y += -I$(srctree)/drivers/iommu/mt6989/vendor/drivers/misc/mediatek/smi
ccflags-y += -I$(srctree)/drivers/misc/mediatek/rothko_modem/include/mt-plat
ccflags-y += -include $(srctree)/drivers/iommu/mt6989/include/dt-bindings/memory/mtk-memory-port.h

mmqos-common-y := mmqos-mtk.o mmqos-hrt.o mmqos-vcp.o mmqos-test.o
obj-$(CONFIG_INTERCONNECT_MTK_MMQOS_COMMON) += mmqos-common.o
obj-$(CONFIG_INTERCONNECT_MTK_MMQOS_MT6989) += mmqos-mt6989.o
obj-$(CONFIG_INTERCONNECT_MTK_EMI) += mtk-emi.o
