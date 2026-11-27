// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 * Author Wy Chuang<wy.chuang@mediatek.com>
 */

#include <linux/module.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/slab.h>

#include "adapter_class.h"

static struct class *adapter_class;

static ssize_t name_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct adapter_device *adapter_dev = to_adapter_device(dev);

	return snprintf(buf, 20, "%s\n",
		       adapter_dev->props.alias_name ?
		       adapter_dev->props.alias_name : "anonymous");
}

static void adapter_device_release(struct device *dev)
{
	struct adapter_device *adapter_dev = to_adapter_device(dev);

	kfree(adapter_dev);
}

int adapter_dev_get_property(struct adapter_device *adapter_dev,
	enum adapter_property sta)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->get_property)
		return adapter_dev->ops->get_property(adapter_dev, sta);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_get_property);

int adapter_dev_get_status(struct adapter_device *adapter_dev,
	struct adapter_status *sta)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->get_status)
		return adapter_dev->ops->get_status(adapter_dev, sta);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_get_status);

int adapter_dev_get_output(struct adapter_device *adapter_dev, int *mV, int *mA)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->get_output)
		return adapter_dev->ops->get_output(adapter_dev, mV, mA);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_get_output);

int adapter_dev_set_cap(struct adapter_device *adapter_dev,
	enum adapter_cap_type type,
	int mV, int mA)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->set_cap)
		return adapter_dev->ops->set_cap(adapter_dev, type, mV, mA);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_set_cap);


int adapter_dev_get_cap(struct adapter_device *adapter_dev,
	enum adapter_cap_type type,
	struct adapter_power_cap *cap)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
		adapter_dev->ops->get_cap)
		return adapter_dev->ops->get_cap(adapter_dev, type, cap);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_get_cap);

int adapter_dev_authentication(struct adapter_device *adapter_dev,
			       struct adapter_auth_data *data)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->authentication)
		return adapter_dev->ops->authentication(adapter_dev, data);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_authentication);

int adapter_dev_is_cc(struct adapter_device *adapter_dev, bool *cc)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->is_cc)
		return adapter_dev->ops->is_cc(adapter_dev, cc);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_is_cc);

int adapter_dev_set_wdt(struct adapter_device *adapter_dev, u32 ms)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->set_wdt)
		return adapter_dev->ops->set_wdt(adapter_dev, ms);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_set_wdt);

int adapter_dev_enable_wdt(struct adapter_device *adapter_dev, bool en)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->enable_wdt)
		return adapter_dev->ops->enable_wdt(adapter_dev, en);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_enable_wdt);

int adapter_dev_sync_volt(struct adapter_device *adapter_dev, u32 mV)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->sync_volt)
		return adapter_dev->ops->sync_volt(adapter_dev, mV);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_sync_volt);

int adapter_dev_send_hardreset(struct adapter_device *adapter_dev)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->send_hardreset)
		return adapter_dev->ops->send_hardreset(adapter_dev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_send_hardreset);

int adapter_dev_exit_mode(struct adapter_device *adapter_dev)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->exit_mode)
		return adapter_dev->ops->exit_mode(adapter_dev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_exit_mode);

static DEVICE_ATTR_RO(name);

int adapter_dev_get_svid(struct adapter_device *adapter_dev)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->get_svid)
		return adapter_dev->ops->get_svid(adapter_dev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_get_svid);

int adapter_dev_request_vdm_cmd(struct adapter_device *adapter_dev,
	enum uvdm_state cmd, unsigned char *data, unsigned int data_len)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->request_vdm_cmd)
		return adapter_dev->ops->request_vdm_cmd(adapter_dev, cmd, data, data_len);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_request_vdm_cmd);

int adapter_dev_get_power_role(struct adapter_device *adapter_dev)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->get_power_role)
		return adapter_dev->ops->get_power_role(adapter_dev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_get_power_role);

int adapter_dev_get_current_state(struct adapter_device *adapter_dev)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->get_pd_state)
		return adapter_dev->ops->get_pd_state(adapter_dev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_get_current_state);

int adapter_dev_get_pdos(struct adapter_device *adapter_dev)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->get_pdos)
		return adapter_dev->ops->get_pdos(adapter_dev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_get_pdos);

