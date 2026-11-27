/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef LINUX_POWER_ADAPTER_CLASS_H
#define LINUX_POWER_ADAPTER_CLASS_H

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/mutex.h>


#define ADAPTER_CAP_MAX_NR 10

struct adapter_power_cap {
	uint8_t selected_cap_idx;
	uint8_t nr;
	uint8_t pdp;
	uint8_t pwr_limit[ADAPTER_CAP_MAX_NR];
	int max_mv[ADAPTER_CAP_MAX_NR];
	int min_mv[ADAPTER_CAP_MAX_NR];
	int ma[ADAPTER_CAP_MAX_NR];
	int maxwatt[ADAPTER_CAP_MAX_NR];
	int minwatt[ADAPTER_CAP_MAX_NR];
	uint8_t type[ADAPTER_CAP_MAX_NR];
	int info[ADAPTER_CAP_MAX_NR];
	u16 source_vid, source_pid;
	u32 source_xid;
	u32 source_port, attach_gen;
	bool source_id_valid;
};

struct adapter_auth_data {
	int vcap_min;
	int vcap_max;
	int icap_min;
	int vta_min;
	int vta_max;
	int ita_max;
	bool pwr_lmt;
	u8 pdp;
	bool support_meas_cap;
	bool support_status;
	bool support_cc;
	u32 vta_step;
	u32 ita_step;
	u32 ita_gap_per_vstep;
};

enum adapter_type {
	PD,
	UFCS,
	MAX_TA_IDX,
};

static const char *const adapter_type_names[] = {
	"pd_adapter", "ufcs_adapter"
};

enum adapter_event {
	TA_ATTACH,
	TA_DETACH,
	TA_DETECT_FAIL,
	TA_HARD_RESET,
	TA_SOFT_RESET,
	MTK_TYPEC_WD_STATUS,
	MTK_TYPEC_HRESET_STATUS,
	MTK_TYPEC_CC_HI_STATUS,
};

enum adapter_property {
	TYPEC_RP_LEVEL,
	CAP_TYPE,
};

enum adapter_cap_type {
	MTK_PD_APDO_START,
	MTK_PD_APDO_END,
	MTK_PD,
	MTK_PD_APDO,
	MTK_PD_APDO_REGAIN,
	MTK_UFCS,
	MTK_CAP_TYPE_UNKNOWN,
};

enum uvdm_state {
	USBPD_UVDM_DISCONNECT = 0,
	USBPD_UVDM_CHARGER_VERSION = 1,
	USBPD_UVDM_CHARGER_VOLTAGE = 2,
	USBPD_UVDM_CHARGER_TEMP = 3,
	USBPD_UVDM_SESSION_SEED = 4,
	USBPD_UVDM_AUTHENTICATION = 5,
	USBPD_UVDM_VERIFIED = 6,
	USBPD_UVDM_REMOVE_COMPENSATION = 7,
	USBPD_UVDM_REVERSE_AUTHEN = 8,
	USBPD_UVDM_CONNECT = 9,
	USBPD_UVDM_NAN_ACK = 10,
};

#define USBPD_UVDM_SS_LEN		4
#define USBPD_UVDM_VERIFIED_LEN		1
#define USBPD_PDO_MAX_NR		7

#define XIAOMI_USB_VID			0x2717
#define XIAOMI_USB_VID_ALT		0x2b01

struct usbpd_vdm_data {
	int ta_version;
	int ta_temp;
	int ta_voltage;
	bool reauth;
	unsigned long s_secert[USBPD_UVDM_SS_LEN];
	unsigned long digest[USBPD_UVDM_SS_LEN];
};

enum adapter_return_value {
	MTK_ADAPTER_OK = 0,
	MTK_ADAPTER_NOT_SUPPORT,
	MTK_ADAPTER_TIMEOUT,
	MTK_ADAPTER_REJECT,
	MTK_ADAPTER_ERROR,
	MTK_ADAPTER_ADJUST,
	MTK_ADAPTER_VBUS_INVALID,
};


struct adapter_status {
	int temperature;
	bool ocp;
	bool otp;
	bool ovp;
};

struct adapter_properties {
	const char *alias_name;
};

struct adapter_device {
	struct adapter_properties props;
	const struct adapter_ops *ops;
	struct mutex ops_lock;
	struct device dev;
	struct srcu_notifier_head evt_nh;
	void	*driver_data;
	uint32_t adapter_svid;
	uint32_t adapter_id;
	uint32_t adapter_fw_ver;
	uint32_t adapter_hw_ver;
	struct usbpd_vdm_data vdm_data;
	int uvdm_state;
	bool verify_process;
	bool verifed;
	bool verify_done;
	uint8_t current_pr;
	uint8_t current_state;
	const char *current_state_name;
	uint32_t received_pdos[USBPD_PDO_MAX_NR];
};

