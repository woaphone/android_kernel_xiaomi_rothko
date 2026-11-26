# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(srctree)
ccflags-y += -I$(srctree)/drivers/mmc/core
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/connectivity/common
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/power_throttling
ccflags-y += -DCFG_CONNADP_BUILD_IN

obj-$(CONFIG_MTK_ROTHKO_CONNINFRA) += connadp.o
connadp-y := common/connectivity_build_in_adapter.o \
	common/wmt_build_in_adapter.o \
	power_throttling/adapter.o \
	power_throttling/core.o
