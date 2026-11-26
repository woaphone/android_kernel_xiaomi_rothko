# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(src) -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include
obj-$(CONFIG_MTK_CM_IPI) += mtk_cm_ipi.o
obj-$(CONFIG_MTK_CM_MGR) += mtk_cm_mgr.o
mtk_cm_mgr-y := mtk_cm_mgr_common.o
obj-$(CONFIG_MTK_CM_MGR_MT6989) += mtk_cm_mgr_mt6989.o
