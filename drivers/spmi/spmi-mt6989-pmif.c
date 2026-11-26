// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2023 MediaTek Inc.

#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sched/clock.h>
#include <linux/sched/mm.h>
#include <linux/spmi.h>
#include <linux/irq.h>
#include "spmi-mt6989.h"
#define DUMP_LIMIT 0x1000

#define SWINF_IDLE	0x00
#define SWINF_WFVLDCLR	0x06

#define GET_SWINF(x)	(((x) >> 1) & 0x7)

#define PMIF_CMD_REG_0		0
#define PMIF_CMD_REG		1
#define PMIF_CMD_EXT_REG	2
#define PMIF_CMD_EXT_REG_LONG	3

/* 0: SPMI-M, 1: SPMI-P */
#define PMIF_PMIFID_SPMI0		0
#define PMIF_PMIFID_SPMI1		1

#define PMIF_DELAY_US   10
//#define PMIF_TIMEOUT    (10 * 1000)
#define PMIF_TIMEOUT    (100 * 1000)

#define PMIF_CHAN_OFFSET 0x5

#define SPMI_OP_ST_BUSY 1

#define PMIF_IRQDESC(name) { #name, pmif_##name##_irq_handler, -1}

#define MT6316_PMIC_RG_SPMI_DBGMUX_OUT_L_ADDR                           (0x42b)
#define MT6316_PMIC_RG_SPMI_DBGMUX_OUT_L_MASK                           (0xff)
#define MT6316_PMIC_RG_SPMI_DBGMUX_OUT_L_SHIFT                          (0)
#define MT6316_PMIC_RG_SPMI_DBGMUX_OUT_H_ADDR                           (0x42c)
#define MT6316_PMIC_RG_SPMI_DBGMUX_OUT_H_MASK                           (0xff)
#define MT6316_PMIC_RG_SPMI_DBGMUX_OUT_H_SHIFT                          (0)
#define MT6316_PMIC_RG_SPMI_DBGMUX_SEL_ADDR                             (0x42d)
#define MT6316_PMIC_RG_SPMI_DBGMUX_SEL_MASK                             (0x3f)
#define MT6316_PMIC_RG_SPMI_DBGMUX_SEL_SHIFT                            (0)
#define MT6316_PMIC_RG_DEBUG_EN_TRIG_ADDR                               (0x42d)
#define MT6316_PMIC_RG_DEBUG_EN_TRIG_MASK                               (0x1)
#define MT6316_PMIC_RG_DEBUG_EN_TRIG_SHIFT                              (6)
#define MT6316_PMIC_RG_DEBUG_DIS_TRIG_ADDR                              (0x42d)
#define MT6316_PMIC_RG_DEBUG_DIS_TRIG_MASK                              (0x1)
#define MT6316_PMIC_RG_DEBUG_DIS_TRIG_SHIFT                             (7)
#define MT6316_PMIC_RG_DEBUG_EN_RD_CMD_ADDR                             (0x42e)
#define MT6316_PMIC_RG_DEBUG_EN_RD_CMD_MASK                             (0x1)
#define MT6316_PMIC_RG_DEBUG_EN_RD_CMD_SHIFT                            (0)

enum {
	SPMI_MASTER_0 = 0,
	SPMI_MASTER_1,
	SPMI_MASTER_P_1,
	SPMI_MASTER_MAX
};

enum spmi_slave {
	SPMI_SLAVE_0 = 0,
	SPMI_SLAVE_1,
	SPMI_SLAVE_2,
	SPMI_SLAVE_3,
	SPMI_SLAVE_4,
	SPMI_SLAVE_5,
	SPMI_SLAVE_6,
	SPMI_SLAVE_7,
	SPMI_SLAVE_8,
	SPMI_SLAVE_9,
	SPMI_SLAVE_10,
	SPMI_SLAVE_11,
	SPMI_SLAVE_12,
	SPMI_SLAVE_13,
	SPMI_SLAVE_14,
	SPMI_SLAVE_15
};

struct pmif_irq_desc {
	const char *name;
	irq_handler_t irq_handler;
	int irq;
};


enum pmif_regs {
	PMIF_INIT_DONE,
	PMIF_INF_EN,
	MD_AUXADC_RDATA_0_ADDR,
	MD_AUXADC_RDATA_1_ADDR,
	MD_AUXADC_RDATA_2_ADDR,
	PMIF_ARB_EN,
	PMIF_CMDISSUE_EN,
	PMIF_TIMER_CTRL,
	PMIF_SPI_MODE_CTRL,
	PMIF_IRQ_EVENT_EN_0,
	PMIF_IRQ_FLAG_0,
	PMIF_IRQ_CLR_0,
	PMIF_IRQ_EVENT_EN_1,
	PMIF_IRQ_FLAG_1,
	PMIF_IRQ_CLR_1,
	PMIF_IRQ_EVENT_EN_2,
	PMIF_IRQ_FLAG_2,
	PMIF_IRQ_CLR_2,
	PMIF_IRQ_EVENT_EN_3,
	PMIF_IRQ_FLAG_3,
	PMIF_IRQ_CLR_3,
	PMIF_IRQ_EVENT_EN_4,
	PMIF_IRQ_FLAG_4,
	PMIF_IRQ_CLR_4,
	PMIF_WDT_EVENT_EN_0,
	PMIF_WDT_FLAG_0,
	PMIF_WDT_EVENT_EN_1,
	PMIF_WDT_FLAG_1,
	PMIF_SWINF_0_STA,
	PMIF_SWINF_0_WDATA_31_0,
	PMIF_SWINF_0_RDATA_31_0,
	PMIF_SWINF_0_ACC,
	PMIF_SWINF_0_VLD_CLR,
	PMIF_SWINF_1_STA,
	PMIF_SWINF_1_WDATA_31_0,
	PMIF_SWINF_1_RDATA_31_0,
	PMIF_SWINF_1_ACC,
	PMIF_SWINF_1_VLD_CLR,
	PMIF_SWINF_2_STA,
	PMIF_SWINF_2_WDATA_31_0,
	PMIF_SWINF_2_RDATA_31_0,
	PMIF_SWINF_2_ACC,
	PMIF_SWINF_2_VLD_CLR,
	PMIF_SWINF_3_STA,
	PMIF_SWINF_3_WDATA_31_0,
	PMIF_SWINF_3_RDATA_31_0,
	PMIF_SWINF_3_ACC,
	PMIF_SWINF_3_VLD_CLR,
	PMIF_PMIC_SWINF_0_PER,
	PMIF_PMIC_SWINF_1_PER,
	PMIF_ACC_VIO_INFO_0,
	PMIF_ACC_VIO_INFO_1,
	PMIF_ACC_VIO_INFO_2,
};
static const u32 mt6xxx_regs[] = {
	[PMIF_INIT_DONE] =			0x0000,
	[PMIF_INF_EN] =				0x0024,
	[MD_AUXADC_RDATA_0_ADDR] =		0x0080,
	[MD_AUXADC_RDATA_1_ADDR] =		0x0084,
	[MD_AUXADC_RDATA_2_ADDR] =		0x0088,
	[PMIF_ARB_EN] =				0x0150,
	[PMIF_CMDISSUE_EN] =			0x03B8,
	[PMIF_TIMER_CTRL] =			0x03E4,
	[PMIF_SPI_MODE_CTRL] =			0x0408,
	[PMIF_IRQ_EVENT_EN_0] =			0x0420,
	[PMIF_IRQ_FLAG_0] =			0x0428,
	[PMIF_IRQ_CLR_0] =			0x042C,
	[PMIF_IRQ_EVENT_EN_1] =			0x0430,
	[PMIF_IRQ_FLAG_1] =			0x0438,
	[PMIF_IRQ_CLR_1] =			0x043C,
	[PMIF_IRQ_EVENT_EN_2] =			0x0440,
	[PMIF_IRQ_FLAG_2] =			0x0448,
	[PMIF_IRQ_CLR_2] =			0x044C,
	[PMIF_IRQ_EVENT_EN_3] =			0x0450,
	[PMIF_IRQ_FLAG_3] =			0x0458,
	[PMIF_IRQ_CLR_3] =			0x045C,
	[PMIF_IRQ_EVENT_EN_4] =			0x0460,
	[PMIF_IRQ_FLAG_4] =			0x0468,
	[PMIF_IRQ_CLR_4] =			0x046C,
	[PMIF_WDT_EVENT_EN_0] =			0x0474,
	[PMIF_WDT_FLAG_0] =			0x0478,
	[PMIF_WDT_EVENT_EN_1] =			0x047C,
	[PMIF_WDT_FLAG_1] =			0x0480,
	[PMIF_SWINF_0_ACC] =			0x0800,
	[PMIF_SWINF_0_WDATA_31_0] =		0x0804,
	[PMIF_SWINF_0_RDATA_31_0] =		0x0814,
	[PMIF_SWINF_0_VLD_CLR] =		0x0824,
	[PMIF_SWINF_0_STA] =			0x0828,
	[PMIF_SWINF_1_ACC] =			0x0840,
	[PMIF_SWINF_1_WDATA_31_0] =		0x0844,
	[PMIF_SWINF_1_RDATA_31_0] =		0x0854,
	[PMIF_SWINF_1_VLD_CLR] =		0x0864,
	[PMIF_SWINF_1_STA] =			0x0868,
	[PMIF_SWINF_2_ACC] =			0x0880,
	[PMIF_SWINF_2_WDATA_31_0] =		0x0884,
	[PMIF_SWINF_2_RDATA_31_0] =		0x0894,
	[PMIF_SWINF_2_VLD_CLR] =		0x08A4,
	[PMIF_SWINF_2_STA] =			0x08A8,
	[PMIF_SWINF_3_ACC] =			0x08C0,
	[PMIF_SWINF_3_WDATA_31_0] =		0x08C4,
	[PMIF_SWINF_3_RDATA_31_0] =		0x08D4,
	[PMIF_SWINF_3_VLD_CLR] =		0x08E4,
	[PMIF_SWINF_3_STA] =			0x08E8,
	[PMIF_PMIC_SWINF_0_PER] =		0x093C,
	[PMIF_PMIC_SWINF_1_PER] =		0x0940,
	[PMIF_ACC_VIO_INFO_0] =			0x0980,
	[PMIF_ACC_VIO_INFO_1] =			0x0984,
	[PMIF_ACC_VIO_INFO_2] =			0x0988,
};

