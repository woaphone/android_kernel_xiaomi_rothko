// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

/*
 *
 * Filename:
 * ---------
 *    mtk_pd.c
 *
 * Project:
 * --------
 *   Android_Software
 *
 * Description:
 * ------------
 *   This Module defines functions of Battery charging
 *
 * Author:
 * -------
 * Wy Chuang
 *
 */
#include <linux/init.h>		/* For init/exit macros */
#include <linux/module.h>	/* For MODULE_ marcros  */
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/power_supply.h>
#include <linux/pm_wakeup.h>
#include <linux/time.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/scatterlist.h>
#include <linux/suspend.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/reboot.h>

#include "adapter_class.h"
#include "mtk_pd.h"
#include "mtk_charger_algorithm_class.h"

static int pd_dbg_level = PD_DEBUG_LEVEL;
#define PD_VBUS_IR_DROP_THRESHOLD 1200
#define PD_FIXED_UP_STANDBY_UA ((2500000 / 9000) * 1000)
#define PD_FIXED_FALLBACK_UA 1500000
#define PD_FIXED_RECONNECT_MS 10000

static bool algo_waiver_test;
module_param(algo_waiver_test, bool, 0644);

int pd_get_debug_level(void)
{
	return pd_dbg_level;
}

static char *pd_state_to_str(int state)
{
	switch (state) {
	case PD_HW_UNINIT:
		return "PD_HW_UNINIT";
	case PD_HW_FAIL:
		return "PD_HW_FAIL";
	case PD_HW_READY:
		return "PD_HW_READY";
	case PD_TA_NOT_SUPPORT:
		return "PD_TA_NOT_SUPPORT";
	case PD_RUN:
		return "PD_RUN";
	case PD_TUNING:
		return "PD_TUNING";
	case PD_POSTCC:
		return "PD_POSTCC";
	default:
		break;
	}
	pd_err("%s unknown state:%d\n", __func__
		, state);
	return "PD_UNKNOWN";
}

static int _pd_init_algo(struct chg_alg_device *alg)
{
	struct mtk_pd *pd;
	int cnt, log_level;

	pd = dev_get_drvdata(&alg->dev);
	pd_dbg("%s\n", __func__);

	mutex_lock(&pd->access_lock);
	if (pd_hal_init_hardware(alg) != 0) {
		pd->state = PD_HW_FAIL;
		pd_err("%s:init hw fail\n", __func__);
	} else
		pd->state = PD_HW_READY;

	pd_hal_vbat_mon_en(alg, CHG1, false);
	pd->old_cv = 0;
	pd->stop_6pin_re_en = 0;

	if (alg->config == DUAL_CHARGERS_IN_PARALLEL) {
		pd_err("%s does not support DUAL_CHARGERS_IN_PARALLEL\n",
			__func__);
		alg->config = SINGLE_CHARGER;
	} else if (alg->config == DUAL_CHARGERS_IN_SERIES) {
		cnt = pd_hal_get_charger_cnt(alg);
		if (cnt == 2)
			alg->config = DUAL_CHARGERS_IN_SERIES;
		else
			alg->config = SINGLE_CHARGER;
	} else
		alg->config = SINGLE_CHARGER;

	log_level = pd_hal_get_log_level(alg);
	pr_notice("%s: log_level=%d", __func__, log_level);
	if (log_level > 0)
		pd_dbg_level = log_level;

	pd->pdc_max_watt_setting = -1;

	pd->check_impedance = true;
	pd->pd_cap_max_watt = -1;
	pd->pd_idx = -1;
	pd->pd_reset_idx = -1;
	pd->pd_boost_idx = 0;
	pd->pd_buck_idx = 0;

	mutex_unlock(&pd->access_lock);
	return 0;
}

static int _pd_is_algo_ready(struct chg_alg_device *alg)
{
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);
	int ret_value;
	int uisoc;

	if (algo_waiver_test) {
		ret_value = ALG_WAIVER;
		goto skip;
	}

	pd_dbg("%s %d\n", __func__, pd->state);
	switch (pd->state) {
	case PD_HW_UNINIT:
	case PD_HW_FAIL:
		ret_value = ALG_INIT_FAIL;
		break;
	case PD_HW_READY:
		ret_value = pd_hal_is_adapter_ready(alg);
		if (ret_value == ALG_READY) {
			uisoc = pd_hal_get_uisoc(alg);
			if (pd->input_current_limit1 != -1 ||
				pd->charging_current_limit1 != -1 ||
				pd->input_current_limit2 != -1 ||
				pd->charging_current_limit2 != -1)
				ret_value = ALG_NOT_READY;
			else if (uisoc >= pd->pd_stop_battery_soc ||
				(uisoc == -1 && pd->ref_vbat > pd->vbat_threshold))
				ret_value = ALG_WAIVER;
		} else if (ret_value == ALG_TA_NOT_SUPPORT)
			pd->state = PD_TA_NOT_SUPPORT;
		else if (ret_value == ALG_TA_CHECKING)
			pd->state = PD_HW_READY;
		else
			pd->state = PD_TA_NOT_SUPPORT;

		break;
	case PD_TA_NOT_SUPPORT:
		ret_value = ALG_TA_NOT_SUPPORT;
		break;
	case PD_RUN:
	case PD_TUNING:
	case PD_POSTCC:
		ret_value = ALG_RUNNING;
		break;
	default:
		pd_err("PD unknown state:%d\n", pd->state);
		ret_value = ALG_INIT_FAIL;
		break;
	}
skip:
	return ret_value;
}

static void __mtk_pdc_init_table(struct chg_alg_device *alg)
{
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);

	pd->cap.nr = 0;
	pd->cap.selected_cap_idx = -1;

	if (pd_hal_is_adapter_ready(alg) == ALG_READY) {
		if (pd_hal_get_adapter_cap(alg, &pd->cap) != MTK_ADAPTER_OK)
			pd->cap.nr = 0;
	} else
		pd_err("mtk_is_pdc_ready is fail\n");

	pd_dbg("[%s] nr:%d default:%d\n", __func__, pd->cap.nr,
	pd->cap.selected_cap_idx);
}

static void __mtk_pdc_get_reset_idx(struct chg_alg_device *alg)
{
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);
	struct pd_power_cap *cap = &pd->cap;
	int i;

	/* The source may replace its capabilities while still attached. */
	pd->pd_reset_idx = -1;
	for (i = 0; i < cap->nr; i++) {
		if (cap->min_mv[i] == pd->vbus_l &&
		    cap->max_mv[i] == pd->vbus_l && cap->ma[i] > 0) {
			pd->pd_reset_idx = i;
			break;
		}
	}
	pd_dbg("[%s]reset idx:%d vbus:%d\n", __func__, pd->pd_reset_idx,
	       pd->vbus_l);
}

