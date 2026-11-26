obj-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_helper.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_excep.o
ifneq ($(filter y m,$(CONFIG_OPLUS_SENSOR_WAKEUP_TRACE)),)
CFLAGS_scp_wrapper_ipi.o += -DOPLUS_FEATURE_SENSOR_WAKEUP_TRACE
endif
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_wrapper_ipi.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_irq.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_logger.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_l1c.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_scpctl.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_chre_manager.o

scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_feature_table.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_awake.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_dvfs.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_hwvoter_dbg.o

scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_low_pwr_dbg.o
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp_mbrain_dbg.o
ifeq ($(CONFIG_SND_SOC_ROTHKO_MT6989),)
scp-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += mtk-afe-external.o
endif

scp-$(CONFIG_MTK_TINYSYS_SAP_SUPPORT) += sap.o

ccflags-y += -D DEBUG_DO
ccflags-y += -I$(src) -I$(src)/../include
ccflags-y += -I$(srctree)/drivers/clk/mediatek/mt6989-fmeter
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include
