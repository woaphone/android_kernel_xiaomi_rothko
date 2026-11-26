// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek PCIe host controller driver.
 *
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Jianjun Wang <jianjun.wang@mediatek.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <linux/soc/mediatek/mtk-pcie.h>

#include "../pci.h"

#define PCIE_BASE_CFG_REG		0x14
#define PCIE_BASE_CFG_SPEED		GENMASK(15, 8)

#define PCIE_SETTING_REG		0x80
#define PCIE_SETTING_LINK_WIDTH		GENMASK(11, 8)
#define PCIE_SETTING_GEN_SUPPORT	GENMASK(14, 12)
#define PCIE_PCI_IDS_1			0x9c
#define PCI_CLASS(class)		(class << 8)
#define PCIE_RC_MODE			BIT(0)

#define PCIE_EQ_PRESET_01_REG		0x100
#define PCIE_VAL_LN0_DOWNSTREAM		GENMASK(6, 0)
#define PCIE_VAL_LN0_UPSTREAM		GENMASK(14, 8)
#define PCIE_VAL_LN1_DOWNSTREAM		GENMASK(22, 16)
#define PCIE_VAL_LN1_UPSTREAM		GENMASK(30, 24)

#define PCIE_CFGNUM_REG			0x140
#define PCIE_CFG_DEVFN(devfn)		((devfn) & GENMASK(7, 0))
#define PCIE_CFG_BUS(bus)		(((bus) << 8) & GENMASK(15, 8))
#define PCIE_CFG_BYTE_EN(bytes)		(((bytes) << 16) & GENMASK(19, 16))
#define PCIE_CFG_FORCE_BYTE_EN		BIT(20)
#define PCIE_CFG_OFFSET_ADDR		0x1000
#define PCIE_CFG_HEADER(bus, devfn) \
	(PCIE_CFG_BUS(bus) | PCIE_CFG_DEVFN(devfn))

#define PCIE_RST_CTRL_REG		0x148
#define PCIE_MAC_RSTB			BIT(0)
#define PCIE_PHY_RSTB			BIT(1)
#define PCIE_BRG_RSTB			BIT(2)
#define PCIE_PE_RSTB			BIT(3)

#define PCIE_LTSSM_STATUS_REG		0x150
#define PCIE_LTSSM_STATE_MASK		GENMASK(28, 24)
#define PCIE_LTSSM_STATE(val)		((val & PCIE_LTSSM_STATE_MASK) >> 24)
#define PCIE_LTSSM_STATE_L0	0x10
#define PCIE_LTSSM_STATE_L2_IDLE	0x14

#define PCIE_LINK_STATUS_REG		0x154
#define PCIE_PORT_LINKUP		BIT(8)

#define PCIE_MSI_SET_NUM		8
#define PCIE_MSI_IRQS_PER_SET		32
#define PCIE_MSI_IRQS_NUM \
	(PCIE_MSI_IRQS_PER_SET * PCIE_MSI_SET_NUM)

#define PCIE_INT_ENABLE_REG		0x180
#define PCIE_MSI_ENABLE			GENMASK(PCIE_MSI_SET_NUM + 8 - 1, 8)
#define PCIE_MSI_SHIFT			8
#define PCIE_INTX_SHIFT			24
#define PCIE_INTX_ENABLE \
	GENMASK(PCIE_INTX_SHIFT + PCI_NUM_INTX - 1, PCIE_INTX_SHIFT)

#define PCIE_INT_STATUS_REG		0x184
#define PCIE_MSI_SET_ENABLE_REG		0x190
#define PCIE_MSI_SET_ENABLE		GENMASK(PCIE_MSI_SET_NUM - 1, 0)

#define PCIE_PIPE4_PIE8_REG		0x338
#define PCIE_K_FINETUNE_MAX		GENMASK(5, 0)
#define PCIE_K_FINETUNE_ERR		GENMASK(7, 6)
#define PCIE_K_PRESET_TO_USE		GENMASK(18, 8)
#define PCIE_K_PHYPARAM_QUERY		BIT(19)
#define PCIE_K_QUERY_TIMEOUT		BIT(20)
#define PCIE_K_PRESET_TO_USE_16G	GENMASK(31, 21)

#define PCIE_MSI_SET_BASE_REG		0xc00
#define PCIE_MSI_SET_OFFSET		0x10
#define PCIE_MSI_SET_STATUS_OFFSET	0x04
#define PCIE_MSI_SET_ENABLE_OFFSET	0x08

#define PCIE_MSI_SET_ADDR_HI_BASE	0xc80
#define PCIE_MSI_SET_ADDR_HI_OFFSET	0x04

#define PCIE_RESOURCE_CTRL_REG		0xd2c
#define PCIE_RSRC_SYS_CLK_RDY_TIME_MASK	GENMASK(7, 0)

#define PCIE_ICMD_PM_REG		0x198
#define PCIE_TURN_OFF_LINK		BIT(4)

#define PCIE_MISC_CTRL_REG		0x348
#define PCIE_DISABLE_DVFSRC_VLT_REQ	BIT(1)

#define PCIE_TRANS_TABLE_BASE_REG	0x800
#define PCIE_ATR_SRC_ADDR_MSB_OFFSET	0x4
#define PCIE_ATR_TRSL_ADDR_LSB_OFFSET	0x8
#define PCIE_ATR_TRSL_ADDR_MSB_OFFSET	0xc
#define PCIE_ATR_TRSL_PARAM_OFFSET	0x10
#define PCIE_ATR_TLB_SET_OFFSET		0x20

#define PCIE_MAX_TRANS_TABLES		8
#define PCIE_ATR_EN			BIT(0)
#define PCIE_ATR_SIZE(size) \
	(((((size) - 1) << 1) & GENMASK(6, 1)) | PCIE_ATR_EN)
#define PCIE_ATR_ID(id)			((id) & GENMASK(3, 0))
#define PCIE_ATR_TYPE_MEM		PCIE_ATR_ID(0)
#define PCIE_ATR_TYPE_IO		PCIE_ATR_ID(1)
#define PCIE_ATR_TLP_TYPE(type)		(((type) << 16) & GENMASK(18, 16))
#define PCIE_ATR_TLP_TYPE_MEM		PCIE_ATR_TLP_TYPE(0)
#define PCIE_ATR_TLP_TYPE_IO		PCIE_ATR_TLP_TYPE(2)

#define MT6989_PCIE_COMPAT		"mediatek,mt6989-pcie"
#define MT6989_CFG_CTRL			0x84
#define MT6989_DISABLE_LTSSM		BIT(2)
#define MT6989_ASPM_CTRL		0x15c
#define MT6989_P2_SLEEP_TIME		GENMASK(31, 28)
#define MT6989_AXI_IF_CTRL		0x1a8
#define MT6989_AXI0_SLV_RESP_MASK	BIT(12)
#define MT6989_WCPL_TIMEOUT		0x340
#define MT6989_WCPL_TIMEOUT_4MS		4
#define MT6989_APSRC_ACK		BIT(10)
#define MT6989_DVFS_REQ_FORCE_OFF	BIT(12)
#define MT6989_AXI_READ_ERR		GENMASK(18, 16)
#define MT6989_SDES_EVT			BIT(31)
#define MT6989_CONF_DEV2_CTL		0x10a8
#define MT6989_CPL_TIMEOUT		GENMASK(3, 0)
#define MT6989_CPL_TIMEOUT_64US		1
#define MT6989_CPL_TIMEOUT_4MS		2
#define MT6989_AER_UNC_STATUS		0x1204
#define MT6989_AER_MALF_TLP		BIT(18)
#define MT6989_AER_CO_STATUS		0x1210
#define MT6989_AER_RX_ERR		BIT(0)
#define MT6989_DRIVER_OWN_IRQ		BIT(27)
#define MT6989_MSI_GRP1_OFFSET		0x0c
#define MT6989_MSI_GRP2_BASE		0xdc0
#define MT6989_MSI_GRP3_BASE		0xde0
#define MT6989_PEXTP_RST_SET		0x8
#define MT6989_PEXTP_RST_CLR		0xc
#define MT6989_PEXTP_RST_PORT0		GENMASK(1, 0)
#define MT6989_PEXTP_PWRCTL_0		0x40
#define MT6989_HW_MTCMOS_EN		BIT(0)
#define MT6989_VLP_PROTECT_STA		0x240
#define MT6989_PORT0_PROTECT		(BIT(13) | BIT(11))
#define MT6989_M07_CFG			0x3c
#define MT6989_IGNORE_SETTLE_TIME	BIT(2)
#define MT6989_BBCK2_FORCE_ACK		GENMASK(4, 3)
#define MT6989_CENTRAL_CFG6		0x74
#define MT6989_PMRC7_VOTE_MASK		BIT(7)
#define MT6989_PCIE_CLKBUF_XO_ID		1
#define MT6989_PMRC7_BBCK2_UNBIND	0x241
#define MT6989_PMRC7_BBCK2_BIND		0x2c0

#define MAX_NUM_PHY_RESETS		3

#define PCIE_MTK_RESET_TIME_US		10

/* Time in ms needed to complete PCIe reset on EN7581 SoC */
#define PCIE_EN7581_RESET_TIME_MS	100

struct mtk_gen3_pcie;

