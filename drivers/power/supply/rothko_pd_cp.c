// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/rothko_hwid.h>
#include <linux/timekeeping.h>

#include "mtk_charger.h"
#include "rothko_pd_cp.h"
#include "sc858x_charger.h"
#include "tcpci.h"
#include "tcpm.h"

#define ROTHKO_PPS_MAX_MV	18000
#define ROTHKO_PPS_MAX_MA	3000
#define ROTHKO_PPS_START_MAX_MA	2000
#define ROTHKO_PPS_TARGET_IBUS_MA	2900
#define ROTHKO_PPS_MAX_IBAT_MA	12000
#define ROTHKO_XM_PPS_MAX_MV	20000
#define ROTHKO_XM_PPS_MAX_PDO_MV	21000
#define ROTHKO_XM_PPS_MAX_MA	6000
#define ROTHKO_XM_PPS_TARGET_IBUS_MA	5900
#define ROTHKO_XM_PPS_MAX_IBAT_MA	21700
#define ROTHKO_VERIFY_GRACE_MS	3500
#define ROTHKO_VERIFY_ACTIVE_MAX_MS	10000
#define ROTHKO_PPS_STEP_MV	20
#define ROTHKO_CP_RATIO		4
#define ROTHKO_CP_HEADROOM_MV	600
#define ROTHKO_CP_MIN_HEADROOM_PCT	3
#define ROTHKO_CP_MAX_HEADROOM_PCT	10
#define ROTHKO_PPS_START_TUNE_LIMIT	6
#define ROTHKO_PPS_SETTLE_MS	1000
#define ROTHKO_PPS_TUNE_INTERVAL_MS	500
#define ROTHKO_CP_SWITCH_TIMEOUT_MS	500

/* All policy entry points run under mtk_charger.charger_lock. */
struct rothko_pd_cp {
	struct tcpc_device *tcpc;
	struct charger_device *master;
	struct charger_device *slave;
	struct power_supply *bms;
	int gpio;
	u32 attach_gen;
	u32 verify_seen_gen;
	ktime_t verify_first_seen;
	bool verify_waiting;
	bool verified_session;
	bool attempted;
	bool detached_exit;
	bool held;
	bool isolated;
	bool running;
	ktime_t enable_at;
	ktime_t next_raise_at;
	u32 sample_mv;
	u32 sample_master_ma;
	u32 sample_slave_ma;
	unsigned int start_tune_steps;
	int request_mv;
	int request_ma;
	int apdo_min_mv;
	int apdo_max_mv;
	int apdo_ma;
};

struct rothko_battery_sample {
	int mv;
	int ma;
};

static bool xiaomi_pd_verified(struct mtk_charger *info)
{
	struct adapter_device *pd = info->adapter_dev[PD];

	return pd && pd->verifed &&
	       (pd->adapter_svid == XIAOMI_USB_VID ||
		pd->adapter_svid == XIAOMI_USB_VID_ALT);
}

static bool pd_verify_in_progress(struct mtk_charger *info)
{
	struct adapter_device *pd = info->adapter_dev[PD];

	return pd && pd->verify_process;
}

static bool pd_verify_finished(struct mtk_charger *info)
{
	struct adapter_device *pd = info->adapter_dev[PD];

	return pd && (pd->verify_done || pd->verifed);
}

static int read_bms(struct power_supply *bms, enum power_supply_property prop, int *value)
{
	union power_supply_propval data;
	int ret = power_supply_get_property(bms, prop, &data);

	if (!ret)
		*value = data.intval;
	return ret;
}

static int battery_sample(struct mtk_charger *info, struct rothko_battery_sample *sample)
{
	struct power_supply *bms = info->pd_cp->bms;
	int uv, ua, soc, temp, ret;

	ret = read_bms(bms, POWER_SUPPLY_PROP_TEMP, &temp);
	if (ret)
		return ret;
	ret = read_bms(bms, POWER_SUPPLY_PROP_VOLTAGE_NOW, &uv);
	if (ret)
		return ret;
	ret = read_bms(bms, POWER_SUPPLY_PROP_CURRENT_NOW, &ua);
	if (ret)
		return ret;
	ret = read_bms(bms, POWER_SUPPLY_PROP_CAPACITY, &soc);
	if (ret)
		return ret;

	if (!is_battery_exist(info))
		return -ERANGE;
	if (xiaomi_pd_verified(info)) {
		if (temp < 100 || temp >= 460 ||
		    uv < 3700000 || uv >= min_t(int, 4420000, info->data.battery_cv - 60000) ||
		    soc < 0 || soc >= 95)
			return -ERANGE;
	} else {
		if (temp < 100 || temp >= 420 ||
		    uv < 3700000 || uv >= min_t(int, 4350000, info->data.battery_cv - 100000) ||
		    soc < 0 || soc >= 85)
			return -ERANGE;
	}

	sample->mv = uv / 1000;
	sample->ma = abs(ua / 1000);
	return 0;
}