static void __mtk_pdc_get_cap_max_watt(struct chg_alg_device *alg)
{
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);
	struct pd_power_cap *cap;
	int i = 0;
	int idx = 0;

	cap = &pd->cap;

	if (pd->pd_cap_max_watt == -1) {
		for (i = 0; i < cap->nr; i++) {
			if (cap->min_mv[i] <= pd->vbus_h &&
				cap->min_mv[i] >= pd->vbus_l &&
				cap->max_mv[i] <= pd->vbus_h &&
				cap->max_mv[i] >= pd->vbus_l) {

				if (cap->maxwatt[i] > pd->pd_cap_max_watt) {
					pd->pd_cap_max_watt = cap->maxwatt[i];
					idx = i;
				}
				pd_dbg("%d %d %d %d %d %d\n",
					cap->min_mv[i],
					cap->max_mv[i],
					pd->vbus_h,
					pd->vbus_l,
					cap->maxwatt[i],
					pd->pd_cap_max_watt);
				continue;
			}
		}
		pd_dbg("[%s]idx:%d vbus:%d %d maxwatt:%d\n", __func__,
			idx, cap->min_mv[idx], cap->max_mv[idx],
			pd->pd_cap_max_watt);
	}
}

static int __mtk_pdc_get_idx(struct chg_alg_device *alg, int selected_idx,
	int *boost_idx, int *buck_idx)
{
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);
	struct pd_power_cap *cap;
	int i = 0;
	int idx = 0;

	cap = &pd->cap;
	idx = selected_idx;

	if (idx < 0 || idx >= cap->nr)
		return -EINVAL;

	/* get boost_idx */
	for (i = 0; i < cap->nr; i++) {

		if (cap->min_mv[i] < pd->vbus_l ||
			cap->max_mv[i] < pd->vbus_l) {
			pd_err("min_mv error:%d %d %d\n",
					cap->min_mv[i],
					cap->max_mv[i],
					pd->vbus_l);
			continue;
		}

		if (cap->min_mv[i] > pd->vbus_h ||
			cap->max_mv[i] > pd->vbus_h) {
			pd_err("max_mv error:%d %d %d\n",
					cap->min_mv[i],
					cap->max_mv[i],
					pd->vbus_h);
			continue;
		}

		if (idx == selected_idx) {
			if (cap->maxwatt[i] > cap->maxwatt[idx])
				idx = i;
		} else {
			if (cap->maxwatt[i] < cap->maxwatt[idx] &&
				cap->maxwatt[i] > cap->maxwatt[selected_idx])
				idx = i;
		}
	}
	*boost_idx = idx;
	idx = selected_idx;

	/* get buck_idx */
	for (i = 0; i < cap->nr; i++) {

		if (cap->min_mv[i] < pd->vbus_l ||
			cap->max_mv[i] < pd->vbus_l) {
			pd_err("min_mv error:%d %d %d\n",
					cap->min_mv[i],
					cap->max_mv[i],
					pd->vbus_l);
			continue;
		}

		if (cap->min_mv[i] > pd->vbus_h ||
			cap->max_mv[i] > pd->vbus_h) {
			pd_err("max_mv error:%d %d %d\n",
					cap->min_mv[i],
					cap->max_mv[i],
					pd->vbus_h);
			continue;
		}

		if (idx == selected_idx) {
			if (cap->maxwatt[i] < cap->maxwatt[idx])
				idx = i;
		} else {
			if (cap->maxwatt[i] > cap->maxwatt[idx] &&
				cap->maxwatt[i] < cap->maxwatt[selected_idx])
				idx = i;
		}
	}
	*buck_idx = idx;

	return 0;
}

static bool mtk_pd_same_source(const struct pd_power_cap *a,
			       const struct pd_power_cap *b)
{
	int i;

	if (a->source_port != b->source_port || a->nr != b->nr)
		return false;
	/* A failed optional Extended query is not evidence of a different source. */
	if (a->source_id_valid && b->source_id_valid &&
	    (a->source_vid != b->source_vid || a->source_pid != b->source_pid ||
	     a->source_xid != b->source_xid))
		return false;
	for (i = 0; i < a->nr; i++) {
		if (a->min_mv[i] != b->min_mv[i] || a->max_mv[i] != b->max_mv[i] ||
		    a->ma[i] != b->ma[i])
			return false;
	}
	return true;
}

static void mtk_pd_block_9v(struct mtk_pd *pd)
{
	pd->failed_9v_cap = pd->cap;
	pd->failed_9v_until = jiffies + msecs_to_jiffies(PD_FIXED_RECONNECT_MS);
	pd_info("fixed PD: defer 9 V on matching source after transition failure\n");
}

static int mtk_pd_input_limit(struct mtk_pd *pd, int mv, int ma)
{
	int limit = ma * 1000;

	if (pd->enable_inductor_protect) {
		if (mv == 5000)
			limit = min(limit, 3000000);
		else if (mv == 9000)
			limit = min(limit, 1500000);
	}
	if (mv == 5000 && pd->fixed_5v_fallback)
		limit = min(limit, PD_FIXED_FALLBACK_UA);
	if (pd->input_current_limit1 != -1)
		limit = min(limit, pd->input_current_limit1);

	return limit;
}