int adapter_dev_set_pd_verify_process(struct adapter_device *adapter_dev,
	int verifying)
{
	if (adapter_dev != NULL && adapter_dev->ops != NULL &&
	    adapter_dev->ops->set_pd_verify_process)
		return adapter_dev->ops->set_pd_verify_process(adapter_dev, verifying);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(adapter_dev_set_pd_verify_process);

static int string_to_hex(const char *str, unsigned char *out, unsigned int max_len)
{
	size_t len = strlen(str);
	unsigned int count = 0;
	size_t i = 0;

	if (len == 1) {
		int v = hex_to_bin(str[0]);

		if (v < 0)
			return -EINVAL;
		out[0] = (unsigned char)v;
		return 1;
	}

	while (i + 1 < len && count < max_len) {
		int hi = hex_to_bin(str[i]);
		int lo = hex_to_bin(str[i + 1]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[count++] = (unsigned char)((hi << 4) | lo);
		i += 2;
	}
	return count;
}

static ssize_t request_vdm_cmd_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct adapter_device *adapter = to_adapter_device(dev);
	int cmd = adapter->uvdm_state;

	switch (cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		return snprintf(buf, PAGE_SIZE, "%d,%x\n", cmd,
				adapter->vdm_data.ta_version);
	case USBPD_UVDM_CHARGER_VOLTAGE:
		return snprintf(buf, PAGE_SIZE, "%d,%d\n", cmd,
				adapter->vdm_data.ta_voltage);
	case USBPD_UVDM_CHARGER_TEMP:
		return snprintf(buf, PAGE_SIZE, "%d,%d\n", cmd,
				adapter->vdm_data.ta_temp);
	case USBPD_UVDM_AUTHENTICATION:
		return snprintf(buf, PAGE_SIZE, "%d,%08lx%08lx%08lx%08lx\n",
				cmd,
				adapter->vdm_data.digest[0],
				adapter->vdm_data.digest[1],
				adapter->vdm_data.digest[2],
				adapter->vdm_data.digest[3]);
	case USBPD_UVDM_REVERSE_AUTHEN:
		return snprintf(buf, PAGE_SIZE, "%d,%d\n", cmd,
				adapter->vdm_data.reauth ? 1 : 0);
	case USBPD_UVDM_DISCONNECT:
	case USBPD_UVDM_SESSION_SEED:
	case USBPD_UVDM_VERIFIED:
	case USBPD_UVDM_REMOVE_COMPENSATION:
	case USBPD_UVDM_CONNECT:
	case USBPD_UVDM_NAN_ACK:
		return snprintf(buf, PAGE_SIZE, "%d,Null\n", cmd);
	default:
		pr_err("feedbak cmd:%d is not support\n", cmd);
		return snprintf(buf, PAGE_SIZE, "%d,\n", cmd);
	}
}

static ssize_t request_vdm_cmd_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t size)
{
	struct adapter_device *adapter = to_adapter_device(dev);
	int cmd = 0;
	char str[65] = {0};
	unsigned char data[32] = {0};
	int count = 0;
	int ret;

	ret = sscanf(buf, "%d,%64s", &cmd, str);
	if (ret < 1)
		return -EINVAL;

	pr_info("%s: cmd:%d, buffer:%s\n", __func__, cmd, str);
	if (ret == 2 && str[0] != '\0') {
		count = string_to_hex(str, data, sizeof(data));
		if (count < 0)
			return count;
	}

	adapter_dev_request_vdm_cmd(adapter, (enum uvdm_state)cmd, data, count);
	return size;
}
static DEVICE_ATTR_RW(request_vdm_cmd);

static ssize_t current_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct adapter_device *adapter = to_adapter_device(dev);
	const char *state;

	adapter_dev_get_current_state(adapter);
	state = adapter->current_state_name ? adapter->current_state_name : "Unknown";
	pr_info("%s: current_state=%d (%s)\n", __func__,
		adapter->current_state, state);
	return snprintf(buf, PAGE_SIZE, "%s\n", state);
}
static DEVICE_ATTR_RO(current_state);

static ssize_t adapter_id_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct adapter_device *adapter = to_adapter_device(dev);

	adapter_dev_get_svid(adapter);
	pr_info("%s: adapter_id is %08x\n", __func__, adapter->adapter_id);
	return snprintf(buf, PAGE_SIZE, "%08x\n", adapter->adapter_id);
}
static DEVICE_ATTR_RO(adapter_id);