static int set_isolation(struct rothko_pd_cp *cp, bool isolate)
{
	int ret = gpio_direction_output(cp->gpio, isolate);

	if (ret)
		return ret;
	ret = gpio_get_value_cansleep(cp->gpio);
	if (ret < 0)
		return ret;
	return !!ret == isolate ? 0 : -EIO;
}

static int read_contract(struct rothko_pd_cp *cp, struct tcpm_remote_power_cap *caps,
			 int *mv, bool *fixed)
{
	int ma, index, ret;

	if (tcpm_inquire_typec_attach_state(cp->tcpc) != TYPEC_ATTACHED_SNK)
		return -ENOTCONN;
	ret = tcpm_inquire_pd_contract(cp->tcpc, mv, &ma);
	if (ret || ma <= 0)
		return -ENOTCONN;
	ret = tcpm_get_remote_power_cap(cp->tcpc, caps);
	if (ret)
		return -ENOTCONN;
	index = caps->selected_cap_idx - 1;
	if (index < 0 || index >= caps->nr)
		return -ENOTCONN;
	*fixed = caps->type[index] == TCPM_POWER_CAP_VAL_TYPE_FIXED;
	return 0;
}

static int read_tcpc_vbus(struct rothko_pd_cp *cp, u32 *mv)
{
	*mv = U32_MAX;
	return tcpci_get_vbus_voltage(cp->tcpc, mv);
}

static bool read_vbus_pair(struct rothko_pd_cp *cp, u32 *tcpc_mv, u32 *master_mv)
{
	return !read_tcpc_vbus(cp, tcpc_mv) &&
	       !sc858x_cp_get_vbus_mv(cp->master, master_mv) &&
	       abs((int)*tcpc_mv - (int)*master_mv) <= 600;
}

static bool vbus_at(struct rothko_pd_cp *cp, int low_mv, int high_mv)
{
	u32 tcpc_mv, master_mv;

	return read_vbus_pair(cp, &tcpc_mv, &master_mv) &&
	       min(tcpc_mv, master_mv) >= low_mv && max(tcpc_mv, master_mv) <= high_mv;
}

static bool wait_vbus(struct rothko_pd_cp *cp, int low_mv, int high_mv)
{
	int i;

	for (i = 0; i < 12; i++) {
		if (vbus_at(cp, low_mv, high_mv))
			return true;
		msleep(50);
	}
	return false;
}

static int stop_pumps(struct rothko_pd_cp *cp)
{
	int master_ret = charger_dev_enable(cp->master, false);
	int slave_ret = charger_dev_enable(cp->slave, false);

	return master_ret ?: slave_ret;
}

static bool fixed_5v(struct rothko_pd_cp *cp)
{
	struct tcpm_remote_power_cap caps;
	bool fixed;
	int mv;

	return !read_contract(cp, &caps, &mv, &fixed) &&
	       caps.attach_gen == cp->attach_gen && fixed && mv == 5000;
}

