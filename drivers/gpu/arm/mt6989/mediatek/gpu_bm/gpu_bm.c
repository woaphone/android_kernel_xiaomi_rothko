// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/scmi_protocol.h>
#include <linux/seq_file.h>
#include <linux/soc/mediatek/tinysys-scmi.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <mt-plat/mtk_gpu_utility.h>
#include <mtk_gpufreq.h>
#include "gpu_bm.h"

static_assert(sizeof(struct v1_data) == 5 * sizeof(u32));

static struct v1_data __iomem *gpu_info_buf;
static DEFINE_SPINLOCK(g_GPU_BM_lock);
static int g_mode_sport_flag;
static unsigned int g_mode;
static unsigned int g_value;

static int _mgq_proc_show(struct seq_file *m, void *v)
{
	struct v1_data info;
	unsigned int mode, value;
	unsigned long flags;

	spin_lock_irqsave(&g_GPU_BM_lock, flags);
	if (!gpu_info_buf) {
		spin_unlock_irqrestore(&g_GPU_BM_lock, flags);
		return -ENODEV;
	}

	info.version = readl(&gpu_info_buf->version);
	info.ctx = readl(&gpu_info_buf->ctx);
	info.frame = readl(&gpu_info_buf->frame);
	info.job = readl(&gpu_info_buf->job);
	info.freq = readl(&gpu_info_buf->freq);
	mode = g_mode;
	value = g_value;
	spin_unlock_irqrestore(&g_GPU_BM_lock, flags);

	seq_printf(m, "version: %d\n", info.version);
	if (info.version == 1) {
		seq_printf(m, "ctx: \t%d\n", info.ctx);
		seq_printf(m, "frame: \t%d\n", info.frame);
		seq_printf(m, "job: \t%d\n", info.job);
		seq_printf(m, "freq: \t%d\n", info.freq);
		seq_printf(m, "mode: \t%d\n", mode);
		seq_printf(m, "value: \t%d\n", value);
	} else {
		seq_printf(m, "unknown version: 0x%x\n", info.version);
	}

	return 0;
}

static int _mgq_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, _mgq_proc_show, NULL);
}

static ssize_t
_mgq_proc_write(struct file *file, const char __user *buffer,
					size_t count, loff_t *f_pos)
{
	int mode = -1;
	char buf[32];
	unsigned int len = 0;
	unsigned long flags;

	len = (count < (sizeof(buf) - 1)) ? count : (sizeof(buf) - 1);
	if (copy_from_user(buf, buffer, len))
		return 0;
	buf[len] = '\0';

	/* 0         : default bw prediction.
	 * 1         : sport mode specialized
	 * 2         : no bw prediction
	 * 10-300    : apply a ratio for bw predict output
	 * 2010-2300 : apply a ratio for no predict output
	 */

	spin_lock_irqsave(&g_GPU_BM_lock, flags);
	if (!gpu_info_buf) {
		spin_unlock_irqrestore(&g_GPU_BM_lock, flags);
		return -ENODEV;
	}

	if (!kstrtoint(buf, 10, &mode)) {
		pr_info("@%s: mode: %d\n", __func__, mode);

		if (mode == GPU_BW_SPORT_MODE) {
			g_mode_sport_flag = GPU_BW_SPORT_MODE;
			g_value = 0;
		} else if (mode == GPU_BW_DEFAULT_MODE) {
			g_mode = GPU_BW_DEFAULT_MODE;
			g_value = 0;
		} else if (mode >= GPU_BW_RATIO_FLOOR && mode <= GPU_BW_RATIO_CEIL) {
			g_mode = GPU_BW_DEFAULT_MODE;
			g_value = mode;
		} else if (mode == GPU_BW_NO_PRED_MODE)
			g_mode = GPU_BW_NO_PRED_MODE;
		else if (mode >= GPU_BW_NO_PRED_RATIO_FLOOR && mode <= GPU_BW_NO_PRED_RATIO_CEIL) {
			g_mode = GPU_BW_NO_PRED_MODE;
			g_value = mode;
		} else
			pr_info("@%s: wrong input: %d\n", __func__, mode);
	}

	if (mode != 1)
		g_mode_sport_flag = 0;

	writel(g_value ? g_value : g_mode, &gpu_info_buf->freq);

	spin_unlock_irqrestore(&g_GPU_BM_lock, flags);

	return count;
}