struct adapter_ops {
	int (*suspend)(struct adapter_device *dev, pm_message_t state);
	int (*resume)(struct adapter_device *dev);
	int (*get_property)(struct adapter_device *dev,
		enum adapter_property pro);
	int (*get_status)(struct adapter_device *dev,
		struct adapter_status *sta);
	int (*set_cap)(struct adapter_device *dev, enum adapter_cap_type type,
		int mV, int mA);
	int (*get_cap)(struct adapter_device *dev, enum adapter_cap_type type,
		struct adapter_power_cap *cap);
	int (*get_output)(struct adapter_device *dev, int *mV, int *mA);
	int (*authentication)(struct adapter_device *dev,
			      struct adapter_auth_data *data);
	int (*is_cc)(struct adapter_device *dev, bool *cc);
	int (*set_wdt)(struct adapter_device *dev, u32 ms);
	int (*enable_wdt)(struct adapter_device *dev, bool en);
	int (*sync_volt)(struct adapter_device *dev, u32 mV);
	int (*send_hardreset)(struct adapter_device *dev);
	int (*exit_mode)(struct adapter_device *dev);
	int (*get_svid)(struct adapter_device *dev);
	int (*request_vdm_cmd)(struct adapter_device *dev, enum uvdm_state cmd,
		unsigned char *data, unsigned int data_len);
	int (*get_power_role)(struct adapter_device *dev);
	int (*get_pd_state)(struct adapter_device *dev);
	int (*get_pdos)(struct adapter_device *dev);
	int (*set_pd_verify_process)(struct adapter_device *dev, int verifying);
};

static inline void *adapter_dev_get_drvdata(
	const struct adapter_device *adapter_dev)
{
	return adapter_dev->driver_data;
}

static inline void adapter_dev_set_drvdata(
	struct adapter_device *adapter_dev, void *data)
{
	adapter_dev->driver_data = data;
}

extern struct adapter_device *adapter_device_register(
	const char *name,
	struct device *parent, void *devdata, const struct adapter_ops *ops,
	const struct adapter_properties *props);
extern void adapter_device_unregister(
	struct adapter_device *adapter_dev);
extern int register_adapter_device_notifier(struct adapter_device *adapter_dev,
				struct notifier_block *nb);
extern int unregister_adapter_device_notifier(
				struct adapter_device *adapter_dev,
				struct notifier_block *nb);
extern struct adapter_device *get_adapter_by_name(
	const char *name);

#define to_adapter_device(obj) container_of(obj, struct adapter_device, dev)

extern int adapter_dev_get_property(struct adapter_device *adapter_dev,
	enum adapter_property sta);
extern int adapter_dev_get_status(struct adapter_device *adapter_dev,
	struct adapter_status *sta);
extern int adapter_dev_get_output(struct adapter_device *adapter_dev,
	int *mV, int *mA);
extern int adapter_dev_set_cap(struct adapter_device *adapter_dev,
	enum adapter_cap_type type,
	int mV, int mA);
extern int adapter_dev_get_cap(struct adapter_device *adapter_dev,
	enum adapter_cap_type type,
	struct adapter_power_cap *cap);
extern int adapter_dev_authentication(struct adapter_device *adapter_dev,
				      struct adapter_auth_data *data);
extern int adapter_dev_is_cc(struct adapter_device *adapter_dev, bool *cc);
extern int adapter_dev_set_wdt(struct adapter_device *adapter_dev, u32 ms);
extern int adapter_dev_enable_wdt(struct adapter_device *adapter_dev, bool en);
extern int adapter_dev_sync_volt(struct adapter_device *adapter_dev, u32 mV);
extern int adapter_dev_send_hardreset(struct adapter_device *adapter_dev);
extern int adapter_dev_exit_mode(struct adapter_device *adapter_dev);
extern int adapter_dev_get_svid(struct adapter_device *adapter_dev);
extern int adapter_dev_request_vdm_cmd(struct adapter_device *adapter_dev,
	enum uvdm_state cmd, unsigned char *data, unsigned int data_len);
extern int adapter_dev_get_power_role(struct adapter_device *adapter_dev);
extern int adapter_dev_get_current_state(struct adapter_device *adapter_dev);
extern int adapter_dev_get_pdos(struct adapter_device *adapter_dev);
extern int adapter_dev_set_pd_verify_process(struct adapter_device *adapter_dev,
	int verifying);

#endif /*LINUX_POWER_ADAPTER_CLASS_H*/