static int __mtk_pdc_setup(struct chg_alg_device *alg, int idx)
{
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);
	struct pd_power_cap *cap = &pd->cap;
	int oldmivr = 4600000, old_idx = cap->selected_cap_idx;
	u32 old_input, input_limit, restore_input;
	bool protected_9v, standby_up, fallback;
	int mv, ma, ret, measured;

	lockdep_assert_held(&pd->access_lock);

	if (idx < 0 || idx >= cap->nr)
		return -EINVAL;
	mv = cap->max_mv[idx];
	ma = cap->ma[idx];
	if (mv < pd->vbus_l || mv > pd->vbus_h || ma <= 0)
		return -EINVAL;

	protected_9v = pd->enable_inductor_protect && alg->config == SINGLE_CHARGER &&
		       of_machine_is_compatible("mediatek,MT6989") && mv == 9000;
	standby_up = protected_9v && old_idx < cap->nr && cap->max_mv[old_idx] == 5000;
	fallback = mv == 5000 && pd->fixed_5v_fallback;

	pd_hal_get_mivr(alg, CHG1, &oldmivr);
	if (pd->pd_idx == idx && old_idx == idx &&
	    mv <= oldmivr / 1000 + PD_VBUS_IR_DROP_THRESHOLD) {
		if (!fallback && !protected_9v)
			return 0;
		measured = pd_hal_get_primary_vbus(alg);
		if (fallback)
			return measured >= 4500 && measured <= 5500 ? 0 : -EAGAIN;
		if (measured >= 8550 && measured <= 9450)
			return 0;
		mtk_pd_block_9v(pd);
		return -EAGAIN;
	}

	ret = pd_hal_get_input_current(alg, CHG1, &old_input);
	if (ret)
		return -EIO;
	input_limit = mtk_pd_input_limit(pd, mv, ma);
	/* Shed the main charger load before Request, not in the async notifier. */
	if (protected_9v || fallback)
		input_limit = min_t(u32, input_limit, PD_FIXED_UP_STANDBY_UA);
	if (old_input > input_limit &&
	    pd_hal_set_input_current(alg, CHG1, input_limit))
		return -EIO;
	if (standby_up) {
		measured = pd_hal_get_primary_vbus(alg);
		if (measured < 4500 || measured > 5500)
			return -EAGAIN;
	}
	pd_hal_set_mivr(alg, CHG1, pd->min_charger_voltage);
	if (protected_9v)
		pd_hal_enable_vbus_ovp(alg, true);
	else if (mv > 5000)
		pd_hal_enable_vbus_ovp(alg, false);

	ret = pd_hal_set_adapter_cap(alg, mv, ma);
	if (ret) {
		if (protected_9v || fallback || old_idx >= cap->nr ||
		    cap->max_mv[old_idx] <= 5000)
			pd_hal_enable_vbus_ovp(alg, true);
		else
			pd_hal_enable_vbus_ovp(alg, false);

		/* Only a Reject confirms that the requested PDO was not applied. */
		if (ret == MTK_ADAPTER_REJECT && old_input > input_limit &&
		    old_idx >= 0 && old_idx < cap->nr) {
			restore_input = min_t(u32, old_input,
				mtk_pd_input_limit(pd, cap->max_mv[old_idx],
						   cap->ma[old_idx]));
			if (restore_input > input_limit &&
			    pd_hal_set_input_current(alg, CHG1, restore_input))
				pd_err("%s: failed to restore AICR\n", __func__);
		}
		pd_hal_set_mivr(alg, CHG1, oldmivr);
		if (standby_up && ret == MTK_ADAPTER_VBUS_INVALID)
			mtk_pd_block_9v(pd);
		if ((protected_9v && ret != MTK_ADAPTER_REJECT) || fallback)
			return -EAGAIN;
		return -EIO;
	}
	if (protected_9v || fallback) {
		measured = pd_hal_get_primary_vbus(alg);
		if (measured < (protected_9v ? 8550 : 4500) ||
		    measured > (protected_9v ? 9450 : 5500)) {
			pd->pd_idx = -1;
			if (protected_9v)
				mtk_pd_block_9v(pd);
			return -EAGAIN;
		}
		pd_info("fixed PD: verified %d mV contract, VBUS %d mV\n", mv, measured);
	}
	if (mv <= 5000)
		pd_hal_enable_vbus_ovp(alg, true);
	pd_hal_set_mivr(alg, CHG1, max(pd->min_charger_voltage,
				      (mv - PD_VBUS_IR_DROP_THRESHOLD) * 1000));
	cap->selected_cap_idx = idx;
	pd->pd_idx = idx;
	return 0;
}

static void mtk_pdc_reset(struct chg_alg_device *alg)
{
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);

	pd_dbg("%s: reset to default profile\n", __func__);
	__mtk_pdc_init_table(alg);
	__mtk_pdc_get_reset_idx(alg);
	__mtk_pdc_setup(alg, pd->pd_reset_idx);
	pd_hal_vbat_mon_en(alg, CHG1, false);
	pd->old_cv = 0;
}

static int __mtk_pdc_get_setting(struct chg_alg_device *alg, int *newvbus, int *newcur,
			int *newidx)
{
	int ret = 0;
	int idx, selected_idx;
	int pd_max_watt, pd_min_watt, now_max_watt;
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);
	int ibus = 0, vbus;
	int chg2_watt = 0;
	bool boost = false, buck = false;
	struct pd_power_cap *cap;
	unsigned int mivr1 = 0;
	unsigned int mivr2 = 0;
	bool chg1_mivr = false;
	bool chg2_mivr = false;
	int chg_cnt, i, is_chip_enabled;

	__mtk_pdc_init_table(alg);
	__mtk_pdc_get_reset_idx(alg);
	__mtk_pdc_get_cap_max_watt(alg);

	cap = &pd->cap;

	if (cap->nr == 0)
		return -1;

	ret = pd_hal_get_ibus(alg, &ibus);
	if (ret < 0) {
		pd_err("[%s] get ibus fail, keep default voltage\n", __func__);
		return -1;
	}

#ifdef FIXME
	if (info->data.parallel_vbus) {
		ret = charger_dev_get_ibat(info->chg1_dev, &chg1_ibat);
		if (ret < 0)
			pd_dbg("[%s] get ibat fail\n", __func__);

		ret = charger_dev_get_ibat(info->chg2_dev, &chg2_ibat);
		if (ret < 0) {
			ibat = battery_get_bat_current();
			chg2_ibat = ibat * 100 - chg1_ibat;
		}

		if (ibat < 0 || chg2_ibat < 0)
			chg2_watt = 0;
		else
			chg2_watt = chg2_ibat / 1000 * battery_get_bat_voltage()
					/ info->data.chg2_eff * 100;

		pd_dbg("[%s] chg2_watt:%d ibat2:%d ibat1:%d ibat:%d\n",
			__func__, chg2_watt, chg2_ibat, chg1_ibat, ibat * 100);
	}