static void stop_pps(struct mtk_charger *info)
{
	struct rothko_pd_cp *cp = info->pd_cp;
	struct device *dev = &info->pdev->dev;
	bool detached, restore;
	u32 mv;

	if (cp->running && cp->sample_mv)
		dev_info(dev, "SC858x: last running sample %u mV, master %u mA, slave %u mA, estimated input %u mW\n",
			 cp->sample_mv, cp->sample_master_ma, cp->sample_slave_ma,
			 cp->sample_mv * (cp->sample_master_ma + cp->sample_slave_ma) / 1000);
	cp->running = false;
	cp->enable_at = 0;

	if (stop_pumps(cp))
		goto held;

	detached = tcpm_inquire_typec_attach_state(cp->tcpc) == TYPEC_UNATTACHED &&
		   !read_tcpc_vbus(cp, &mv) && mv <= 800;
	if (!detached) {
		if (!fixed_5v(cp) &&
		    tcpm_dpm_pd_exit_pps(cp->tcpc, cp->attach_gen, 500) != TCP_DPM_RET_SUCCESS)
			goto held;
		if (!fixed_5v(cp))
			goto held;
		if (cp->isolated && !wait_vbus(cp, 4400, 5500))
			goto held;
	}

	if (cp->isolated) {
		if (set_isolation(cp, false))
			goto held;
		cp->isolated = false;
	}
	if (!detached) {
		int lo, hi, ret;

		ret = charger_dev_get_adc(info->chg1_dev, ADC_CHANNEL_VBUS, &lo, &hi);
		if (ret || lo < 4400000 || hi > 5500000 || !fixed_5v(cp))
			goto held;
	}
	/* ADC samples stay available until the charger-side voltage is verified. */
	sc858x_cp_enable_adc(cp->master, false);
	sc858x_cp_enable_adc(cp->slave, false);
	restore = !detached && READ_ONCE(info->enable_pp[CHG1_SETTING]) &&
		  !READ_ONCE(info->force_disable_pp[CHG1_SETTING]);
	if (mt6375_pps_hold(info->chg1_dev, false, restore))
		goto held;
	tcpm_release_board_pps(cp->tcpc);
	cp->held = false;
	cp->verified_session = false;
	cp->detached_exit = detached;
	dev_info(dev, "SC858x: both pumps stopped, exit=%s\n", detached ? "detach" : "5 V");
	return;

held:
	dev_warn_ratelimited(dev, "SC858x: exit unconfirmed, MT6375 interlock held\n");
}

static bool pump_ready(struct charger_device *pump, bool switching, u32 allowed_faults)
{
	bool enabled, ready;
	u32 faults;
	int temp;

	if (sc858x_cp_get_chip_ok(pump, &ready) || !ready ||
	    charger_dev_is_enabled(pump, &enabled) || enabled != switching ||
	    sc858x_cp_get_fault_type(pump, &faults) || (faults & ~allowed_faults))
		return false;
	if (switching && (sc858x_cp_get_tdie_c(pump, &temp) || temp >= 100))
		return false;
	return true;
}

static int select_pps(struct mtk_charger *info, int mv)
{
	struct rothko_pd_cp *cp = info->pd_cp;
	struct tcpm_power_cap_val cap;
	struct pd_source_cap_ext source;
	bool verified = xiaomi_pd_verified(info);
	int max_mv_limit = verified ? ROTHKO_XM_PPS_MAX_MV : ROTHKO_PPS_MAX_MV;
	int max_ma_limit = verified ? ROTHKO_XM_PPS_MAX_MA : ROTHKO_PPS_MAX_MA;
	int pdp = 0, ma;
	u8 index = 0;

	if (!verified &&
	    tcpm_dpm_pd_get_source_cap_ext(cp->tcpc, NULL, &source) == TCP_DPM_RET_SUCCESS)
		pdp = source.source_pdp;

	cp->apdo_ma = 0;
	while (!tcpm_inquire_pd_source_apdo(cp->tcpc, TCPM_POWER_CAP_APDO_TYPE_PPS,
					   &index, &cap)) {
		if (cap.min_mv > mv || cap.max_mv < mv)
			continue;
		if (verified && cap.max_mv > ROTHKO_XM_PPS_MAX_PDO_MV)
			continue;
		if (!verified && cap.pwr_limit && !pdp)
			continue;
		ma = min(cap.ma, max_ma_limit);
		if (!verified && pdp)
			ma = min(ma, pdp * 1000000 / min(cap.max_mv, max_mv_limit));
		if (ma <= cp->apdo_ma)
			continue;
		cp->apdo_ma = ma;
		cp->apdo_min_mv = cap.min_mv;
		cp->apdo_max_mv = min(cap.max_mv, max_mv_limit);
	}
	return cp->apdo_ma >= 1000 ? 0 : -EOPNOTSUPP;
}