static const u32 mt6853_spmi_regs[] = {
	[SPMI_OP_ST_CTRL] =	0x0000,
	[SPMI_GRP_ID_EN] =	0x0004,
	[SPMI_OP_ST_STA] =	0x0008,
	[SPMI_MST_SAMPL] =	0x000C,
	[SPMI_MST_REQ_EN] =	0x0010,
	[SPMI_MST_RCS_CTRL] =	0x0014,
	[SPMI_SLV_3_0_EINT] =	0x0020,
	[SPMI_SLV_7_4_EINT] =	0x0024,
	[SPMI_SLV_B_8_EINT] =	0x0028,
	[SPMI_SLV_F_C_EINT] =	0x002C,
	[SPMI_REC_CTRL] =	0x0040,
	[SPMI_REC0] =		0x0044,
	[SPMI_REC1] =		0x0048,
	[SPMI_REC2] =		0x004C,
	[SPMI_REC3] =		0x0050,
	[SPMI_REC4] =		0x0054,
	[SPMI_REC_CMD_DEC] =	0x005C,
	[SPMI_DEC_DBG] =	0x00F8,
	[SPMI_MST_DBG] =	0x00FC,
};

enum {
	SPMI_RCS_SR_BIT,
	SPMI_RCS_A_BIT
};

enum {
	SPMI_RCS_MST_W = 1,
	SPMI_RCS_SLV_W = 3
};

enum {
	/* MT6885/MT6873 series */
	IRQ_PMIC_CMD_ERR_PARITY_ERR = 17,
	IRQ_PMIF_ACC_VIO = 20,
	IRQ_PMIC_ACC_VIO = 21,
	IRQ_LAT_LIMIT_REACHED = 6,
	IRQ_HW_MONITOR = 7,
	IRQ_WDT = 8,
	/* MT6853 series */
	IRQ_PMIF_ACC_VIO_V2 = 31,
	IRQ_PMIC_ACC_VIO_V2 = 0,
	IRQ_HW_MONITOR_V2 = 18,
	IRQ_WDT_V2 = 19,
	IRQ_ALL_PMIC_MPU_VIO_V2 = 20,
	/* MT6833/MT6877 series */
	IRQ_HW_MONITOR_V3 = 12,
	IRQ_WDT_V3 = 13,
	IRQ_ALL_PMIC_MPU_VIO_V3 = 14,
	/* MT6983/MT6879 */
	IRQ_HW_MONITOR_V4 = 29,
	IRQ_WDT_V4 = 30,
	IRQ_ALL_PMIC_MPU_VIO_V4 = 31,
	/* MT6985/MT6897 */
	IRQ_PMIF_ACC_VIO_V3 = 27,
	IRQ_PMIF_SWINF_ACC_ERR_0 = 3,
	IRQ_PMIF_SWINF_ACC_ERR_1 = 4,
	IRQ_PMIF_SWINF_ACC_ERR_2 = 5,
	IRQ_PMIF_SWINF_ACC_ERR_3 = 6,
	IRQ_PMIF_SWINF_ACC_ERR_4 = 7,
	IRQ_PMIF_SWINF_ACC_ERR_5 = 8,
	IRQ_PMIF_SWINF_ACC_ERR_0_V2 = 23,
	IRQ_PMIF_SWINF_ACC_ERR_1_V2 = 24,
	IRQ_PMIF_SWINF_ACC_ERR_2_V2 = 25,
	IRQ_PMIF_SWINF_ACC_ERR_3_V2 = 26,
	IRQ_PMIF_SWINF_ACC_ERR_4_V2 = 27,
	IRQ_PMIF_SWINF_ACC_ERR_5_V2 = 28,
};
static void spmi_dev_parse(struct platform_device *pdev, struct pmif *arb)
{
	struct device_node *child;
	u32 mask = 0, sid;
	unsigned int i;

	if (of_property_read_u32(pdev->dev.of_node, "spmi-dev-mask", &mask))
		dev_info(&pdev->dev, "No spmi-dev-mask; PMICs use SPMI-M\n");

	for (i = 0; i < SPMI_MAX_SLAVE_ID; i++)
		arb->slave_path[i] = !!(mask & BIT(i));

	for_each_available_child_of_node(pdev->dev.of_node, child) {
		if (of_device_is_compatible(child, "mediatek,mt6316") &&
		    !of_property_read_u32(child, "reg", &sid) && sid < SPMI_MAX_SLAVE_ID)
			arb->mt6316_slave_mask |= BIT(sid);
	}
}

static unsigned long long get_current_time_ms(void)
{
	unsigned long long cur_ts;

	cur_ts = sched_clock();
	do_div(cur_ts, 1000000);
	return cur_ts;
}

static u32 pmif_readl(void __iomem *addr, struct pmif *arb, enum pmif_regs reg)
{
	return readl(addr + arb->regs[reg]);
}

static void pmif_writel(void __iomem *addr, struct pmif *arb, u32 val, enum pmif_regs reg)
{
	writel(val, addr + arb->regs[reg]);
}

static void mtk_spmi_writel(void __iomem *addr, struct pmif *arb, u32 val, enum spmi_regs reg)
{
	writel(val, addr + arb->spmimst_regs[reg]);
}


static u32 mtk_spmi_readl(void __iomem *addr, struct pmif *arb, enum spmi_regs reg)
{
	return readl(addr + arb->spmimst_regs[reg]);
}

static bool pmif_is_fsm_vldclr(void __iomem *addr, struct pmif *arb)
{
	u32 reg_rdata;

	reg_rdata = pmif_readl(addr, arb, arb->chan.ch_sta);
	return GET_SWINF(reg_rdata) == SWINF_WFVLDCLR;
}

static int pmif_arb_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid)
{
	struct pmif *arb = spmi_controller_get_drvdata(ctrl);
	u32 rdata;
	u8 cmd;
	int ret;
	unsigned long flags;
	raw_spinlock_t *lock;

	if (sid >= SPMI_MAX_SLAVE_ID)
		return -EINVAL;

	/* Check the opcode */
	if (opc == SPMI_CMD_RESET)
		cmd = 0;
	else if (opc == SPMI_CMD_SLEEP)
		cmd = 1;
	else if (opc == SPMI_CMD_SHUTDOWN)
		cmd = 2;
	else if (opc == SPMI_CMD_WAKEUP)
		cmd = 3;
	else
		return -EINVAL;

	lock = arb->slave_path[sid] ? &arb->lock_p : &arb->lock_m;
	raw_spin_lock_irqsave(lock, flags);
	mtk_spmi_writel(arb->spmimst_base[arb->slave_path[sid]], arb, (cmd << 0x4) | sid, SPMI_OP_ST_CTRL);
	ret = readl_poll_timeout_atomic(arb->spmimst_base[arb->slave_path[sid]] + arb->spmimst_regs[SPMI_OP_ST_STA],
					rdata, (rdata & SPMI_OP_ST_BUSY) == SPMI_OP_ST_BUSY,
					PMIF_DELAY_US, PMIF_TIMEOUT);
	raw_spin_unlock_irqrestore(lock, flags);
	if (ret < 0)
		dev_err(&ctrl->dev, "timeout, err = %d\r\n", ret);

	return ret;
}