#endif

	pd_hal_get_mivr_state(alg, CHG1, &chg1_mivr);
	pd_hal_get_mivr(alg, CHG1, &mivr1);

	chg_cnt = pd_hal_get_charger_cnt(alg);
	if (chg_cnt > 1 && alg->config == DUAL_CHARGERS_IN_SERIES) {
		for (i = CHG2; i < CHG_MAX; i++) {
			is_chip_enabled =
					pd_hal_is_chip_enable(alg, i);
			if (is_chip_enabled) {
				pd_hal_get_mivr_state(alg, CHG2, &chg2_mivr);
				pd_hal_get_mivr(alg, CHG2, &mivr2);
			}

		}
	}

	vbus = pd_hal_get_vbus(alg);
	if (vbus < 0)
		return vbus;
	ibus = ibus / 1000;
	if (ibus == 0)
		ibus = 1000;

	selected_idx = cap->selected_cap_idx;
	if (selected_idx >= cap->nr)
		return -ENODATA;

	/* A charger polling cycle can miss detach; key fallback to the PE attach. */
	pd->fixed_5v_fallback = pd->failed_9v_cap.nr &&
		mtk_pd_same_source(cap, &pd->failed_9v_cap) &&
		((pd->fixed_5v_fallback && pd->fixed_5v_gen == cap->attach_gen) ||
		 (cap->attach_gen - pd->failed_9v_cap.attach_gen <= 1 &&
		  time_before(jiffies, pd->failed_9v_until)));
	if (pd->fixed_5v_fallback) {
		pd->fixed_5v_gen = cap->attach_gen;
		if (pd->pd_reset_idx < 0)
			return -ENODATA;
		*newidx = pd->pd_reset_idx;
		*newvbus = cap->max_mv[*newidx];
		*newcur = cap->ma[*newidx];
		return 0;
	}
	pd->failed_9v_cap.nr = 0;

	if ((chg1_mivr && (vbus < mivr1 / 1000 - 500)) ||
	    (chg2_mivr && (vbus < mivr2 / 1000 - 500)) ||
	    cap->min_mv[selected_idx] < pd->vbus_l ||
	    cap->max_mv[selected_idx] > pd->vbus_h ||
	    cap->ma[selected_idx] <= 0)
		goto reset;

	idx = selected_idx;

	/* Recompute indices from the same snapshot used to select the PDO. */
	ret = __mtk_pdc_get_idx(alg, idx, &pd->pd_boost_idx, &pd->pd_buck_idx);
	if (ret)
		return ret;

	pd_dbg("idx:%d %d %d %d %d %d\n", idx,
		cap->max_mv[idx],
		cap->ma[idx],
		cap->maxwatt[idx],
		pd->ibus_err,
		ibus);

	pd_max_watt = cap->max_mv[idx] * (cap->ma[idx]
			/ 100 * (100 - pd->ibus_err) - 100);

	pd_dbg("pd_max_watt:%d %d %d %d %d\n", idx,
		cap->max_mv[idx],
		cap->ma[idx],
		pd->ibus_err,
		pd_max_watt);


	now_max_watt = cap->max_mv[idx] * ibus + chg2_watt;
	pd_dbg("now_max_watt:%d %d %d %d %d\n", idx,
		cap->max_mv[idx],
		ibus,
		chg2_watt,
		now_max_watt);

	pd_min_watt = cap->max_mv[pd->pd_buck_idx] * cap->ma[pd->pd_buck_idx]
			/ 100 * (100 - pd->ibus_err)
			- pd->vsys_watt;

	pd_dbg("pd_min_watt:%d %d %d %d %d\n", pd->pd_buck_idx,
		cap->max_mv[pd->pd_buck_idx],
		cap->ma[pd->pd_buck_idx],
		pd->ibus_err,
		pd->vsys_watt);

	if (pd_min_watt <= 5000000)
		pd_min_watt = 5000000;

	if ((now_max_watt >= pd_max_watt) || chg1_mivr || chg2_mivr) {
		*newidx = pd->pd_boost_idx;
		boost = true;
	} else if (now_max_watt <= pd_min_watt) {
		*newidx = pd->pd_buck_idx;
		buck = true;
	} else {
		*newidx = selected_idx;
		boost = false;
		buck = false;
	}

	*newvbus = cap->max_mv[*newidx];
	*newcur = cap->ma[*newidx];

	pd_dbg("[%s]watt:%d,%d,%d up:%d,%d vbus:%d ibus:%d, mivr:%d,%d\n",
		__func__,
		pd_max_watt, now_max_watt, pd_min_watt,
		boost, buck,
		vbus, ibus, chg1_mivr, chg2_mivr);

	pd_dbg("[%s]vbus:%d:%d:%d current:%d idx:%d default_idx:%d\n",
		__func__, pd->vbus_h, pd->vbus_l, *newvbus,
		*newcur, *newidx, selected_idx);

	return 0;

reset:
	if (pd->pd_reset_idx < 0)
		return -ENODATA;
	*newidx = pd->pd_reset_idx;
	pd->old_cv = 0;
	*newvbus = cap->max_mv[*newidx];
	*newcur = cap->ma[*newidx];

	return 0;
}

static int pd_sc_set_charger(struct chg_alg_device *alg)
{
	struct mtk_pd *pd;
	int ichg1_min = -1, aicr1_min = -1;
	int ret;

	pd = dev_get_drvdata(&alg->dev);

	if (pd->input_current_limit1 == 0 ||
		pd->charging_current_limit1 == 0) {
		pr_notice("input/charging current is 0, end Pd\n");
		return -1;
	}

	mutex_lock(&pd->data_lock);
	pd->charging_current1 = pd->sc_charger_current;
	if (pd->charging_current_limit1 != -1) {
		pd->charging_current1 = min(pd->charging_current1,
					    pd->charging_current_limit1);
		ret = pd_hal_get_min_charging_current(alg, CHG1, &ichg1_min);
		if (ret != -EOPNOTSUPP &&
		    pd->charging_current1 < ichg1_min)
			pd->charging_current1 = 0;
	}

	if (pd->input_current_limit1 != -1 &&
		pd->input_current_limit1 <
		pd->sc_input_current) {
		pd->input_current1 = pd->input_current_limit1;
		ret = pd_hal_get_min_input_current(alg, CHG1, &aicr1_min);
		if (ret != -EOPNOTSUPP &&
			pd->input_current_limit1 < aicr1_min)
			pd->input_current1 = 0;
	} else
		pd->input_current1 = pd->sc_input_current;
	mutex_unlock(&pd->data_lock);


	if (pd->input_current1 == 0 ||
		pd->charging_current1 == 0) {
		pd_err("current is zero %d %d\n",
			pd->input_current1,
			pd->charging_current1);
		return -1;
	}

	pd_hal_set_charging_current(alg,
		CHG1, pd->charging_current1);
	pd_hal_set_input_current(alg,
		CHG1, pd->input_current1);

	if (pd->old_cv == 0 || (pd->old_cv != pd->cv) || pd->pd_6pin_en == 0) {
		pd_hal_vbat_mon_en(alg, CHG1, false);
		pd_hal_set_cv(alg, CHG1, pd->cv);
		if (pd->pd_6pin_en && pd->stop_6pin_re_en != 1)
			pd_hal_vbat_mon_en(alg, CHG1, true);

		pd->old_cv = pd->cv;
	} else {
		if (pd->pd_6pin_en && pd->stop_6pin_re_en != 1) {
			pd->stop_6pin_re_en = 1;
			pd_hal_vbat_mon_en(alg, CHG1, true);
		}
	}

	pd_dbg("%s old_cv=%d, new_cv=%d, pd_6pin_en=%d 6pin_re_en=%d\n", __func__,
		pd->old_cv, pd->cv, pd->pd_6pin_en, pd->stop_6pin_re_en);

	return 0;
}

