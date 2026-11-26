// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>
#include <linux/soc/mediatek/mtk_ise.h>
#include <linux/soc/mediatek/mtk_ise_lpm.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/soc/mediatek/tinysys-scmi.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>

#include <linux/scmi_protocol.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define MTK_SIP_KERNEL_ISE_CONTROL MTK_SIP_SMC_CMD(0x53D)

#define ISE_LPM_FREERUN_TIMEOUT_MS		7000
#define ISE_LPM_PWR_OFF_DEBOUNCE_MS		1000
#define ISE_LPM_DEINIT_TIMEOUT_MS		10000

enum ise_scmi_cmd {
	SCMI_MBOX_CMD_NULL = 0x0,
	SCMI_MBOX_CMD_ISE_PWR_ON = 0x1,
	SCMI_MBOX_CMD_ISE_PWR_OFF = 0x2,
	SCMI_MBOX_CMD_ISE_BOOT_DEINIT = 0x3,
};

enum ise_power_state {
	ISE_NO_DEFINE = 0x0,
	ISE_ACTIVE,
	ISE_SLEEP,
	ISE_STAND_BY,
	ISE_POWER_OFF,
	ISE_DEINIT
};

enum MTK_ISE_LPM_KERNEL_OP {
	MTK_ISE_LPM_KERNEL_OP_REQ_DRAM = 0,
	MTK_ISE_LPM_KERNEL_OP_REL_DRAM,
	MTK_ISE_LPM_KERNEL_OP_NUM
};

struct ise_lpm_work_struct {
	struct work_struct work;
	unsigned int flags;
};

enum ise_lpm_wq_cmd {
	ISE_LPM_FREERUN,
	ISE_PWR_OFF,
	ISE_LPM_DEINIT,
	ISE_LPM_WQ_CMD_NUM
};

enum ise_pwr_ut_id_enum {
	ISE_AWAKE_LOCK = 1,
	ISE_AWAKE_UNLOCK,
	ISE_LOWPOWER_UT
};

static u32 ise_scmi_id;
static uint32_t ise_awake_cnt;
static struct scmi_tinysys_info_st *_tinfo;

static DEFINE_MUTEX(mutex_ise_lpm);
static struct timer_list ise_lpm_timer;
static struct timer_list ise_lpm_pd_timer;
static struct timer_list ise_lpm_deinit_timer;
static struct ise_lpm_work_struct ise_lpm_work[ISE_LPM_WQ_CMD_NUM];
static struct workqueue_struct *ise_lpm_wq;
static struct proc_dir_entry *ise_lpm_proc;

static uint32_t ise_wakelock_en;
static uint32_t ise_lpm_freerun_en;
static uint32_t ise_req_pending_cnt;
static uint32_t ise_awake_user_list[ISE_AWAKE_ID_NUM];
static uint64_t ise_boot_cnt;

static int ise_power_on(void);
static int ise_power_off(void);
static int ise_deinit(void);