static int pmif_spmi_read_cmd_one(struct spmi_controller *ctrl, u8 opc, u8 sid,
			      u16 addr, u8 *buf, size_t len)
{
	struct pmif *arb = spmi_controller_get_drvdata(ctrl);
	struct ch_reg *inf_reg = NULL;
	int ret;
	u32 data = 0;
	u8 bc = len - 1;
	unsigned long flags_m;
	raw_spinlock_t *lock;

	/* Check for argument validation. */
	if (sid >= SPMI_MAX_SLAVE_ID || !len || len > sizeof(data))
		return -EINVAL;

	if (!arb)
		return -EINVAL;

	inf_reg = &arb->chan;
	/* Check the opcode */
	if (opc >= 0x60 && opc <= 0x7f)
		opc = PMIF_CMD_REG;
	else if (opc >= 0x20 && opc <= 0x2f)
		opc = PMIF_CMD_EXT_REG_LONG;
	else if (opc >= 0x38 && opc <= 0x3f)
		opc = PMIF_CMD_EXT_REG_LONG;
	else
		return -EINVAL;

	lock = arb->slave_path[sid] ? &arb->lock_p : &arb->lock_m;
	raw_spin_lock_irqsave(lock, flags_m);

	/* Wait for Software Interface FSM state to be IDLE. */
	ret = readl_poll_timeout_atomic(arb->pmif_base[arb->slave_path[sid]] + arb->regs[inf_reg->ch_sta],
					data, GET_SWINF(data) == SWINF_IDLE,
					PMIF_DELAY_US, PMIF_TIMEOUT);
	if (ret < 0) {
		dev_err(&ctrl->dev, "check IDLE timeout, read 0x%x, sta=0x%x, SPMI_DBG=0x%x\n",
			addr, pmif_readl(arb->pmif_base[arb->slave_path[sid]], arb, inf_reg->ch_sta),
			readl(arb->spmimst_base[arb->slave_path[sid]] + arb->spmimst_regs[SPMI_MST_DBG]));
		/* set channel ready if the data has transferred */
		if (pmif_is_fsm_vldclr(arb->pmif_base[arb->slave_path[sid]], arb))
			pmif_writel(arb->pmif_base[arb->slave_path[sid]], arb, 1, inf_reg->ch_rdy);

		raw_spin_unlock_irqrestore(lock, flags_m);

		return ret;
	}

	/* Send the command. */
	pmif_writel(arb->pmif_base[arb->slave_path[sid]], arb,
		    (opc << 30) | (sid << 24) | (bc << 16) | addr,
		    inf_reg->ch_send);

	/* Wait for Software Interface FSM state to be WFVLDCLR,
	 *
	 * read the data and clear the valid flag.
	 */
	ret = readl_poll_timeout_atomic(arb->pmif_base[arb->slave_path[sid]] + arb->regs[inf_reg->ch_sta],
					data, GET_SWINF(data) == SWINF_WFVLDCLR,
					PMIF_DELAY_US, PMIF_TIMEOUT);
	if (ret < 0) {
		dev_err(&ctrl->dev, "check WFVLDCLR timeout, read 0x%x, sta=0x%x, SPMI_DBG=0x%x\n",
			addr, pmif_readl(arb->pmif_base[arb->slave_path[sid]], arb, inf_reg->ch_sta),
			readl(arb->spmimst_base[arb->slave_path[sid]] + arb->spmimst_regs[SPMI_MST_DBG]));

		raw_spin_unlock_irqrestore(lock, flags_m);

		return ret;
	}

	data = pmif_readl(arb->pmif_base[arb->slave_path[sid]], arb, inf_reg->rdata);
	memcpy(buf, &data, len);
	pmif_writel(arb->pmif_base[arb->slave_path[sid]], arb, 1, inf_reg->ch_rdy);

	raw_spin_unlock_irqrestore(lock, flags_m);

	return 0;
}

static int pmif_spmi_write_cmd_one(struct spmi_controller *ctrl, u8 opc, u8 sid,
			       u16 addr, const u8 *buf, size_t len)
{
	struct pmif *arb = spmi_controller_get_drvdata(ctrl);
	struct ch_reg *inf_reg = NULL;
	int ret;
	u32 data = 0;
	u8 bc = len - 1;
	unsigned long flags_m;
	raw_spinlock_t *lock;

	/* Check for argument validation. */
	if (sid >= SPMI_MAX_SLAVE_ID || !len || len > sizeof(data))
		return -EINVAL;

	if (!arb)
		return -EINVAL;

	inf_reg = &arb->chan;

	/* Check the opcode */
	if (opc >= 0x40 && opc <= 0x5F)
		opc = PMIF_CMD_REG;
	else if (opc <= 0x0F)
		opc = PMIF_CMD_EXT_REG_LONG;
	else if (opc >= 0x30 && opc <= 0x37)
		opc = PMIF_CMD_EXT_REG_LONG;
	else if (opc >= 0x80)
		opc = PMIF_CMD_REG_0;
	else
		return -EINVAL;

	lock = arb->slave_path[sid] ? &arb->lock_p : &arb->lock_m;
	raw_spin_lock_irqsave(lock, flags_m);
	/* Wait for Software Interface FSM state to be IDLE. */
	ret = readl_poll_timeout_atomic(arb->pmif_base[arb->slave_path[sid]] + arb->regs[inf_reg->ch_sta],
					data, GET_SWINF(data) == SWINF_IDLE,
					PMIF_DELAY_US, PMIF_TIMEOUT);
	if (ret < 0) {
		dev_err(&ctrl->dev, "check IDLE timeout, read 0x%x, sta=0x%x, SPMI_DBG=0x%x\n",
			addr, pmif_readl(arb->pmif_base[arb->slave_path[sid]], arb, inf_reg->ch_sta),
			readl(arb->spmimst_base[arb->slave_path[sid]] + arb->spmimst_regs[SPMI_MST_DBG]));
		/* set channel ready if the data has transferred */
		if (pmif_is_fsm_vldclr(arb->pmif_base[arb->slave_path[sid]], arb))
			pmif_writel(arb->pmif_base[arb->slave_path[sid]], arb, 1, inf_reg->ch_rdy);

		raw_spin_unlock_irqrestore(lock, flags_m);

		return ret;
	}

	/* Set the write data. */
	memcpy(&data, buf, len);
	pmif_writel(arb->pmif_base[arb->slave_path[sid]], arb, data, inf_reg->wdata);

	/* Send the command. */
	pmif_writel(arb->pmif_base[arb->slave_path[sid]], arb,
		    (opc << 30) | BIT(29) | (sid << 24) | (bc << 16) | addr,
		    inf_reg->ch_send);

	raw_spin_unlock_irqrestore(lock, flags_m);

	return 0;
}

/* PMIF exposes one 32-bit payload per software-interface transaction. */
static int pmif_spmi_read_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			    u16 addr, u8 *buf, size_t len)
{
	size_t count;
	int ret;

	if (!len)
		return -EINVAL;
	while (len) {
		count = min_t(size_t, len, sizeof(u32));
		ret = pmif_spmi_read_cmd_one(ctrl, opc, sid, addr, buf, count);
		if (ret)
			return ret;
		addr += count;
		buf += count;
		len -= count;
	}
	return 0;
}

static int pmif_spmi_write_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			     u16 addr, const u8 *buf, size_t len)
{
	size_t count;
	int ret;

	if (!len)
		return -EINVAL;
	while (len) {
		count = min_t(size_t, len, sizeof(u32));
		ret = pmif_spmi_write_cmd_one(ctrl, opc, sid, addr, buf, count);
		if (ret)
			return ret;
		addr += count;
		buf += count;
		len -= count;
	}
	return 0;
}

static struct platform_driver mtk_spmi_driver;

static const struct pmif mt6989_pmif_data = {
	.regs = mt6xxx_regs,
	.spmimst_regs = mt6853_spmi_regs,
	.soc_chan = 2,
	.mstid = SPMI_MASTER_1,
	.pmifid = PMIF_PMIFID_SPMI0,
	.read_cmd = pmif_spmi_read_cmd,
	.write_cmd = pmif_spmi_write_cmd,
	.caps = 2,
};

/* PMIF Exception IRQ Handler */
static void pmif_cmd_err_parity_err_irq_handler(int irq, void *data)
{
	mt6989_spmi_dump_spmimst_all_reg();
	mt6989_spmi_dump_pmif_record_reg(0, 0);
	pr_notice("[PMIF]:parity error\n");
}

static void pmif_pmif_acc_vio_irq_handler(int irq, void *data)
{
	mt6989_spmi_dump_pmif_record_reg(0, 0);
	mt6989_spmi_dump_pmif_acc_vio_reg();
	pr_notice("[PMIF]:pmif_acc_vio\n");
}

static void pmif_pmic_acc_vio_irq_handler(int irq, void *data)
{
	mt6989_spmi_dump_pmic_acc_vio_reg();
	pr_notice("[PMIF]:pmic_acc_vio\n");
}

static void pmif_lat_limit_reached_irq_handler(int irq, void *data)
{
	mt6989_spmi_dump_pmif_busy_reg();
	mt6989_spmi_dump_pmif_record_reg(0, 0);
}

static void pmif_acc_vio_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;

	pr_notice("[PMIF]:PMIF ACC violation debug info\n");
	pr_notice("[PMIF]:PMIF-M\n");
	pr_notice("[PMIF]:PMIF_ACC_VIO_INFO_0 = 0x%x\n", pmif_readl(arb->pmif_base[0],
		arb, PMIF_ACC_VIO_INFO_0));
	pr_notice("[PMIF]:PMIF_ACC_VIO_INFO_1 = 0x%x\n", pmif_readl(arb->pmif_base[0],
		arb, PMIF_ACC_VIO_INFO_1));
	pr_notice("[PMIF]:PMIF_ACC_VIO_INFO_2 = 0x%x\n", pmif_readl(arb->pmif_base[0],
		arb, PMIF_ACC_VIO_INFO_2));

	if (!IS_ERR(arb->pmif_base[1])) {
		pr_notice("[PMIF]:PMIF-P\n");
		pr_notice("[PMIF]:PMIF_ACC_VIO_INFO_0 = 0x%x\n", pmif_readl(arb->pmif_base[1],
			arb, PMIF_ACC_VIO_INFO_0));
		pr_notice("[PMIF]:PMIF_ACC_VIO_INFO_1 = 0x%x\n", pmif_readl(arb->pmif_base[1],
			arb, PMIF_ACC_VIO_INFO_1));
		pr_notice("[PMIF]:PMIF_ACC_VIO_INFO_2 = 0x%x\n", pmif_readl(arb->pmif_base[1],
			arb, PMIF_ACC_VIO_INFO_2));
	}
}