static ssize_t adapter_svid_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct adapter_device *adapter = to_adapter_device(dev);

	adapter_dev_get_svid(adapter);
	pr_info("%s: adapter_svid is %04x\n", __func__, adapter->adapter_svid);
	return snprintf(buf, PAGE_SIZE, "%04x\n", adapter->adapter_svid);
}
static DEVICE_ATTR_RO(adapter_svid);

static ssize_t verify_process_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct adapter_device *adapter = to_adapter_device(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", adapter->verify_process ? 1 : 0);
}

static ssize_t verify_process_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	struct adapter_device *adapter = to_adapter_device(dev);
	int val = 0;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	pr_info("%s: batterysecret verify process :%d\n", __func__, val);
	adapter->verify_process = !!val;
	adapter_dev_set_pd_verify_process(adapter, val);
	return size;
}
static DEVICE_ATTR_RW(verify_process);

static ssize_t usbpd_verifed_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct adapter_device *adapter = to_adapter_device(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", adapter->verifed ? 1 : 0);
}

static ssize_t usbpd_verifed_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	struct adapter_device *adapter = to_adapter_device(dev);
	struct adapter_power_cap cap;
	int val = 0;

	if (sscanf(buf, "%d", &val) != 1) {
		adapter->verifed = false;
		return -EINVAL;
	}

	pr_info("%s: batteryd set usbpd verifyed :%d\n", __func__, val);
	adapter->verifed = !!val;
	memset(&cap, 0, sizeof(cap));
	adapter_dev_get_cap(adapter, val ? MTK_PD_APDO_REGAIN : MTK_PD_APDO, &cap);
	return size;
}
static DEVICE_ATTR_RW(usbpd_verifed);

static ssize_t current_pr_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct adapter_device *adapter = to_adapter_device(dev);
	const char *pr = "none";

	adapter_dev_get_power_role(adapter);
	if (adapter->current_pr == 0)
		pr = "sink";
	else if (adapter->current_pr == 1)
		pr = "source";
	pr_info("%s: current_pr is %d\n", __func__, adapter->current_pr);
	return snprintf(buf, PAGE_SIZE, "%s\n", pr);
}
static DEVICE_ATTR_RO(current_pr);

static ssize_t pdo_show(struct device *dev, struct device_attribute *attr,
			char *buf, int idx)
{
	struct adapter_device *adapter = to_adapter_device(dev);

	if (idx < 0 || idx >= USBPD_PDO_MAX_NR)
		return -EINVAL;
	adapter_dev_get_pdos(adapter);
	return snprintf(buf, PAGE_SIZE, "%08x\n", adapter->received_pdos[idx]);
}

#define ADAPTER_PDO_ATTR(n)						\
static ssize_t pdo##n##_show(struct device *dev,			\
			     struct device_attribute *attr, char *buf)	\
{									\
	return pdo_show(dev, attr, buf, (n) - 1);			\
}									\
static DEVICE_ATTR_RO(pdo##n)

ADAPTER_PDO_ATTR(1);
ADAPTER_PDO_ATTR(2);
ADAPTER_PDO_ATTR(3);
ADAPTER_PDO_ATTR(4);
ADAPTER_PDO_ATTR(5);
ADAPTER_PDO_ATTR(6);
ADAPTER_PDO_ATTR(7);

static struct attribute *adapter_class_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_request_vdm_cmd.attr,
	&dev_attr_current_state.attr,
	&dev_attr_adapter_id.attr,
	&dev_attr_adapter_svid.attr,
	&dev_attr_verify_process.attr,
	&dev_attr_usbpd_verifed.attr,
	&dev_attr_current_pr.attr,
	&dev_attr_pdo1.attr,
	&dev_attr_pdo2.attr,
	&dev_attr_pdo3.attr,
	&dev_attr_pdo4.attr,
	&dev_attr_pdo5.attr,
	&dev_attr_pdo6.attr,
	&dev_attr_pdo7.attr,
	NULL,
};

static const struct attribute_group adapter_group = {
	.attrs = adapter_class_attrs,
};

static const struct attribute_group *adapter_groups[] = {
	&adapter_group,
	NULL,
};

int register_adapter_device_notifier(struct adapter_device *adapter_dev,
				struct notifier_block *nb)
{
	int ret = 0;

	if (!adapter_dev)
		return -ENODEV;

