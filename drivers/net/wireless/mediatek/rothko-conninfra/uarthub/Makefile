# SPDX-License-Identifier: GPL-2.0
obj-y += uarthub_drv.o
uarthub_drv-y := common/uarthub_drv_core.o common/uarthub_drv_debug.o 		 common/uarthub_drv_ut_test.o common/uarthub_drv_export.o 		 common/platform/6989/mt6989.o common/platform/6989/mt6989_debug.o
ccflags-y += -I$(src)/common
ccflags-$(CONFIG_DRM_MEDIATEK_MT6989) += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/mediatek_v2