static void pmif_swinf_err_irq_handler(int irq_m, int irq_p, void *data, int idx)
{
	struct pmif *arb = data;

	mt6989_spmi_dump_pmif_record_reg(irq_m, irq_p);
	pr_notice("[PMIF] %s swintf %d err irq_m/p 0x%x/0x%x\n", __func__,
		idx - (arb->swintf_err_idx[0])%32, irq_m, irq_p);
}

static void pmif_hwinf_err_irq_handler(int irq_m, int irq_p, void *data, int idx)
{
	struct pmif *arb = data;

	mt6989_spmi_dump_pmif_record_reg(irq_m, irq_p);
	pr_notice("[PMIF] %s hwintf %d err irq_m/p 0x%x/0x%x\n", __func__,
		idx - (arb->hwintf_err_idx[0])%32, irq_m, irq_p);
}

static void pmif_hw_monitor_irq_handler(int irq, void *data)
{
	mt6989_spmi_dump_pmif_record_reg(0, 0);
	pr_err("PMIF: pmif_hw_monitor_match\n");

	pr_notice("[PMIF]:pmif_hw_monitor_match\n");
}

static void pmif_wdt_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;

	mt6989_spmi_dump_pmif_busy_reg();
	mt6989_spmi_dump_pmif_record_reg(0, 0);
	mt6989_spmi_dump_wdt_reg();
	pmif_writel(arb->pmif_base[0], arb, 0x40000000, PMIF_IRQ_CLR_0);
	if (!IS_ERR(arb->pmif_base[1]))
		pmif_writel(arb->pmif_base[1], arb, 0x40000000, PMIF_IRQ_CLR_0);
	pr_notice("[PMIF]:WDT IRQ HANDLER DONE\n");
}

