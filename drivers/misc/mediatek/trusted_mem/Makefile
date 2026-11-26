# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_MTK_TRUSTED_MEMORY_SUBSYSTEM) += tmem_ffa.o
tmem_ffa-y := dev_mgr.o entry.o region_mgr.o ssmr_mgr.o peer_mgr.o proc.o \
 ssheap_proc.o alloc_api.o ssmr/memory_ssmr.o ssheap/ssheap_lib.o \
 mtee_devices.o mtee_impl/mtee_ops.o mtee_impl/tmem_ffa.o
ccflags-y += -DMTEE_DEVICES_SUPPORT
ccflags-y += -I$(src) -I$(src)/public -I$(src)/ssmr -I$(src)/ssheap