static int target_current(struct mtk_charger *info, int *ibat_ma)
{
	struct charger_data *primary = &info->chg_data[CHG1_SETTING];
	int fcc = READ_ONCE(primary->thermal_charging_current_limit);
	int aicr = READ_ONCE(primary->thermal_input_current_limit);
	int master = READ_ONCE(info->chg_data[HVDVCHG1_SETTING].thermal_input_current_limit);
	int slave = READ_ONCE(info->chg_data[HVDVCHG2_SETTING].thermal_input_current_limit);
	int smart = READ_ONCE(info->sc.sc_ibat);
	int ma = info->pd_cp->apdo_ma;

	*ibat_ma = xiaomi_pd_verified(info) ? ROTHKO_XM_PPS_MAX_IBAT_MA : ROTHKO_PPS_MAX_IBAT_MA;
	if (fcc >= 0)
		*ibat_ma = min(*ibat_ma, fcc / 1000);
	if (smart >= 0)
		*ibat_ma = min(*ibat_ma, smart / 1000);
	ma = min(ma, *ibat_ma / ROTHKO_CP_RATIO);
	if (aicr >= 0)
		ma = min(ma, aicr / 1000);
	if (master >= 0)
		ma = min(ma, master / 1000);
	if (slave >= 0)
		ma = min(ma, slave / 1000);
	return rounddown(ma, 50);
}

static int request_pps(struct rothko_pd_cp *cp, int mv, int ma)
{
	struct tcpm_remote_power_cap caps;
	bool fixed;
	int active_mv;

	if (tcpm_dpm_pd_request_pps(cp->tcpc, cp->attach_gen, mv, ma) != TCP_DPM_RET_SUCCESS ||
	    read_contract(cp, &caps, &active_mv, &fixed) || fixed ||
	    caps.attach_gen != cp->attach_gen || active_mv != mv)
		return -EIO;

	cp->request_mv = mv;
	cp->request_ma = ma;
	cp->next_raise_at = ktime_add_ms(ktime_get_boottime(), ROTHKO_PPS_TUNE_INTERVAL_MS);
	return 0;
}

static bool charger_policies_allow(struct mtk_charger *info)
{
	return info->enable_hv_charging && info->select_adapter_idx == PD &&
	       (!info->enable_fast_charging_indicator || info->fast_charging_indicator & PE5_ID) &&
	       !READ_ONCE(info->force_disable_pp[CHG1_SETTING]) &&
	       READ_ONCE(info->enable_pp[CHG1_SETTING]) &&
	       !READ_ONCE(info->sc.disable_charger) &&
	       info->bootmode != 1 && info->bootmode != 2 &&
	       info->bootmode != 4 && info->bootmode != 8 && info->bootmode != 9;
}

static int configure_pumps(struct rothko_pd_cp *cp, bool after_detach)
{
	int ret;

	ret = sc858x_cp_prepare_pps(cp->master, after_detach);
	if (!ret)
		ret = sc858x_cp_prepare_pps(cp->slave, after_detach);
	if (!ret)
		ret = sc858x_cp_enable_adc(cp->master, true);
	if (!ret)
		ret = sc858x_cp_enable_adc(cp->slave, true);
	return ret;
}

