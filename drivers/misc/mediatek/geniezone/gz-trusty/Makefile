ccflags-y += -I$(srctree)/drivers/misc/mediatek/geniezone/public

obj-$(CONFIG_MTK_ENABLE_GENIEZONE) += gz_trusty_mod.o
gz_trusty_mod-y := trusty.o smcnr-table.o

obj-$(CONFIG_MTK_GZ_IRQ) += gz_irq_mod.o
gz_irq_mod-y := trusty-irq.o

obj-$(CONFIG_MTK_GZ_VIRTIO) += gz_virtio_mod.o
gz_virtio_mod-y := trusty-virtio.o trusty-mem.o

obj-$(CONFIG_MTK_GZ_VIRTIO_IPC) += gz_ipc_mod.o
gz_ipc_mod-y := trusty-ipc.o
