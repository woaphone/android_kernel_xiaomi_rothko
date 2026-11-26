# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_VIDEO_MEDIATEK_JPEG) += mtk_jpeg.o \
	mtk-jpeg-enc-hw.o \
	mtk-jpeg-dec-hw.o

mtk_jpeg-y := mtk_jpeg_core.o \
		 mtk_jpeg_dec_parse.o

mtk-jpeg-enc-hw-y := mtk_jpeg_enc_hw.o
mtk-jpeg-dec-hw-y := mtk_jpeg_dec_hw.o

jpeg_mt6989 := $(srctree)/drivers/gpu/drm/mediatek/mt6989
CFLAGS_mtk_jpeg_core.o += -I$(jpeg_mt6989)/include
CFLAGS_mtk_jpeg_core.o += -I$(jpeg_mt6989)/vendor/drivers/misc/mediatek/mtk-interconnect
CFLAGS_mtk_jpeg_core.o += -I$(jpeg_mt6989)/vendor/drivers/misc/mediatek/slbc