static bool start_pps(struct mtk_charger *info, const struct rothko_battery_sample *bat)
{
	struct rothko_pd_cp *cp = info->pd_cp;
	u32 allowed_faults = BIT(SC858X_FAULT_TSBAT_BIT);
	bool after_detach = cp->detached_exit;
	struct tcpm_remote_power_cap caps;
	struct rothko_battery_sample sample;
	int mv, ma, ibat, i, chgvin_min, chgvin_max;
	bool fixed, buck;

	if (read_contract(cp, &caps, &mv, &fixed) || !fixed || mv != 5000 ||
	    (cp->attempted && cp->attach_gen == caps.attach_gen))
		return false;
	for (i = 0; i < MAX_ALG_NO; i++)
		if (chg_alg_is_algo_running(info->alg[i]) > 0)
			return false;

	mv = roundup(bat->mv * ROTHKO_CP_RATIO + ROTHKO_CP_HEADROOM_MV, ROTHKO_PPS_STEP_MV);
	if (select_pps(info, mv))
		return false;
	ma = min(target_current(info, &ibat), ROTHKO_PPS_START_MAX_MA);
	if (after_detach)
		allowed_faults |= BIT(SC858X_FAULT_IBUS_UCP_FALL_BIT);
	if (ma < 1000 || charger_dev_is_powerpath_enabled(info->chg1_dev, &buck) || !buck ||
	    charger_dev_get_adc(info->chg1_dev, ADC_CHANNEL_VBUS, &chgvin_min, &chgvin_max) ||
	    chgvin_min < 4400000 || chgvin_max > 5500000 ||
	    !pump_ready(cp->master, false, allowed_faults) ||
	    !pump_ready(cp->slave, false, allowed_faults) ||
	    battery_sample(info, &sample))
		return false;

	if (tcpm_claim_board_pps(cp->tcpc, caps.attach_gen))
		return false;

	cp->attach_gen = caps.attach_gen;
	cp->verified_session = xiaomi_pd_verified(info);
	cp->attempted = true;
	dev_info(&info->pdev->dev, "SC858x: start PPS (xm_verified=%d apdo=%d-%dmV/%dmA)\n",
		 cp->verified_session, cp->apdo_min_mv, cp->apdo_max_mv, cp->apdo_ma);
	cp->detached_exit = false;
	cp->start_tune_steps = 0;
	cp->sample_mv = 0;
	cp->held = true;
	if (mt6375_pps_hold(info->chg1_dev, true, false) || configure_pumps(cp, after_detach))
		goto fail;
	/* Mark the GPIO as unknown until the new level reads back. */
	cp->isolated = true;
	if (set_isolation(cp, true) || !fixed_5v(cp) || !wait_vbus(cp, 4400, 5500) ||
	    !pump_ready(cp->master, false, 0) || !pump_ready(cp->slave, false, 0) ||
	    request_pps(cp, mv, ma) || !wait_vbus(cp, mv - 600, mv + 500))
		goto fail;
	cp->enable_at = ktime_add_ms(ktime_get_boottime(), ROTHKO_PPS_SETTLE_MS);
	return true;

fail:
	stop_pps(info);
	return cp->held;
}

static bool enable_pumps_after_settle(struct mtk_charger *info,
				     const struct rothko_battery_sample *bat)
{
	struct rothko_pd_cp *cp = info->pd_cp;
	struct device *dev = &info->pdev->dev;
	struct tcpm_remote_power_cap caps;
	const int step_mv = ROTHKO_CP_RATIO * ROTHKO_PPS_STEP_MV;
	ktime_t switch_deadline;
	u32 tcpc_mv, master_mv, master_faults, slave_faults;
	int mv, base_mv, floor_mv, ceiling_mv, next_mv, low_mv, high_mv, ibat;
	bool fixed;

	if (gpio_get_value_cansleep(cp->gpio) != 1 ||
	    read_contract(cp, &caps, &mv, &fixed) || fixed ||
	    caps.attach_gen != cp->attach_gen || mv != cp->request_mv ||
	    !pump_ready(cp->master, false, 0) || !pump_ready(cp->slave, false, 0) ||
	    target_current(info, &ibat) < cp->request_ma)
		return false;
	if (ktime_before(ktime_get_boottime(), cp->enable_at))
		return true;

	if (!read_vbus_pair(cp, &tcpc_mv, &master_mv)) {
		dev_warn(dev, "SC858x: cannot confirm PPS VBUS at both inputs\n");
		return false;
	}

	low_mv = min(tcpc_mv, master_mv);
	high_mv = max(tcpc_mv, master_mv);
	/* Stock INIT_VBUS tunes in ratio * PPS-step increments before enable. */
	base_mv = bat->mv * ROTHKO_CP_RATIO;
	floor_mv = base_mv + base_mv * ROTHKO_CP_MIN_HEADROOM_PCT / 100;
	ceiling_mv = base_mv + base_mv * ROTHKO_CP_MAX_HEADROOM_PCT / 100;
	if (low_mv < mv - 600 || high_mv > mv + 500 ||
	    (low_mv <= floor_mv && high_mv >= ceiling_mv)) {
		dev_warn(dev, "SC858x: inconsistent PPS VBUS (request %d, TCPC %u, master %u mV)\n",
			 mv, tcpc_mv, master_mv);
		return false;
	}
	if (low_mv <= floor_mv || high_mv >= ceiling_mv) {
		next_mv = mv + (low_mv <= floor_mv ? step_mv : -step_mv);
		next_mv = clamp(next_mv, cp->apdo_min_mv, cp->apdo_max_mv);
		if (next_mv == mv || cp->start_tune_steps >= ROTHKO_PPS_START_TUNE_LIMIT) {
			dev_warn(dev, "SC858x: PPS startup tune limit (TCPC %u, master %u, window %d-%d mV)\n",
				 tcpc_mv, master_mv, floor_mv, ceiling_mv);
			return false;
		}
		dev_info(dev, "SC858x: PPS startup tune %d -> %d mV (TCPC %u, master %u, window %d-%d mV)\n",
			 mv, next_mv, tcpc_mv, master_mv, floor_mv, ceiling_mv);
		if (request_pps(cp, next_mv, cp->request_ma))
			return false;
		cp->start_tune_steps++;
		cp->enable_at = ktime_add_ms(ktime_get_boottime(), ROTHKO_PPS_SETTLE_MS);
		return true;
	}

	dev_info(dev, "SC858x: PPS settled, enabling pumps (request %d mV, VBAT %d mV)\n",
		 mv, bat->mv);
	if (charger_dev_enable(cp->master, true) || charger_dev_enable(cp->slave, true))
		return false;

	switch_deadline = ktime_add_ms(ktime_get_boottime(), ROTHKO_CP_SWITCH_TIMEOUT_MS);
	for (;;) {
		if (sc858x_cp_get_fault_type(cp->master, &master_faults) ||
		    sc858x_cp_get_fault_type(cp->slave, &slave_faults) ||
		    master_faults || slave_faults)
			return false;
		if (pump_ready(cp->master, true, 0) && pump_ready(cp->slave, true, 0)) {
			cp->enable_at = 0;
			cp->running = true;
			dev_info(dev, "SC858x: both pumps on at %d mV, %d mA\n",
				 mv, cp->request_ma);
			return true;
		}
		if (!ktime_before(ktime_get_boottime(), switch_deadline))
			break;
		msleep(25);
	}

	dev_warn(dev, "SC858x: dual pump switching unconfirmed after %d ms\n",
		 ROTHKO_CP_SWITCH_TIMEOUT_MS);
	return false;
}

