ccflags-y += -I$(src)/include
ccflags-y += -I$(src)/include/mt-plat
ccflags-y += -I$(src)/eccci
ccflags-y += -I$(src)/eccci/inc
ccflags-y += -I$(src)/eccci/fsm
ccflags-y += -I$(src)/eccci/hif
ccflags-y += -I$(src)/eccci/port
ccflags-y += -I$(src)/eccci/udc
ccflags-y += -I$(src)/eccci/udc/udc_lib_inc
ccflags-y += -I$(src)/ccci_util
ccflags-y += -I$(src)/ccmni
ccflags-y += -I$(src)/conn_md/include
ccflags-y += -I$(src)/memory-amms
ccflags-y += -I$(src)/rps
ccflags-y += -DCCCI_KMODULE_ENABLE

obj-$(CONFIG_MTK_ECCCI_DRIVER) += ccci_util_lib.o
ccci_util_lib-y := ccci_util/ccci_util_lib_fo.o \
	ccci_util/ccci_util_lib_load_img.o ccci_util/ccci_util_lib_sys.o \
	ccci_util/ccci_private_log.o ccci_util/ccci_util_lib_main.o \
	ccci_util/ccci_util_ld_md_errno.o ccci_util/ccci_util_broadcast.o \
	ccci_util/ccci_util_pin_broadcast.o ccci_util/ccci_util_sib.o \
	ccci_util/ccci_util_lib_reserved_mem.o ccci_util/ccci_util_md_rat.o \
	ccci_util/ccci_util_boot_args.o ccci_util/ccci_util_img_parsing.o \
	ccci_util/ccci_util_md_mem.o

obj-$(CONFIG_MTK_ECCCI_DRIVER) += ccci_ccif.o ccci_dpmaif.o ccci_cldma.o
obj-$(CONFIG_MTK_NET_CCMNI) += ccmni/ccmni.o
obj-$(CONFIG_MTK_NET_RPS) += rps/rps_perf.o
obj-$(CONFIG_MTK_CONN_MD) += conn_md_drv.o
obj-$(CONFIG_MTK_AMMS) += memory-amms/memory-amms.o
obj-$(CONFIG_MTK_ECCCI_DRIVER) += udc_lib/
obj-$(CONFIG_MTK_ECCCI_DRIVER) += ccci_md_all.o
ccci_md_all-y := eccci/ccci_core.o eccci/ccci_bm.o eccci/ccci_modem.o \
	eccci/ccci_auxadc.o eccci/udc/udc.o eccci/hif/ccci_hif.o \
	eccci/port/port_cfg.o eccci/port/port_char.o eccci/port/port_ctlmsg.o \
	eccci/port/port_udc.o eccci/port/port_ipc.o eccci/port/port_net.o \
	eccci/port/port_poller.o eccci/port/port_proxy.o eccci/port/port_rpc.o \
	eccci/port/port_smem.o eccci/port/port_sysmsg.o eccci/port/ccci_swtp.o \
	eccci/port/port_kernel_interface.o eccci/fsm/ccci_fsm.o \
	eccci/fsm/ccci_fsm_ioctl.o eccci/fsm/ccci_fsm_monitor.o \
	eccci/fsm/ccci_fsm_poller.o eccci/fsm/ccci_fsm_ee.o \
	eccci/fsm/ccci_fsm_sys.o eccci/fsm/mdee_dumper_v1.o \
	eccci/fsm/mdee_dumper_v2.o eccci/fsm/mdee_dumper_v3.o \
	eccci/fsm/mdee_dumper_v5.o eccci/fsm/mdee_dumper_v6.o \
	eccci/fsm/modem_sys1.o eccci/fsm/md_sys1_platform.o \
	eccci/fsm/ap_md_reg_dump.o eccci/fsm/ap_md_mem.o

obj-$(CONFIG_MTK_ECCCI_DRIVER) += ccci_fsm_scp.o
ccci_fsm_scp-y := eccci/fsm/ccci_fsm_scp_c.o
scp-src := $(srctree)/drivers/gpu/drm/mediatek/mt6989/vendor/drivers/misc/mediatek/scp
CFLAGS_eccci/fsm/ccci_fsm_scp_c.o += -I$(scp-src)/include -I$(scp-src)/rv
ccci_ccif-y := eccci/hif/ccci_ringbuf.o eccci/hif/ccci_hif_ccif.o
ccci_cldma-y := eccci/hif/ccci_cldma_plat.o eccci/hif/ccci_hif_cldma.o
ccci_dpmaif-y := eccci/hif/net_pool.o eccci/hif/md_spd_dvfs_fn.o \
	eccci/hif/md_spd_dvfs_method.o eccci/hif/net_speed.o \
	eccci/hif/ccci_dpmaif_com.o eccci/hif/ccci_dpmaif_bat.o \
	eccci/hif/ccci_dpmaif_debug.o eccci/hif/ccci_dpmaif_resv_mem.o \
	eccci/hif/ccci_dpmaif_drv_com.o eccci/hif/ccci_dpmaif_drv_v1.o \
	eccci/hif/ccci_dpmaif_drv_v2.o eccci/hif/ccci_dpmaif_drv_v3.o
ifeq ($(CONFIG_PAGE_POOL),y)
ccflags-y += -DRX_PAGE_POOL
ccci_dpmaif-y += eccci/hif/ccci_dpmaif_page_pool.o
endif

conn_md_drv-y := conn_md/conn_md.o conn_md/conn_md_exp.o \
	conn_md/conn_md_log.o conn_md/conn_md_dump.o conn_md/conn_md_dbg.o \
	conn_md/conn_md_test.o
