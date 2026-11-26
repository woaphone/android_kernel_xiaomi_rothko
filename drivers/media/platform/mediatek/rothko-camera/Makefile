# SPDX-License-Identifier: GPL-2.0-only
ROTHKO_CAMERA_PATH := $(srctree)/drivers/media/platform/mediatek/rothko-camera
subdir-ccflags-y += -D__XIAOMI_CAMERA__
subdir-ccflags-y += -I$(ROTHKO_CAMERA_PATH) -I$(ROTHKO_CAMERA_PATH)/include
subdir-ccflags-y += -I$(srctree)/drivers/dma-buf/heaps/mt6989
subdir-ccflags-y += -I$(srctree)/drivers/iommu/mt6989/include
subdir-ccflags-y += -I$(srctree)/drivers/iommu/mt6989
subdir-ccflags-y += -I$(srctree)/drivers/iommu/mt6989/arm/arm-smmu-v3
subdir-ccflags-y += -I$(srctree)/drivers/iommu/mt6989/vendor/drivers/misc/mediatek/smi
subdir-ccflags-y += -I$(srctree)/drivers/iommu/mt6989/vendor/drivers/misc/mediatek/iommu
subdir-ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include
subdir-ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include/mt-plat
subdir-ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/mtk-interconnect
subdir-ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/slbc
subdir-ccflags-y += -I$(srctree)/drivers/clk/mediatek/mt6989-fmeter
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/geniezone/public
obj-y += camsys/rpmsg/ camsys/remoteproc/ ccusys/ imgsensor/src-v4l2/ camsys/isp7sp/cam/

obj-$(CONFIG_VIDEO_ROTHKO_CAMERA_COMPOSITE) += camera_af_media.o
obj-$(CONFIG_VIDEO_ROTHKO_CAMERA_AF) += main_vcm.o main2_vcm.o
camera_af_media-y := af/mtk-composite.o
main_vcm-y := af/main_vcm.o
main2_vcm-y := af/main2_vcm.o

obj-$(CONFIG_VIDEO_ROTHKO_CAMERA_PDA) += pda_drv_mt6989.o
pda_drv_mt6989-y := pda/camera_pda.o pda/pda_mt6989.o

obj-$(CONFIG_VIDEO_ROTHKO_CAMERA_DPE) += camera_dpe_isp7sp.o
camera_dpe_isp7sp-y := dpe/camera_dpe.o dpe/engine_request.o dpe/videobuf2-dma-contig.o

obj-$(CONFIG_VIDEO_ROTHKO_CAMERA_EEPROM) += camera_eeprom_v4l2.o
camera_eeprom_v4l2-y := cam_cal/eeprom_driver.o

obj-$(CONFIG_VIDEO_ROTHKO_CAMERA_TORCH) += ocp81373.o
CFLAGS_ocp81373.o += -I$(srctree)/drivers/power/supply/rothko-ppb