static bool update_pps(struct mtk_charger *info, const struct rothko_battery_sample *bat)
{
	struct rothko_pd_cp *cp = info->pd_cp;
	struct device *dev = &info->pdev->dev;
	struct tcpm_remote_power_cap caps;
	u32 master_ma, slave_ma, master_mv, tcpc_mv;
	int mv, ma, ibat, measured, base_mv, max_mv, total_ma, ibus_target, step_mv;
	int old_ma = cp->request_ma;
	bool fixed;

	ma = target_current(info, &ibat);
	if (ma < 1000 || gpio_get_value_cansleep(cp->gpio) != 1 ||
	    read_contract(cp, &caps, &measured, &fixed) || fixed ||
	    caps.attach_gen != cp->attach_gen || measured != cp->request_mv ||
	    !read_vbus_pair(cp, &tcpc_mv, &master_mv) ||
	    min(tcpc_mv, master_mv) < measured - 800 ||
	    max(tcpc_mv, master_mv) > measured + 500 ||
	    !pump_ready(cp->master, true, 0) || !pump_ready(cp->slave, true, 0) ||
	    sc858x_cp_get_ibus_ma(cp->master, &master_ma) ||
	    sc858x_cp_get_ibus_ma(cp->slave, &slave_ma) ||
	    master_ma + slave_ma > ma + 200 || bat->ma > ibat + 500)
		return false;

	cp->sample_mv = master_mv;
	cp->sample_master_ma = master_ma;
	cp->sample_slave_ma = slave_ma;
	total_ma = master_ma + slave_ma;
	base_mv = bat->mv * ROTHKO_CP_RATIO;
	max_mv = rounddown(base_mv + base_mv * ROTHKO_CP_MAX_HEADROOM_PCT / 100,
			   ROTHKO_PPS_STEP_MV);
	max_mv = min(max_mv, cp->apdo_max_mv);
	if (max_mv < cp->apdo_min_mv || max_mv < cp->request_mv - 80)
		return false;
	ibus_target = min(ma - 100, cp->verified_session ?
			  ROTHKO_XM_PPS_TARGET_IBUS_MA : ROTHKO_PPS_TARGET_IBUS_MA);
	mv = cp->request_mv;
	if (bat->ma >= ibat - 250 || total_ma >= ibus_target) {
		mv -= ROTHKO_PPS_STEP_MV;
	} else if (master_ma && slave_ma && total_ma < ibus_target - 100 &&
		   bat->ma < ibat - 500 &&
		   !ktime_before(ktime_get_boottime(), cp->next_raise_at)) {
		step_mv = total_ma < ibus_target - 400 && bat->ma < ibat - 1500 ?
			  ROTHKO_CP_RATIO * ROTHKO_PPS_STEP_MV : ROTHKO_PPS_STEP_MV;
		mv = max(mv + step_mv,
			 rounddown(base_mv + ROTHKO_CP_HEADROOM_MV, ROTHKO_PPS_STEP_MV));
	}
	mv = clamp(mv, cp->request_mv - 80, cp->request_mv + 80);
	mv = clamp(mv, cp->apdo_min_mv, max_mv);
	if (mv == cp->request_mv && ma == old_ma)
		return true;
	if (request_pps(cp, mv, ma))
		return false;
	if (ma != old_ma)
		dev_info(dev, "SC858x: PPS running limit %d -> %d mA, IBUS target %d mA\n",
			 old_ma, ma, ibus_target);
	return true;
}