	ret = srcu_notifier_chain_register(&adapter_dev->evt_nh, nb);
	return ret;
}
EXPORT_SYMBOL(register_adapter_device_notifier);

int unregister_adapter_device_notifier(struct adapter_device *adapter_dev,
				struct notifier_block *nb)
{
	if (!adapter_dev)
		return -ENODEV;

	return srcu_notifier_chain_unregister(&adapter_dev->evt_nh, nb);
}
EXPORT_SYMBOL(unregister_adapter_device_notifier);

/**
 * adapter_device_register - create and register a new object of
 *   adapter_device class.
 * @name: the name of the new object
 * @parent: a pointer to the parent device
 * @devdata: an optional pointer to be stored for private driver use.
 * The methods may retrieve it by using adapter_get_data(adapter_dev).
 * @ops: the charger operations structure.
 *
 * Creates and registers new charger device. Returns either an
 * ERR_PTR() or a pointer to the newly allocated device.
 */
struct adapter_device *adapter_device_register(const char *name,
		struct device *parent, void *devdata,
		const struct adapter_ops *ops,
		const struct adapter_properties *props)
{
	struct adapter_device *adapter_dev;
	static struct lock_class_key key;
	struct srcu_notifier_head *head;
	int rc;

	pr_notice("%s: name=%s\n", __func__, name);
	adapter_dev = kzalloc(sizeof(*adapter_dev), GFP_KERNEL);
	if (!adapter_dev)
		return ERR_PTR(-ENOMEM);

	head = &adapter_dev->evt_nh;
	srcu_init_notifier_head(head);
	/* Rename srcu's lock to avoid LockProve warning */
	lockdep_init_map(&(&head->srcu)->dep_map, name, &key, 0);
	mutex_init(&adapter_dev->ops_lock);
	adapter_dev->dev.class = adapter_class;
	adapter_dev->dev.parent = parent;
	adapter_dev->dev.release = adapter_device_release;
	dev_set_name(&adapter_dev->dev, name);
	dev_set_drvdata(&adapter_dev->dev, devdata);

	/* Copy properties */
	if (props) {
		memcpy(&adapter_dev->props, props,
		       sizeof(struct adapter_properties));
	}
	rc = device_register(&adapter_dev->dev);
	if (rc) {
		kfree(adapter_dev);
		return ERR_PTR(rc);
	}
	adapter_dev->ops = ops;
	return adapter_dev;
}
EXPORT_SYMBOL(adapter_device_register);

/**
 * adapter_device_unregister - unregisters a switching charger device
 * object.
 * @adapter_dev: the switching charger device object to be unregistered
 * and freed.
 *
 * Unregisters a previously registered via adapter_device_register object.
 */
void adapter_device_unregister(struct adapter_device *adapter_dev)
{
	if (!adapter_dev)
		return;

	mutex_lock(&adapter_dev->ops_lock);
	adapter_dev->ops = NULL;
	mutex_unlock(&adapter_dev->ops_lock);
	device_unregister(&adapter_dev->dev);
}
EXPORT_SYMBOL(adapter_device_unregister);


static int adapter_match_device_by_name(struct device *dev,
	const void *data)
{
	const char *name = data;

	return strcmp(dev_name(dev), name) == 0;
}

struct adapter_device *get_adapter_by_name(const char *name)
{
	struct device *dev;

	if (!name)
		return (struct adapter_device *)NULL;
	dev = class_find_device(adapter_class, NULL, name,
				adapter_match_device_by_name);

	return dev ? to_adapter_device(dev) : NULL;

}
EXPORT_SYMBOL(get_adapter_by_name);

static void __exit adapter_class_exit(void)
{
	class_destroy(adapter_class);
}

static int __init adapter_class_init(void)
{
	adapter_class = class_create("Charging_Adapter");
	if (IS_ERR(adapter_class)) {
		pr_notice("Unable to create Charging Adapter class; errno = %ld\n",
			PTR_ERR(adapter_class));
		return PTR_ERR(adapter_class);
	}
	adapter_class->dev_groups = adapter_groups;
	return 0;
}

module_init(adapter_class_init);
module_exit(adapter_class_exit);

MODULE_DESCRIPTION("Adapter Class Device");
MODULE_AUTHOR("Wy Chuang <wy.chuang@mediatek.com>");
MODULE_LICENSE("GPL");
