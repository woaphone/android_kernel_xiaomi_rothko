# SPDX-License-Identifier: GPL-2.0

subdir-ccflags-y += -Werror

ccflags-y += -I$(MT6989_DISPLAY_PATH)/dpc \
             -I$(MT6989_DISPLAY_PATH)/mml \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mdp \
             -I$(MT6989_DISPLAY_PATH)/mediatek_v2 \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/cmdq/mailbox \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mmp \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mmdvfs \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/smi \
             -I$(DEVICE_MODULES_PATH)/drivers/soc/mediatek

obj-$(CONFIG_MTK_DPC) := mtk_dpc.o

mtk_dpc-objs += mtk_dpc_driver.o
mtk_dpc-objs += mtk_dpc_mmp.o mdp_dpc.o

obj-$(CONFIG_MTK_DPC) += mtk_vdisp.o