static bool should_wait_pd_verify(struct mtk_charger *info)
{
	struct rothko_pd_cp *cp = info->pd_cp;
	struct tcpm_remote_power_cap caps;
	ktime_t now;
	s64 elapsed_ms;
	int mv;
	bool fixed;

	if (!cp->tcpc || read_contract(cp, &caps, &mv, &fixed) || !fixed || mv != 5000) {
		cp->verify_waiting = false;
		return false;
	}

	now = ktime_get_boottime();
	if (cp->verify_seen_gen != caps.attach_gen) {
		cp->verify_seen_gen = caps.attach_gen;
		cp->verify_first_seen = now;
	}

	if (pd_verify_finished(info) && !pd_verify_in_progress(info)) {
		cp->verify_waiting = false;
		return false;
	}

	elapsed_ms = ktime_ms_delta(now, cp->verify_first_seen);
	if (pd_verify_in_progress(info)) {
		cp->verify_waiting = elapsed_ms < ROTHKO_VERIFY_ACTIVE_MAX_MS;
		return cp->verify_waiting;
	}

	cp->verify_waiting = elapsed_ms < ROTHKO_VERIFY_GRACE_MS;
	return cp->verify_waiting;
}

static bool poll_pumps(struct mtk_charger *info, bool allow)
{
	struct rothko_pd_cp *cp = info->pd_cp;
	struct rothko_battery_sample bat;
	bool allowed;

	allowed = allow && charger_policies_allow(info);
	if (allowed) {
		if (!cp->master)
			cp->master = get_charger_by_name("cp_master");
		if (!cp->slave)
			cp->slave = get_charger_by_name("cp_slave");
		if (!cp->bms)
			cp->bms = power_supply_get_by_name("bms");
		allowed = cp->tcpc && cp->master && cp->slave && cp->bms &&
			  !battery_sample(info, &bat);
	}
	if (cp->held) {
		cp->verify_waiting = false;
		/*
		 * If Xiaomi UVDM verification starts late or succeeds after we
		 * entered an unverified PPS session, cleanly exit to fixed 5 V
		 * and allow one re-entry with the unlocked 120 W APDO.
		 */
		if (!cp->verified_session &&
		    (pd_verify_in_progress(info) || xiaomi_pd_verified(info))) {
			dev_info(&info->pdev->dev,
				 "SC858x: stopping unverified PPS for Xiaomi 120W upgrade\n");
			stop_pps(info);
			if (!cp->held)
				cp->attempted = false;
			return true;
		}
		if (allowed && cp->enable_at && enable_pumps_after_settle(info, &bat))
			return true;
		if (allowed && cp->running && update_pps(info, &bat))
			return true;
		stop_pps(info);
		return cp->held;
	}

	if (allowed && should_wait_pd_verify(info))
		return true;

	cp->verify_waiting = false;
	return allowed && start_pps(info, &bat);
}