#define PCIE_CONF_LINK2_CTL_STS		(PCIE_CFG_OFFSET_ADDR + 0xb0)
#define PCIE_CONF_LINK2_LCR2_LINK_SPEED	GENMASK(3, 0)

enum mtk_gen3_pcie_flags {
	MT6989_PORT0 = BIT(1),
	SKIP_PCIE_RSTB	= BIT(0), /* Skip PERST# assertion during device
				   * probing or suspend/resume phase to
				   * avoid hw bugs/issues.
				   */
};

/**
 * struct mtk_gen3_pcie_pdata - differentiate between host generations
 * @power_up: pcie power_up callback
 * @phy_resets: phy reset lines SoC data.
 * @sys_clk_rdy_time_us: System clock ready time override (microseconds)
 * @flags: pcie device flags.
 */
struct mtk_gen3_pcie_pdata {
	int (*power_up)(struct mtk_gen3_pcie *pcie);
	struct {
		const char *id[MAX_NUM_PHY_RESETS];
		int num_resets;
	} phy_resets;
	u8 sys_clk_rdy_time_us;
	u32 flags;
};

/**
 * struct mtk_msi_set - MSI information for each set
 * @base: IO mapped register base
 * @msg_addr: MSI message address
 * @saved_irq_state: IRQ enable state saved at suspend time
 */
struct mtk_msi_set {
	void __iomem *base;
	phys_addr_t msg_addr;
	u32 saved_irq_state;
};

/**
 * struct mtk_gen3_pcie - PCIe port information
 * @dev: pointer to PCIe device
 * @base: IO mapped register base
 * @reg_base: physical register base
 * @mac_reset: MAC reset control
 * @phy_resets: PHY reset controllers
 * @phy: PHY controller block
 * @clks: PCIe clocks
 * @num_clks: PCIe clocks count for this port
 * @max_link_speed: Maximum link speed (PCIe Gen) for this port
 * @num_lanes: Number of PCIe lanes for this port
 * @irq: PCIe controller interrupt number
 * @saved_irq_state: IRQ enable state saved at suspend time
 * @irq_lock: lock protecting IRQ register access
 * @intx_domain: legacy INTx IRQ domain
 * @msi_bottom_domain: MSI IRQ bottom domain
 * @msi_sets: MSI sets information
 * @lock: lock protecting IRQ bit map
 * @msi_irq_in_use: bit map for assigned MSI IRQ
 * @soc: pointer to SoC-dependent operations
 */
struct mtk_gen3_pcie {
	struct device *dev;
	void __iomem *base;
	phys_addr_t reg_base;
	struct reset_control *mac_reset;
	struct reset_control_bulk_data phy_resets[MAX_NUM_PHY_RESETS];
	struct phy *phy;
	struct clk_bulk_data *clks;
	int num_clks;
	u8 max_link_speed;
	u8 num_lanes;

	int irq;
	u32 saved_irq_state;
	raw_spinlock_t irq_lock;
	struct irq_domain *intx_domain;
	struct irq_domain *msi_bottom_domain;
	struct mtk_msi_set msi_sets[PCIE_MSI_SET_NUM];
	struct mutex lock;
	DECLARE_BITMAP(msi_irq_in_use, PCIE_MSI_IRQS_NUM);

	const struct mtk_gen3_pcie_pdata *soc;

	/* MT6989 WLAN port 0 */
	void __iomem *pextp;
	void __iomem *vlp;
	void __iomem *srclken;
	bool rc_vote;
	bool ep_vote;
};

/* Serialize endpoint operations with host teardown and config TLP headers. */
static DEFINE_RAW_SPINLOCK(mt6989_lock);
static struct mtk_gen3_pcie *mt6989_port0;
static struct platform_driver mtk_pcie_driver;

#if IS_REACHABLE(CONFIG_MTK_CLKBUF_CTRL)
extern int clkbuf_xo_ctrl(char *cmd, int xo_id, u32 input);
#endif

static void mt6989_update_bits(void __iomem *reg, u32 mask, u32 value)
{
	u32 val = readl_relaxed(reg);

	writel_relaxed((val & ~mask) | value, reg);
}

static bool mt6989_sleep_protect_ready(struct mtk_gen3_pcie *pcie)
{
	if (readl_relaxed(pcie->vlp + MT6989_VLP_PROTECT_STA) & MT6989_PORT0_PROTECT)
		return false;

	/* The signal can briefly deassert during HW MTCMOS power-on. */
	udelay(6);
	return !(readl_relaxed(pcie->vlp + MT6989_VLP_PROTECT_STA) & MT6989_PORT0_PROTECT);
}

static void mt6989_select_root_config(struct mtk_gen3_pcie *pcie)
{
	writel_relaxed(PCIE_CFG_FORCE_BYTE_EN | PCIE_CFG_BYTE_EN(0xf) |
		       PCIE_CFG_HEADER(0, 0), pcie->base + PCIE_CFGNUM_REG);
}

static void mt6989_set_completion_timeout(struct mtk_gen3_pcie *pcie, u16 timeout)
{
	u16 val;

	mt6989_select_root_config(pcie);
	val = readw_relaxed(pcie->base + MT6989_CONF_DEV2_CTL);
	val = (val & ~MT6989_CPL_TIMEOUT) | timeout;
	writew_relaxed(val, pcie->base + MT6989_CONF_DEV2_CTL);
}

static struct platform_device *mt6989_find_port0(void)
{
	struct platform_device *pdev;
	struct device_node *np;

	for_each_compatible_node(np, NULL, MT6989_PCIE_COMPAT) {
		if (of_device_is_available(np) && of_get_pci_domain_nr(np) == 0) {
			pdev = of_find_device_by_node(np);
			of_node_put(np);
			return pdev;
		}
	}

	return NULL;
}

