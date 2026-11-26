#
# Copyright (C) 2015 MediaTek Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#

ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/clk/mediatek/
ccflags-y += -I$(MT6989_DISPLAY_PATH)/include/
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mmdvfs/
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/smi/
obj-$(CONFIG_MTK_MMDVFS) += mtk-mmdvfs-ftrace.o mtk-mmdvfs-debug.o mtk-mmdvfs-v3-start.o

# A built-in consumer cannot call a modular CCU provider.
ifeq ($(CONFIG_MTK_CCU_RPROC),y)
obj-$(CONFIG_MTK_MMDVFS) += mtk-mmdvfs-ccu.o
else ifeq ($(CONFIG_MTK_MMDVFS)$(CONFIG_MTK_CCU_RPROC),mm)
obj-m += mtk-mmdvfs-ccu.o
endif

