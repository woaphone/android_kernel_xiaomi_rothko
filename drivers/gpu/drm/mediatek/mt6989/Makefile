# SPDX-License-Identifier: GPL-2.0
export MT6989_DISPLAY_PATH := $(srctree)/drivers/gpu/drm/mediatek/mt6989
export DEVICE_MODULES_PATH := $(MT6989_DISPLAY_PATH)/vendor
subdir-ccflags-y += -I$(MT6989_DISPLAY_PATH)/include -I$(MT6989_DISPLAY_PATH)/include/uapi
subdir-ccflags-y += -I$(MT6989_DISPLAY_PATH)/mediatek_v2 -I$(MT6989_DISPLAY_PATH)/mml -I$(MT6989_DISPLAY_PATH)/dpc
subdir-ccflags-y += -I$(MT6989_DISPLAY_PATH)/include/mt-plat
# Register the panel driver before the DSI host creates its child devices.
obj-$(CONFIG_DRM_ROTHKO_N12) += n12/vis_display.o n12/panel-n12-42-02-0a-dsc-cmd.o
CFLAGS_n12/panel-n12-42-02-0a-dsc-cmd.o += \
	-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mtk-interconnect \
	-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/cmdq/mailbox \
	-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mmp \
	-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/slbc \
	-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/smi \
	-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mminfra \
	-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mmdvfs \
	-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/vcp/include \
	-I$(srctree)/drivers/gpu/drm \
	-I$(srctree)/drivers/gpu/drm/display
obj-$(CONFIG_DRM_MEDIATEK_MT6989) += mediatek_v2/
obj-$(CONFIG_MTK_MML) += mml/
obj-$(CONFIG_MTK_DPC) += dpc/
obj-$(CONFIG_MTK_CMDQ_MBOX_EXT) += vendor/drivers/misc/mediatek/cmdq/mailbox/
obj-$(CONFIG_MTK_MDP_MT6989) += vendor/drivers/misc/mediatek/mdp/
obj-$(CONFIG_DEVICE_MODULES_MTK_SMI) += vendor/drivers/misc/mediatek/smi/
obj-$(CONFIG_MTK_MMINFRA) += vendor/drivers/misc/mediatek/mminfra/
ifeq ($(CONFIG_MTK_MMINFRA),)
obj-$(CONFIG_MTK_MT6989_SMMU_SHARE_GROUP) += vendor/drivers/misc/mediatek/mminfra/
endif
obj-$(CONFIG_MTK_MMDVFS) += vendor/drivers/misc/mediatek/mmdvfs/
obj-$(CONFIG_MMPROFILE) += vendor/drivers/misc/mediatek/mmp/
obj-$(CONFIG_INTERCONNECT_MTK_EXTENSION) += vendor/drivers/misc/mediatek/mtk-interconnect/
obj-$(CONFIG_MTK_SLBC) += vendor/drivers/misc/mediatek/slbc/
subdir-ccflags-y += -I$(srctree)/drivers/iommu/mt6989 -I$(srctree)/drivers/iommu/mt6989/arm/arm-smmu-v3
obj-$(CONFIG_MTK_MMDVFS) += vendor/drivers/soc/mediatek/
obj-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vendor/drivers/misc/mediatek/vcp/

subdir-ccflags-y += -iquote $(MT6989_DISPLAY_PATH)/include

obj-$(CONFIG_LEDS_MTK) += leds/

obj-$(CONFIG_DEVICE_MODULES_MTK_SMI) += smi_core/

subdir-ccflags-y += -include $(srctree)/drivers/iommu/mt6989/include/dt-bindings/memory/mtk-memory-port.h