int mtk_pcie_probe_port(int port)
{
	struct platform_device *pdev;
	int ret;

	if (port != 0)
		return -EOPNOTSUPP;

	pdev = mt6989_find_port0();
	if (!pdev)
		return -ENODEV;

	if (driver_find(mtk_pcie_driver.driver.name, &platform_bus_type) !=
	    &mtk_pcie_driver.driver) {
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}
	ret = device_driver_attach(&mtk_pcie_driver.driver, &pdev->dev);
	if (ret == -EBUSY) {
		device_lock(&pdev->dev);
		if (pdev->dev.driver == &mtk_pcie_driver.driver)
			ret = 0;
		device_unlock(&pdev->dev);
	}
	put_device(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL(mtk_pcie_probe_port);

int mtk_pcie_remove_port(int port)
{
	struct platform_device *pdev;
	bool bound;
	int ret = 0;

	if (port != 0)
		return -EOPNOTSUPP;

	pdev = mt6989_find_port0();
	if (!pdev)
		return -ENODEV;

	device_lock(&pdev->dev);
	bound = pdev->dev.driver == &mtk_pcie_driver.driver;
	if (pdev->dev.driver && !bound)
		ret = -EBUSY;
	device_unlock(&pdev->dev);
	if (bound)
		device_release_driver(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL(mtk_pcie_remove_port);

int mtk_pcie_hw_control_vote(int port, bool hw_mode_en, u8 who)
{
	struct mtk_gen3_pcie *pcie;
	unsigned long flags;
	bool enable, was_enabled;
	u32 val;
	int ret = 0;

	if (port != 0 || who > 1)
		return -EINVAL;

	raw_spin_lock_irqsave(&mt6989_lock, flags);
	pcie = mt6989_port0;
	if (!pcie) {
		ret = -ENODEV;
		goto out;
	}

	was_enabled = pcie->rc_vote && pcie->ep_vote;
	if (who)
		pcie->ep_vote = hw_mode_en;
	else
		pcie->rc_vote = hw_mode_en;

	enable = pcie->rc_vote && pcie->ep_vote;
	mt6989_update_bits(pcie->pextp + MT6989_PEXTP_PWRCTL_0,
			   MT6989_HW_MTCMOS_EN, enable ? MT6989_HW_MTCMOS_EN : 0);
	if (!enable && was_enabled) {
		ret = readl_poll_timeout_atomic(pcie->vlp + MT6989_VLP_PROTECT_STA, val,
					       !(val & MT6989_PORT0_PROTECT), 10, 10000);
		if (!ret && !mt6989_sleep_protect_ready(pcie))
			ret = -ETIMEDOUT;
	}
out:
	raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	return ret;
}
EXPORT_SYMBOL(mtk_pcie_hw_control_vote);

/*
 * MT6639 status ABI: LTSSM [4:0], DL_UP [5], AXI [6], RxErr [7],
 * MalfTLP [8], driver-own MSI [9], SDES [10].
 */
u32 mtk_pcie_dump_link_info(int port)
{
	struct mtk_gen3_pcie *pcie;
	unsigned long flags;
	u32 val, state = 0;

	if (port != 0)
		return 0;

	raw_spin_lock_irqsave(&mt6989_lock, flags);
	pcie = mt6989_port0;
	if (!pcie || !mt6989_sleep_protect_ready(pcie))
		goto out;

	state = PCIE_LTSSM_STATE(readl_relaxed(pcie->base + PCIE_LTSSM_STATUS_REG));
	if (readl_relaxed(pcie->base + PCIE_LINK_STATUS_REG) & PCIE_PORT_LINKUP)
		state |= BIT(5);

	val = readl_relaxed(pcie->base + PCIE_INT_STATUS_REG);
	if (val & MT6989_AXI_READ_ERR)
		state |= BIT(6);
	if (val & MT6989_SDES_EVT)
		state |= BIT(10);

	mt6989_select_root_config(pcie);
	if (readl_relaxed(pcie->base + MT6989_AER_CO_STATUS) & MT6989_AER_RX_ERR)
		state |= BIT(7);
	if (readl_relaxed(pcie->base + MT6989_AER_UNC_STATUS) & MT6989_AER_MALF_TLP)
		state |= BIT(8);
	if (readl_relaxed(pcie->msi_sets[1].base + PCIE_MSI_SET_STATUS_OFFSET) &
	    MT6989_DRIVER_OWN_IRQ)
		state |= BIT(9);
out:
	raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	return state;
}
EXPORT_SYMBOL(mtk_pcie_dump_link_info);

static bool mt6989_link_ready(void)
{
	struct mtk_gen3_pcie *pcie;
	unsigned long flags;
	bool ready = false;
	u32 ltssm;

	raw_spin_lock_irqsave(&mt6989_lock, flags);
	pcie = mt6989_port0;
	if (pcie && mt6989_sleep_protect_ready(pcie) &&
	    (readl_relaxed(pcie->base + PCIE_LINK_STATUS_REG) & PCIE_PORT_LINKUP)) {
		ltssm = PCIE_LTSSM_STATE(readl_relaxed(pcie->base + PCIE_LTSSM_STATUS_REG));
		/* L0/L0s/L1 can carry traffic or wake on a transaction. */
		ready = ltssm >= PCIE_LTSSM_STATE_L0 &&
			ltssm < PCIE_LTSSM_STATE_L2_IDLE;
	}
	raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	return ready;
}

int mtk_pcie_wait_link_ready(int port)
{
	bool ready;

	if (port != 0)
		return -EOPNOTSUPP;

	return read_poll_timeout_atomic(mt6989_link_ready, ready, ready, 20, 10000,
					false);
}
EXPORT_SYMBOL(mtk_pcie_wait_link_ready);

int mtk_pcie_disable_data_trans(int port)
{
	struct mtk_gen3_pcie *pcie;
	unsigned long flags;
	int ret = 0;

	if (port != 0)
		return -EOPNOTSUPP;

	raw_spin_lock_irqsave(&mt6989_lock, flags);
	pcie = mt6989_port0;
	if (!pcie) {
		ret = -ENODEV;
		goto out;
	}
	if (!mt6989_sleep_protect_ready(pcie)) {
		ret = -EPERM;
		goto out;
	}

	mt6989_update_bits(pcie->base + PCIE_RST_CTRL_REG,
			   PCIE_MAC_RSTB | PCIE_PHY_RSTB, PCIE_MAC_RSTB | PCIE_PHY_RSTB);
	mt6989_update_bits(pcie->base + MT6989_CFG_CTRL,
			   MT6989_DISABLE_LTSSM, MT6989_DISABLE_LTSSM);
	mt6989_update_bits(pcie->base + PCIE_RST_CTRL_REG, PCIE_MAC_RSTB, 0);
	/* Bound outstanding reads after MAC reset to avoid a bus timeout. */
	mt6989_set_completion_timeout(pcie, MT6989_CPL_TIMEOUT_64US);
out:
	raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	return ret;
}
EXPORT_SYMBOL(mtk_pcie_disable_data_trans);

/* LTSSM state in PCIE_LTSSM_STATUS_REG bit[28:24] */
static const char *const ltssm_str[] = {
	"detect.quiet",			/* 0x00 */
	"detect.active",		/* 0x01 */
	"polling.active",		/* 0x02 */
	"polling.compliance",		/* 0x03 */
	"polling.configuration",	/* 0x04 */
	"config.linkwidthstart",	/* 0x05 */
	"config.linkwidthaccept",	/* 0x06 */
	"config.lanenumwait",		/* 0x07 */
	"config.lanenumaccept",		/* 0x08 */
	"config.complete",		/* 0x09 */
	"config.idle",			/* 0x0A */
	"recovery.receiverlock",	/* 0x0B */
	"recovery.equalization",	/* 0x0C */
	"recovery.speed",		/* 0x0D */
	"recovery.receiverconfig",	/* 0x0E */
	"recovery.idle",		/* 0x0F */
	"L0",				/* 0x10 */
	"L0s",				/* 0x11 */
	"L1.entry",			/* 0x12 */
	"L1.idle",			/* 0x13 */
	"L2.idle",			/* 0x14 */
	"L2.transmitwake",		/* 0x15 */
	"disable",			/* 0x16 */
	"loopback.entry",		/* 0x17 */
	"loopback.active",		/* 0x18 */
	"loopback.exit",		/* 0x19 */
	"hotreset",			/* 0x1A */
};

/**
 * mtk_pcie_config_tlp_header() - Configure a configuration TLP header
 * @bus: PCI bus to query
 * @devfn: device/function number
 * @where: offset in config space
 * @size: data size in TLP header
 *
 * Set byte enable field and device information in configuration TLP header.
 */
static void mtk_pcie_config_tlp_header(struct pci_bus *bus, unsigned int devfn,
					int where, int size)
{
	struct mtk_gen3_pcie *pcie = bus->sysdata;
	int bytes;
	u32 val;

	bytes = (GENMASK(size - 1, 0) & 0xf) << (where & 0x3);

	val = PCIE_CFG_FORCE_BYTE_EN | PCIE_CFG_BYTE_EN(bytes) |
	      PCIE_CFG_HEADER(bus->number, devfn);

	writel_relaxed(val, pcie->base + PCIE_CFGNUM_REG);
}

static void __iomem *mtk_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				      int where)
{
	struct mtk_gen3_pcie *pcie = bus->sysdata;

	return pcie->base + PCIE_CFG_OFFSET_ADDR + where;
}

static int mtk_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 *val)
{
	mtk_pcie_config_tlp_header(bus, devfn, where, size);

	return pci_generic_config_read32(bus, devfn, where, size, val);
}

static int mtk_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 val)
{
	mtk_pcie_config_tlp_header(bus, devfn, where, size);

	if (size <= 2)
		val <<= (where & 0x3) * 8;

	return pci_generic_config_write32(bus, devfn, where, 4, val);
}

static int mt6989_config_read(struct pci_bus *bus, unsigned int devfn,
			      int where, int size, u32 *val)
{
	struct mtk_gen3_pcie *pcie = bus->sysdata;
	unsigned long flags;
	int ret = PCIBIOS_DEVICE_NOT_FOUND;

	raw_spin_lock_irqsave(&mt6989_lock, flags);
	if (mt6989_sleep_protect_ready(pcie))
		ret = mtk_pcie_config_read(bus, devfn, where, size, val);
	else
		*val = ~0U;
	raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	return ret;
}

static int mt6989_config_write(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 val)
{
	struct mtk_gen3_pcie *pcie = bus->sysdata;
	unsigned long flags;
	int ret = PCIBIOS_DEVICE_NOT_FOUND;

	raw_spin_lock_irqsave(&mt6989_lock, flags);
	if (mt6989_sleep_protect_ready(pcie))
		ret = mtk_pcie_config_write(bus, devfn, where, size, val);
	raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	return ret;
}

static struct pci_ops mt6989_pcie_ops = {
	.map_bus = mtk_pcie_map_bus,
	.read = mt6989_config_read,
	.write = mt6989_config_write,
};

static struct pci_ops mtk_pcie_ops = {
	.map_bus = mtk_pcie_map_bus,
	.read  = mtk_pcie_config_read,
	.write = mtk_pcie_config_write,
};

static int mtk_pcie_set_trans_table(struct mtk_gen3_pcie *pcie,
				    resource_size_t cpu_addr,
				    resource_size_t pci_addr,
				    resource_size_t size,
				    unsigned long type, int *num)
{
	resource_size_t remaining = size;
	resource_size_t table_size;
	resource_size_t addr_align;
	const char *range_type;
	void __iomem *table;
	u32 val;

	while (remaining && (*num < PCIE_MAX_TRANS_TABLES)) {
		/* Table size needs to be a power of 2 */
		table_size = BIT(fls(remaining) - 1);

		if (cpu_addr > 0) {
			addr_align = BIT(ffs(cpu_addr) - 1);
			table_size = min(table_size, addr_align);
		}

		/* Minimum size of translate table is 4KiB */
		if (table_size < 0x1000) {
			dev_err(pcie->dev, "illegal table size %#llx\n",
				(unsigned long long)table_size);
			return -EINVAL;
		}

		table = pcie->base + PCIE_TRANS_TABLE_BASE_REG + *num * PCIE_ATR_TLB_SET_OFFSET;
		writel_relaxed(lower_32_bits(cpu_addr) | PCIE_ATR_SIZE(fls(table_size) - 1), table);
		writel_relaxed(upper_32_bits(cpu_addr), table + PCIE_ATR_SRC_ADDR_MSB_OFFSET);
		writel_relaxed(lower_32_bits(pci_addr), table + PCIE_ATR_TRSL_ADDR_LSB_OFFSET);
		writel_relaxed(upper_32_bits(pci_addr), table + PCIE_ATR_TRSL_ADDR_MSB_OFFSET);

		if (type == IORESOURCE_IO) {
			val = PCIE_ATR_TYPE_IO | PCIE_ATR_TLP_TYPE_IO;
			range_type = "IO";
		} else {
			val = PCIE_ATR_TYPE_MEM | PCIE_ATR_TLP_TYPE_MEM;
			range_type = "MEM";
		}

		writel_relaxed(val, table + PCIE_ATR_TRSL_PARAM_OFFSET);

		dev_dbg(pcie->dev, "set %s trans window[%d]: cpu_addr = %#llx, pci_addr = %#llx, size = %#llx\n",
			range_type, *num, (unsigned long long)cpu_addr,
			(unsigned long long)pci_addr,
			(unsigned long long)table_size);

		cpu_addr += table_size;
		pci_addr += table_size;
		remaining -= table_size;
		(*num)++;
	}

	if (remaining)
		dev_warn(pcie->dev, "not enough translate table for addr: %#llx, limited to [%d]\n",
			 (unsigned long long)cpu_addr, PCIE_MAX_TRANS_TABLES);

	return 0;
}

static void mtk_pcie_enable_msi(struct mtk_gen3_pcie *pcie)
{
	int i;
	u32 val;

	for (i = 0; i < PCIE_MSI_SET_NUM; i++) {
		struct mtk_msi_set *msi_set = &pcie->msi_sets[i];

		msi_set->base = pcie->base + PCIE_MSI_SET_BASE_REG +
				i * PCIE_MSI_SET_OFFSET;
		msi_set->msg_addr = pcie->reg_base + PCIE_MSI_SET_BASE_REG +
				    i * PCIE_MSI_SET_OFFSET;

		/* Configure the MSI capture address */
		writel_relaxed(lower_32_bits(msi_set->msg_addr), msi_set->base);
		writel_relaxed(upper_32_bits(msi_set->msg_addr),
			       pcie->base + PCIE_MSI_SET_ADDR_HI_BASE +
			       i * PCIE_MSI_SET_ADDR_HI_OFFSET);
	}

	val = readl_relaxed(pcie->base + PCIE_MSI_SET_ENABLE_REG);
	val |= PCIE_MSI_SET_ENABLE;
	writel_relaxed(val, pcie->base + PCIE_MSI_SET_ENABLE_REG);

	val = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);
	val |= PCIE_MSI_ENABLE;
	writel_relaxed(val, pcie->base + PCIE_INT_ENABLE_REG);
}