static int rothko_apdo_tier(const struct tcpm_remote_power_cap *caps)
{
	int index, watts = 0;

	for (index = 0; index < caps->nr; index++) {
		if (caps->type[index] == TCPM_POWER_CAP_VAL_TYPE_AUGMENT)
			watts = max(watts, caps->max_mv[index] * caps->ma[index] / 1000000);
	}
	switch (watts) {
	case 0: return 0;
	case 50: return 50;
	case 51 ... 64: return 55;
	case 65: return 65;
	case 66 ... 67: return 67;
	case 68 ... 95: return 90;
	case 96 ... 109: return 100;
	case 110 ... 129: return 120;
	default: return 33;
	}
}

bool rothko_pd_cp_poll(struct mtk_charger *info, bool allow)
{
	struct rothko_pd_cp *cp = info->pd_cp;
	struct tcpm_remote_power_cap caps;
	int mv, watts = 0, type = 0;
	bool active, fixed, pps, was_running;

	if (!cp)
		return false;
	if (!cp->tcpc)
		cp->tcpc = tcpc_dev_get_by_name("type_c_port0");
	was_running = cp->running;
	active = poll_pumps(info, allow);
	if (was_running != cp->running && !IS_ERR_OR_NULL(info->psy1))
		power_supply_changed(info->psy1);
	if (cp->tcpc && !read_contract(cp, &caps, &mv, &fixed)) {
		watts = rothko_apdo_tier(&caps);
		if (allow && info->battery_temp >= 0 && info->battery_temp <= 48) {
			pps = !fixed && caps.type[caps.selected_cap_idx - 1] == TCPM_POWER_CAP_VAL_TYPE_AUGMENT;
			type = 1;
			if (watts && (pps || adapter_dev_get_property(info->adapter_dev[PD], CAP_TYPE) == MTK_PD_APDO))
				type = watts >= 50 ? 4 : 3;
		}
	}
	mt6375_report_pd_tier(info, watts, type);
	{
		struct adapter_device *pd = info->adapter_dev[PD];
		int cap_type = pd ? adapter_dev_get_property(pd, CAP_TYPE) : MTK_CAP_TYPE_UNKNOWN;
		int pd_type_val = 0;

		if (cap_type == MTK_PD_APDO)
			pd_type_val = 5;
		else if (cap_type == MTK_PD)
			pd_type_val = 3;
		mt6375_report_pd_auth(info,
				      pd && pd->verifed,
				      pd && pd->verify_process,
				      pd && pd->verify_done,
				      pd_type_val);
	}
	return active;
}

bool rothko_pd_cp_active(struct mtk_charger *info)
{
	return info->pd_cp && info->pd_cp->held;
}

bool rothko_pd_cp_attached(struct mtk_charger *info)
{
	return rothko_pd_cp_active(info) &&
	       tcpm_inquire_typec_attach_state(info->pd_cp->tcpc) == TYPEC_ATTACHED_SNK;
}

bool rothko_pd_cp_switching(struct mtk_charger *info)
{
	return rothko_pd_cp_active(info) && info->pd_cp->running;
}

bool rothko_pd_cp_verifying(struct mtk_charger *info)
{
	return info->pd_cp && info->pd_cp->verify_waiting;
}

static void free_isolation_gpio(void *data)
{
	struct rothko_pd_cp *cp = data;

	gpio_free(cp->gpio);
}

void rothko_pd_cp_init(struct mtk_charger *info)
{
	struct device *dev = &info->pdev->dev;
	struct rothko_pd_cp *cp;
	struct device_node *node;
	const char *product = product_name_get();
	int gpio, ret;

	if (!product || (strcmp(product, "rothko") && strcmp(product, "rothko_global")))
		return;

	node = of_find_compatible_node(NULL, NULL, "pd_cp_manager");
	if (!node)
		return;
	gpio = of_device_is_available(node) ?
	       of_get_named_gpio(node, "mt6375_control_gpio", 0) : -ENODEV;
	of_node_put(node);
	if (gpio < 0) {
		dev_warn(dev, "SC858x: cannot resolve PPS isolation GPIO: %d\n", gpio);
		return;
	}
	cp = devm_kzalloc(dev, sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return;
	cp->gpio = gpio;

	ret = gpio_request(gpio, "mt6375-pps-isolation");
	if (ret) {
		dev_warn(dev, "SC858x: cannot own PPS isolation GPIO: %d\n", ret);
		return;
	}
	if (devm_add_action_or_reset(dev, free_isolation_gpio, cp))
		return;

	info->pd_cp = cp;
}