static int pd_dcs_set_charger(struct chg_alg_device *alg)
{
	struct mtk_pd *pd;
	bool chg2_enable = true;
	bool chg2_chip_enabled = false;
	int ret;
	int ichg1_min = -1, ichg2_min = -1;
	int aicr1_min = -1;

	pd = dev_get_drvdata(&alg->dev);

	if (pd->input_current_limit1 == 0 ||
		pd->charging_current_limit1 == 0 ||
		pd->charging_current_limit2 == 0) {
		pr_notice("input/charging current is 0, end PD\n");
		return -1;
	}

	mutex_lock(&pd->data_lock);
	if (pd->input_current_limit1 != -1 &&
		pd->input_current_limit1 <
		pd->dcs_input_current) {
		pd->input_current1 = pd->input_current_limit1;
		ret = pd_hal_get_min_input_current(alg, CHG1, &aicr1_min);
		if (ret != -EOPNOTSUPP &&
			pd->input_current_limit1 < aicr1_min)
			pd->input_current1 = 0;
	} else
		pd->input_current1 = pd->dcs_input_current;

	if (pd->charging_current_limit1 != -1 &&
		pd->charging_current_limit1 <
		pd->dcs_chg1_charger_current) {
		pd->charging_current1 = pd->charging_current_limit1;
		ret = pd_hal_get_min_charging_current(alg, CHG1, &ichg1_min);
		if (ret != -EOPNOTSUPP &&
			pd->charging_current_limit1 < ichg1_min)
			pd->charging_current1 = 0;
	} else
		pd->charging_current1 = pd->dcs_chg1_charger_current;

	if (pd->state == PD_RUN)
		pd->charging_current2 = pd->dcs_chg2_charger_current;

	if (pd->charging_current_limit2 != -1 &&
		pd->charging_current_limit2 <
		pd->charging_current2) {
		pd->charging_current2 = pd->charging_current_limit2;
		ret = pd_hal_get_min_charging_current(alg, CHG2, &ichg2_min);
		if (ret != -EOPNOTSUPP &&
			pd->charging_current_limit2 < ichg2_min)
			pd->charging_current2 = 0;
	}
	mutex_unlock(&pd->data_lock);

	if (pd->input_current1 == 0 ||
		pd->charging_current1 == 0 ||
		pd->charging_current2 == 0) {
		pd_err("current is zero %d %d %d\n",
			pd->input_current1,
			pd->charging_current1,
			pd->charging_current2);
		pd_hal_enable_charger(alg, CHG2, false);
		pd_hal_charger_enable_chip(alg, CHG2, false);
		return -1;
	}

	chg2_chip_enabled = pd_hal_is_chip_enable(alg, CHG2);
	pd_dbg("chg2_en:%d %d %d\n",
		chg2_enable, chg2_chip_enabled, pd->state);
	if (pd->state == PD_RUN) {
		if (!chg2_chip_enabled)
			pd_hal_charger_enable_chip(alg, CHG2, true);
		pd_hal_enable_charger(alg, CHG2, true);
		pd_hal_set_input_current(alg,
			CHG2, pd->charging_current2);
		pd_hal_set_charging_current(alg,
			CHG2, pd->charging_current2);

		pd_hal_set_eoc_current(alg, CHG1,
			pd->dual_polling_ieoc);
		pd_hal_enable_termination(alg, CHG1, false);
		pd_hal_safety_check(alg, pd->dual_polling_ieoc);
	} else if (pd->state == PD_TUNING) {
		if (!chg2_chip_enabled)
			pd_hal_charger_enable_chip(alg, CHG2, true);
		pd_hal_enable_charger(alg, CHG2, true);
		pd_hal_set_eoc_current(alg, CHG1, pd->dual_polling_ieoc);
		pd_hal_enable_termination(alg, CHG1, false);
		pd_hal_safety_check(alg, pd->dual_polling_ieoc);
	} else if (pd->state == PD_POSTCC) {
		pd_hal_set_eoc_current(alg, CHG1, 150000);
		pd_hal_enable_termination(alg, CHG1, true);
	} else {
		pd_err("%s state error!", __func__);
		return -1;
	}

	pd_hal_set_charging_current(alg,
		CHG1, pd->charging_current1);
	pd_hal_set_input_current(alg,
		CHG1, pd->input_current1);
	pd_hal_set_cv(alg,
		CHG1, pd->cv);

	pd_dbg("%s m:%d s:%d cv:%d chg1:%d,%d chg2:%d,%d chg2en:%d min:%d,%d,%d\n",
		__func__,
		alg->config,
		pd->state,
		pd->cv,
		pd->input_current1,
		pd->charging_current1,
		pd->input_current2,
		pd->charging_current2,
		chg2_enable,
		ichg1_min,
		ichg2_min,
		aicr1_min);

	return 0;
}

static int __pd_run(struct chg_alg_device *alg)
{
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);
	int vbus, cur, idx, limit, ret, ret_value = ALG_RUNNING;

	ret = __mtk_pdc_get_setting(alg, &vbus, &cur, &idx);
	if (!ret)
		ret = __mtk_pdc_setup(alg, idx);

	if (!ret) {
		limit = mtk_pd_input_limit(pd, vbus, cur);
	} else {
		limit = ret == -EAGAIN || pd->fixed_5v_fallback ?
			PD_FIXED_UP_STANDBY_UA : PD_FAIL_CURRENT;
		if (pd->charging_current_limit1 == -1 ||
		    pd->charging_current_limit1 > PD_FAIL_CURRENT)
			pd->charging_current_limit1 = PD_FAIL_CURRENT;
	}
	if (pd->input_current_limit1 == -1 || pd->input_current_limit1 > limit)
		pd->input_current_limit1 = limit;

	if (alg->config == DUAL_CHARGERS_IN_SERIES) {
		if (pd_dcs_set_charger(alg) != 0) {
			ret_value = ALG_DONE;
			//goto out;
		}
	} else {
		if (pd_sc_set_charger(alg) != 0) {
			ret_value = ALG_DONE;
			//goto out;
		}
	}

	return ret_value;
}

