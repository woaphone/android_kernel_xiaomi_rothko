# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(MT6989_DISPLAY_PATH)/include/
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/smi/
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/cmdq/mailbox/
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/tinysys_scmi/
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/slbc
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/iommu/
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/vcp/rv
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/vcp/include
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/clk/mediatek

obj-$(CONFIG_MTK_MMINFRA) += mtk-mminfra-debug.o mtk-mminfra-imax.o
ifeq ($(CONFIG_MTK_MT6989_SMMU_SHARE_GROUP),y)
obj-y += mm-fake-engine.o
else
obj-$(CONFIG_MTK_MMINFRA) += mm-fake-engine.o
endif