static void pmif_swinf_acc_err_0_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;

	pmif_writel(arb->pmif_base[0], arb, 0x0, MD_AUXADC_RDATA_0_ADDR);
	pmif_writel(arb->pmif_base[0], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	if (!IS_ERR(arb->pmif_base[1])) {
		pmif_writel(arb->pmif_base[1], arb, 0x0, MD_AUXADC_RDATA_0_ADDR);
		pmif_writel(arb->pmif_base[1], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	}
	pr_notice("[PMIF]:SWINF_ACC_ERR_0\n");
}

static void pmif_swinf_acc_err_1_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;

	pmif_writel(arb->pmif_base[0], arb, 0x1, MD_AUXADC_RDATA_0_ADDR);
	pmif_writel(arb->pmif_base[0], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	if (!IS_ERR(arb->pmif_base[1])) {
		pmif_writel(arb->pmif_base[1], arb, 0x1, MD_AUXADC_RDATA_0_ADDR);
		pmif_writel(arb->pmif_base[1], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	}
	pr_notice("[PMIF]:SWINF_ACC_ERR_1\n");
}

static void pmif_swinf_acc_err_2_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;

	pmif_writel(arb->pmif_base[0], arb, 0x2, MD_AUXADC_RDATA_0_ADDR);
	pmif_writel(arb->pmif_base[0], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	if (!IS_ERR(arb->pmif_base[1])) {
		pmif_writel(arb->pmif_base[1], arb, 0x2, MD_AUXADC_RDATA_0_ADDR);
		pmif_writel(arb->pmif_base[1], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	}
	pr_notice("[PMIF]:SWINF_ACC_ERR_2\n");
}

static void pmif_swinf_acc_err_3_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;

	pmif_writel(arb->pmif_base[0], arb, 0x3, MD_AUXADC_RDATA_0_ADDR);
	pmif_writel(arb->pmif_base[0], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	if (!IS_ERR(arb->pmif_base[1])) {
		pmif_writel(arb->pmif_base[1], arb, 0x3, MD_AUXADC_RDATA_0_ADDR);
		pmif_writel(arb->pmif_base[1], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	}
	pr_notice("[PMIF]:SWINF_ACC_ERR_3\n");
}

static void pmif_swinf_acc_err_4_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;

	pmif_writel(arb->pmif_base[0], arb, 0x4, MD_AUXADC_RDATA_0_ADDR);
	pmif_writel(arb->pmif_base[0], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	if (!IS_ERR(arb->pmif_base[1])) {
		pmif_writel(arb->pmif_base[1], arb, 0x4, MD_AUXADC_RDATA_0_ADDR);
		pmif_writel(arb->pmif_base[1], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	}
	pr_notice("[PMIF]:SWINF_ACC_ERR_4\n");
}

static void pmif_swinf_acc_err_5_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;

	pmif_writel(arb->pmif_base[0], arb, 0x5, MD_AUXADC_RDATA_0_ADDR);
	pmif_writel(arb->pmif_base[0], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	if (!IS_ERR(arb->pmif_base[1])) {
		pmif_writel(arb->pmif_base[1], arb, 0x5, MD_AUXADC_RDATA_0_ADDR);
		pmif_writel(arb->pmif_base[1], arb, (u32)get_current_time_ms(), MD_AUXADC_RDATA_1_ADDR);
	}
	pr_notice("[PMIF]:SWINF_ACC_ERR_5\n");
}

static irqreturn_t pmif_event_0_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;
	int irq_f = 0, irq_f_p = 0, idx = 0;

	__pm_stay_awake(arb->pmif_m_Thread_lock);
	mutex_lock(&arb->pmif_m_mutex);

	irq_f = pmif_readl(arb->pmif_base[0], arb, PMIF_IRQ_FLAG_0);
	if (!IS_ERR(arb->pmif_base[1]))
		irq_f_p = pmif_readl(arb->pmif_base[1], arb, PMIF_IRQ_FLAG_0);

	if ((irq_f == 0) && (irq_f_p == 0)) {
		mutex_unlock(&arb->pmif_m_mutex);
		__pm_relax(arb->pmif_m_Thread_lock);
		return IRQ_NONE;
	}

	for (idx = 0; idx < 32; idx++) {
		if (((irq_f & (0x1 << idx)) != 0) || ((irq_f_p & (0x1 << idx)) != 0)) {
			switch (idx) {
			case IRQ_PMIF_ACC_VIO_V3:
				pmif_acc_vio_irq_handler(irq, data);
			break;
			case IRQ_WDT_V4:
				pmif_wdt_irq_handler(irq, data);
			break;
			case IRQ_HW_MONITOR_V4:
				pmif_hw_monitor_irq_handler(irq, data);
			break;
			case IRQ_ALL_PMIC_MPU_VIO_V4:
				pmif_pmif_acc_vio_irq_handler(irq, data);
			break;
			default:
				pr_notice("%s IRQ[%d] triggered\n",
					__func__, idx);
				mt6989_spmi_dump_pmif_record_reg(0, 0);
			break;
			}
		}
	}
	if (irq_f)
		pmif_writel(arb->pmif_base[0], arb, irq_f, PMIF_IRQ_CLR_0);
	if (irq_f_p)
		pmif_writel(arb->pmif_base[1], arb, irq_f_p, PMIF_IRQ_CLR_0);
	mutex_unlock(&arb->pmif_m_mutex);
	__pm_relax(arb->pmif_m_Thread_lock);

	return IRQ_HANDLED;
}

static irqreturn_t pmif_event_1_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;
	int irq_f = 0, irq_f_p = 0, idx = 0;

	__pm_stay_awake(arb->pmif_m_Thread_lock);
	mutex_lock(&arb->pmif_m_mutex);

	irq_f = pmif_readl(arb->pmif_base[0], arb, PMIF_IRQ_FLAG_1);
	if (!IS_ERR(arb->pmif_base[1]))
		irq_f_p = pmif_readl(arb->pmif_base[1], arb, PMIF_IRQ_FLAG_1);

	if ((irq_f == 0) && (irq_f_p == 0)) {
		mutex_unlock(&arb->pmif_m_mutex);
		__pm_relax(arb->pmif_m_Thread_lock);
		return IRQ_NONE;
	}
	for (idx = 0; idx < 32; idx++) {
		if (((irq_f & (0x1 << idx)) != 0) || ((irq_f_p & (0x1 << idx)) != 0)) {
			if ((idx >= (arb->hwintf_err_idx[0] % 32)) && (idx <= (arb->hwintf_err_idx[1] % 32))) {
				pmif_hwinf_err_irq_handler(irq_f, irq_f_p, data, idx);
			} else {
				switch (idx) {
				default:
					pr_notice("%s IRQ[%d] triggered\n",
						__func__, idx);
					mt6989_spmi_dump_pmif_record_reg(0, 0);
				break;
				}
			}
		}
	}
	if (irq_f) {
		pmif_writel(arb->pmif_base[0], arb, irq_f, PMIF_IRQ_CLR_1);
	}
	if (irq_f_p) {
		pmif_writel(arb->pmif_base[1], arb, irq_f_p, PMIF_IRQ_CLR_1);
	}
	mutex_unlock(&arb->pmif_m_mutex);
	__pm_relax(arb->pmif_m_Thread_lock);

	return IRQ_HANDLED;
}

static irqreturn_t pmif_event_2_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;
	int irq_f = 0, irq_f_p = 0, idx = 0;

	__pm_stay_awake(arb->pmif_m_Thread_lock);
	mutex_lock(&arb->pmif_m_mutex);

	irq_f = pmif_readl(arb->pmif_base[0], arb, PMIF_IRQ_FLAG_2);
	if (!IS_ERR(arb->pmif_base[1]))
		irq_f_p = pmif_readl(arb->pmif_base[1], arb, PMIF_IRQ_FLAG_2);

	if ((irq_f == 0) && (irq_f_p == 0)) {
		mutex_unlock(&arb->pmif_m_mutex);
		__pm_relax(arb->pmif_m_Thread_lock);
		return IRQ_NONE;
	}

	for (idx = 0; idx < 32; idx++) {
		if (((irq_f & (0x1 << idx)) != 0) || ((irq_f_p & (0x1 << idx)) != 0)) {
			switch (idx) {
			case IRQ_PMIC_CMD_ERR_PARITY_ERR:
				pmif_cmd_err_parity_err_irq_handler(irq, data);
			break;
			case IRQ_PMIF_ACC_VIO_V2:
				pmif_pmif_acc_vio_irq_handler(irq, data);
			break;
			case IRQ_PMIC_ACC_VIO:
				pmif_pmic_acc_vio_irq_handler(irq, data);
			break;
			default:
				pr_notice("%s IRQ[%d] triggered\n",
					__func__, idx);
				mt6989_spmi_dump_pmif_record_reg(0, 0);
			break;
			}
		}
	}
	if (irq_f)
		pmif_writel(arb->pmif_base[0], arb, irq_f, PMIF_IRQ_CLR_2);
	if (irq_f_p)
		pmif_writel(arb->pmif_base[1], arb, irq_f_p, PMIF_IRQ_CLR_2);
	mutex_unlock(&arb->pmif_m_mutex);
	__pm_relax(arb->pmif_m_Thread_lock);

	return IRQ_HANDLED;
}

static irqreturn_t pmif_event_3_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;
	int irq_f = 0, irq_f_p = 0, idx = 0;

	__pm_stay_awake(arb->pmif_m_Thread_lock);
	mutex_lock(&arb->pmif_m_mutex);

	irq_f = pmif_readl(arb->pmif_base[0], arb, PMIF_IRQ_FLAG_3);
	if (!IS_ERR(arb->pmif_base[1]))
		irq_f_p = pmif_readl(arb->pmif_base[1], arb, PMIF_IRQ_FLAG_3);

	if ((irq_f == 0) && (irq_f_p == 0)) {
		mutex_unlock(&arb->pmif_m_mutex);
		__pm_relax(arb->pmif_m_Thread_lock);
		return IRQ_NONE;
	}

	for (idx = 0; idx < 32; idx++) {
		if (((irq_f & (0x1 << idx)) != 0) || ((irq_f_p & (0x1 << idx)) != 0)) {
			if ((idx >= (arb->swintf_err_idx[0] % 32)) && (idx <= (arb->swintf_err_idx[1] % 32))) {
				pmif_swinf_err_irq_handler(irq_f, irq_f_p, data, idx);
			} else {
				switch (idx) {
				case IRQ_PMIF_SWINF_ACC_ERR_0:
				case IRQ_PMIF_SWINF_ACC_ERR_0_V2:
					pmif_swinf_acc_err_0_irq_handler(irq, data);
				break;
				case IRQ_PMIF_SWINF_ACC_ERR_1:
				case IRQ_PMIF_SWINF_ACC_ERR_1_V2:
					pmif_swinf_acc_err_1_irq_handler(irq, data);
				break;
				case IRQ_PMIF_SWINF_ACC_ERR_2:
				case IRQ_PMIF_SWINF_ACC_ERR_2_V2:
					pmif_swinf_acc_err_2_irq_handler(irq, data);
				break;
				/* Use caps to distinguish platform if they have same irq number */
				case IRQ_LAT_LIMIT_REACHED:
				case IRQ_PMIF_SWINF_ACC_ERR_3_V2:
					if (arb->caps == 1)
						pmif_lat_limit_reached_irq_handler(irq, data);
					else
						pmif_swinf_acc_err_3_irq_handler(irq, data);
				break;
				case IRQ_PMIF_SWINF_ACC_ERR_4:
				case IRQ_PMIF_SWINF_ACC_ERR_4_V2:
					pmif_swinf_acc_err_4_irq_handler(irq, data);
				break;
				case IRQ_PMIF_SWINF_ACC_ERR_5:
				case IRQ_PMIF_SWINF_ACC_ERR_5_V2:
					pmif_swinf_acc_err_5_irq_handler(irq, data);
				break;
				case IRQ_HW_MONITOR_V2:
				case IRQ_HW_MONITOR_V3:
					pmif_hw_monitor_irq_handler(irq, data);
				break;
				case IRQ_WDT_V2:
				case IRQ_WDT_V3:
					pmif_wdt_irq_handler(irq, data);
				break;
				case IRQ_PMIC_ACC_VIO_V2:
					pmif_pmic_acc_vio_irq_handler(irq, data);
				break;
				case IRQ_ALL_PMIC_MPU_VIO_V2:
				case IRQ_ALL_PMIC_MPU_VIO_V3:
					pmif_pmif_acc_vio_irq_handler(irq, data);
				break;
				default:
					pr_notice("%s IRQ[%d] triggered\n",
						__func__, idx);
					mt6989_spmi_dump_pmif_record_reg(0, 0);
				break;
				}
			}
		}
	}
	/* Don't clear MD SW SWINF ACC ERR flag for re-send mechanism */
	if (irq_f) {
		if ((!(irq_f & (0x1 << IRQ_PMIF_SWINF_ACC_ERR_0))) &&
			(!(irq_f & (0x1 << IRQ_PMIF_SWINF_ACC_ERR_0_V2))))
			pmif_writel(arb->pmif_base[0], arb, irq_f, PMIF_IRQ_CLR_3);
	}
	if (irq_f_p) {
		if ((!(irq_f_p & (0x1 << IRQ_PMIF_SWINF_ACC_ERR_0))) &&
			(!(irq_f_p & (0x1 << IRQ_PMIF_SWINF_ACC_ERR_0_V2))))
			pmif_writel(arb->pmif_base[1], arb, irq_f_p, PMIF_IRQ_CLR_3);
	}
	mutex_unlock(&arb->pmif_m_mutex);
	__pm_relax(arb->pmif_m_Thread_lock);

	return IRQ_HANDLED;
}

static irqreturn_t pmif_event_4_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;
	int irq_f = 0, irq_f_p = 0, idx = 0;

	__pm_stay_awake(arb->pmif_m_Thread_lock);
	mutex_lock(&arb->pmif_m_mutex);

	irq_f = pmif_readl(arb->pmif_base[0], arb, PMIF_IRQ_FLAG_4);
	if (!IS_ERR(arb->pmif_base[1]))
		irq_f_p = pmif_readl(arb->pmif_base[1], arb, PMIF_IRQ_FLAG_4);

	if ((irq_f == 0) && (irq_f_p == 0)) {
		mutex_unlock(&arb->pmif_m_mutex);
		__pm_relax(arb->pmif_m_Thread_lock);
		return IRQ_NONE;
	}

	for (idx = 0; idx < 32; idx++) {
		if (((irq_f & (0x1 << idx)) != 0) || ((irq_f_p & (0x1 << idx)) != 0)) {
			switch (idx) {
			default:
				pr_notice("%s IRQ[%d] triggered\n",
					__func__, idx);
				mt6989_spmi_dump_pmif_record_reg(0, 0);
			break;
			}
		}
	}
	if (irq_f)
		pmif_writel(arb->pmif_base[0], arb, irq_f, PMIF_IRQ_CLR_4);
	if (irq_f_p)
		pmif_writel(arb->pmif_base[1], arb, irq_f_p, PMIF_IRQ_CLR_4);
	mutex_unlock(&arb->pmif_m_mutex);
	__pm_relax(arb->pmif_m_Thread_lock);

	return IRQ_HANDLED;
}

static struct pmif_irq_desc pmif_event_irq[] = {
	PMIF_IRQDESC(event_0),
	PMIF_IRQDESC(event_1),
	PMIF_IRQDESC(event_2),
	PMIF_IRQDESC(event_3),
	PMIF_IRQDESC(event_4),
};

static irqreturn_t pmif_irq_dispatch(int irq, void *data)
{
	unsigned int i;
	irqreturn_t handled = IRQ_NONE;

	for (i = 0; i < ARRAY_SIZE(pmif_event_irq); i++)
		if (pmif_event_irq[i].irq_handler(irq, data) == IRQ_HANDLED)
			handled = IRQ_HANDLED;
	return handled;
}