static int mtk_pcie_startup_port(struct mtk_gen3_pcie *pcie)
{
	struct resource_entry *entry;
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
	unsigned int table_index = 0;
	int err;
	u32 val;

	/* Set as RC mode and set controller PCIe Gen speed restriction, if any */
	val = readl_relaxed(pcie->base + PCIE_SETTING_REG);
	val |= PCIE_RC_MODE;
	if (pcie->max_link_speed) {
		val &= ~PCIE_SETTING_GEN_SUPPORT;

		/* Can enable link speed support only from Gen2 onwards */
		if (pcie->max_link_speed >= 2)
			val |= FIELD_PREP(PCIE_SETTING_GEN_SUPPORT,
					  GENMASK(pcie->max_link_speed - 2, 0));
	}
	if (pcie->num_lanes) {
		val &= ~PCIE_SETTING_LINK_WIDTH;

		/* Zero means one lane, each bit activates x2/x4/x8/x16 */
		if (pcie->num_lanes > 1)
			val |= FIELD_PREP(PCIE_SETTING_LINK_WIDTH,
					  GENMASK(fls(pcie->num_lanes >> 2), 0));
	}
	writel_relaxed(val, pcie->base + PCIE_SETTING_REG);

	/* Set Link Control 2 (LNKCTL2) speed restriction, if any */
	if (pcie->max_link_speed) {
		val = readl_relaxed(pcie->base + PCIE_CONF_LINK2_CTL_STS);
		val &= ~PCIE_CONF_LINK2_LCR2_LINK_SPEED;
		val |= FIELD_PREP(PCIE_CONF_LINK2_LCR2_LINK_SPEED, pcie->max_link_speed);
		writel_relaxed(val, pcie->base + PCIE_CONF_LINK2_CTL_STS);
	}

	/* If parameter is present, adjust SYS_CLK_RDY_TIME to avoid glitching */
	if (pcie->soc->sys_clk_rdy_time_us) {
		val = readl_relaxed(pcie->base + PCIE_RESOURCE_CTRL_REG);
		FIELD_MODIFY(PCIE_RSRC_SYS_CLK_RDY_TIME_MASK, &val,
			     pcie->soc->sys_clk_rdy_time_us);
		writel_relaxed(val, pcie->base + PCIE_RESOURCE_CTRL_REG);
	}

	/* Set class code */
	val = readl_relaxed(pcie->base + PCIE_PCI_IDS_1);
	val &= ~GENMASK(31, 8);
	val |= PCI_CLASS(PCI_CLASS_BRIDGE_PCI_NORMAL);
	writel_relaxed(val, pcie->base + PCIE_PCI_IDS_1);

	/* Mask all INTx interrupts */
	val = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);
	val &= ~PCIE_INTX_ENABLE;
	writel_relaxed(val, pcie->base + PCIE_INT_ENABLE_REG);

	/* Disable DVFSRC voltage request */
	val = readl_relaxed(pcie->base + PCIE_MISC_CTRL_REG);
	if (pcie->soc->flags & MT6989_PORT0) {
		/* Bit 1 forces the DVFS request on for MT6989. */
		val &= ~BIT(1);
		val |= MT6989_DVFS_REQ_FORCE_OFF;
	} else {
		val |= PCIE_DISABLE_DVFSRC_VLT_REQ;
	}
	writel_relaxed(val, pcie->base + PCIE_MISC_CTRL_REG);

	if (pcie->soc->flags & MT6989_PORT0) {
		mt6989_update_bits(pcie->base + PCIE_RESOURCE_CTRL_REG,
				   MT6989_APSRC_ACK, MT6989_APSRC_ACK);
		mt6989_update_bits(pcie->base + MT6989_AXI_IF_CTRL,
				   MT6989_AXI0_SLV_RESP_MASK, MT6989_AXI0_SLV_RESP_MASK);
		writel_relaxed(MT6989_WCPL_TIMEOUT_4MS, pcie->base + MT6989_WCPL_TIMEOUT);
		mt6989_update_bits(pcie->base + MT6989_ASPM_CTRL, MT6989_P2_SLEEP_TIME,
				   FIELD_PREP(MT6989_P2_SLEEP_TIME, 4));
	}

	/*
	 * Airoha EN7581 has a hw bug asserting/releasing PCIE_PE_RSTB signal
	 * causing occasional PCIe link down. In order to overcome the issue,
	 * PCIE_RSTB signals are not asserted/released at this stage and the
	 * PCIe block is reset using en7523_reset_assert() and
	 * en7581_pci_enable().
	 */
	if (!(pcie->soc->flags & SKIP_PCIE_RSTB)) {
		/* Assert all reset signals */
		val = readl_relaxed(pcie->base + PCIE_RST_CTRL_REG);
		val |= PCIE_MAC_RSTB | PCIE_PHY_RSTB | PCIE_BRG_RSTB |
		       PCIE_PE_RSTB;
		writel_relaxed(val, pcie->base + PCIE_RST_CTRL_REG);

		/*
		 * Described in PCIe CEM specification revision 6.0.
		 *
		 * The deassertion of PERST# should be delayed 100ms (TPVPERL)
		 * for the power and clock to become stable.
		 */
		if (pcie->soc->flags & MT6989_PORT0) {
			msleep(50);
			val &= ~(PCIE_MAC_RSTB | PCIE_PHY_RSTB | PCIE_BRG_RSTB);
			writel_relaxed(val, pcie->base + PCIE_RST_CTRL_REG);
			msleep(50);
		} else {
			msleep(PCIE_T_PVPERL_MS);
		}

		/* De-assert reset signals */
		val &= ~(PCIE_MAC_RSTB | PCIE_PHY_RSTB | PCIE_BRG_RSTB |
			 PCIE_PE_RSTB);
		writel_relaxed(val, pcie->base + PCIE_RST_CTRL_REG);
	}

	/* Check if the link is up or not */
	err = readl_poll_timeout(pcie->base + PCIE_LINK_STATUS_REG, val,
				 !!(val & PCIE_PORT_LINKUP), 20,
				 PCI_PM_D3COLD_WAIT * USEC_PER_MSEC);
	if (err) {
		const char *ltssm_state;
		int ltssm_index;

		val = readl_relaxed(pcie->base + PCIE_LTSSM_STATUS_REG);
		ltssm_index = PCIE_LTSSM_STATE(val);
		ltssm_state = ltssm_index >= ARRAY_SIZE(ltssm_str) ?
			      "Unknown state" : ltssm_str[ltssm_index];
		dev_err(pcie->dev,
			"PCIe link down, current LTSSM state: %s (%#x)\n",
			ltssm_state, val);
		return err;
	}

	mtk_pcie_enable_msi(pcie);
	if (pcie->soc->flags & MT6989_PORT0)
		mt6989_set_completion_timeout(pcie, MT6989_CPL_TIMEOUT_4MS);

	/* Set PCIe translation windows */
	resource_list_for_each_entry(entry, &host->windows) {
		struct resource *res = entry->res;
		unsigned long type = resource_type(res);
		resource_size_t cpu_addr;
		resource_size_t pci_addr;
		resource_size_t size;

		if (type == IORESOURCE_IO)
			cpu_addr = pci_pio_to_address(res->start);
		else if (type == IORESOURCE_MEM)
			cpu_addr = res->start;
		else
			continue;

		pci_addr = res->start - entry->offset;
		size = resource_size(res);
		err = mtk_pcie_set_trans_table(pcie, cpu_addr, pci_addr, size,
					       type, &table_index);
		if (err)
			return err;
	}

	return 0;
}