static int _pd_start_algo(struct chg_alg_device *alg)
{
	int ret_value = 0;
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);
	bool again = false;
	int uisoc;

	mutex_lock(&pd->access_lock);

	if (algo_waiver_test) {
		ret_value = ALG_WAIVER;
		goto skip;
	}

	do {
		pd_info("%s state:%d %s %d\n", __func__,
			pd->state,
			pd_state_to_str(pd->state),
			again);
		again = false;

		switch (pd->state) {
		case PD_HW_UNINIT:
		case PD_HW_FAIL:
			ret_value = ALG_INIT_FAIL;
			break;
		case PD_HW_READY:
			ret_value = pd_hal_is_adapter_ready(alg);
			if (ret_value == ALG_TA_NOT_SUPPORT)
				pd->state = PD_TA_NOT_SUPPORT;
			else if (ret_value == ALG_READY) {
				uisoc = pd_hal_get_uisoc(alg);
				if (pd->input_current_limit1 != -1 ||
					pd->charging_current_limit1 != -1 ||
					pd->input_current_limit2 != -1 ||
					pd->charging_current_limit2 != -1)
					ret_value = ALG_NOT_READY;
				else if (uisoc >= pd->pd_stop_battery_soc ||
					(uisoc == -1 && pd->ref_vbat > pd->vbat_threshold))
					ret_value = ALG_WAIVER;
				else {
					pd->state = PD_RUN;
					again = true;
				}
			}
			break;
		case PD_TA_NOT_SUPPORT:
			ret_value = ALG_TA_NOT_SUPPORT;
			break;
		case PD_RUN:
		case PD_TUNING:
		case PD_POSTCC:
			ret_value = __pd_run(alg);
			break;
		default:
			pd_err("PD unknown state:%d\n", pd->state);
			ret_value = ALG_INIT_FAIL;
			break;
		}
	} while (again == true);
skip:
	mutex_unlock(&pd->access_lock);

	return ret_value;
}


static bool _pd_is_algo_running(struct chg_alg_device *alg)
{
	struct mtk_pd *pd;

	pd_dbg("%s\n", __func__);
	pd = dev_get_drvdata(&alg->dev);

	if (pd->state == PD_RUN || pd->state == PD_TUNING
		|| pd->state == PD_POSTCC)
		return true;

	return false;
}

static int _pd_stop_algo(struct chg_alg_device *alg)
{
	int ret_value = 0;
	struct mtk_pd *pd = dev_get_drvdata(&alg->dev);

	mutex_lock(&pd->access_lock);

	pd_info("%s state:%d %s\n", __func__,
		pd->state,
		pd_state_to_str(pd->state));

	switch (pd->state) {
	case PD_HW_UNINIT:
	case PD_HW_FAIL:
	case PD_HW_READY:
	case PD_TA_NOT_SUPPORT:
		break;
	case PD_TUNING:
	case PD_POSTCC:
	case PD_RUN:
		mtk_pdc_reset(alg);
		pd_hal_set_charging_current(alg,
			CHG1, PD_FAIL_CURRENT);
		pd_hal_set_input_current(alg,
			CHG1, PD_FAIL_CURRENT);
		pd_hal_set_cv(alg,
			CHG1, pd->cv);
		pd->state = PD_HW_READY;
		if (alg->config == DUAL_CHARGERS_IN_SERIES) {
			pd_hal_enable_charger(alg, CHG2, false);
			pd_hal_charger_enable_chip(alg,
			CHG2, false);
		}
		break;
	default:
		pd_err("PD unknown state:%d\n", pd->state);
		ret_value = ALG_INIT_FAIL;
		break;
	}
	mutex_unlock(&pd->access_lock);

	return ret_value;
}

static int pd_full_evt(struct chg_alg_device *alg)
{
	struct mtk_pd *pd;
	int ret = 0;
	bool chg_en = true, chg2_enabled = false;
	int ichg2 = 0, ichg2_min = 100000;
	int ret_value = 0;

	pd = dev_get_drvdata(&alg->dev);
	switch (pd->state) {
	case PD_HW_UNINIT:
	case PD_HW_FAIL:
	case PD_HW_READY:
	case PD_TA_NOT_SUPPORT:
		break;
	case PD_TUNING:
	case PD_POSTCC:
	case PD_RUN:
		if (alg->config == DUAL_CHARGERS_IN_SERIES) {
			pd_hal_is_charger_enable(
				alg, CHG2, &chg_en);
			chg2_enabled = pd_hal_is_chip_enable(alg, CHG2);

			if (!chg_en || !chg2_enabled) {
				/* notify eoc , fix me */
				pd->state = PD_HW_READY;
				pd_err("%s: charging done:%d %d\n",
					__func__, chg_en, chg2_enabled);
				if (alg->is_polling_mode == false)
					ret_value = 1;
			} else {
				pd_hal_get_charging_current(alg, CHG2, &ichg2);
				ret = pd_hal_get_min_charging_current(
					alg, CHG2, &ichg2_min);
				if (ret == -EOPNOTSUPP)
					ichg2_min = 100000;

				pd_err("ichg2:%d, ichg2_min:%d state:%d\n",
					ichg2, ichg2_min, pd->state);
				if (ichg2 - 500000 <= ichg2_min) {
					pd->state = PD_POSTCC;
					pd_hal_enable_charger(alg,
						CHG2, false);
					pd_hal_set_eoc_current(alg,
						CHG1, 150000);
					pd_hal_enable_termination(alg,
						CHG1, true);
				} else {
					pd->state = PD_TUNING;
					mutex_lock(&pd->data_lock);
					if (pd->charging_current2 >= 500000)
						pd->charging_current2 =
							ichg2 - 500000;
					pd_hal_set_charging_current(alg,
						CHG2, pd->charging_current2);
					mutex_unlock(&pd->data_lock);
				}
				ret_value = 1;
			}

		} else {
			if (pd->state == PD_RUN) {
				pd_err("%s evt full\n",  __func__);
				pd->state = PD_HW_READY;
			}
		}
		break;
	default:
		pd_err("PD unknown state:%d\n", pd->state);
		ret_value = ALG_INIT_FAIL;
		break;
	}
	return ret_value;
}

