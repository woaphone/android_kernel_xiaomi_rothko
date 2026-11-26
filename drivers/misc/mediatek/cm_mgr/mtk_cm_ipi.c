// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/scmi_protocol.h>
#include <linux/soc/mediatek/tinysys-scmi.h>

#include "mtk_cm_ipi.h"

static DEFINE_MUTEX(cm_ipi_lock);
static int cm_ipi_enable;
static u32 scmi_cm_id;
static const struct scmi_protocol_handle *cm_ph;

int cm_get_ipi_enable(void)
{
	return READ_ONCE(cm_ipi_enable);
}
EXPORT_SYMBOL(cm_get_ipi_enable);

void cm_set_ipi_enable(int enable)
{
	mutex_lock(&cm_ipi_lock);
	WRITE_ONCE(cm_ipi_enable, cm_ph && enable == 1);
	mutex_unlock(&cm_ipi_lock);
}
EXPORT_SYMBOL(cm_set_ipi_enable);

int cm_mgr_to_sspm_command(unsigned int cmd, unsigned int val)
{
	struct scmi_tinysys_status rvalue;
	unsigned int type = cmd & 0xf0000000;
	int ret;

	mutex_lock(&cm_ipi_lock);
	if (!cm_ipi_enable) {
		ret = -ENODEV;
		goto out;
	}
	cmd &= 0x0fffffff;
	switch (type) {
	case IPI_CM_MGR_SCMI_SET:
		ret = scmi_tinysys_common_set(cm_ph, scmi_cm_id, cmd, val, 0, 0, 0);
		break;
	case IPI_CM_MGR_SCMI_GET:
		ret = scmi_tinysys_common_get(cm_ph, scmi_cm_id, cmd, &rvalue);
		if (!ret)
			ret = rvalue.r1;
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&cm_ipi_lock);
	return ret;
}
EXPORT_SYMBOL(cm_mgr_to_sspm_command);

static void cm_ipi_release(void *unused)
{
	mutex_lock(&cm_ipi_lock);
	WRITE_ONCE(cm_ipi_enable, 0);
	cm_ph = NULL;
	mutex_unlock(&cm_ipi_lock);
}

int cm_ipi_init(struct device *dev)
{
	struct scmi_tinysys_info_st *info = get_scmi_tinysys_info();
	u32 id;
	int ret;

	if (!info)
		return -EPROBE_DEFER;
	if (!device_link_add(dev, &info->sdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	ret = of_property_read_u32(info->sdev->dev.of_node, "scmi-cm", &id);
	if (ret)
		return ret;
	mutex_lock(&cm_ipi_lock);
	if (cm_ph) {
		mutex_unlock(&cm_ipi_lock);
		return -EBUSY;
	}
	scmi_cm_id = id;
	cm_ph = info->ph;
	WRITE_ONCE(cm_ipi_enable, 1);
	mutex_unlock(&cm_ipi_lock);
	return devm_add_action_or_reset(dev, cm_ipi_release, NULL);
}
EXPORT_SYMBOL(cm_ipi_init);

MODULE_DESCRIPTION("MediaTek CM IPI over Tinysys SCMI");
MODULE_LICENSE("GPL");
