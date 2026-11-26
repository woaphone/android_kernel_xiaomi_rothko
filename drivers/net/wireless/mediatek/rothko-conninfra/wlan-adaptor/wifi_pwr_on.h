/*
* Copyright (C) 2019 MediaTek Inc.
*
* This program is free software: you can redistribute it and/or modify it under the terms of the
* GNU General Public License version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with this program.
* If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _WIFI_PWR_ON_H_
#define _WIFI_PWR_ON_H_

#include <linux/types.h>

int wifi_pwr_on_init(void);
int wifi_pwr_on_deinit(void);

typedef int(*wlan_probe_cb) (void);
typedef int(*wlan_remove_cb) (void);


struct MTK_WCN_WLAN_CB_INFO {
	wlan_probe_cb wlan_probe_cb;
	wlan_remove_cb wlan_remove_cb;
};
extern int mtk_wcn_wlan_reg(struct MTK_WCN_WLAN_CB_INFO *pWlanCbInfo);
int mtk_wcn_wlan_unreg(void);

struct net_device;

/* enable: 0 tears down P2P/AP, 1 starts P2P (mode 0) or AP (mode 1). */
struct PARAM_CUSTOM_P2P_SET_STRUCT {
	uint32_t u4Enable;
	uint32_t u4Mode;
};

enum ENUM_RESET_STATUS {
	RESET_FAIL,
	RESET_SUCCESS
};

typedef int (*set_p2p_mode)(struct net_device *netdev,
			   struct PARAM_CUSTOM_P2P_SET_STRUCT p2pmode);
typedef uint8_t (*file_buf_handler)(void *ctx, const char __user *buf,
				    uint16_t length);

void register_set_p2p_mode_handler(set_p2p_mode handler);
void register_file_buf_handler(file_buf_handler handler, void *handler_ctx,
			       uint8_t ucType);
void update_driver_loaded_status(uint8_t loaded);
void update_driver_reset_status(uint8_t fgIsResetting);
void update_driver_l0_reset_status(uint8_t fgIsL0Resetting);
void update_pre_cal_status(uint8_t fgIsPreCal);
uint8_t get_pre_cal_status(void);
uint32_t get_low_latency_mode(void);
uint32_t get_wifi_standalone_log_mode(void);
int32_t get_wifi_powered_status(void);
int32_t get_wifi_process_status(void);
int wifi_reset_start(void);
int wifi_reset_end(enum ENUM_RESET_STATUS status);

enum ENUM_WLAN_OPID {
	WLAN_OPID_FUNC_ON = 0,
	WLAN_OPID_FUNC_OFF = 1,
	WLAN_OPID_MAX
};
extern int mtk_wcn_wlan_func_ctrl(enum ENUM_WLAN_OPID opId);

extern wlan_probe_cb mtk_wlan_probe_function;
extern wlan_remove_cb mtk_wlan_remove_function;

extern bool g_fgIsWiFiOn;

#ifndef MTK_WCN_BOOL_TRUE
#define MTK_WCN_BOOL_FALSE               ((int) 0)
#define MTK_WCN_BOOL_TRUE                ((int) 1)
#endif

#endif /*_WIFI_PWR_ON_H_*/