static const struct proc_ops _mgq_proc_fops __maybe_unused = {
	.proc_open = _mgq_proc_open,
	.proc_read = seq_read,
	.proc_write = _mgq_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static void gpu_qos_update_mode(int seg_flag)
{
	unsigned int loading = 0;
	int idx, opp_num, min_idx, high_idx, low_idx;

#if IS_ENABLED(CONFIG_MTK_GPUFREQ_V2)
	idx = gpufreq_get_cur_oppidx(TARGET_DEFAULT);
	opp_num = gpufreq_get_opp_num(TARGET_DEFAULT);
#else
	idx = mt_gpufreq_get_cur_freq_index();
	opp_num = mt_gpufreq_get_dvfs_table_num();
#endif
	if (idx < 0 || idx >= opp_num)
		return;

	min_idx = opp_num - 1;
	high_idx = min_idx / 4 + 1;
	low_idx = min_idx / 3 * 2 + 1;

	/* sport mode */
	if (g_mode_sport_flag) {
		/* if gpu freq at top quartile, boost dram freq. */
		if (idx <= high_idx) {
			if (g_mode == GPU_BW_DEFAULT_MODE)
				writel(GPU_BW_RATIO_CEIL, &gpu_info_buf->freq);
			else if (g_mode == GPU_BW_NO_PRED_MODE)
				writel(GPU_BW_RATIO_CEIL + 2000, &gpu_info_buf->freq);
		} else {
			if (g_mode == GPU_BW_DEFAULT_MODE)
				writel(0, &gpu_info_buf->freq);
			else if (g_mode == GPU_BW_NO_PRED_MODE)
				writel(2000, &gpu_info_buf->freq);
		}
		return;
	}

	/* default prediction  */
	if (g_mode == GPU_BW_DEFAULT_MODE) {
		/*
		 * if gpu loading < 40% and gpu freq is lowest,
		 * don't do GPU QoS prediction.
		 */
		if (idx == min_idx && !mtk_get_gpu_loading(&loading))
			return;

		if (idx == min_idx && loading < 40)
			writel(GPU_BW_NO_PRED_MODE, &gpu_info_buf->freq);
		else {
			if (seg_flag && idx >= low_idx)
				writel(GPU_BW_LP_MODE, &gpu_info_buf->freq);
			else if (idx <= GPU_BM_PEAK_INDEX_TOP_LIMIT)
				writel(GPU_BM_PEAK_PERF_MODE_LIMIT, &gpu_info_buf->freq);
			else if (idx <= high_idx)
				writel(GPU_BM_PEAK_PERF_MODE, &gpu_info_buf->freq);
			else
				writel(0, &gpu_info_buf->freq);

			if (g_value >= GPU_BW_RATIO_FLOOR && g_value <= GPU_BW_RATIO_CEIL)
				/* apply a ratio for bw prediction */
				writel(g_value, &gpu_info_buf->freq);
		}
		return;
	}

	/* no bw prediction */
	if (g_mode == GPU_BW_NO_PRED_MODE) {
		writel(GPU_BW_NO_PRED_MODE, &gpu_info_buf->freq);
		if (g_value >= GPU_BW_NO_PRED_RATIO_FLOOR && g_value <= GPU_BW_NO_PRED_RATIO_CEIL)
			/* apply a ratio for bw prediction */
			writel(g_value, &gpu_info_buf->freq);
		return;
	}
}

void MTKGPUQoS_mode(int seg_flag)
{
	unsigned long flags;

	spin_lock_irqsave(&g_GPU_BM_lock, flags);
	if (gpu_info_buf)
		gpu_qos_update_mode(seg_flag);
	spin_unlock_irqrestore(&g_GPU_BM_lock, flags);
}
EXPORT_SYMBOL(MTKGPUQoS_mode);

void MTKGPUQoS_update_info(u32 pid, u32 frame, u32 job, int seg_flag)
{
	unsigned long flags;

	spin_lock_irqsave(&g_GPU_BM_lock, flags);
	if (gpu_info_buf) {
		writel(pid, &gpu_info_buf->ctx);
		writel(frame, &gpu_info_buf->frame);
		writel(job, &gpu_info_buf->job);
		gpu_qos_update_mode(seg_flag);
	}
	spin_unlock_irqrestore(&g_GPU_BM_lock, flags);
}
EXPORT_SYMBOL(MTKGPUQoS_update_info);

int MTKGPUQoS_is_inited(void)
{
	return READ_ONCE(gpu_info_buf) != NULL;
}
EXPORT_SYMBOL(MTKGPUQoS_is_inited);

#if IS_REACHABLE(CONFIG_MTK_TINYSYS_SCMI)
/* Commands within the Tinysys scmi-qos feature, as used by MT6989. */
#define QOS_IPI_QOS_ENABLE		0
#define QOS_IPI_SETUP_GPU_INFO		14

static struct proc_dir_entry *mgq_dir;
static u32 saved_ctx;

static void bw_v1_gpu_power_change_notify(int power_on)
{
	unsigned long flags;

	spin_lock_irqsave(&g_GPU_BM_lock, flags);
	if (gpu_info_buf) {
		if (power_on) {
			writel(saved_ctx, &gpu_info_buf->ctx);
			gpu_qos_update_mode(0);
		} else {
			saved_ctx = readl(&gpu_info_buf->ctx);
			writel(0, &gpu_info_buf->ctx);
		}
	}
	spin_unlock_irqrestore(&g_GPU_BM_lock, flags);
}

static int mtk_gpu_qos_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct scmi_tinysys_info_st *info;
	struct v1_data __iomem *buf;
	struct resource *res;
	unsigned long flags;
	u32 sysram, feature_id, mode = 0, value = 0, freq = 0;
	int ret;

	if (!of_machine_is_compatible("mediatek,MT6989"))
		return -ENODEV;

	ret = of_property_read_u32(dev->of_node, "qos-sysram-support", &sysram);
	if (ret || !sysram)
		return dev_err_probe(dev, -EOPNOTSUPP, "SYSRAM QoS is required\n");

	info = get_scmi_tinysys_info();
	if (!info)
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for Tinysys SCMI\n");

	ret = of_property_read_u32(info->sdev->dev.of_node, "scmi-qos", &feature_id);
	if (ret)
		return dev_err_probe(dev, ret, "missing Tinysys scmi-qos feature ID\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < sizeof(*buf) || !IS_ALIGNED(res->start, sizeof(u32)))
		return dev_err_probe(dev, -EINVAL, "invalid GPU QoS SYSRAM resource\n");

	/* The wider gpu-fdvfs resource includes these five SYSRAM words. */
	buf = devm_ioremap(dev, res->start, sizeof(*buf));
	if (!buf)
		return dev_err_probe(dev, -ENOMEM, "cannot map GPU QoS SYSRAM\n");

	of_property_read_u32(dev->of_node, "qos-mode", &mode);
	of_property_read_u32(dev->of_node, "qos-value", &value);
	if (mode == GPU_BW_DEFAULT_MODE || mode == GPU_BW_NO_PRED_MODE) {
		freq = mode;
		if (value >= GPU_BW_RATIO_FLOOR && value <= GPU_BW_RATIO_CEIL)
			freq = value + (mode == GPU_BW_NO_PRED_MODE ? 2000 : 0);
	}

	if (!mtk_register_gpu_power_change("qpu_qos", bw_v1_gpu_power_change_notify))
		return dev_err_probe(dev, -ENOMEM, "cannot register GPU power callback\n");

	ret = scmi_tinysys_common_set(info->ph, feature_id, QOS_IPI_QOS_ENABLE,
				     0, 0, 0, 0);
	if (ret) {
		dev_err_probe(dev, ret, "Tinysys GPU QoS enable failed\n");
		goto err_unregister;
	}

	writel(0, &buf->ctx);
	writel(0, &buf->frame);
	writel(0, &buf->job);
	writel(freq, &buf->freq);
	writel(1, &buf->version);
	if (readl(&buf->version) != 1) {
		ret = dev_err_probe(dev, -EIO, "GPU QoS SYSRAM version readback failed\n");
		goto err_unregister;
	}

	/* The firmware size field is the number of u32 counters. */
	ret = scmi_tinysys_common_set(info->ph, feature_id, QOS_IPI_SETUP_GPU_INFO,
				     lower_32_bits(res->start), upper_32_bits(res->start),
				     sizeof(*buf) / sizeof(u32), 0);
	if (ret) {
		dev_err_probe(dev, ret, "Tinysys GPU QoS SYSRAM setup failed\n");
		goto err_unregister;
	}

	spin_lock_irqsave(&g_GPU_BM_lock, flags);
	g_mode = mode;
	g_value = value;
	g_mode_sport_flag = 0;
	saved_ctx = 0;
	WRITE_ONCE(gpu_info_buf, buf);
	spin_unlock_irqrestore(&g_GPU_BM_lock, flags);

	mgq_dir = proc_mkdir("mgq", NULL);
	if (mgq_dir && !proc_create("job_status", 0664, mgq_dir, &_mgq_proc_fops))
		dev_dbg(dev, "cannot create /proc/mgq/job_status\n");

	dev_info(dev, "Tinysys GPU QoS SYSRAM setup acknowledged\n");
	return 0;

err_unregister:
	mtk_unregister_gpu_power_change("qpu_qos");
	return ret;
}

static void mtk_gpu_qos_remove(struct platform_device *pdev)
{
	unsigned long flags;

	spin_lock_irqsave(&g_GPU_BM_lock, flags);
	writel(0, &gpu_info_buf->ctx);
	WRITE_ONCE(gpu_info_buf, NULL);
	spin_unlock_irqrestore(&g_GPU_BM_lock, flags);

	mtk_unregister_gpu_power_change("qpu_qos");
	proc_remove(mgq_dir);
	mgq_dir = NULL;
}

static const struct of_device_id mtk_gpu_qos_of_match[] = {
	{ .compatible = "mediatek,gpu_qos" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_gpu_qos_of_match);

static struct platform_driver mtk_gpu_qos_driver = {
	.probe = mtk_gpu_qos_probe,
	.remove = mtk_gpu_qos_remove,
	.driver = {
		.name = "mtk-gpu-qos",
		.of_match_table = mtk_gpu_qos_of_match,
	},
};
module_platform_driver(mtk_gpu_qos_driver);
#endif /* CONFIG_MTK_TINYSYS_SCMI */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek GPU QOS");
MODULE_AUTHOR("MediaTek Inc.");
