obj-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_status.o

obj-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_helper.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_excep.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_irq.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_logger.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_l1c.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_vcpctl.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_feature_table.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_awake.o
vcp-$(CONFIG_MTK_TINYSYS_VCP_SUPPORT) += vcp_hwvoter_dbg.o

ccflags-y += -D DEBUG_DO
ifeq ($(CONFIG_MTK_TINYSYS_VCP_SUPPORT),m)
ccflags-y += -fno-pic -mcmodel=large
endif
ccflags-y += -I$(src)/../include
ccflags-y += -I$(src)
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/include/mt-plat
ccflags-y += -I$(srctree)/drivers/clk/mediatek
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/clk/mediatek
ccflags-y += -I$(srctree)/drivers/iommu/mt6989/arm/arm-smmu-v3
ccflags-y += -I$(srctree)/drivers/dma-buf/heaps/mt6989
