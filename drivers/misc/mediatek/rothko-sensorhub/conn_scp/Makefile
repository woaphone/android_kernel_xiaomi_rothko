# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(srctree)/drivers/misc/mediatek/rothko-sensorhub/conn_scp/conap_scp
ccflags-y += -I$(srctree)/drivers/misc/mediatek/rothko-sensorhub/conn_scp/aolserv
ccflags-y += -I$(srctree)/drivers/misc/mediatek/rothko-sensorhub/conn_scp/aolextra
ccflags-y += -I$(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/scp/include
ccflags-y += -I$(srctree)/drivers/net/wireless/mediatek/rothko-conninfra/vendor/drivers/misc/mediatek/connectivity/common

obj-$(CONFIG_MTK_CONN_SCP) += connscp.o
connscp-y := conap_scp/msg_thread.o conap_scp/conap_scp_core.o \
	conap_scp/conap_scp_ipi.o conap_scp/conap_platform_data.o conn_scp.o \
	aolserv/aol_buf_list.o aolserv/aol_flp.o aolserv/aol_geofence.o \
	aolextra/aolextra_core.o aolextra/aolextra_netlink.o \
	aolextra/aolextra_ring_buffer.o
