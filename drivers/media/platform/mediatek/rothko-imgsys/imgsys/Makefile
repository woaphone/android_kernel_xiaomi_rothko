# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src) -I$(src)/../mtk-hcp
ccflags-y += -I$(src)/platforms/isp7sp
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include/soc/mediatek
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/cmdq/mailbox
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/mtk-interconnect
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/vcp/rv
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/vcp/include
ccflags-y += -I$(srctree)/drivers/iommu/mt6989
ccflags-y += -I$(srctree)/drivers/iommu/mt6989/include/soc/mediatek
ccflags-y += -I$(srctree)/drivers/iommu/mt6989/vendor/drivers/misc/mediatek/iommu
ccflags-y += -I$(srctree)/drivers/iommu/mt6989/vendor/drivers/misc/mediatek/smi
ccflags-y += -DIMGSYS_TF_DUMP_7SP_L -DMTK_IOVA_SINK2KERNEL

mtk_imgsys_isp7sp-y := mtk_imgsys-v4l2.o mtk_imgsys-dev.o \
	mtk_imgsys-of.o mtk_imgsys-sys.o mtk_imgsys-worker.o \
	mtk_imgsys-trace.o mtk_imgsys-isp7sp.o mtk_imgsys-cmdq.o \
	platforms/isp71/mtk_imgsys-cmdq-plat.o \
	platforms/isp7s/mtk_imgsys-cmdq-plat.o \
	platforms/isp7sp/mtk_imgsys-cmdq-plat.o \
	platforms/isp7sp/mtk_imgsys-cmdq-qof.o \
	platforms/isp7sp/mtk_imgsys-debug.o \
	platforms/isp7sp/modules/mtk_imgsys-dip.o \
	platforms/isp7sp/modules/mtk_imgsys-traw.o \
	platforms/isp7sp/modules/mtk_imgsys-pqdip.o \
	platforms/isp7sp/modules/mtk_imgsys-wpe.o \
	platforms/isp7sp/modules/mtk_imgsys-me.o \
	platforms/isp7sp/modules/mtk_imgsys-adl.o

obj-$(CONFIG_VIDEO_MTK_ISP_IMGSYS_7SP) += mtk_imgsys_isp7sp.o