static int pd_plugout_reset(struct chg_alg_device *alg)
{
	struct mtk_pd *pd;

	pd = dev_get_drvdata(&alg->dev);
	switch (pd->state) {
	case PD_HW_UNINIT:
	case PD_HW_FAIL:
	case PD_HW_READY:
		break;
	case PD_TA_NOT_SUPPORT:
		pd->state = PD_HW_READY;
		break;
	case PD_TUNING:
	case PD_POSTCC:
	case PD_RUN:
		pd->state = PD_HW_READY;
		pd_hal_enable_vbus_ovp(alg, true);
		if (alg->config == DUAL_CHARGERS_IN_SERIES) {
			pd_hal_enable_charger(alg, CHG2, false);
			pd_hal_charger_enable_chip(alg,
			CHG2, false);
		}
		pd->pd_cap_max_watt = -1;
		pd->pd_idx = -1;
		pd->pd_reset_idx = -1;
		pd->pd_boost_idx = 0;
		pd->pd_buck_idx = 0;
		break;
	default:
		pd_err("PD unknown state:%d\n", pd->state);
		break;
	}
	return 0;
}

static int _pd_notifier_call(struct chg_alg_device *alg,
			 struct chg_alg_notify *notify)
{
	struct mtk_pd *pd;
	int ret_value = 0;

	pd = dev_get_drvdata(&alg->dev);
	pd_dbg("%s evt:%d state:%s\n", __func__, notify->evt,
		pd_state_to_str(pd->state));

	switch (notify->evt) {
	case EVT_PLUG_OUT:
		mutex_lock(&pd->access_lock);
		pd->fixed_5v_fallback = false;
		pd->stop_6pin_re_en = 0;
		ret_value = pd_plugout_reset(alg);
		mutex_unlock(&pd->access_lock);
		break;
	case EVT_FULL:
		pd->stop_6pin_re_en = 1;
		ret_value = pd_full_evt(alg);
		break;
	case EVT_BATPRO_DONE:
		pd->pd_6pin_en = 0;
		ret_value = 0;
		break;
	default:
		ret_value = -EINVAL;
	}

	return ret_value;
}

static void mtk_pd_parse_dt(struct mtk_pd *pd,
				struct device *dev)
{
	struct device_node *np = dev->of_node;
	u32 val;

	if (of_property_read_u32(np, "min_charger_voltage", &val) >= 0)
		pd->min_charger_voltage = val;
	else if (of_property_read_u32(np, "min-charger-voltage", &val) >= 0)
		pd->min_charger_voltage = val;
	else {
		pd_err("use default V_CHARGER_MIN:%d\n", V_CHARGER_MIN);
		pd->min_charger_voltage = V_CHARGER_MIN;
	}

	/* PD */
	if (of_property_read_u32(np, "pd_vbus_upper_bound", &val) >= 0)
		pd->vbus_h = val / 1000;
	else if (of_property_read_u32(np, "pd-vbus-upper-bound", &val) >= 0)
		pd->vbus_h = val / 1000;
	else {
		pd_err("use default pd_vbus_upper_bound:%d\n",
			PD_VBUS_UPPER_BOUND);
		pd->vbus_h = PD_VBUS_UPPER_BOUND / 1000;
	}

	if (of_property_read_u32(np, "pd_vbus_low_bound", &val) >= 0)
		pd->vbus_l = val / 1000;
	else if (of_property_read_u32(np, "pd-vbus-low-bound", &val) >= 0)
		pd->vbus_l = val / 1000;
	else {
		pd_err("use default pd_vbus_low_bound:%d\n",
			PD_VBUS_LOW_BOUND);
		pd->vbus_l = PD_VBUS_LOW_BOUND / 1000;
	}

	if (of_property_read_u32(np, "vsys_watt", &val) >= 0)
		pd->vsys_watt = val;
	else if (of_property_read_u32(np, "vsys-watt", &val) >= 0)
		pd->vsys_watt = val;
	else {
		pd_err("use default vsys_watt:%d\n",
			VSYS_WATT);
		pd->vsys_watt = VSYS_WATT;
	}

	if (of_property_read_u32(np, "ibus_err", &val) >= 0)
		pd->ibus_err = val;
	else if (of_property_read_u32(np, "ibus-err", &val) >= 0)
		pd->ibus_err = val;
	else {
		pd_err("use default ibus_err:%d\n",
			IBUS_ERR);
		pd->ibus_err = IBUS_ERR;
	}

	if (of_property_read_u32(np, "pd_stop_battery_soc", &val) >= 0)
		pd->pd_stop_battery_soc = val;
	else if (of_property_read_u32(np, "pd-stop-battery-soc", &val) >= 0)
		pd->pd_stop_battery_soc = val;
	else {
		pd_err("use default pd_stop_battery_soc:%d\n",
			PD_STOP_BATTERY_SOC);
		pd->pd_stop_battery_soc = PD_STOP_BATTERY_SOC;
	}

	/* single charger */
	if (of_property_read_u32(np, "sc_input_current", &val) >= 0)
		pd->sc_input_current = val;
	else if (of_property_read_u32(np, "sc-input-current", &val) >= 0)
		pd->sc_input_current = val;
	else {
		pd_err("use default sc_input_current:%d\n",
			PD_SC_INPUT_CURRENT);
		pd->sc_input_current = PD_SC_INPUT_CURRENT;
	}

	if (of_property_read_u32(np, "sc_charger_current", &val) >= 0)
		pd->sc_charger_current = val;
	else if (of_property_read_u32(np, "sc-charger-current", &val) >= 0)
		pd->sc_charger_current = val;
	else {
		pd_err("use default sc_charger_current:%d\n",
			PD_SC_CHARGER_CURRENT);
		pd->sc_charger_current = PD_SC_CHARGER_CURRENT;
	}

	/* dual charger in series*/
	if (of_property_read_u32(np, "dcs_input_current", &val) >= 0)
		pd->dcs_input_current = val;
	else if (of_property_read_u32(np, "dcs-input-current", &val) >= 0)
		pd->dcs_input_current = val;
	else {
		pd_err("use default dcs_input_current:%d\n",
			PD_DCS_INPUT_CURRENT);
		pd->dcs_input_current = PD_DCS_INPUT_CURRENT;
	}

	if (of_property_read_u32(np, "dcs_chg1_charger_current", &val) >= 0)
		pd->dcs_chg1_charger_current = val;
	else if (of_property_read_u32(np, "dcs-chg1-charger-current", &val) >= 0)
		pd->dcs_chg1_charger_current = val;
	else {
		pd_err("use default dcs_chg1_charger_current:%d\n",
			PD_DCS_CHG1_CHARGER_CURRENT);
		pd->dcs_chg1_charger_current = PD_DCS_CHG1_CHARGER_CURRENT;
	}