static int pmif_irq_register(struct platform_device *pdev, struct pmif *arb)
{
	u32 event_enable[2][5];
	int ret, path, group, irq;

	ret = of_property_read_u32_array(pdev->dev.of_node, "irq-event-en",
					event_enable[0], ARRAY_SIZE(event_enable[0]));
	if (ret)
		return ret;
	ret = of_property_read_u32_array(pdev->dev.of_node, "irq-event-en-p",
					event_enable[1], ARRAY_SIZE(event_enable[1]));
	if (ret)
		return ret;

	for (path = 0; path < 2; path++) {
		irq = path ? arb->irq_p : arb->irq;
		ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
				pmif_irq_dispatch, IRQF_ONESHOT | IRQF_SHARED,
				path ? "mt6989-pmif-p" : "mt6989-pmif-m", arb);
		if (ret)
			return ret;
	}
	for (path = 0; path < 2; path++)
		for (group = 0; group < 5; group++)
			pmif_writel(arb->pmif_base[path], arb,
				event_enable[path][group] | pmif_readl(arb->pmif_base[path],
					arb, PMIF_IRQ_EVENT_EN_0 + group * 3),
				PMIF_IRQ_EVENT_EN_0 + group * 3);
	return 0;
}

static void dump_spmip_pmic_dbg_rg(struct pmif *arb, unsigned int slvid)
{
	u8 rdata = 0, rdata1 = 0, rdata2 = 0, val = 0, org = 0;
	u8 dbg_data = 0, idx, addr, data = 0, cmd, addr1;
	u16 pmic_addr;
	int i;
	unsigned short PMIC_SPMI_DBG_SEL = 0x42d, PMIC_SPMI_DBG_L = 0x42b;
	unsigned short PMIC_SPMI_DBG_H = 0x42c, mt6316INTSTA = 0x240;
	unsigned short hwcidaddr_mt6316 = 0x209;
	unsigned int sid = 0;

	/* Disable read command log */
	val = 0;
	arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
		MT6316_PMIC_RG_DEBUG_EN_RD_CMD_ADDR, &val, 1);
	/* pause debug log feature by setting RG_DEBUG_DIS_TRIG 0->1->0 */
	val = 0;
	arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
		MT6316_PMIC_RG_DEBUG_DIS_TRIG_ADDR, &val, 1);
	val = 0x1 << MT6316_PMIC_RG_DEBUG_DIS_TRIG_SHIFT;
	arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
		MT6316_PMIC_RG_DEBUG_DIS_TRIG_ADDR, &val, 1);
	val = 0;
	arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
		MT6316_PMIC_RG_DEBUG_DIS_TRIG_ADDR, &val, 1);

		/* DBGMUX_SEL = 0 */
	arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL,
		slvid, MT6316_PMIC_RG_SPMI_DBGMUX_SEL_ADDR, &org, 1);
	org &= ~(MT6316_PMIC_RG_SPMI_DBGMUX_SEL_MASK << MT6316_PMIC_RG_SPMI_DBGMUX_SEL_SHIFT);
	org |= (0 << MT6316_PMIC_RG_SPMI_DBGMUX_SEL_SHIFT);
	arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
		MT6316_PMIC_RG_SPMI_DBGMUX_SEL_ADDR, &org, 1);
	/* read spmi_debug[15:0] data*/
	arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, slvid,
		MT6316_PMIC_RG_SPMI_DBGMUX_OUT_L_ADDR, &dbg_data, 1);

	pr_info("%s dbg_data:0x%x\n", __func__, dbg_data);
	idx = dbg_data & 0xF;
	for (i = 0; i < 16; i++) {
		/* debug_addr start from index 1 */
		arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL,
			slvid, MT6316_PMIC_RG_SPMI_DBGMUX_SEL_ADDR, &org, 1);
		org &= ~(MT6316_PMIC_RG_SPMI_DBGMUX_SEL_MASK << MT6316_PMIC_RG_SPMI_DBGMUX_SEL_SHIFT);
		org |= ((((idx + i) % 16) + 1) << MT6316_PMIC_RG_SPMI_DBGMUX_SEL_SHIFT);
		arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
			MT6316_PMIC_RG_SPMI_DBGMUX_SEL_ADDR, &org, 1);
		/* read spmi_debug[15:0] data*/
		arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, slvid,
			MT6316_PMIC_RG_SPMI_DBGMUX_OUT_L_ADDR, &addr, 1);
		arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, slvid,
			MT6316_PMIC_RG_SPMI_DBGMUX_OUT_H_ADDR, &addr1, 1);

		pmic_addr = (addr1 << 8) | addr;
		arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL,
			slvid, MT6316_PMIC_RG_SPMI_DBGMUX_SEL_ADDR, &org, 1);
		org &= ~(MT6316_PMIC_RG_SPMI_DBGMUX_SEL_MASK << MT6316_PMIC_RG_SPMI_DBGMUX_SEL_SHIFT);
		org |= ((((idx + i) % 16) + 17) << MT6316_PMIC_RG_SPMI_DBGMUX_SEL_SHIFT);
		arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
			MT6316_PMIC_RG_SPMI_DBGMUX_SEL_ADDR, &org, 1);
		/* read spmi_debug[15:0] data*/
		arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, slvid,
			MT6316_PMIC_RG_SPMI_DBGMUX_OUT_L_ADDR, &data, 1);
		arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, slvid,
			MT6316_PMIC_RG_SPMI_DBGMUX_OUT_H_ADDR, &dbg_data, 1);
		cmd = dbg_data & 0x7;
		pr_info("slvid=0x%x record %s addr=0x%x, data=0x%x, cmd=%d%s\n",
					slvid, cmd <= 3 ? "write" : "read",
					pmic_addr, data, cmd,
					i == 15 ? "(the last)" : "");
	}

	for (i = 33; i < 38; i++) {
		val = i;
		for (sid = 0; sid < SPMI_MAX_SLAVE_ID; sid++) {
			if (!(arb->mt6316_slave_mask & BIT(sid)))
				continue;
			arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, sid,
				PMIC_SPMI_DBG_SEL, &val, 1);
			arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, sid,
				PMIC_SPMI_DBG_SEL, &rdata, 1);
			arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, sid,
				PMIC_SPMI_DBG_H, &rdata1, 1);
			arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, sid,
				PMIC_SPMI_DBG_L, &rdata2, 1);
			pr_notice("%s sid 0x%x DBG_SEL %d DBG_OUT_H 0x%x DBG_OUT_L 0x%x\n",
				__func__, sid, rdata, rdata1, rdata2);
		}
	}
	val = 0;
	for (sid = 0; sid < SPMI_MAX_SLAVE_ID; sid++) {
		if (!(arb->mt6316_slave_mask & BIT(sid)))
			continue;
		arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, sid,
			PMIC_SPMI_DBG_SEL, &val, 1);
	}
	/* Disable read command log */
	/* pause debug log feature by setting RG_DEBUG_DIS_TRIG 1->0 */
	val = 0;
	arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
		MT6316_PMIC_RG_DEBUG_DIS_TRIG_ADDR, &val, 1);
	/* enable debug log feature by setting RG_DEBUG_EN_TRIG 0->1->0 */
	val = 0x1 << MT6316_PMIC_RG_DEBUG_EN_TRIG_SHIFT;
	arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
		MT6316_PMIC_RG_DEBUG_EN_TRIG_ADDR, &val, 1);
	val = 0;
	arb->spmic->write_cmd(arb->spmic, SPMI_CMD_EXT_WRITEL, slvid,
		MT6316_PMIC_RG_DEBUG_EN_TRIG_ADDR, &val, 1);

	for (sid = 0; sid < SPMI_MAX_SLAVE_ID; sid++) {
		if (!(arb->mt6316_slave_mask & BIT(sid)))
			continue;
		arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, sid,
			mt6316INTSTA, &rdata, 1);
		arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, sid,
			hwcidaddr_mt6316, &rdata1, 1);
		pr_notice("%s sid 0x%x INT_RAW_STA 0x%x cid 0x%x\n",
			__func__, sid, rdata, rdata1);
	}
}

