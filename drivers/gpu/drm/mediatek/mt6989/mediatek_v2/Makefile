subdir-ccflags-y += -Werror



CFLAGS_mtk_layering_rule_base.o := -I$(MT6989_DISPLAY_PATH)/mediatek_v2
CFLAGS_mtk_drm_ddp.o += -I$(srctree)/sound/soc/mediatek/rothko-mt6989/common

mediatek-drm-y := mtk_drm_drv.o \
			  rothko_fod.o \
		  mtk_disp_tdshp.o \
		  mtk_disp_color.o \
		  mtk_disp_ccorr.o \
		  mtk_disp_c3d.o   \
		  mtk_disp_gamma.o \
		  mtk_disp_aal.o \
		  mtk_disp_dither.o \
		  mtk_disp_chist.o \
		  mtk_disp_ovl.o \
		  mtk_disp_rdma.o \
		  mtk_disp_mdp_rdma.o \
		  mtk_disp_rsz.o \
		  mtk_disp_mdp_rsz.o \
		  mtk_disp_wdma.o \
		  mtk_drm_crtc.o \
		  mtk_drm_ddp.o \
		  mtk_drm_ddp_addon.o \
		  mtk_drm_ddp_comp.o \
		  mtk_drm_fb.o \
		  mtk_drm_gem.o \
		  mtk_drm_plane.o \
		  mtk_dsi.o \
		  mtk_mipi_tx.o \
		  mtk_writeback.o \
		  mtk_fence.o \
		  mtk_drm_session.o \
		  mtk_dump.o \
		  mtk_debug.o \
		  mtk_layering_rule.o \
		  mtk_layering_rule_base.o \
		  mtk_rect.o \
		  mtk_drm_mmp.o \
		  mtk_drm_trace.o \
		  mtk_drm_helper.o \
		  mtk_drm_lowpower.o \
		  mtk_disp_postmask.o \
		  mtk_drm_assert.o \
		  mtk_drm_fbconsole.o \
		  mtk_disp_recovery.o \
		  mtk_disp_pmqos.o \
		  mtk_disp_dsc.o \
		  mtk_disp_cm.o \
		  mtk_disp_spr.o \
		  mtk_disp_postalign.o \
		  mtk_disp_oddmr/mtk_disp_oddmr_parse_data.o \
		  mtk_disp_oddmr/mtk_disp_oddmr.o \
		  mtk_drm_arr.o \
		  mtk_disp_merge.o \
		  mtk_dmdp_aal.o \
		  mtk_fbconfig_kdebug.o \
		  mtk_disp_y2r0.o \
		  mtk_disp_dlo_async.o \
		  mtk_disp_dli_async.o \
		  mtk_disp_inlinerotate.o \
		  mtk_mmlsys_bypass_mode.o \
		  mtk_lease.o \
		  platform/mtk_drm_6835.o \
		  mtk_disp_pq_helper.o \
		  mtk_disp_vidle.o \
		  mtk_disp_r2y0.o \
		  mtk_disp_splitter.o

#ccflags-y += -DDRM_CMDQ_DISABLE
#mediatek-drm-y += mtk_cmdq_dummy.o

ifeq ($(CONFIG_DRM_MEDIATEK_DEBUG_FS),y)
mediatek-drm-y += mtk_drm_debugfs.o
endif

mediatek-drm-y += mtk_dp_intf.o \
	mtk_dp.o \
	mtk_dp_hal.o \
	mtk_dp_debug.o



#ifeq ($(CONFIG_TRUSTONIC_TEE_SUPPORT),m)
#obj-$(CONFIG_DRM_MEDIATEK_MT6989) += ca/
#ccflags-y += -DDPTX_HDCP_ENABLE
#mediatek-drm-y += mtk_dp_hdcp1x.o \
#	mtk_dp_hdcp2.o
#endif

mediatek-drm-$(CONFIG_DRM_FBDEV_EMULATION) += mtk_drm_fbdev.o

obj-$(CONFIG_DRM_MEDIATEK_MT6989) += mediatek-drm.o
obj-$(CONFIG_DRM_MEDIATEK_MT6989) += mtk_sync.o

obj-$(CONFIG_DRM_MEDIATEK_MT6989) += mtk_panel_ext.o
obj-$(CONFIG_DRM_MEDIATEK_MT6989) += mtk_disp_notify.o
obj-$(CONFIG_MTK_AOD_SCP) += mtk_aod_scp.o

obj-$(CONFIG_MTK_DISP_SEC) += mtk_disp_sec.o

ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/scp/include/
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/smi/			\
	     -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mminfra/			\
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mmp/			\
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mmdvfs/			\
             -I$(srctree)/drivers/iommu/mt6989/				\
             -I$(srctree)/drivers/gpu/drm/				\
             -I$(srctree)/drivers/gpu/drm/display/				\
             -I$(MT6989_DISPLAY_PATH)/include/mt-plat/ \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/cmdq/mailbox/        \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/iommu/		\
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/mtk-interconnect/        \
             -I$(MT6989_DISPLAY_PATH)/mediatek_v2/ \
             -I$(srctree)/drivers/gpu/drm/mediatek \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/slbc/ \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/vcp/rv/ \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/vcp/include/ \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/cmdq/mailbox/ \
             -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/tinysys_scmi/ \
             -I$(srctree)/drivers/dma-buf/heaps/mt6989/ \
             -I$(MT6989_DISPLAY_PATH)/mml \
             -I$(MT6989_DISPLAY_PATH)/dpc \
	     -I$(DEVICE_MODULES_PATH)/drivers/clk/mediatek

ccflags-$(CONFIG_MTK_CMDQ_MBOX_EXT) += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/cmdq/mailbox

#ccflags-y += -DDRM_MMPATH
ccflags-y += -DNOT_SUPPORT_CABC_HW
ccflags-y += -DDISP_MDP_COLOR_ON
ccflags-y += -DCCORR_SUPPORT

subdir-ccflags-y += -I$(MT6989_DISPLAY_PATH)/include/
subdir-ccflags-y += -I$(MT6989_DISPLAY_PATH)/include/mt-plat/

