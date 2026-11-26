# SPDX-License-Identifier: GPL-2.0
# MT6989 v061 object list from the MediaTek OnePlus donor (d60d564c).

obj-$(CONFIG_MTK_ROTHKO_GPS_DATA_LINK) += gps_drv_dl_v061.o

gps_drv_dl_v061-y := \
	fw_log/fw_log_gps_lib.o \
	fw_log_gps.o \
	gps_dl_context.o \
	gps_dl_hw_dep_adie_ctrl_v061_1.o \
	gps_dl_hw_dep_adie_ctrl_v061_3.o \
	gps_dl_hw_dep_bgf.o \
	gps_dl_hw_dep_debug.o \
	gps_dl_hw_dep_dma.o \
	gps_dl_hw_dep_gps.o \
	gps_dl_hw_dep_met2_0.o \
	gps_dl_module.o \
	gps_each_link.o \
	gps_mcudl/data_intf/common/gps_mcudl_data_pkt_parser.o \
	gps_mcudl/data_intf/common/gps_mcudl_data_pkt_rbuf.o \
	gps_mcudl/data_intf/common/gps_mcudl_data_pkt_slot.o \
	gps_mcudl/data_intf/host/gps_mcudl_data_pkt_host_impl.o \
	gps_mcudl/fw/gps_mcudl_fw_code.o \
	gps_mcudl/gps_hif/common/gps_mcu_hif_common.o \
	gps_mcudl/gps_hif/common/gps_mcu_hif_core.o \
	gps_mcudl/gps_hif/host/gps_mcu_hif_host.o \
	gps_mcudl/gps_hif/host/gps_mcu_hif_mgmt_cmd_send.o \
	gps_mcudl/gps_mcudl_context.o \
	gps_mcudl/gps_mcudl_name_list.o \
	gps_mcudl/hal/gps_mcudl_hal_ccif.o \
	gps_mcudl/hal/gps_mcudl_hal_conn.o \
	gps_mcudl/hal/gps_mcudl_hal_mcu.o \
	gps_mcudl/hal/gps_mcudl_hal_user_fw_own_ctrl.o \
	gps_mcudl/hw/gps_mcudl_hw_atf_ccif.o \
	gps_mcudl/hw/gps_mcudl_hw_atf_mcu.o \
	gps_mcudl/linux/gps_mcudl_devices.o \
	gps_mcudl/linux/gps_mcudl_each_device.o \
	gps_mcudl/linux/gps_mcudl_linux_xlink_buf.o \
	gps_mcudl/linux/gps_nv_each_device.o \
	gps_mcudl/mcu_sys/common/gps_mcusys_nv_common_impl.o \
	gps_mcudl/mcu_sys/host/gps_mcusys_data_dispatcher_host.o \
	gps_mcudl/mcu_sys/host/gps_mcusys_data_sync2target.o \
	gps_mcudl/mcu_sys/host/gps_mcusys_fsm_host.o \
	gps_mcudl/mcu_sys/host/gps_mcusys_nv_data_host.o \
	gps_mcudl/mcu_sys/host/gps_mcusys_nv_lock_host.o \
	gps_mcudl/plat/gps_mcudl_plat_dedicated_mcu.o \
	gps_mcudl/xlink/gps_mcudl_each_link.o \
	gps_mcudl/xlink/gps_mcudl_link_state.o \
	gps_mcudl/xlink/gps_mcudl_link_sync.o \
	gps_mcudl/xlink/gps_mcudl_link_util.o \
	gps_mcudl/xlink/gps_mcudl_reset.o \
	gps_mcudl/xlink/gps_mcudl_xlink.o \
	gps_mcudl/xlink/gps_mcudl_xlink_event_proc.o \
	gps_mcudl/ylink/gps_mcudl_ylink.o \
	gps_mcudl/ylink/gps_mcudl_ylink_event_proc.o \
	hal/gps_dl_conn_infra.o \
	hal/gps_dl_dma.o \
	hal/gps_dl_hal.o \
	hal/gps_dl_hal_met2_0.o \
	hal/gps_dl_hal_util.o \
	hal/gps_dl_isr.o \
	hal/gps_dl_mcub.o \
	hal/gps_dl_power_ctrl.o \
	hal/gps_dl_zbus.o \
	hal/gps_dsp_fsm.o \
	hw/gps_dl_hw_atf_conn_infra.o \
	hw/gps_dl_hw_atf_gps.o \
	hw/gps_dl_hw_atf_mvcd.o \
	hw/gps_dl_hw_atf_power_ctrl.o \
	hw/gps_dl_hw_atf_usrt_apb.o \
	hw/gps_dl_hw_bgf.o \
	hw/gps_dl_hw_common.o \
	hw/gps_dl_hw_semaphore.o \
	hw/gps_dl_hw_util.o \
	lib/gps_dl_dma_buf.o \
	lib/gps_dl_dma_buf_lock.o \
	lib/gps_dl_hist_rec.o \
	lib/gps_dl_hist_rec2.o \
	lib/gps_dl_lib_misc.o \
	lib/gps_dl_name_list.o \
	lib/gps_dl_time_tick.o \
	link/gps_dl_link_event_proc.o \
	link/gps_dl_link_hal_ctrl.o \
	link/gps_dl_link_irq_ctrl.o \
	link/gps_dl_link_state.o \
	link/gps_dl_link_sync.o \
	link/gps_dl_link_util.o \
	link/gps_dl_subsys_reset.o \
	linux/gps_data_link_devices.o \
	linux/gps_dl_ctrld.o \
	linux/gps_dl_emi.o \
	linux/gps_dl_info_node.o \
	linux/gps_dl_iomem_dump.o \
	linux/gps_dl_linux.o \
	linux/gps_dl_linux_clock_mng.o \
	linux/gps_dl_linux_plat_drv.o \
	linux/gps_dl_linux_reserved_mem.o \
	linux/gps_dl_linux_reserved_mem_v2.o \
	linux/gps_dl_osal.o \
	linux/gps_dl_procfs.o \
	linux/gps_each_device.o