static irqreturn_t spmi_nack_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;
	int flag = 0;
	unsigned int spmi_nack = 0, spmi_p_nack = 0, spmi_nack_data = 0, spmi_p_nack_data = 0;
	unsigned int spmi_rcs_nack = 0, spmi_debug_nack = 0, spmi_mst_nack = 0,
		spmi_p_rcs_nack = 0, spmi_p_debug_nack = 0, spmi_p_mst_nack = 0;
	u8 rdata = 0, rdata1 = 0;
	unsigned short VIO18_SWITCH_6363 = 0x53, hwcidaddr_mt6363 = 0x9;

	__pm_stay_awake(arb->pmif_m_Thread_lock);
	mutex_lock(&arb->pmif_m_mutex);

	spmi_nack = mtk_spmi_readl(arb->spmimst_base[0], arb, SPMI_REC0);
	spmi_nack_data = mtk_spmi_readl(arb->spmimst_base[0], arb, SPMI_REC1);
	spmi_rcs_nack = mtk_spmi_readl(arb->spmimst_base[0], arb, SPMI_REC_CMD_DEC);
	spmi_debug_nack = mtk_spmi_readl(arb->spmimst_base[0], arb, SPMI_DEC_DBG);
	spmi_mst_nack = mtk_spmi_readl(arb->spmimst_base[0], arb, SPMI_MST_DBG);

	if (!IS_ERR(arb->spmimst_base[1])) {
		spmi_p_nack = mtk_spmi_readl(arb->spmimst_base[1], arb, SPMI_REC0);
		spmi_p_nack_data = mtk_spmi_readl(arb->spmimst_base[1], arb, SPMI_REC1);
		spmi_p_rcs_nack = mtk_spmi_readl(arb->spmimst_base[1], arb, SPMI_REC_CMD_DEC);
		spmi_p_debug_nack = mtk_spmi_readl(arb->spmimst_base[1], arb, SPMI_DEC_DBG);
		spmi_p_mst_nack = mtk_spmi_readl(arb->spmimst_base[1], arb, SPMI_MST_DBG);
	}
	if ((spmi_nack & 0xD8) || (spmi_p_nack & 0xD8)) {
		if ((spmi_p_nack & 0xD8) &&
		    (arb->mt6316_slave_mask & BIT((spmi_p_nack & 0x0f00) >> 8)))
			dump_spmip_pmic_dbg_rg(arb, (spmi_p_nack & 0x0f00) >> 8);
		pr_notice("%s spmi transaction fail irq triggered", __func__);
		pr_notice("SPMI_REC0 m/p:0x%x/0x%x SPMI_REC1 m/p 0x%x/0x%x\n",
			spmi_nack, spmi_p_nack, spmi_nack_data, spmi_p_nack_data);
		flag = 1;
	}
	if ((spmi_rcs_nack & 0xC0000) || (spmi_p_rcs_nack & 0xC0000)) {
		pr_notice("%s spmi_rcs transaction fail irq triggered SPMI_REC_CMD_DEC m/p:0x%x/0x%x\n",
			__func__, spmi_rcs_nack, spmi_p_rcs_nack);
		flag = 0;
	}
	if ((spmi_debug_nack & 0xF0000) || (spmi_p_debug_nack & 0xF0000)) {
		pr_notice("%s spmi_debug_nack transaction fail irq triggered SPMI_DEC_DBG m/p: 0x%x/0x%x\n",
			__func__, spmi_debug_nack, spmi_p_debug_nack);
		flag = 0;
	}
	if ((spmi_mst_nack & 0xC0000) || (spmi_p_mst_nack & 0xC0000)) {
		pr_notice("%s spmi_mst_nack transaction fail irq triggered SPMI_MST_DBG m/p: 0x%x/0x%x\n",
		__func__, spmi_mst_nack, spmi_p_mst_nack);
		flag = 0;
	}
	if ((spmi_nack & 0x20) || (spmi_p_nack & 0x20))
		flag = 0;
	if (flag)
		pr_err("SPMI: transaction failed\n");
	/* clear irq*/
	if ((spmi_nack & 0xF8) || (spmi_rcs_nack & 0xC0000) ||
		(spmi_debug_nack & 0xF0000) || (spmi_mst_nack & 0xC0000)) {
		mtk_spmi_writel(arb->spmimst_base[0], arb, 0x3, SPMI_REC_CTRL);
	}
	if ((spmi_p_nack & 0xF8) || (spmi_p_rcs_nack & 0xC0000) ||
		(spmi_p_debug_nack & 0xF0000) || (spmi_p_mst_nack & 0xC0000)) {
		if (spmi_p_nack & 0xD8) {
			pr_notice("%s SPMI_REC0 m/p:0x%x/0x%x, SPMI_REC1 m/p:0x%x/0x%x\n",
				__func__, spmi_nack, spmi_p_nack, spmi_nack_data, spmi_p_nack_data);
			pr_notice("%s SPMI_REC_CMD_DEC m/p:0x%x/0x%x\n", __func__,
				spmi_rcs_nack, spmi_p_rcs_nack);
			pr_notice("%s SPMI_DEC_DBG m/p:0x%x/0x%x\n", __func__,
				spmi_debug_nack, spmi_p_debug_nack);
			pr_notice("%s SPMI_MST_DBG m/p:0x%x/0x%x\n", __func__,
				spmi_mst_nack, spmi_p_mst_nack);
			arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, 0x4,
				hwcidaddr_mt6363, &rdata, 1);
			arb->spmic->read_cmd(arb->spmic, SPMI_CMD_EXT_READL, 0x4,
				VIO18_SWITCH_6363, &rdata1, 1);
			pr_notice("%s 6363 CID/VIO18_SWITCH 0x%x/0x%x\n", __func__, rdata, rdata1);
		}
		mtk_spmi_writel(arb->spmimst_base[1], arb, 0x3, SPMI_REC_CTRL);
	}
	if (!((spmi_nack & 0xF8) || (spmi_rcs_nack & 0xC0000) ||
	      (spmi_debug_nack & 0xF0000) || (spmi_mst_nack & 0xC0000) ||
	      (spmi_p_nack & 0xF8) || (spmi_p_rcs_nack & 0xC0000) ||
	      (spmi_p_debug_nack & 0xF0000) || (spmi_p_mst_nack & 0xC0000))) {
		pr_notice("%s SPMI_REC0 m/p:0x%x/0x%x, SPMI_REC1 m/p:0x%x/0x%x\n",
			__func__, spmi_nack, spmi_p_nack, spmi_nack_data, spmi_p_nack_data);
		pr_notice("%s SPMI_REC_CMD_DEC m/p:0x%x/0x%x\n", __func__,
			spmi_rcs_nack, spmi_p_rcs_nack);
		pr_notice("%s SPMI_DEC_DBG m/p:0x%x/0x%x\n", __func__,
			spmi_debug_nack, spmi_p_debug_nack);
		pr_notice("%s SPMI_MST_DBG m/p:0x%x/0x%x\n", __func__,
			spmi_mst_nack, spmi_p_mst_nack);
		mtk_spmi_writel(arb->spmimst_base[0], arb, 0x3, SPMI_REC_CTRL);
		mtk_spmi_writel(arb->spmimst_base[1], arb, 0x3, SPMI_REC_CTRL);
		pr_notice("%s IRQ not cleared\n", __func__);
	}

	mutex_unlock(&arb->pmif_m_mutex);
	__pm_relax(arb->pmif_m_Thread_lock);

	return IRQ_HANDLED;
}

static int spmi_nack_irq_register(struct platform_device *pdev,
		struct pmif *arb, int irq)
{
	int ret = 0;

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
				spmi_nack_irq_handler,
				IRQF_TRIGGER_HIGH | IRQF_ONESHOT | IRQF_SHARED,
				"spmi_nack_irq", arb);
	if (ret < 0) {
		dev_notice(&pdev->dev, "request %s irq fail\n",
			"spmi_nack_irq");
	}
	return ret;
}

static void rcs_irq_lock(struct irq_data *data)
{
	struct pmif *arb = irq_data_get_irq_chip_data(data);

	mutex_lock(&arb->rcs_m_irqlock);
}

static void rcs_irq_sync_unlock(struct irq_data *data)
{
	struct pmif *arb = irq_data_get_irq_chip_data(data);

	mutex_unlock(&arb->rcs_m_irqlock);
}

static void rcs_irq_enable(struct irq_data *data)
{
	unsigned int hwirq = irqd_to_hwirq(data);
	struct pmif *arb = irq_data_get_irq_chip_data(data);

	WRITE_ONCE(arb->rcs_enable_hwirq[hwirq], true);
}

static void rcs_irq_disable(struct irq_data *data)
{
	unsigned int hwirq = irqd_to_hwirq(data);
	struct pmif *arb = irq_data_get_irq_chip_data(data);

	WRITE_ONCE(arb->rcs_enable_hwirq[hwirq], false);
}

static const struct irq_chip rcs_irq_chip = {
	.name			= "rcs_irq",
	.irq_bus_lock		= rcs_irq_lock,
	.irq_bus_sync_unlock	= rcs_irq_sync_unlock,
	.irq_enable		= rcs_irq_enable,
	.irq_disable		= rcs_irq_disable,
};

static const struct irq_chip rcs_irq_chip_p = {
	.name			= "rcs_irq_p",
	.irq_bus_lock		= rcs_irq_lock,
	.irq_bus_sync_unlock	= rcs_irq_sync_unlock,
	.irq_enable		= rcs_irq_enable,
	.irq_disable		= rcs_irq_disable,
};

static int rcs_irq_map(struct irq_domain *d, unsigned int virq,
			irq_hw_number_t hw)
{
	struct pmif *arb = d->host_data;
	unsigned int path = arb->slave_path[hw];

	irq_set_chip_data(virq, arb);
	irq_set_chip(virq, path ? &arb->irq_chip_p : &arb->irq_chip);
	irq_set_nested_thread(virq, 1);
	irq_set_parent(virq, path ? arb->rcs_irq_p : arb->rcs_irq);
	irq_set_noprobe(virq);
	return 0;
}

static const struct irq_domain_ops rcs_irq_domain_ops = {
	.map	= rcs_irq_map,
	.xlate	= irq_domain_xlate_onetwocell,
};

static irqreturn_t rcs_irq_handler(int irq, void *data)
{
	struct pmif *arb = data;
	unsigned int path = irq == arb->rcs_irq_p;
	unsigned int status, shift, virq;
	unsigned int i;
	irqreturn_t handled = IRQ_NONE;

	for (i = 0; i < SPMI_MAX_SLAVE_ID; i++) {
		shift = (i % 4) * 8;
		status = mtk_spmi_readl(arb->spmimst_base[path], arb,
				       SPMI_SLV_3_0_EINT + (i / 4));
		status = (status >> shift) & 0xff;
		if (!status)
			continue;

		/* Clear the whole byte, including events arriving after the read. */
		mtk_spmi_writel(arb->spmimst_base[path], arb, 0xff << shift,
				SPMI_SLV_3_0_EINT + (i / 4));
		handled = IRQ_HANDLED;
		if (arb->slave_path[i] != path ||
		    !READ_ONCE(arb->rcs_enable_hwirq[i]))
			continue;
		virq = irq_find_mapping(arb->domain, i);
		if (virq)
			handle_nested_irq(virq);
	}
	return handled;
}