#define MTK_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				MSI_FLAG_USE_DEF_CHIP_OPS	| \
				MSI_FLAG_NO_AFFINITY		| \
				MSI_FLAG_PCI_MSI_MASK_PARENT)

#define MTK_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK		| \
				 MSI_FLAG_PCI_MSIX		| \
				 MSI_FLAG_MULTI_PCI_MSI)

static const struct msi_parent_ops mtk_msi_parent_ops = {
	.required_flags		= MTK_MSI_FLAGS_REQUIRED,
	.supported_flags	= MTK_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.prefix			= "MTK3-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

static void mtk_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct mtk_msi_set *msi_set = irq_data_get_irq_chip_data(data);
	struct mtk_gen3_pcie *pcie = data->domain->host_data;
	unsigned long hwirq;

	hwirq =	data->hwirq % PCIE_MSI_IRQS_PER_SET;

	msg->address_hi = upper_32_bits(msi_set->msg_addr);
	msg->address_lo = lower_32_bits(msi_set->msg_addr);
	msg->data = hwirq;
	dev_dbg(pcie->dev, "msi#%#lx address_hi %#x address_lo %#x data %d\n",
		hwirq, msg->address_hi, msg->address_lo, msg->data);
}

static void mtk_msi_bottom_irq_ack(struct irq_data *data)
{
	struct mtk_msi_set *msi_set = irq_data_get_irq_chip_data(data);
	unsigned long hwirq;

	hwirq =	data->hwirq % PCIE_MSI_IRQS_PER_SET;

	writel_relaxed(BIT(hwirq), msi_set->base + PCIE_MSI_SET_STATUS_OFFSET);
}

static void mtk_msi_bottom_irq_mask(struct irq_data *data)
{
	struct mtk_msi_set *msi_set = irq_data_get_irq_chip_data(data);
	struct mtk_gen3_pcie *pcie = data->domain->host_data;
	unsigned long hwirq, flags;
	u32 val;

	hwirq =	data->hwirq % PCIE_MSI_IRQS_PER_SET;

	raw_spin_lock_irqsave(&pcie->irq_lock, flags);
	val = readl_relaxed(msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	val &= ~BIT(hwirq);
	writel_relaxed(val, msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);
}

static void mtk_msi_bottom_irq_unmask(struct irq_data *data)
{
	struct mtk_msi_set *msi_set = irq_data_get_irq_chip_data(data);
	struct mtk_gen3_pcie *pcie = data->domain->host_data;
	unsigned long hwirq, flags;
	u32 val;

	hwirq =	data->hwirq % PCIE_MSI_IRQS_PER_SET;

	raw_spin_lock_irqsave(&pcie->irq_lock, flags);
	val = readl_relaxed(msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	val |= BIT(hwirq);
	writel_relaxed(val, msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);
}

static struct irq_chip mtk_msi_bottom_irq_chip = {
	.irq_ack		= mtk_msi_bottom_irq_ack,
	.irq_mask		= mtk_msi_bottom_irq_mask,
	.irq_unmask		= mtk_msi_bottom_irq_unmask,
	.irq_compose_msi_msg	= mtk_compose_msi_msg,
	.name			= "MSI",
};

static void __iomem *mt6989_msi_group_reg(struct mtk_gen3_pcie *pcie,
					 unsigned int set, u32 group)
{
	if (group == 1)
		return pcie->msi_sets[set].base + MT6989_MSI_GRP1_OFFSET;
	if (group == 2)
		return pcie->base + MT6989_MSI_GRP2_BASE + set * sizeof(u32);
	return pcie->base + MT6989_MSI_GRP3_BASE + set * sizeof(u32);
}

int mtk_msi_unmask_to_other_mcu(struct irq_data *data, u32 group)
{
	struct mtk_gen3_pcie *pcie;
	unsigned long flags;
	unsigned int set, bit;
	void __iomem *reg;
	int ret = 0;

	if (group < 1 || group > 3)
		return -EINVAL;
	for (; data; data = data->parent_data)
		if (irq_data_get_irq_chip(data) == &mtk_msi_bottom_irq_chip)
			break;
	if (!data)
		return -EINVAL;

	raw_spin_lock_irqsave(&mt6989_lock, flags);
	pcie = mt6989_port0;
	if (!pcie || data->domain != pcie->msi_bottom_domain) {
		ret = -ENODEV;
		goto out;
	}
	if (!mt6989_sleep_protect_ready(pcie)) {
		ret = -EPERM;
		goto out;
	}

	set = data->hwirq / PCIE_MSI_IRQS_PER_SET;
	bit = data->hwirq % PCIE_MSI_IRQS_PER_SET;
	reg = mt6989_msi_group_reg(pcie, set, group);

	mt6989_update_bits(reg, BIT(bit), BIT(bit));
out:
	raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	return ret;
}
EXPORT_SYMBOL(mtk_msi_unmask_to_other_mcu);

static int mtk_msi_bottom_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *arg)
{
	struct mtk_gen3_pcie *pcie = domain->host_data;
	struct mtk_msi_set *msi_set;
	int i, hwirq, set_idx;

	mutex_lock(&pcie->lock);

	hwirq = bitmap_find_free_region(pcie->msi_irq_in_use, PCIE_MSI_IRQS_NUM,
					order_base_2(nr_irqs));

	mutex_unlock(&pcie->lock);

	if (hwirq < 0)
		return -ENOSPC;

	set_idx = hwirq / PCIE_MSI_IRQS_PER_SET;
	msi_set = &pcie->msi_sets[set_idx];

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &mtk_msi_bottom_irq_chip, msi_set,
				    handle_edge_irq, NULL, NULL);

	return 0;
}

static void mtk_msi_bottom_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	struct mtk_gen3_pcie *pcie = domain->host_data;
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);
	unsigned long flags, hwirq;
	unsigned int i, group, set;
	u32 mask;

	mutex_lock(&pcie->lock);

	/* The same vector can be reallocated to a different endpoint MSI. */
	if (pcie->soc->flags & MT6989_PORT0) {
		raw_spin_lock_irqsave(&mt6989_lock, flags);
		if (mt6989_sleep_protect_ready(pcie)) {
			for (i = 0; i < nr_irqs; i++) {
				hwirq = data->hwirq + i;
				set = hwirq / PCIE_MSI_IRQS_PER_SET;
				mask = BIT(hwirq % PCIE_MSI_IRQS_PER_SET);
				for (group = 1; group <= 3; group++)
					mt6989_update_bits(mt6989_msi_group_reg(pcie, set, group),
							   mask, 0);
			}
		}
		raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	}

	bitmap_release_region(pcie->msi_irq_in_use, data->hwirq,
			      order_base_2(nr_irqs));

	mutex_unlock(&pcie->lock);

	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

static const struct irq_domain_ops mtk_msi_bottom_domain_ops = {
	.alloc = mtk_msi_bottom_domain_alloc,
	.free = mtk_msi_bottom_domain_free,
};

static void mtk_intx_mask(struct irq_data *data)
{
	struct mtk_gen3_pcie *pcie = irq_data_get_irq_chip_data(data);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&pcie->irq_lock, flags);
	val = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);
	val &= ~BIT(data->hwirq + PCIE_INTX_SHIFT);
	writel_relaxed(val, pcie->base + PCIE_INT_ENABLE_REG);
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);
}

static void mtk_intx_unmask(struct irq_data *data)
{
	struct mtk_gen3_pcie *pcie = irq_data_get_irq_chip_data(data);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&pcie->irq_lock, flags);
	val = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);
	val |= BIT(data->hwirq + PCIE_INTX_SHIFT);
	writel_relaxed(val, pcie->base + PCIE_INT_ENABLE_REG);
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);
}

/**
 * mtk_intx_eoi() - Clear INTx IRQ status at the end of interrupt
 * @data: pointer to chip specific data
 *
 * As an emulated level IRQ, its interrupt status will remain
 * until the corresponding de-assert message is received; hence that
 * the status can only be cleared when the interrupt has been serviced.
 */
static void mtk_intx_eoi(struct irq_data *data)
{
	struct mtk_gen3_pcie *pcie = irq_data_get_irq_chip_data(data);
	unsigned long hwirq;

	hwirq = data->hwirq + PCIE_INTX_SHIFT;
	writel_relaxed(BIT(hwirq), pcie->base + PCIE_INT_STATUS_REG);
}

static struct irq_chip mtk_intx_irq_chip = {
	.irq_mask		= mtk_intx_mask,
	.irq_unmask		= mtk_intx_unmask,
	.irq_eoi		= mtk_intx_eoi,
	.name			= "INTx",
};

static int mtk_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, domain->host_data);
	irq_set_chip_and_handler_name(irq, &mtk_intx_irq_chip,
				      handle_fasteoi_irq, "INTx");
	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = mtk_pcie_intx_map,
};