# MCU ROM boot stays enabled independently of the optional AP firmware loader.
gps_drv_dl_v061-$(CONFIG_MTK_ROTHKO_GPS_DATA_LINK_FW_LOADER) += gps_mcudl/linux/gps_mcudl_linux_fw_loading.o

ccflags-y += -DMTK_GENERIC_HAL -DGPS_DL_HAS_CONNINFRA_DRV=1
ccflags-y += -DGPS_DL_HAS_MCUDL=1 -DGPS_DL_HAS_MCUDL_HAL=1
ccflags-y += -DGPS_DL_HAS_MCUDL_HW=1 -DGPS_DL_HAS_MCUDL_FW=1
ccflags-y += -Wno-error=missing-prototypes

ccflags-y += -I$(src) -I$(src)/inc -I$(src)/hw/inc/v061
ccflags-y += -I$(src)/common-inc -I$(src)/hw/inc/v060
ccflags-y += -I$(src)/linux/inc -I$(src)/link/inc -I$(src)/lib/inc
ccflags-y += -I$(src)/hal/inc -I$(src)/hw/inc -I$(src)/hw
ccflags-y += -I$(src)/fw_log/inc -I$(src)/gps_mcudl
ccflags-y += -I$(src)/gps_mcudl/inc
ccflags-y += -I$(src)/gps_mcudl/linux/inc
ccflags-y += -I$(src)/gps_mcudl/xlink/inc
ccflags-y += -I$(src)/gps_mcudl/ylink/inc
ccflags-y += -I$(src)/gps_mcudl/hal/inc
ccflags-y += -I$(src)/gps_mcudl/hw/inc
ccflags-y += -I$(src)/gps_mcudl/fw/inc
ccflags-y += -I$(src)/gps_mcudl/plat/v060/hw/inc
ccflags-y += -I$(src)/gps_mcudl/plat/v060/fw/inc
ccflags-y += -I$(src)/gps_mcudl/mcu_sys/common/inc
ccflags-y += -I$(src)/gps_mcudl/mcu_sys/host/inc
ccflags-y += -I$(src)/gps_mcudl/data_intf/common/inc
ccflags-y += -I$(src)/gps_mcudl/data_intf/host/inc
ccflags-y += -I$(src)/gps_mcudl/gps_hif/common/inc
ccflags-y += -I$(src)/gps_mcudl/gps_hif/host/inc

ccflags-y += -I$(src)/../conninfra/include
ccflags-y += -I$(src)/../conninfra/conn_drv/connv2/debug_utility
ccflags-y += -I$(src)/../conninfra/conn_drv/connv2/debug_utility/include
ccflags-y += -I$(src)/../conninfra/conn_drv/connv2/debug_utility/metlog
ccflags-y += -I$(src)/../conninfra/conn_drv/connv2/debug_utility/connsyslog
ccflags-y += -I$(src)/../conninfra/conn_drv/connv2/debug_utility/coredump
