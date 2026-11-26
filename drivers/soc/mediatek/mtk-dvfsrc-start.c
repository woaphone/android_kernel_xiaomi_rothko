// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 MediaTek Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

#define MTK_SIP_DVFSRC_START	0x01

/* Release the firmware's high-OPP initialization lock at late init. */
static int mtk_dvfsrc_start_probe(struct platform_device *pdev)
{
	struct arm_smccc_res ares;

	arm_smccc_smc(MTK_SIP_DVFSRC_VCOREFS_CONTROL, MTK_SIP_DVFSRC_START,
		      0, 0, 0, 0, 0, 0, &ares);
	if (ares.a0)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Cannot start DVFSRC: %lu\n", ares.a0);

	return 0;
}

static struct platform_driver mtk_dvfsrc_start_driver = {
	.probe = mtk_dvfsrc_start_probe,
	.driver = {
		.name = "mtk-dvfsrc-start",
	},
};

static int __init mtk_dvfsrc_start_init(void)
{
	return platform_driver_register(&mtk_dvfsrc_start_driver);
}
late_initcall_sync(mtk_dvfsrc_start_init);

static void __exit mtk_dvfsrc_start_exit(void)
{
	platform_driver_unregister(&mtk_dvfsrc_start_driver);
}
module_exit(mtk_dvfsrc_start_exit);

MODULE_ALIAS("platform:mtk-dvfsrc-start");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek DVFSRC late firmware enable");
