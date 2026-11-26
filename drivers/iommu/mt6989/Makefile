MT6989_IOMMU_PATH := $(srctree)/drivers/iommu/mt6989
DEVICE_MODULES_PATH := $(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor
subdir-ccflags-y += -I$(MT6989_IOMMU_PATH)/include
subdir-ccflags-y += -I$(MT6989_IOMMU_PATH)/vendor/drivers/misc/mediatek/iommu
subdir-ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/smi
subdir-ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include
# SPDX-License-Identifier: GPL-2.0
obj-y += arm/

ccflags-y += -I$(MT6989_IOMMU_PATH)

obj-$(CONFIG_DEVICE_MODULES_MTK_IOMMU) += mtk_iommu.o

subdir-ccflags-y += -I$(MT6989_IOMMU_PATH)/arm/arm-smmu-v3
obj-$(CONFIG_MTK_IOMMU_MISC_SECURE) += vendor/drivers/misc/mediatek/iommu/smmu_secure.o