static int mtk_pcie_init_irq_domains(struct mtk_gen3_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *intc_node, *node = dev->of_node;
	int ret;

	raw_spin_lock_init(&pcie->irq_lock);

	/* Setup INTx */
	intc_node = of_get_child_by_name(node, "interrupt-controller");
	if (!intc_node) {
		dev_err(dev, "missing interrupt-controller node\n");
		return -ENODEV;
	}

	pcie->intx_domain = irq_domain_create_linear(of_fwnode_handle(intc_node), PCI_NUM_INTX,
						     &intx_domain_ops, pcie);
	if (!pcie->intx_domain) {
		dev_err(dev, "failed to create INTx IRQ domain\n");
		ret = -ENODEV;
		goto out_put_node;
	}

	/* Setup MSI */
	mutex_init(&pcie->lock);

	struct irq_domain_info info = {
		.fwnode		= dev_fwnode(dev),
		.ops		= &mtk_msi_bottom_domain_ops,
		.host_data	= pcie,
		.size		= PCIE_MSI_IRQS_NUM,
	};

	pcie->msi_bottom_domain = msi_create_parent_irq_domain(&info, &mtk_msi_parent_ops);
	if (!pcie->msi_bottom_domain) {
		dev_err(dev, "failed to create MSI bottom domain\n");
		ret = -ENODEV;
		goto err_msi_bottom_domain;
	}

	of_node_put(intc_node);
	return 0;

err_msi_bottom_domain:
	irq_domain_remove(pcie->intx_domain);
out_put_node:
	of_node_put(intc_node);
	return ret;
}

static void mtk_pcie_irq_teardown(struct mtk_gen3_pcie *pcie)
{
	irq_set_chained_handler_and_data(pcie->irq, NULL, NULL);

	if (pcie->intx_domain)
		irq_domain_remove(pcie->intx_domain);

	if (pcie->msi_bottom_domain)
		irq_domain_remove(pcie->msi_bottom_domain);

	irq_dispose_mapping(pcie->irq);
}

static void mtk_pcie_msi_handler(struct mtk_gen3_pcie *pcie, int set_idx)
{
	struct mtk_msi_set *msi_set = &pcie->msi_sets[set_idx];
	unsigned long msi_enable, msi_status;
	irq_hw_number_t bit, hwirq;

	msi_enable = readl_relaxed(msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);

	do {
		msi_status = readl_relaxed(msi_set->base +
					   PCIE_MSI_SET_STATUS_OFFSET);
		msi_status &= msi_enable;
		if (!msi_status)
			break;

		for_each_set_bit(bit, &msi_status, PCIE_MSI_IRQS_PER_SET) {
			hwirq = bit + set_idx * PCIE_MSI_IRQS_PER_SET;
			generic_handle_domain_irq(pcie->msi_bottom_domain, hwirq);
		}
	} while (true);
}

static void mtk_pcie_irq_handler(struct irq_desc *desc)
{
	struct mtk_gen3_pcie *pcie = irq_desc_get_handler_data(desc);
	struct irq_chip *irqchip = irq_desc_get_chip(desc);
	unsigned long status;
	irq_hw_number_t irq_bit = PCIE_INTX_SHIFT;

	chained_irq_enter(irqchip, desc);

	status = readl_relaxed(pcie->base + PCIE_INT_STATUS_REG);
	for_each_set_bit_from(irq_bit, &status, PCI_NUM_INTX +
			      PCIE_INTX_SHIFT)
		generic_handle_domain_irq(pcie->intx_domain,
					  irq_bit - PCIE_INTX_SHIFT);

	irq_bit = PCIE_MSI_SHIFT;
	for_each_set_bit_from(irq_bit, &status, PCIE_MSI_SET_NUM +
			      PCIE_MSI_SHIFT) {
		mtk_pcie_msi_handler(pcie, irq_bit - PCIE_MSI_SHIFT);

		writel_relaxed(BIT(irq_bit), pcie->base + PCIE_INT_STATUS_REG);
	}

	chained_irq_exit(irqchip, desc);
}

static int mtk_pcie_setup_irq(struct mtk_gen3_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	int err;

	pcie->irq = platform_get_irq(pdev, 0);
	if (pcie->irq < 0)
		return pcie->irq;

	err = mtk_pcie_init_irq_domains(pcie);
	if (err)
		return err;

	irq_set_chained_handler_and_data(pcie->irq, mtk_pcie_irq_handler, pcie);

	return 0;
}

static void mt6989_unmap(void *addr)
{
	iounmap((void __force __iomem *)addr);
}

static void __iomem *mt6989_map_node(struct device *dev, struct device_node *np)
{
	void __iomem *addr;
	int ret;

	if (!np)
		return IOMEM_ERR_PTR(-ENODEV);

	addr = of_iomap(np, 0);
	of_node_put(np);
	if (!addr)
		return IOMEM_ERR_PTR(-ENOMEM);

	ret = devm_add_action_or_reset(dev, mt6989_unmap, (void __force *)addr);
	return ret ? IOMEM_ERR_PTR(ret) : addr;
}

static int mt6989_parse_port(struct mtk_gen3_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *np = dev->of_node;
	struct device_node *clkbuf;

	/* Only the WLAN port with the stock reset and suspend modes is supported. */
	if (of_get_pci_domain_nr(np) != 0 || !pcie->phy || !pcie->num_clks ||
	    !of_property_read_bool(np, "mediatek,peri-reset-dis") ||
	    !of_property_read_bool(np, "mediatek,dvfs-req-dis") ||
	    !of_property_read_bool(np, "mediatek,suspend-mode-l12"))
		return -EOPNOTSUPP;

	pcie->pextp = mt6989_map_node(dev, of_parse_phandle(np, "pextpcfg", 0));
	if (IS_ERR(pcie->pextp))
		return PTR_ERR(pcie->pextp);

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6989-vlpcfg_bus");
	pcie->vlp = mt6989_map_node(dev, np);
	if (IS_ERR(pcie->vlp))
		return PTR_ERR(pcie->vlp);

	clkbuf = of_find_compatible_node(NULL, NULL, "mediatek,mt6989-clkbuf");
	if (!clkbuf)
		return -ENODEV;

	np = of_parse_phandle(clkbuf, "srclken-rc", 0);
	of_node_put(clkbuf);
	pcie->srclken = mt6989_map_node(dev, np);
	return PTR_ERR_OR_ZERO(pcie->srclken);
}

static int mtk_pcie_parse_port(struct mtk_gen3_pcie *pcie)
{
	int i, ret, num_resets = pcie->soc->phy_resets.num_resets;
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *regs;
	u32 num_lanes;

	regs = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pcie-mac");
	if (!regs)
		return -EINVAL;
	pcie->base = devm_ioremap_resource(dev, regs);
	if (IS_ERR(pcie->base)) {
		dev_err(dev, "failed to map register base\n");
		return PTR_ERR(pcie->base);
	}

	pcie->reg_base = regs->start;

	for (i = 0; i < num_resets; i++)
		pcie->phy_resets[i].id = pcie->soc->phy_resets.id[i];

	ret = devm_reset_control_bulk_get_optional_shared(dev, num_resets,
							  pcie->phy_resets);
	if (ret) {
		dev_err(dev, "failed to get PHY bulk reset\n");
		return ret;
	}

	pcie->mac_reset = devm_reset_control_get_optional_exclusive(dev, "mac");
	if (IS_ERR(pcie->mac_reset)) {
		ret = PTR_ERR(pcie->mac_reset);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get MAC reset\n");

		return ret;
	}

	pcie->phy = devm_phy_optional_get(dev, "pcie-phy");
	if (IS_ERR(pcie->phy)) {
		ret = PTR_ERR(pcie->phy);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get PHY\n");

		return ret;
	}

	pcie->num_clks = devm_clk_bulk_get_all(dev, &pcie->clks);
	if (pcie->num_clks < 0) {
		dev_err(dev, "failed to get clocks\n");
		return pcie->num_clks;
	}

	ret = of_property_read_u32(dev->of_node, "num-lanes", &num_lanes);
	if (ret == 0) {
		if (num_lanes == 0 || num_lanes > 16 ||
		    (num_lanes != 1 && num_lanes % 2))
			dev_warn(dev, "invalid num-lanes, using controller defaults\n");
		else
			pcie->num_lanes = num_lanes;
	}

	if (pcie->soc->flags & MT6989_PORT0)
		return mt6989_parse_port(pcie);

	return 0;
}

