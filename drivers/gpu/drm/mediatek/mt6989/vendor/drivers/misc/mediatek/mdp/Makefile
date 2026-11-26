# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/cmdq/mailbox \
	     -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/iommu \
	     -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mmp \
	     -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mtk-interconnect \
	     -I$(srctree)/drivers/dma-buf/heaps/mt6989

# Keep the vendor CMDQ callbacks' original linkage during the API port.
ccflags-y += -Wno-missing-prototypes -Wno-unused-but-set-variable

obj-$(CONFIG_MTK_MDP_MT6989) += cmdq_helper_inf.o mdp_drv_mt6989.o
mdp_drv_mt6989-y := mdp_mt6989.o mdp_driver.o mdp_ioctl_ex.o mdp_common.o \
		    cmdq_record.o cmdq_virtual.o cmdq_device.o mdp_event_common.o \
		    cmdq_subsys_common.o cmdq_helper_ext.o cmdq_mmp.o mdp_fence.o