	if (of_property_read_u32(np, "dcs_chg2_charger_current", &val) >= 0)
		pd->dcs_chg2_charger_current = val;
	else if (of_property_read_u32(np, "dcs-chg2-charger-current", &val) >= 0)
		pd->dcs_chg2_charger_current = val;
	else {
		pd_err("use default dcs_chg2_charger_current:%d\n",
			PD_DCS_CHG2_CHARGER_CURRENT);
		pd->dcs_chg2_charger_current = PD_DCS_CHG2_CHARGER_CURRENT;
	}

	/* dual charger */
	if (of_property_read_u32(np, "slave_mivr_diff", &val) >= 0)
		pd->slave_mivr_diff = val;
	else if (of_property_read_u32(np, "slave-mivr-diff", &val) >= 0)
		pd->slave_mivr_diff = val;
	else {
		pd_err("use default SLAVE_MIVR_DIFF:%d\n", SLAVE_MIVR_DIFF);
		pd->slave_mivr_diff = SLAVE_MIVR_DIFF;
	}

	if (of_property_read_u32(np, "dual_polling_ieoc", &val) >= 0)
		pd->dual_polling_ieoc = val;
	else if (of_property_read_u32(np, "dual-polling-ieoc", &val) >= 0)
		pd->dual_polling_ieoc = val;
	else {
		pd_err("use default dual_polling_ieoc :%d\n", 750000);
		pd->dual_polling_ieoc = 750000;
	}

	if (of_property_read_u32(np, "vbat_threshold", &val) >= 0)
		pd->vbat_threshold = val;
	else if (of_property_read_u32(np, "vbat-threshold", &val) >= 0)
		pd->vbat_threshold = val;
	else {
		pr_notice("turn off vbat_threshold checking:%d\n",
			DISABLE_VBAT_THRESHOLD);
		pd->vbat_threshold = DISABLE_VBAT_THRESHOLD;
	}

	pd->enable_inductor_protect = false;
	if (of_property_read_u32(np, "enable-inductor-protect", &val) >= 0)
		pd->enable_inductor_protect = true;

	if (!pd->enable_inductor_protect)
		pr_notice("disable inductor protection\n");
}

static int _pd_get_prop(struct chg_alg_device *alg,
		enum chg_alg_props s, int *value)
{

	pr_notice("%s\n", __func__);
	if (s == ALG_MAX_VBUS)
		*value = 10000;
	else
		pr_notice("%s does not support prop:%d\n", __func__, s);
	return 0;
}

static int _pd_set_setting(struct chg_alg_device *alg_dev,
	struct chg_limit_setting *setting)
{
	struct mtk_pd *pd;

	pd_dbg("%s cv:%d icl:%d,%d cc:%d,%d\n",
		__func__,
		setting->cv,
		setting->input_current_limit1,
		setting->input_current_limit2,
		setting->charging_current_limit1,
		setting->charging_current_limit2);
	pd = dev_get_drvdata(&alg_dev->dev);

	mutex_lock(&pd->access_lock);
	pd->cv = setting->cv;
	pd->pd_6pin_en = setting->vbat_mon_en;
	pd->input_current_limit1 = setting->input_current_limit1;
	pd->charging_current_limit1 = setting->charging_current_limit1;
	pd->input_current_limit2 = setting->input_current_limit2;
	pd->charging_current_limit2 = setting->charging_current_limit2;
	mutex_unlock(&pd->access_lock);

	return 0;
}

static int _pd_set_prop(struct chg_alg_device *alg,
		enum chg_alg_props s, int value)
{
	struct mtk_pd *pd;

	pr_notice("%s %d %d\n", __func__, s, value);

	pd = dev_get_drvdata(&alg->dev);

	switch (s) {
	case ALG_LOG_LEVEL:
		pd_dbg_level = value;
		break;
	case ALG_REF_VBAT:
		pd->ref_vbat = value;
		break;
	default:
		break;
	}

	return 0;
}


static struct chg_alg_ops pd_alg_ops = {
	.init_algo = _pd_init_algo,
	.is_algo_ready = _pd_is_algo_ready,
	.start_algo = _pd_start_algo,
	.is_algo_running = _pd_is_algo_running,
	.stop_algo = _pd_stop_algo,
	.notifier_call = _pd_notifier_call,
	.get_prop = _pd_get_prop,
	.set_prop = _pd_set_prop,
	.set_current_limit = _pd_set_setting,
};

static int mtk_pd_probe(struct platform_device *pdev)
{
	struct mtk_pd *pd = NULL;

	pr_notice("%s: starts\n", __func__);

	pd = devm_kzalloc(&pdev->dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;
	platform_set_drvdata(pdev, pd);
	pd->pdev = pdev;
	mutex_init(&pd->access_lock);
	mutex_init(&pd->data_lock);
	mtk_pd_parse_dt(pd, &pdev->dev);
	pd->bat_psy = devm_power_supply_get_by_reference(&pdev->dev, "gauge");
	if (IS_ERR_OR_NULL(pd->bat_psy))
		pd_err("%s: devm power fail to get bat_psy\n", __func__);

	pd->alg = chg_alg_device_register("pd", &pdev->dev,
					pd, &pd_alg_ops, NULL);
	return 0;
}

static void mtk_pd_remove(struct platform_device *dev)
{
}

static void mtk_pd_shutdown(struct platform_device *dev)
{

}

static const struct of_device_id mtk_pd_of_match[] = {
	{.compatible = "mediatek,charger,pd",},
	{},
};

MODULE_DEVICE_TABLE(of, mtk_pd_of_match);

struct platform_device pd_device = {
	.name = "pd",
	.id = -1,
};

static struct platform_driver pd_driver = {
	.probe = mtk_pd_probe,
	.remove = mtk_pd_remove,
	.shutdown = mtk_pd_shutdown,
	.driver = {
		   .name = "pd",
		   .of_match_table = mtk_pd_of_match,
	},
};

static int __init mtk_pd_init(void)
{
	return platform_driver_register(&pd_driver);
}
module_init(mtk_pd_init);

static void __exit mtk_pd_exit(void)
{
	platform_driver_unregister(&pd_driver);
}
module_exit(mtk_pd_exit);


MODULE_AUTHOR("wy.chuang <wy.chuang@mediatek.com>");
MODULE_DESCRIPTION("MTK PD algorithm Driver");
MODULE_LICENSE("GPL");