static int mtk_pcie_en7581_power_up(struct mtk_gen3_pcie *pcie)
{
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
	struct device *dev = pcie->dev;
	struct resource_entry *entry;
	struct regmap *pbus_regmap;
	u32 val, args[2], size;
	resource_size_t addr;
	int err;

	/*
	 * The controller may have been left out of reset by the bootloader
	 * so make sure that we get a clean start by asserting resets here.
	 */
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);

	/* Wait for the time needed to complete the reset lines assert. */
	msleep(PCIE_EN7581_RESET_TIME_MS);

	/*
	 * Configure PBus base address and base address mask to allow the
	 * hw to detect if a given address is accessible on PCIe controller.
	 */
	pbus_regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
							   "mediatek,pbus-csr",
							   ARRAY_SIZE(args),
							   args);
	if (IS_ERR(pbus_regmap))
		return PTR_ERR(pbus_regmap);

	entry = resource_list_first_type(&host->windows, IORESOURCE_MEM);
	if (!entry)
		return -ENODEV;

	addr = entry->res->start - entry->offset;
	regmap_write(pbus_regmap, args[0], lower_32_bits(addr));
	size = lower_32_bits(resource_size(entry->res));
	regmap_write(pbus_regmap, args[1], GENMASK(31, __fls(size)));

	/*
	 * Unlike the other MediaTek Gen3 controllers, the Airoha EN7581
	 * requires PHY initialization and power-on before PHY reset deassert.
	 */
	err = phy_init(pcie->phy);
	if (err) {
		dev_err(dev, "failed to initialize PHY\n");
		return err;
	}

	err = phy_power_on(pcie->phy);
	if (err) {
		dev_err(dev, "failed to power on PHY\n");
		goto err_phy_on;
	}

	err = reset_control_bulk_deassert(pcie->soc->phy_resets.num_resets,
					  pcie->phy_resets);
	if (err) {
		dev_err(dev, "failed to deassert PHYs\n");
		goto err_phy_deassert;
	}

	/*
	 * Wait for the time needed to complete the bulk de-assert above.
	 * This time is specific for EN7581 SoC.
	 */
	msleep(PCIE_EN7581_RESET_TIME_MS);

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	val = FIELD_PREP(PCIE_VAL_LN0_DOWNSTREAM, 0x47) |
	      FIELD_PREP(PCIE_VAL_LN1_DOWNSTREAM, 0x47) |
	      FIELD_PREP(PCIE_VAL_LN0_UPSTREAM, 0x41) |
	      FIELD_PREP(PCIE_VAL_LN1_UPSTREAM, 0x41);
	writel_relaxed(val, pcie->base + PCIE_EQ_PRESET_01_REG);

	val = PCIE_K_PHYPARAM_QUERY | PCIE_K_QUERY_TIMEOUT |
	      FIELD_PREP(PCIE_K_PRESET_TO_USE_16G, 0x80) |
	      FIELD_PREP(PCIE_K_PRESET_TO_USE, 0x2) |
	      FIELD_PREP(PCIE_K_FINETUNE_MAX, 0xf);
	writel_relaxed(val, pcie->base + PCIE_PIPE4_PIE8_REG);

	err = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	if (err) {
		dev_err(dev, "failed to prepare clock\n");
		goto err_clk_prepare_enable;
	}

	/*
	 * Airoha EN7581 performs PCIe reset via clk callbacks since it has a
	 * hw issue with PCIE_PE_RSTB signal. Add wait for the time needed to
	 * complete the PCIe reset.
	 */
	msleep(PCIE_T_PVPERL_MS);

	return 0;

err_clk_prepare_enable:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);
err_phy_deassert:
	phy_power_off(pcie->phy);
err_phy_on:
	phy_exit(pcie->phy);

	return err;
}

static int mtk_pcie_power_up(struct mtk_gen3_pcie *pcie)
{
	struct device *dev = pcie->dev;
	int err;

	/*
	 * The controller may have been left out of reset by the bootloader
	 * so make sure that we get a clean start by asserting resets here.
	 */
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);
	reset_control_assert(pcie->mac_reset);
	usleep_range(PCIE_MTK_RESET_TIME_US, 2 * PCIE_MTK_RESET_TIME_US);

	/* PHY power on and enable pipe clock */
	err = reset_control_bulk_deassert(pcie->soc->phy_resets.num_resets,
					  pcie->phy_resets);
	if (err) {
		dev_err(dev, "failed to deassert PHYs\n");
		return err;
	}

	err = phy_init(pcie->phy);
	if (err) {
		dev_err(dev, "failed to initialize PHY\n");
		goto err_phy_init;
	}

	err = phy_power_on(pcie->phy);
	if (err) {
		dev_err(dev, "failed to power on PHY\n");
		goto err_phy_on;
	}

	/* MAC power on and enable transaction layer clocks */
	reset_control_deassert(pcie->mac_reset);

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	err = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	if (err) {
		dev_err(dev, "failed to enable clocks\n");
		goto err_clk_init;
	}

	return 0;

err_clk_init:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	reset_control_assert(pcie->mac_reset);
	phy_power_off(pcie->phy);
err_phy_on:
	phy_exit(pcie->phy);
err_phy_init:
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);

	return err;
}

static int mt6989_clkbuf_vote(u32 value)
{
#if IS_REACHABLE(CONFIG_MTK_CLKBUF_CTRL)
	return clkbuf_xo_ctrl("SET_XO_VOTER", MT6989_PCIE_CLKBUF_XO_ID, value);
#else
	return -EOPNOTSUPP;
#endif
}

static void mt6989_release_clock_request(struct mtk_gen3_pcie *pcie)
{
	mt6989_update_bits(pcie->srclken + MT6989_M07_CFG,
			   MT6989_BBCK2_FORCE_ACK | MT6989_IGNORE_SETTLE_TIME, 0);
	mt6989_update_bits(pcie->srclken + MT6989_CENTRAL_CFG6,
			   MT6989_PMRC7_VOTE_MASK, 0);
}

static void mt6989_power_down_tail(struct mtk_gen3_pcie *pcie)
{
	int ret;

	writel_relaxed(MT6989_PEXTP_RST_PORT0, pcie->pextp + MT6989_PEXTP_RST_SET);
	ret = mt6989_clkbuf_vote(MT6989_PMRC7_BBCK2_BIND);
	if (ret)
		dev_err(pcie->dev, "failed to restore PMRC7 BBCK2 vote: %d\n", ret);
	mt6989_release_clock_request(pcie);
}

static int mt6989_power_up(struct mtk_gen3_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct pinctrl *pinctrl;
	u32 val, central_cfg6, m07_cfg;
	int ret;

	if (!IS_REACHABLE(CONFIG_MTK_CLKBUF_CTRL))
		return -EOPNOTSUPP;

	central_cfg6 = readl_relaxed(pcie->srclken + MT6989_CENTRAL_CFG6);
	m07_cfg = readl_relaxed(pcie->srclken + MT6989_M07_CFG);
	mt6989_update_bits(pcie->srclken + MT6989_CENTRAL_CFG6,
			   MT6989_PMRC7_VOTE_MASK, MT6989_PMRC7_VOTE_MASK);
	mt6989_update_bits(pcie->srclken + MT6989_M07_CFG,
			   MT6989_BBCK2_FORCE_ACK | MT6989_IGNORE_SETTLE_TIME,
			   MT6989_BBCK2_FORCE_ACK | MT6989_IGNORE_SETTLE_TIME);
	ret = mt6989_clkbuf_vote(MT6989_PMRC7_BBCK2_UNBIND);
	if (ret) {
		mt6989_update_bits(pcie->srclken + MT6989_M07_CFG,
				   MT6989_BBCK2_FORCE_ACK | MT6989_IGNORE_SETTLE_TIME,
				   m07_cfg & (MT6989_BBCK2_FORCE_ACK | MT6989_IGNORE_SETTLE_TIME));
		mt6989_update_bits(pcie->srclken + MT6989_CENTRAL_CFG6,
				   MT6989_PMRC7_VOTE_MASK,
				   central_cfg6 & MT6989_PMRC7_VOTE_MASK);
		return dev_err_probe(dev, ret == -ENODEV ? -EPROBE_DEFER : ret,
				     "failed to acquire BBCK2 clock vote\n");
	}
	udelay(400);

	writel_relaxed(MT6989_PEXTP_RST_PORT0, pcie->pextp + MT6989_PEXTP_RST_CLR);
	ret = phy_init(pcie->phy);
	if (ret)
		goto err_reset;
	ret = phy_power_on(pcie->phy);
	if (ret)
		goto err_phy_exit;

	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto err_pm_disable;
	ret = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	if (ret)
		goto err_pm_put;

	ret = readl_poll_timeout(pcie->vlp + MT6989_VLP_PROTECT_STA, val,
				!(val & MT6989_PORT0_PROTECT), 10, 10000);
	if (ret)
		goto err_clk_disable;

	pinctrl = pinctrl_get_select(dev, "work");
	if (IS_ERR(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		goto err_clk_disable;
	}
	pinctrl_put(pinctrl);

	mt6989_update_bits(pcie->pextp + MT6989_PEXTP_PWRCTL_0, MT6989_HW_MTCMOS_EN, 0);
	return 0;

err_clk_disable:
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
err_pm_put:
	pm_runtime_put_sync(dev);
err_pm_disable:
	pm_runtime_disable(dev);
	phy_power_off(pcie->phy);
err_phy_exit:
	phy_exit(pcie->phy);
err_reset:
	mt6989_power_down_tail(pcie);
	return ret;
}

static void mtk_pcie_power_down(struct mtk_gen3_pcie *pcie)
{
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);

	pm_runtime_put_sync(pcie->dev);
	pm_runtime_disable(pcie->dev);
	reset_control_assert(pcie->mac_reset);

	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);

	if (pcie->soc->flags & MT6989_PORT0)
		mt6989_power_down_tail(pcie);
}