static void rcs_irq_domain_remove(void *data)
{
	struct pmif *arb = data;
	unsigned int i, virq;

	for (i = 0; i < SPMI_MAX_SLAVE_ID; i++) {
		virq = irq_find_mapping(arb->domain, i);
		if (virq)
			irq_dispose_mapping(virq);
	}
	irq_domain_remove(arb->domain);
}

static void rcs_irq_wake_disable(void *data)
{
	disable_irq_wake(*(int *)data);
}

static int rcs_irq_register(struct platform_device *pdev, struct pmif *arb)
{
	int i, path, ret;
	int *irq;

	mutex_init(&arb->rcs_m_irqlock);
	mutex_init(&arb->rcs_p_irqlock);
	arb->irq_chip = rcs_irq_chip;
	arb->irq_chip_p = rcs_irq_chip_p;
	arb->rcs_enable_hwirq = devm_kcalloc(&pdev->dev, SPMI_MAX_SLAVE_ID,
					     sizeof(*arb->rcs_enable_hwirq), GFP_KERNEL);
	if (!arb->rcs_enable_hwirq)
		return -ENOMEM;

	/* Both parent IRQs feed one slave-ID domain. */
	arb->domain = irq_domain_create_linear(dev_fwnode(&pdev->dev),
				SPMI_MAX_SLAVE_ID, &rcs_irq_domain_ops, arb);
	if (!arb->domain)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, rcs_irq_domain_remove, arb);
	if (ret)
		return ret;

	for (path = 0; path < 2; path++) {
		for (i = 0; i < SPMI_MAX_SLAVE_ID / 4; i++)
			mtk_spmi_writel(arb->spmimst_base[path], arb, U32_MAX,
					SPMI_SLV_3_0_EINT + i);
		irq = path ? &arb->rcs_irq_p : &arb->rcs_irq;
		ret = devm_request_threaded_irq(&pdev->dev, *irq, NULL,
				rcs_irq_handler, IRQF_ONESHOT,
				path ? rcs_irq_chip_p.name : rcs_irq_chip.name, arb);
		if (ret)
			return ret;
		ret = enable_irq_wake(*irq);
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(&pdev->dev, rcs_irq_wake_disable, irq);
		if (ret)
			return ret;
	}
	return 0;
}


static void pmif_wakeup_unregister(void *data)
{
	wakeup_source_unregister(data);
}

static int mtk_spmi_probe(struct platform_device *pdev)
{
	struct pmif *arb;
	const struct pmif *match;
	struct spmi_controller *ctrl;
	int err;

	ctrl = devm_spmi_controller_alloc(&pdev->dev, sizeof(*arb));
	if (!ctrl)
		return -ENOMEM;
	match = device_get_match_data(&pdev->dev);
	arb = spmi_controller_get_drvdata(ctrl);
	*arb = *match;
	arb->spmic = ctrl;
	ctrl->cmd = pmif_arb_cmd;
	ctrl->read_cmd = pmif_spmi_read_cmd;
	ctrl->write_cmd = pmif_spmi_write_cmd;

	arb->pmif_base[0] = devm_platform_ioremap_resource_byname(pdev, "pmif");
	if (IS_ERR(arb->pmif_base[0]))
		return PTR_ERR(arb->pmif_base[0]);
	arb->spmimst_base[0] = devm_platform_ioremap_resource_byname(pdev, "spmimst");
	if (IS_ERR(arb->spmimst_base[0]))
		return PTR_ERR(arb->spmimst_base[0]);
	arb->pmif_base[1] = devm_platform_ioremap_resource_byname(pdev, "pmif-p");
	if (IS_ERR(arb->pmif_base[1]))
		return PTR_ERR(arb->pmif_base[1]);
	arb->spmimst_base[1] = devm_platform_ioremap_resource_byname(pdev, "spmimst-p");
	if (IS_ERR(arb->spmimst_base[1]))
		return PTR_ERR(arb->spmimst_base[1]);

	arb->irq = platform_get_irq_byname(pdev, "pmif_irq");
	if (arb->irq < 0)
		return arb->irq;
	arb->irq_p = platform_get_irq_byname(pdev, "pmif_p_irq");
	if (arb->irq_p < 0)
		return arb->irq_p;
	arb->rcs_irq = platform_get_irq_byname(pdev, "rcs_irq");
	if (arb->rcs_irq < 0)
		return arb->rcs_irq;
	arb->rcs_irq_p = platform_get_irq_byname(pdev, "rcs_irq_p");
	if (arb->rcs_irq_p < 0)
		return arb->rcs_irq_p;
	arb->spmi_nack_irq = platform_get_irq_byname(pdev, "spmi_nack_irq");
	if (arb->spmi_nack_irq < 0)
		return arb->spmi_nack_irq;
	arb->spmi_p_nack_irq = platform_get_irq_byname(pdev, "spmi_p_nack_irq");
	if (arb->spmi_p_nack_irq < 0)
		return arb->spmi_p_nack_irq;

	err = of_property_read_u32_array(pdev->dev.of_node, "hwinf-err-irq-idx",
				arb->hwintf_err_idx, ARRAY_SIZE(arb->hwintf_err_idx));
	if (err)
		return err;
	err = of_property_read_u32_array(pdev->dev.of_node, "swinf-err-irq-idx",
				arb->swintf_err_idx, ARRAY_SIZE(arb->swintf_err_idx));
	if (err)
		return err;
	spmi_dev_parse(pdev, arb);

	/* caps2 uses the dual PMIF already initialized by the boot firmware. */
	arb->chan.ch_sta = PMIF_SWINF_0_STA + PMIF_CHAN_OFFSET * arb->soc_chan;
	arb->chan.wdata = PMIF_SWINF_0_WDATA_31_0 + PMIF_CHAN_OFFSET * arb->soc_chan;
	arb->chan.rdata = PMIF_SWINF_0_RDATA_31_0 + PMIF_CHAN_OFFSET * arb->soc_chan;
	arb->chan.ch_send = PMIF_SWINF_0_ACC + PMIF_CHAN_OFFSET * arb->soc_chan;
	arb->chan.ch_rdy = PMIF_SWINF_0_VLD_CLR + PMIF_CHAN_OFFSET * arb->soc_chan;
	raw_spin_lock_init(&arb->lock_m);
	raw_spin_lock_init(&arb->lock_p);
	mutex_init(&arb->pmif_m_mutex);
	mutex_init(&arb->pmif_p_mutex);
	arb->pmif_m_Thread_lock = wakeup_source_register(&pdev->dev, "pmif_m wakelock");
	if (!arb->pmif_m_Thread_lock)
		return -ENOMEM;
	err = devm_add_action_or_reset(&pdev->dev, pmif_wakeup_unregister,
				     arb->pmif_m_Thread_lock);
	if (err)
		return err;
	arb->pmif_p_Thread_lock = wakeup_source_register(&pdev->dev, "pmif_p wakelock");
	if (!arb->pmif_p_Thread_lock)
		return -ENOMEM;
	err = devm_add_action_or_reset(&pdev->dev, pmif_wakeup_unregister,
				     arb->pmif_p_Thread_lock);
	if (err)
		return err;

	mt6989_spmi_pmif_dbg_init(ctrl);
	err = mt6989_spmi_pmif_create_attr(ctrl);
	if (err)
		dev_warn(&pdev->dev, "SPMI debugfs unavailable: %d\n", err);
	err = rcs_irq_register(pdev, arb);
	if (err)
		return err;
	err = pmif_irq_register(pdev, arb);
	if (err)
		return err;
	err = spmi_nack_irq_register(pdev, arb, arb->spmi_nack_irq);
	if (err)
		return err;
	err = spmi_nack_irq_register(pdev, arb, arb->spmi_p_nack_irq);
	if (err)
		return err;

	platform_set_drvdata(pdev, ctrl);
	return spmi_controller_add(ctrl);
}

static void mtk_spmi_remove(struct platform_device *pdev)
{
	struct spmi_controller *ctrl = platform_get_drvdata(pdev);

	spmi_controller_remove(ctrl);
}

static const struct of_device_id mtk_spmi_match_table[] = {
	{ .compatible = "mediatek,mt6989-spmi", .data = &mt6989_pmif_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_spmi_match_table);

static struct platform_driver mtk_spmi_driver = {
	.driver		= {
		.name	= "spmi-mt6989",
		.of_match_table = of_match_ptr(mtk_spmi_match_table),
	},
	.probe		= mtk_spmi_probe,
	.remove		= mtk_spmi_remove,
};
module_platform_driver(mtk_spmi_driver);

MODULE_AUTHOR("Hsin-Hsiung Wang <hsin-hsiung.wang@mediatek.com>");
MODULE_DESCRIPTION("MediaTek SPMI Driver");
MODULE_LICENSE("GPL");
