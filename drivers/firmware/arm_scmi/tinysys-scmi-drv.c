// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2021 MediaTek Inc. */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/scmi_protocol.h>
#include <linux/soc/mediatek/tinysys-scmi.h>

static struct scmi_tinysys_info_st *t_info;
static const struct scmi_tinysys_proto_ops *tinysys_ops;
static f_handler_t cb_array[SCMI_TINYSYS_CB_MAX];
static struct notifier_block tinysys_nb[SCMI_TINYSYS_CB_MAX];
static bool notify_enabled[SCMI_TINYSYS_CB_MAX];
static DEFINE_MUTEX(tinysys_notify_lock);

struct scmi_tinysys_info_st *get_scmi_tinysys_info(void)
{
	return smp_load_acquire(&t_info);
}
EXPORT_SYMBOL(get_scmi_tinysys_info);

int scmi_tinysys_common_set(const struct scmi_protocol_handle *ph, u32 feature_id,
			  u32 p1, u32 p2, u32 p3, u32 p4, u32 p5)
{
	const struct scmi_tinysys_proto_ops *ops = READ_ONCE(tinysys_ops);

	if (!ops || !ph)
		return -ENODEV;
	return ops->common_set(ph, feature_id, p1, p2, p3, p4, p5);
}
EXPORT_SYMBOL(scmi_tinysys_common_set);

int scmi_tinysys_common_get(const struct scmi_protocol_handle *ph, u32 feature_id,
			  u32 p1, struct scmi_tinysys_status *rvalue)
{
	const struct scmi_tinysys_proto_ops *ops = READ_ONCE(tinysys_ops);

	if (!ops || !ph)
		return -ENODEV;
	return ops->common_get(ph, feature_id, p1, rvalue);
}
EXPORT_SYMBOL(scmi_tinysys_common_get);

static int scmi_tinysys_notifier_fn(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	const struct scmi_tinysys_notifier_report *r = data;
	f_handler_t func;
	scmi_tinysys_report report;

	if (r->f_id >= SCMI_TINYSYS_CB_MAX)
		return NOTIFY_DONE;
	func = READ_ONCE(cb_array[r->f_id]);
	if (!func)
		return NOTIFY_DONE;

	report.feature_id = r->f_id;
	report.p1 = r->p1_status;
	report.p2 = r->p2_status;
	report.p3 = r->p3_status;
	report.p4 = r->p4_status;
	func(r->f_id, &report);
	return NOTIFY_OK;
}

int scmi_tinysys_event_notify(u32 feature_id, u32 notify_enable)
{
	struct scmi_device *sdev;
	struct scmi_tinysys_info_st *info;
	const struct scmi_notify_ops *ops;
	int ret = 0;

	if (feature_id >= SCMI_TINYSYS_CB_MAX)
		return -EINVAL;

	mutex_lock(&tinysys_notify_lock);
	info = get_scmi_tinysys_info();
	if (!info) {
		ret = -ENODEV;
		goto out;
	}
	sdev = info->sdev;
	ops = sdev->handle->notify_ops;
	if (!ops) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (!!notify_enable == notify_enabled[feature_id])
		goto out;

	if (notify_enable)
		ret = ops->devm_event_notifier_register(sdev,
				SCMI_PROTOCOL_TINYSYS, SCMI_EVENT_TINYSYS_NOTIFIER,
				&feature_id, &tinysys_nb[feature_id]);
	else
		ret = ops->devm_event_notifier_unregister(sdev,
							 &tinysys_nb[feature_id]);
	if (!ret)
		notify_enabled[feature_id] = !!notify_enable;
out:
	mutex_unlock(&tinysys_notify_lock);
	return ret;
}
EXPORT_SYMBOL(scmi_tinysys_event_notify);

void scmi_tinysys_register_event_notifier(u32 feature_id, f_handler_t hand)
{
	if (feature_id < SCMI_TINYSYS_CB_MAX)
		WRITE_ONCE(cb_array[feature_id], hand);
	else
		pr_notice("Tinysys feature ID %u exceeds callback table\n", feature_id);
}
EXPORT_SYMBOL(scmi_tinysys_register_event_notifier);

static void scmi_tinysys_unregister_action(void *unused)
{
	scmi_tinysys_unregister();
}

static int scmi_tinysys_probe(struct scmi_device *sdev)
{
	const struct scmi_handle *handle = sdev->handle;
	const struct scmi_tinysys_proto_ops *ops;
	struct scmi_tinysys_info_st *info;
	struct scmi_protocol_handle *ph;
	int i, ret;

	/* This 0x80 wire protocol is the MT6989 board's Tinysys protocol. */
	if (!of_machine_is_compatible("mediatek,MT6989"))
		return -ENODEV;
	if (!handle)
		return -ENODEV;
	if (t_info)
		return -EBUSY;

	ret = scmi_tinysys_register(handle->version->vendor_id);
	if (ret)
		return ret;
	/* Added first: devres drops the protocol handle before unregistering. */
	ret = devm_add_action_or_reset(&sdev->dev,
				      scmi_tinysys_unregister_action, NULL);
	if (ret)
		return ret;

	ops = handle->devm_protocol_get(sdev, SCMI_PROTOCOL_TINYSYS, &ph);
	if (IS_ERR(ops))
		return PTR_ERR(ops);

	info = devm_kzalloc(&sdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->sdev = sdev;
	info->ph = ph;
	for (i = 0; i < SCMI_TINYSYS_CB_MAX; i++)
		tinysys_nb[i].notifier_call = scmi_tinysys_notifier_fn;
	WRITE_ONCE(tinysys_ops, ops);
	smp_store_release(&t_info, info);
	return 0;
}

static void scmi_tinysys_remove(struct scmi_device *sdev)
{
	int i;

	mutex_lock(&tinysys_notify_lock);
	WRITE_ONCE(t_info, NULL);
	WRITE_ONCE(tinysys_ops, NULL);
	/* The managed SCMI notifiers are released before the protocol handle. */
	for (i = 0; i < SCMI_TINYSYS_CB_MAX; i++) {
		notify_enabled[i] = false;
		WRITE_ONCE(cb_array[i], NULL);
	}
	mutex_unlock(&tinysys_notify_lock);
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_TINYSYS, "tinysys" },
	{ }
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_tinysys_driver = {
	.name = "scmi-tinysys",
	.probe = scmi_tinysys_probe,
	.remove = scmi_tinysys_remove,
	.id_table = scmi_id_table,
};
module_scmi_driver(scmi_tinysys_driver);
MODULE_DESCRIPTION("SCMI MediaTek Tinysys driver");
MODULE_LICENSE("GPL v2");