static int ise_req_dram(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_ISE_CONTROL,
			ISE_MODULE_LPM,
			MTK_ISE_LPM_KERNEL_OP_REQ_DRAM,
			0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

static int ise_rel_dram(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_ISE_CONTROL,
			ISE_MODULE_LPM,
			MTK_ISE_LPM_KERNEL_OP_REL_DRAM,
			0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

static void inc_ise_awake_cnt(enum mtk_ise_awake_id_t mtk_ise_awake_id)
{
	ise_awake_user_list[mtk_ise_awake_id]++;
	ise_awake_cnt++;
}

static void dec_ise_awake_cnt(enum mtk_ise_awake_id_t mtk_ise_awake_id)
{
	ise_awake_user_list[mtk_ise_awake_id]--;
	ise_awake_cnt--;
}

enum mtk_ise_awake_ack_t mtk_ise_awake_lock(enum mtk_ise_awake_id_t mtk_ise_awake_id)
{
	uint64_t start_time, end_time;
	int ret = ISE_SUCCESS;

	start_time = cpu_clock(0);
	mutex_lock(&mutex_ise_lpm);
	if (!ise_wakelock_en) {
		pr_notice("ise wakelock disable!!\n");
		ret = ISE_ERR_WAKELOCK_DISABLE;
		goto out;
	}
	if (mtk_ise_awake_id >= ISE_AWAKE_ID_NUM) {
		pr_notice("err id %d\n", mtk_ise_awake_id);
		ret = ISE_ERR_UID;
		goto out;
	}

	pr_notice("%s cnt%d user%d\n", __func__, ise_awake_cnt, mtk_ise_awake_id);
	inc_ise_awake_cnt(mtk_ise_awake_id);
	if (ise_awake_cnt == 1) {
		if (ise_req_pending_cnt == 1)
			ise_req_pending_cnt--;
		else {
			ret = ise_power_on();
			if (ret)
				dec_ise_awake_cnt(mtk_ise_awake_id);
		}
	}
	end_time = cpu_clock(0);
	pr_notice("%s cnt%d user%d start=%llu, end=%llu diff=%llu\n",
		__func__, ise_awake_cnt, mtk_ise_awake_id,
		start_time, end_time, end_time - start_time);
out:
	mutex_unlock(&mutex_ise_lpm);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_ise_awake_lock);

enum mtk_ise_awake_ack_t mtk_ise_awake_unlock(enum mtk_ise_awake_id_t mtk_ise_awake_id)
{
	uint64_t start_time, end_time;
	int ret = ISE_SUCCESS;

	start_time = cpu_clock(0);
	mutex_lock(&mutex_ise_lpm);
	if (!ise_wakelock_en) {
		pr_notice("ise wakelock disable!!\n");
		ret = ISE_ERR_WAKELOCK_DISABLE;
		goto out;
	}
	if (mtk_ise_awake_id >= ISE_AWAKE_ID_NUM) {
		pr_notice("err id %d\n", mtk_ise_awake_id);
		ret = ISE_ERR_UID;
		goto out;
	}
	if (ise_awake_user_list[mtk_ise_awake_id] == 0) {
		pr_notice("unlock err id %d\n", mtk_ise_awake_id);
		ret = ISE_ERR_UNLOCK_BEFORE_LOCK;
		goto out;
	}
	if (ise_awake_cnt == 0) {
		pr_notice("ref cnt err id %d\n", mtk_ise_awake_id);
		ret = ISE_ERR_REF_CNT;
		goto out;
	}

	pr_notice("%s cnt%d user%d\n", __func__, ise_awake_cnt, mtk_ise_awake_id);
	dec_ise_awake_cnt(mtk_ise_awake_id);
	if (ise_awake_cnt == 0) {
		ise_req_pending_cnt++;
		mod_timer(&ise_lpm_pd_timer,
			jiffies + msecs_to_jiffies(ISE_LPM_PWR_OFF_DEBOUNCE_MS));
	}
	end_time = cpu_clock(0);
	pr_notice("%s cnt%d user%d start=%llu, end=%llu diff=%llu\n",
		__func__, ise_awake_cnt, mtk_ise_awake_id,
		start_time, end_time, end_time - start_time);
out:
	mutex_unlock(&mutex_ise_lpm);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_ise_awake_unlock);

static int ise_power_on(void)
{
	struct scmi_tinysys_status rvalue;
	unsigned int retry = 20;
	int ret;

	ret = ise_req_dram();
	if (ret)
		return ret;
	do {
		ret = scmi_tinysys_common_get(_tinfo->ph, ise_scmi_id,
				SCMI_MBOX_CMD_ISE_PWR_ON, &rvalue);
		if (ret) {
			pr_notice("[ise_lpm] scmi power on failed, ret = %d\n", ret);
			return ret;
		}
		pr_debug("scmi ack r1,r2,r3 = 0x%08x, 0x%08x, 0x%08x\n",
				rvalue.r1,
				rvalue.r2,
				rvalue.r3);
		if (rvalue.r1 == (uint32_t)ISE_ACTIVE) {
			pr_info("[ise_lpm] power on done, retry=%d, cnt%llu\n",
				retry, ++ise_boot_cnt);
			return 0;
		}
		udelay(500);
	} while (--retry);

	pr_notice("[ise_lpm]iSE power on failed\n");
	WARN_ON_ONCE(1);
	return -ETIMEDOUT;
}

static int ise_power_off(void)
{
	struct scmi_tinysys_status rvalue;
	unsigned int retry = 400;
	int ret;

	do {
		ret = scmi_tinysys_common_get(_tinfo->ph, ise_scmi_id,
				SCMI_MBOX_CMD_ISE_PWR_OFF, &rvalue);
		if (ret) {
			pr_notice("[ise_lpm] scmi power off failed, ret = %d\n", ret);
			return ret;
		}
		pr_debug("scmi ack r1,r2,r3 = 0x%08x, 0x%08x, 0x%08x\n",
				rvalue.r1,
				rvalue.r2,
				rvalue.r3);
		if (rvalue.r1 == (uint32_t)ISE_POWER_OFF) {
			pr_info("[ise_lpm] power off done, retry=%d\n", retry);
			return ise_rel_dram();
		}
		udelay(1000);
	} while (--retry);

	/* Do not release DRAM unless the firmware confirmed ISE_POWER_OFF. */
	pr_notice("[ise_lpm] power off failed\n");
	WARN_ON_ONCE(1);
	return -ETIMEDOUT;
}

static int ise_deinit(void)
{
	struct scmi_tinysys_status rvalue;
	unsigned int retry = 5;
	int ret;

	do {
		ret = scmi_tinysys_common_get(_tinfo->ph, ise_scmi_id,
				SCMI_MBOX_CMD_ISE_BOOT_DEINIT, &rvalue);
		if (ret) {
			pr_notice("[ise_lpm] scmi deinit failed, ret = %d\n", ret);
			return ret;
		}
		pr_debug("scmi ack r1,r2,r3 = 0x%08x, 0x%08x, 0x%08x\n",
				rvalue.r1,
				rvalue.r2,
				rvalue.r3);
		if (rvalue.r1 == (uint32_t)ISE_DEINIT) {
			pr_info("[ise_lpm] deinit done, retry=%d\n", retry);
			return 0;
		}
		udelay(1000);
	} while (--retry);
	pr_notice("[ise_lpm] deinit failed\n");
	WARN_ON_ONCE(1);
	return -ETIMEDOUT;
}

static void ise_lpm_schedule_work(struct ise_lpm_work_struct *ise_lpm_ws)
{
	queue_work(ise_lpm_wq, &ise_lpm_ws->work);
}

static void ise_lpm_send_wq(enum ise_lpm_wq_cmd cmd)
{
	ise_lpm_schedule_work(&ise_lpm_work[cmd]);
}

static void ise_lpm_timeout(struct timer_list *t)
{
	ise_lpm_send_wq(ISE_LPM_FREERUN);
}

static void ise_lpm_pwr_off_cb(struct timer_list *t)
{
	ise_lpm_send_wq(ISE_PWR_OFF);
}

static void ise_lpm_deinit_cb(struct timer_list *t)
{
	ise_lpm_send_wq(ISE_LPM_DEINIT);
}

static void ise_lpm_work_handle(struct work_struct *ws)
{
	struct ise_lpm_work_struct *ise_lpm_ws
		= container_of(ws, struct ise_lpm_work_struct, work);
	uint32_t ise_lpm_cmd = ise_lpm_ws->flags;
	int ret;

	pr_notice("%s cmd=%d\n", __func__, ise_lpm_cmd);
	switch (ise_lpm_cmd) {
	case ISE_LPM_FREERUN:
		ret = mtk_ise_awake_unlock(ISE_PM_INIT);
		if (ret != ISE_SUCCESS) {
			pr_notice("%s err %d", __func__, ret);
			WARN_ON_ONCE(1);
		}
		break;
	case ISE_PWR_OFF:
		mutex_lock(&mutex_ise_lpm);
		if (ise_req_pending_cnt > 0) {
			ise_req_pending_cnt--;
			ret = ise_power_off();
			if (ret)
				pr_notice("%s power off err %d\n", __func__, ret);
		} else
			pr_notice("%s drop pwr off %d", __func__, ise_req_pending_cnt);
		mutex_unlock(&mutex_ise_lpm);
		break;
	case ISE_LPM_DEINIT:
		ret = ise_deinit();
		if (ret)
			pr_notice("%s deinit err %d\n", __func__, ret);
		break;
	}
}

static int ise_scmi_init(struct device *dev)
{
	struct scmi_tinysys_info_st *info;
	int ret;

	info = get_scmi_tinysys_info();
	if (!info)
		return -EPROBE_DEFER;
	ret = of_property_read_u32(info->sdev->dev.of_node, "scmi-ise",
			&ise_scmi_id);
	if (ret)
		return ret;
	if (!device_link_add(dev, &info->sdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -ENOMEM;
	_tinfo = info;
	pr_info("#@# %s(%d) scmi-ise_id %u\n", __func__, __LINE__, ise_scmi_id);
	return 0;
}

static ssize_t ise_lpm_dbg(struct file *file, const char __user *buffer,
			size_t count, loff_t *data)
{
	char *parm_str, *cmd_str, *pinput;
	char input[32] = {0};
	long param;
	uint32_t len;
	int err;

	len = (count < (sizeof(input) - 1)) ? count : (sizeof(input) - 1);
	if (copy_from_user(input, buffer, len)) {
		pr_notice("%s: copy from user failed\n", __func__);
		return -EFAULT;
	}

	input[len] = '\0';
	pinput = input;

	cmd_str = strsep(&pinput, " ");

	if (!cmd_str)
		return -EINVAL;

	parm_str = strsep(&pinput, " ");

	if (!parm_str)
		return -EINVAL;

	err = kstrtol(parm_str, 10, &param);

	if (err)
		return err;

	if (!strncmp(cmd_str, "ise_pwr", sizeof("ise_pwr"))) {
		if (param == ISE_AWAKE_LOCK)
			mtk_ise_awake_lock(ISE_PM_INIT);
		else if (param == ISE_AWAKE_UNLOCK)
			mtk_ise_awake_unlock(ISE_PM_INIT);
		else if (param == ISE_LOWPOWER_UT) {
			mtk_ise_awake_lock(ISE_PM_UT);
			/* Do iSE jobs here */
			mtk_ise_awake_unlock(ISE_PM_UT);
		} else
			pr_notice("%s unknown pwr ut\n", __func__);
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct proc_ops ise_lpm_dbg_fops = {
	.proc_write = ise_lpm_dbg,
};

static int ise_lpm_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	u32 wakelock_en = 0;
	int i, ret;

	if (!node) {
		dev_info(&pdev->dev, "of_node required\n");
		return -EINVAL;
	}

	ret = ise_scmi_init(&pdev->dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Tinysys ISE feature unavailable\n");

	if (!of_property_read_u32(node, "ise-wakelock", &wakelock_en))
		pr_notice("ise-wakelock %u\n", wakelock_en);

	ise_lpm_freerun_en = 0;
	if (!of_property_read_u32(pdev->dev.of_node, "ise-lpm-freerun", &ise_lpm_freerun_en))
		pr_notice("ise_lpm_freerun_en %d\n", ise_lpm_freerun_en);

	ise_lpm_wq = create_singlethread_workqueue("ISE_LPM_WQ");
	if (!ise_lpm_wq)
		return -ENOMEM;
	for (i = 0; i < ISE_LPM_WQ_CMD_NUM; i++) {
		ise_lpm_work[i].flags = i;
		INIT_WORK(&ise_lpm_work[i].work, ise_lpm_work_handle);
	}
	timer_setup(&ise_lpm_pd_timer, ise_lpm_pwr_off_cb, 0);
	timer_setup(&ise_lpm_timer, ise_lpm_timeout, 0);
	timer_setup(&ise_lpm_deinit_timer, ise_lpm_deinit_cb, 0);

	if (wakelock_en) {
		ret = ise_req_dram();
		if (ret) {
			destroy_workqueue(ise_lpm_wq);
			return dev_err_probe(&pdev->dev, ret, "ISE DRAM request failed\n");
		}
	}
	mutex_lock(&mutex_ise_lpm);
	ise_boot_cnt = 0;
	ise_awake_cnt = 0;
	ise_req_pending_cnt = 0;
	memset(ise_awake_user_list, 0, sizeof(ise_awake_user_list));
	if (wakelock_en) {
		/*
		 * Since iSE already boot up at BL2 phase, so init following
		 * ise_awake_cnt = 1
		 * ise_awake_user_list[ISE_PM_INIT] = 1
		 */
		ise_awake_cnt = 1;
		ise_awake_user_list[ISE_PM_INIT] = 1;

	}
	ise_wakelock_en = wakelock_en;
	mutex_unlock(&mutex_ise_lpm);
	if (wakelock_en)
		ise_lpm_proc = proc_create("ise_lpm_dbg", 0664, NULL, &ise_lpm_dbg_fops);

	if (ise_lpm_freerun_en) {
		mod_timer(&ise_lpm_timer,
			jiffies + msecs_to_jiffies(ISE_LPM_FREERUN_TIMEOUT_MS));
	}

	if (!ise_lpm_freerun_en && !wakelock_en) {
		pr_notice("ise not enable, start to deinit\n ");
		mod_timer(&ise_lpm_deinit_timer,
			jiffies + msecs_to_jiffies(ISE_LPM_DEINIT_TIMEOUT_MS));
	}
	return 0;
}

static void ise_lpm_remove(struct platform_device *pdev)
{
	mutex_lock(&mutex_ise_lpm);
	ise_wakelock_en = 0;
	mutex_unlock(&mutex_ise_lpm);
	proc_remove(ise_lpm_proc);
	ise_lpm_proc = NULL;
	timer_shutdown_sync(&ise_lpm_timer);
	timer_shutdown_sync(&ise_lpm_pd_timer);
	timer_shutdown_sync(&ise_lpm_deinit_timer);
	destroy_workqueue(ise_lpm_wq);
	_tinfo = NULL;
}

static void ise_lpm_shutdown(struct platform_device *pdev)
{
	/* stop service ise wakelock */
	mutex_lock(&mutex_ise_lpm);
	ise_wakelock_en = 0;
	mutex_unlock(&mutex_ise_lpm);
	pr_notice("%s ise-wakelock %d\n", __func__, ise_wakelock_en);
}

static const struct of_device_id ise_lpm_of_match[] = {
	{ .compatible = "mediatek,ise-lpm", },
	{},
};
MODULE_DEVICE_TABLE(of, ise_lpm_of_match);

static struct platform_driver ise_lpm_driver = {
	.probe = ise_lpm_probe,
	.remove = ise_lpm_remove,
	.shutdown = ise_lpm_shutdown,
	.driver	= {
		.name = "ise-lpm",
		.owner = THIS_MODULE,
		.of_match_table = ise_lpm_of_match,
	},
};

static int __init ise_lpm_driver_init(void)
{
	return platform_driver_register(&ise_lpm_driver);
}

static void __exit ise_lpm_driver_exit(void)
{
	platform_driver_unregister(&ise_lpm_driver);
}
device_initcall_sync(ise_lpm_driver_init);
module_exit(ise_lpm_driver_exit);

MODULE_DESCRIPTION("MEDIATEK Module iSE_lpm driver");
MODULE_AUTHOR("Mediatek");
MODULE_LICENSE("GPL");
