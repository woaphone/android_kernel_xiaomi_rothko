# SPDX-License-Identifier: GPL-2.0
fpsgo := $(src)/fpsgo_v3
ccflags-y += -I$(srctree)/kernel -I$(src)/perf_ioctl -I$(src)/powerhal_cpu_ctrl
ccflags-y += -I$(fpsgo) -I$(fpsgo)/base/include -I$(fpsgo)/composer/include
ccflags-y += -I$(fpsgo)/fbt/include -I$(fpsgo)/fstb -I$(fpsgo)/gbe
ccflags-y += -I$(srctree)/drivers/gpu/arm/mt6989/mediatek/ged/include
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/mediatek_v2

obj-$(CONFIG_MTK_PERFORMANCE_MODULE) += mtk_perf_ioctl.o mtk_perf_ioctl_magt.o
obj-$(CONFIG_MTK_PERFORMANCE_MODULE) += mtk_ioctl_powerhal.o mtk_powerhal_cpu_ctrl.o
mtk_perf_ioctl-y := perf_ioctl/perf_ioctl.o
mtk_perf_ioctl_magt-y := perf_ioctl/perf_ioctl_magt.o
mtk_ioctl_powerhal-y := perf_ioctl/ioctl_powerhal.o
mtk_powerhal_cpu_ctrl-y := powerhal_cpu_ctrl/powerhal_cpu_ctrl.o

obj-$(CONFIG_MTK_FPSGO_V3) += mtk_fpsgo.o
mtk_fpsgo-y := fpsgo_v3/fpsgo_main.o \
	fpsgo_v3/base/src/fpsgo_base.o \
	fpsgo_v3/base/src/fpsgo_sysfs.o \
	fpsgo_v3/base/src/fpsgo_cpu_policy.o \
	fpsgo_v3/composer/src/fps_composer.o \
	fpsgo_v3/fbt/src/fbt_cpu.o \
	fpsgo_v3/fbt/src/fbt_cpu_ctrl.o \
	fpsgo_v3/fbt/src/fbt_cpu_ux.o \
	fpsgo_v3/fbt/platform/fbt_cpu_platform.o \
	fpsgo_v3/fbt/src/xgf.o \
	fpsgo_v3/fbt/src/mini_top.o \
	fpsgo_v3/fstb/fstb.o \
	fpsgo_v3/gbe/gbe1_main.o \
	fpsgo_v3/gbe/gbe2_main.o \
	fpsgo_v3/gbe/gbe_common.o \
	fpsgo_v3/gbe/gbe_sysfs.o

# The 6.1 vendor core has externally visible helpers without header declarations.
$(foreach obj,$(mtk_fpsgo-y),$(eval CFLAGS_$(obj) += -Wno-missing-prototypes))
mtk_fpsgo-$(CONFIG_MTK_FPSGO_NATIVE_XGF) += fpsgo_cus/xgf_estimator.o