static int mtk_pcie_get_controller_max_link_speed(struct mtk_gen3_pcie *pcie)
{
	u32 val;
	int ret;

	val = readl_relaxed(pcie->base + PCIE_BASE_CFG_REG);
	val = FIELD_GET(PCIE_BASE_CFG_SPEED, val);
	ret = fls(val);

	return ret > 0 ? ret : -EINVAL;
}

static int mtk_pcie_setup(struct mtk_gen3_pcie *pcie)
{
	int err, max_speed;

	err = mtk_pcie_parse_port(pcie);
	if (err)
		return err;

	/*
	 * Deassert the line in order to avoid unbalance in deassert_count
	 * counter since the bulk is shared.
	 */
	reset_control_bulk_deassert(pcie->soc->phy_resets.num_resets,
				    pcie->phy_resets);

	/* Don't touch the hardware registers before power up */
	err = pcie->soc->power_up(pcie);
	if (err)
		return err;

	err = of_pci_get_max_link_speed(pcie->dev->of_node);
	if (err) {
		/* Get the maximum speed supported by the controller */
		max_speed = mtk_pcie_get_controller_max_link_speed(pcie);

		/* Set max_link_speed only if the controller supports it */
		if (max_speed >= 0 && ((pcie->soc->flags & MT6989_PORT0) ?
				      err > 0 && err <= max_speed : max_speed <= err)) {
			pcie->max_link_speed = err;
			dev_info(pcie->dev,
				 "maximum controller link speed Gen%d, overriding to Gen%u",
				 max_speed, pcie->max_link_speed);
		}
	}

	/* Try link up */
	err = mtk_pcie_startup_port(pcie);
	if (err)
		goto err_setup;

	err = mtk_pcie_setup_irq(pcie);
	if (err)
		goto err_setup;

	return 0;

err_setup:
	mtk_pcie_power_down(pcie);

	return err;
}

static int mtk_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_gen3_pcie *pcie;
	struct pci_host_bridge *host;
	unsigned long flags;
	int err;

	host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!host)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(host);

	pcie->dev = dev;
	pcie->soc = device_get_match_data(dev);
	platform_set_drvdata(pdev, pcie);

	err = mtk_pcie_setup(pcie);
	if (err)
		goto probe_failed;

	host->ops = pcie->soc->flags & MT6989_PORT0 ? &mt6989_pcie_ops : &mtk_pcie_ops;
	host->sysdata = pcie;

	if (pcie->soc->flags & MT6989_PORT0) {
		raw_spin_lock_irqsave(&mt6989_lock, flags);
		mt6989_port0 = pcie;
		raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	}

	err = pci_host_probe(host);
	if (!err)
		return 0;

	if (pcie->soc->flags & MT6989_PORT0) {
		raw_spin_lock_irqsave(&mt6989_lock, flags);
		mt6989_port0 = NULL;
		raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	}
	mtk_pcie_irq_teardown(pcie);
	mtk_pcie_power_down(pcie);

probe_failed:
	if (pcie->soc->flags & MT6989_PORT0)
		pinctrl_pm_select_sleep_state(dev);
	return err;
}

static void mtk_pcie_remove(struct platform_device *pdev)
{
	struct mtk_gen3_pcie *pcie = platform_get_drvdata(pdev);
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
	unsigned long flags;

	pci_lock_rescan_remove();
	pci_stop_root_bus(host->bus);
	pci_remove_root_bus(host->bus);
	pci_unlock_rescan_remove();

	if (pcie->soc->flags & MT6989_PORT0) {
		raw_spin_lock_irqsave(&mt6989_lock, flags);
		mt6989_port0 = NULL;
		raw_spin_unlock_irqrestore(&mt6989_lock, flags);
	}
	mtk_pcie_irq_teardown(pcie);
	mtk_pcie_power_down(pcie);
	if (pcie->soc->flags & MT6989_PORT0)
		pinctrl_pm_select_sleep_state(pcie->dev);
}

static void mtk_pcie_irq_save(struct mtk_gen3_pcie *pcie)
{
	int i;

	raw_spin_lock(&pcie->irq_lock);

	pcie->saved_irq_state = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);

	for (i = 0; i < PCIE_MSI_SET_NUM; i++) {
		struct mtk_msi_set *msi_set = &pcie->msi_sets[i];

		msi_set->saved_irq_state = readl_relaxed(msi_set->base +
					   PCIE_MSI_SET_ENABLE_OFFSET);
	}

	raw_spin_unlock(&pcie->irq_lock);
}

static void mtk_pcie_irq_restore(struct mtk_gen3_pcie *pcie)
{
	int i;

	raw_spin_lock(&pcie->irq_lock);

	writel_relaxed(pcie->saved_irq_state, pcie->base + PCIE_INT_ENABLE_REG);

	for (i = 0; i < PCIE_MSI_SET_NUM; i++) {
		struct mtk_msi_set *msi_set = &pcie->msi_sets[i];

		writel_relaxed(msi_set->saved_irq_state,
			       msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	}

	raw_spin_unlock(&pcie->irq_lock);
}

static int mtk_pcie_turn_off_link(struct mtk_gen3_pcie *pcie)
{
	u32 val;

	val = readl_relaxed(pcie->base + PCIE_ICMD_PM_REG);
	val |= PCIE_TURN_OFF_LINK;
	writel_relaxed(val, pcie->base + PCIE_ICMD_PM_REG);

	/* Check the link is L2 */
	return readl_poll_timeout(pcie->base + PCIE_LTSSM_STATUS_REG, val,
				  (PCIE_LTSSM_STATE(val) ==
				   PCIE_LTSSM_STATE_L2_IDLE), 20,
				   50 * USEC_PER_MSEC);
}

static int mtk_pcie_suspend_noirq(struct device *dev)
{
	struct mtk_gen3_pcie *pcie = dev_get_drvdata(dev);
	int err;
	u32 val;

	if (pcie->soc->flags & MT6989_PORT0)
		return mtk_pcie_hw_control_vote(0, true, 0);

	/* Trigger link to L2 state */
	err = mtk_pcie_turn_off_link(pcie);
	if (err) {
		dev_err(pcie->dev, "cannot enter L2 state\n");
		return err;
	}

	if (!(pcie->soc->flags & SKIP_PCIE_RSTB)) {
		/* Assert the PERST# pin */
		val = readl_relaxed(pcie->base + PCIE_RST_CTRL_REG);
		val |= PCIE_PE_RSTB;
		writel_relaxed(val, pcie->base + PCIE_RST_CTRL_REG);
	}

	dev_dbg(pcie->dev, "entered L2 states successfully");

	mtk_pcie_irq_save(pcie);
	mtk_pcie_power_down(pcie);

	return 0;
}

static int mtk_pcie_resume_noirq(struct device *dev)
{
	struct mtk_gen3_pcie *pcie = dev_get_drvdata(dev);
	int err;

	if (pcie->soc->flags & MT6989_PORT0)
		return mtk_pcie_hw_control_vote(0, false, 0);

	err = pcie->soc->power_up(pcie);
	if (err)
		return err;

	err = mtk_pcie_startup_port(pcie);
	if (err) {
		mtk_pcie_power_down(pcie);
		return err;
	}

	mtk_pcie_irq_restore(pcie);

	return 0;
}

static const struct dev_pm_ops mtk_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(mtk_pcie_suspend_noirq,
				  mtk_pcie_resume_noirq)
};

static const struct mtk_gen3_pcie_pdata mtk_pcie_soc_mt8192 = {
	.power_up = mtk_pcie_power_up,
	.phy_resets = {
		.id[0] = "phy",
		.num_resets = 1,
	},
};

static const struct mtk_gen3_pcie_pdata mtk_pcie_soc_mt8196 = {
	.power_up = mtk_pcie_power_up,
	.phy_resets = {
		.id[0] = "phy",
		.num_resets = 1,
	},
	.sys_clk_rdy_time_us = 10,
};

static const struct mtk_gen3_pcie_pdata mtk_pcie_soc_en7581 = {
	.power_up = mtk_pcie_en7581_power_up,
	.phy_resets = {
		.id[0] = "phy-lane0",
		.id[1] = "phy-lane1",
		.id[2] = "phy-lane2",
		.num_resets = 3,
	},
	.flags = SKIP_PCIE_RSTB,
};

static const struct mtk_gen3_pcie_pdata mtk_pcie_soc_mt6989 = {
	.power_up = mt6989_power_up,
	.flags = MT6989_PORT0,
};

static const struct of_device_id mtk_pcie_of_match[] = {
	{ .compatible = MT6989_PCIE_COMPAT, .data = &mtk_pcie_soc_mt6989 },
	{ .compatible = "airoha,en7581-pcie", .data = &mtk_pcie_soc_en7581 },
	{ .compatible = "mediatek,mt8192-pcie", .data = &mtk_pcie_soc_mt8192 },
	{ .compatible = "mediatek,mt8196-pcie", .data = &mtk_pcie_soc_mt8196 },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_pcie_of_match);

static struct platform_driver mtk_pcie_driver = {
	.probe = mtk_pcie_probe,
	.remove = mtk_pcie_remove,
	.driver = {
		.name = "mtk-pcie-gen3",
		.of_match_table = mtk_pcie_of_match,
		.pm = &mtk_pcie_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_platform_driver(mtk_pcie_driver);
MODULE_DESCRIPTION("MediaTek Gen3 PCIe host controller driver");
MODULE_LICENSE("GPL v2");
