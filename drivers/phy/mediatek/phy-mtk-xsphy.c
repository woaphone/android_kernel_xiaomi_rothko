// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek USB3.1 gen2 xsphy Driver
 *
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Chunfeng Yun <chunfeng.yun@mediatek.com>
 *
 */

#include <dt-bindings/phy/phy.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "phy-mtk-io.h"

/* u2 phy banks */
#define SSUSB_SIFSLV_MISC		0x000
#define SSUSB_SIFSLV_U2FREQ		0x100
#define SSUSB_SIFSLV_U2PHY_COM	0x300

/* u3 phy shared banks */
#define SSPXTP_SIFSLV_DIG_GLB		0x000
#define SSPXTP_SIFSLV_PHYA_GLB		0x100

/* u3 phy banks */
#define SSPXTP_SIFSLV_DIG_LN_TOP	0x000
#define SSPXTP_SIFSLV_DIG_LN_TX0	0x100
#define SSPXTP_SIFSLV_DIG_LN_RX0	0x200
#define SSPXTP_SIFSLV_DIG_LN_DAIF	0x300
#define SSPXTP_SIFSLV_PHYA_LN		0x400

#define XSP_U2FREQ_FMCR0	((SSUSB_SIFSLV_U2FREQ) + 0x00)
#define P2F_RG_FREQDET_EN	BIT(24)
#define P2F_RG_CYCLECNT		GENMASK(23, 0)

#define XSP_U2FREQ_MMONR0  ((SSUSB_SIFSLV_U2FREQ) + 0x0c)

#define XSP_U2FREQ_FMMONR1	((SSUSB_SIFSLV_U2FREQ) + 0x10)
#define P2F_RG_FRCK_EN		BIT(8)
#define P2F_USB_FM_VALID	BIT(0)

#define XSP_USBPHYACR0	((SSUSB_SIFSLV_U2PHY_COM) + 0x00)
#define P2A0_RG_INTR_EN	BIT(5)

#define XSP_USBPHYACR1		((SSUSB_SIFSLV_U2PHY_COM) + 0x04)
#define P2A1_RG_INTR_CAL		GENMASK(23, 19)
#define P2A1_RG_VRT_SEL			GENMASK(14, 12)
#define P2A1_RG_TERM_SEL		GENMASK(10, 8)

#define XSP_USBPHYACR5		((SSUSB_SIFSLV_U2PHY_COM) + 0x014)
#define P2A5_RG_HSTX_SRCAL_EN	BIT(15)
#define P2A5_RG_HSTX_SRCTRL		GENMASK(14, 12)

#define XSP_USBPHYACR6		((SSUSB_SIFSLV_U2PHY_COM) + 0x018)
#define P2A6_RG_BC11_SW_EN	BIT(23)
#define P2A6_RG_OTG_VBUSCMP_EN	BIT(20)

#define XSP_U2PHYDTM1		((SSUSB_SIFSLV_U2PHY_COM) + 0x06C)
#define P2D_FORCE_IDDIG		BIT(9)
#define P2D_RG_VBUSVALID	BIT(5)
#define P2D_RG_SESSEND		BIT(4)
#define P2D_RG_AVALID		BIT(2)
#define P2D_RG_IDDIG		BIT(1)

#define SSPXTP_PHYA_GLB_00		((SSPXTP_SIFSLV_PHYA_GLB) + 0x00)
#define RG_XTP_GLB_BIAS_INTR_CTRL		GENMASK(21, 16)

#define SSPXTP_PHYA_LN_04	((SSPXTP_SIFSLV_PHYA_LN) + 0x04)
#define RG_XTP_LN0_TX_IMPSEL		GENMASK(4, 0)

#define SSPXTP_PHYA_LN_14	((SSPXTP_SIFSLV_PHYA_LN) + 0x014)
#define RG_XTP_LN0_RX_IMPSEL		GENMASK(4, 0)

#define XSP_REF_CLK		26	/* MHZ */
#define XSP_SLEW_RATE_COEF	17
#define XSP_SR_COEF_DIVISOR	1000
#define XSP_FM_DET_CYCLE_CNT	1024

/* PHY switch between pcie/usb3/sgmii */
#define USB_PHY_SWITCH_CTRL	0x0
#define RG_PHY_SW_TYPE		GENMASK(3, 0)
#define RG_PHY_SW_PCIE		0x0
#define RG_PHY_SW_USB3		0x1
#define RG_PHY_SW_SGMII		0x2

struct xsphy_instance {
	struct phy *phy;
	void __iomem *port_base;
	struct clk *ref_clk;	/* reference clock of anolog phy */
	u32 index;
	u32 type;
	struct regmap *type_sw;
	u32 type_sw_reg;
	u32 type_sw_index;
	/* only for HQA test */
	int efuse_intr;
	int efuse_tx_imp;
	int efuse_rx_imp;
	/* u2 eye diagram */
	int eye_src;
	int eye_vrt;
	int eye_term;
	int efuse_term_cal;
	int intr_ofs, host_intr_ofs;
	int term_ofs, host_term_ofs;
	int discth, rx_sqth, host_rx_sqth, rev6;
	int eye_src_host, eye_vrt_host, eye_term_host, rev6_host;
	int pll_fbksel, pll_posdiv;
	int tx_lctxcm1, tx_lctxc0, tx_lctxcp1;
	bool u3_rx_fix, u3_gen2_hqa;
	bool lpm_quirk;
	u32 lpm_para[3];
};

struct mtk_xsphy {
	struct device *dev;
	void __iomem *glb_base;	/* only shared u3 sif */
	struct xsphy_instance **phys;
	int nphys;
	int src_ref_clk; /* MHZ, reference clock for slew rate calibrate */
	int src_coef;    /* coefficient for slew rate calibrate */
	bool mt6989;
	bool tx_chirpK_disable;
	bool bc11_switch_disable;
};

#include "phy-mtk-xsphy-mt6989.h"

static void u2_phy_slew_rate_calibrate(struct mtk_xsphy *xsphy,
					struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;
	int calib_val;
	int fm_out;
	u32 tmp;

	/* use force value */
	if (xsphy->mt6989 ? inst->eye_src >= 0 : inst->eye_src)
		return;

	/* enable USB ring oscillator */
	mtk_phy_set_bits(pbase + XSP_USBPHYACR5, P2A5_RG_HSTX_SRCAL_EN);
	udelay(1);	/* wait clock stable */

	/* enable free run clock */
	mtk_phy_set_bits(pbase + XSP_U2FREQ_FMMONR1, P2F_RG_FRCK_EN);

	/* set cycle count as 1024 */
	mtk_phy_update_field(pbase + XSP_U2FREQ_FMCR0, P2F_RG_CYCLECNT,
			     XSP_FM_DET_CYCLE_CNT);

	/* enable frequency meter */
	mtk_phy_set_bits(pbase + XSP_U2FREQ_FMCR0, P2F_RG_FREQDET_EN);

	/* ignore return value */
	readl_poll_timeout(pbase + XSP_U2FREQ_FMMONR1, tmp,
			   (tmp & P2F_USB_FM_VALID), 10, 200);

	fm_out = readl(pbase + XSP_U2FREQ_MMONR0);

	/* disable frequency meter */
	mtk_phy_clear_bits(pbase + XSP_U2FREQ_FMCR0, P2F_RG_FREQDET_EN);

	/* disable free run clock */
	mtk_phy_clear_bits(pbase + XSP_U2FREQ_FMMONR1, P2F_RG_FRCK_EN);

	if (fm_out) {
		/* (1024 / FM_OUT) x reference clock frequency x coefficient */
		tmp = xsphy->src_ref_clk * xsphy->src_coef;
		tmp = (tmp * XSP_FM_DET_CYCLE_CNT) / fm_out;
		calib_val = DIV_ROUND_CLOSEST(tmp, XSP_SR_COEF_DIVISOR);
	} else {
		/* if FM detection fail, set default value */
		calib_val = 3;
	}
	dev_dbg(xsphy->dev, "phy.%d, fm_out:%d, calib:%d (clk:%d, coef:%d)\n",
		inst->index, fm_out, calib_val,
		xsphy->src_ref_clk, xsphy->src_coef);

	/* set HS slew rate */
	mtk_phy_update_field(pbase + XSP_USBPHYACR5, P2A5_RG_HSTX_SRCTRL, calib_val);

	/* disable USB ring oscillator */
	mtk_phy_clear_bits(pbase + XSP_USBPHYACR5, P2A5_RG_HSTX_SRCAL_EN);
}

static void u2_phy_instance_init(struct mtk_xsphy *xsphy,
				 struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;

	/* DP/DM BC1.1 path Disable */
	mtk_phy_clear_bits(pbase + XSP_USBPHYACR6, P2A6_RG_BC11_SW_EN);

	mtk_phy_set_bits(pbase + XSP_USBPHYACR0, P2A0_RG_INTR_EN);
}

static void u2_phy_instance_power_on(struct mtk_xsphy *xsphy,
				     struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;
	u32 index = inst->index;

	mtk_phy_set_bits(pbase + XSP_USBPHYACR6, P2A6_RG_OTG_VBUSCMP_EN);

	mtk_phy_update_bits(pbase + XSP_U2PHYDTM1,
			    P2D_RG_VBUSVALID | P2D_RG_AVALID | P2D_RG_SESSEND,
			    P2D_RG_VBUSVALID | P2D_RG_AVALID);

	dev_dbg(xsphy->dev, "%s(%d)\n", __func__, index);
}

static void u2_phy_instance_power_off(struct mtk_xsphy *xsphy,
				      struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;
	u32 index = inst->index;

	mtk_phy_clear_bits(pbase + XSP_USBPHYACR6, P2A6_RG_OTG_VBUSCMP_EN);

	mtk_phy_update_bits(pbase + XSP_U2PHYDTM1,
			    P2D_RG_VBUSVALID | P2D_RG_AVALID | P2D_RG_SESSEND,
			    P2D_RG_SESSEND);

	dev_dbg(xsphy->dev, "%s(%d)\n", __func__, index);
}

static void u2_phy_instance_set_mode(struct mtk_xsphy *xsphy,
				     struct xsphy_instance *inst,
				     enum phy_mode mode)
{
	u32 tmp;

	tmp = readl(inst->port_base + XSP_U2PHYDTM1);
	switch (mode) {
	case PHY_MODE_USB_DEVICE:
		tmp |= P2D_FORCE_IDDIG | P2D_RG_IDDIG;
		break;
	case PHY_MODE_USB_HOST:
		tmp |= P2D_FORCE_IDDIG;
		tmp &= ~P2D_RG_IDDIG;
		break;
	case PHY_MODE_USB_OTG:
		tmp &= ~(P2D_FORCE_IDDIG | P2D_RG_IDDIG);
		break;
	default:
		return;
	}
	writel(tmp, inst->port_base + XSP_U2PHYDTM1);
}

static void mt6989_phy_parse_property(struct xsphy_instance *inst)
{
	struct device *dev = &inst->phy->dev;
	const char *offset;

#define READ_PARAM(property, member) \
	do { \
		if (device_property_read_u32(dev, "mediatek," property, \
					    &inst->member)) \
			inst->member = -EINVAL; \
	} while (0)
	READ_PARAM("efuse-intr", efuse_intr);
	if (inst->type == PHY_TYPE_USB2) {
		READ_PARAM("efuse-term", efuse_term_cal);
		READ_PARAM("eye-src", eye_src);
		READ_PARAM("eye-vrt", eye_vrt);
		READ_PARAM("eye-term", eye_term);
		READ_PARAM("discth", discth);
		READ_PARAM("rx-sqth", rx_sqth);
		READ_PARAM("host-rx-sqth", host_rx_sqth);
		READ_PARAM("rev6", rev6);
		READ_PARAM("eye-src-host", eye_src_host);
		READ_PARAM("eye-vrt-host", eye_vrt_host);
		READ_PARAM("eye-term-host", eye_term_host);
		READ_PARAM("rev6-host", rev6_host);
		READ_PARAM("pll-fbksel", pll_fbksel);
		READ_PARAM("pll-posdiv", pll_posdiv);
		if (!device_property_read_u32_array(dev, "mediatek,lpm-parameter",
						inst->lpm_para, PHY_PLL_PARA_CNT))
			inst->lpm_quirk = true;
#define READ_OFFSET(property, member, mask) \
		do { \
			if (device_property_read_string(dev, "mediatek," property, &offset) || \
			    kstrtoint(offset, 10, &inst->member) < 0) \
				inst->member = -(mask + 1); \
		} while (0)
		READ_OFFSET("intr-ofs", intr_ofs, P2AR_RG_INTR_CAL_MASK);
		READ_OFFSET("host-intr-ofs", host_intr_ofs, P2AR_RG_INTR_CAL_MASK);
		READ_OFFSET("term-ofs", term_ofs, P2ARA_RG_TERM_CAL_MASK);
		READ_OFFSET("host-term-ofs", host_term_ofs, P2ARA_RG_TERM_CAL_MASK);
#undef READ_OFFSET
	} else if (inst->type == PHY_TYPE_USB3) {
		READ_PARAM("efuse-tx-imp", efuse_tx_imp);
		READ_PARAM("efuse-rx-imp", efuse_rx_imp);
		READ_PARAM("tx-lctxcm1", tx_lctxcm1);
		READ_PARAM("tx-lctxc0", tx_lctxc0);
		READ_PARAM("tx-lctxcp1", tx_lctxcp1);
		inst->u3_rx_fix = device_property_read_bool(dev, "mediatek,u3-rx-fix");
		inst->u3_gen2_hqa = device_property_read_bool(dev, "mediatek,u3-gen2-hqa");
	}
#undef READ_PARAM
}

static int mt6989_phy_read_efuse(struct xsphy_instance *inst,
			       const char *name, int *parameter)
{
	struct device *dev = &inst->phy->dev;
	u32 mask, value;
	int index, ret;

	index = of_property_match_string(dev->of_node, "nvmem-cell-names", name);
	if (index < 0)
		return 0;
	ret = of_property_read_u32_index(dev->of_node, "nvmem-cell-masks", index, &mask);
	if (ret || !mask)
		return ret ? ret : -EINVAL;
	ret = nvmem_cell_read_u32(dev, name, &value);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read %s calibration\n", name);
	value = (value & mask) >> (ffs(mask) - 1);
	/* A zero efuse field retains the DT or hardware default, as in the donor. */
	if (value)
		*parameter = value;
	return 0;
}

static int mt6989_phy_parse_efuse(struct xsphy_instance *inst)
{
	int ret;

	if (inst->type == PHY_TYPE_USB2) {
		ret = mt6989_phy_read_efuse(inst, "intr_cal", &inst->efuse_intr);
		if (ret)
			return ret;
		return mt6989_phy_read_efuse(inst, "term_cal", &inst->efuse_term_cal);
	}
	if (inst->type == PHY_TYPE_USB3) {
		ret = mt6989_phy_read_efuse(inst, "iext_intr_ctrl", &inst->efuse_intr);
		if (ret)
			return ret;
		ret = mt6989_phy_read_efuse(inst, "rx_impsel", &inst->efuse_rx_imp);
		if (ret)
			return ret;
		return mt6989_phy_read_efuse(inst, "tx_impsel", &inst->efuse_tx_imp);
	}
	return 0;
}

static void phy_parse_property(struct mtk_xsphy *xsphy,
				struct xsphy_instance *inst)
{
	struct device *dev = &inst->phy->dev;

	switch (inst->type) {
	case PHY_TYPE_USB2:
		device_property_read_u32(dev, "mediatek,efuse-intr",
					 &inst->efuse_intr);
		device_property_read_u32(dev, "mediatek,eye-src",
					 &inst->eye_src);
		device_property_read_u32(dev, "mediatek,eye-vrt",
					 &inst->eye_vrt);
		device_property_read_u32(dev, "mediatek,eye-term",
					 &inst->eye_term);
		dev_dbg(dev, "intr:%d, src:%d, vrt:%d, term:%d\n",
			inst->efuse_intr, inst->eye_src,
			inst->eye_vrt, inst->eye_term);
		break;
	case PHY_TYPE_USB3:
		device_property_read_u32(dev, "mediatek,efuse-intr",
					 &inst->efuse_intr);
		device_property_read_u32(dev, "mediatek,efuse-tx-imp",
					 &inst->efuse_tx_imp);
		device_property_read_u32(dev, "mediatek,efuse-rx-imp",
					 &inst->efuse_rx_imp);
		dev_dbg(dev, "intr:%d, tx-imp:%d, rx-imp:%d\n",
			inst->efuse_intr, inst->efuse_tx_imp,
			inst->efuse_rx_imp);
		break;
	case PHY_TYPE_PCIE:
	case PHY_TYPE_SGMII:
		/* nothing to do */
		break;
	default:
		dev_err(xsphy->dev, "incompatible phy type\n");
		return;
	}
}

static void u2_phy_props_set(struct mtk_xsphy *xsphy,
			     struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;

	if (inst->efuse_intr)
		mtk_phy_update_field(pbase + XSP_USBPHYACR1, P2A1_RG_INTR_CAL,
				     inst->efuse_intr);

	if (inst->eye_src)
		mtk_phy_update_field(pbase + XSP_USBPHYACR5, P2A5_RG_HSTX_SRCTRL,
				     inst->eye_src);

	if (inst->eye_vrt)
		mtk_phy_update_field(pbase + XSP_USBPHYACR1, P2A1_RG_VRT_SEL,
				     inst->eye_vrt);

	if (inst->eye_term)
		mtk_phy_update_field(pbase + XSP_USBPHYACR1, P2A1_RG_TERM_SEL,
				     inst->eye_term);
}

static void u3_phy_props_set(struct mtk_xsphy *xsphy,
			     struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;

	if (inst->efuse_intr)
		mtk_phy_update_field(xsphy->glb_base + SSPXTP_PHYA_GLB_00,
				     RG_XTP_GLB_BIAS_INTR_CTRL, inst->efuse_intr);

	if (inst->efuse_tx_imp)
		mtk_phy_update_field(pbase + SSPXTP_PHYA_LN_04,
				     RG_XTP_LN0_TX_IMPSEL, inst->efuse_tx_imp);

	if (inst->efuse_rx_imp)
		mtk_phy_update_field(pbase + SSPXTP_PHYA_LN_14,
				     RG_XTP_LN0_RX_IMPSEL, inst->efuse_rx_imp);
}

/* type switch for usb3/pcie/sgmii */
static int phy_type_syscon_get(struct xsphy_instance *instance,
			       struct device_node *dn)
{
	struct of_phandle_args args;
	int ret;

	/* type switch function is optional */
	if (!of_property_present(dn, "mediatek,syscon-type"))
		return 0;

	ret = of_parse_phandle_with_fixed_args(dn, "mediatek,syscon-type",
					       2, 0, &args);
	if (ret)
		return ret;

	instance->type_sw_reg = args.args[0];
	instance->type_sw_index = args.args[1] & 0x3; /* <=3 */
	instance->type_sw = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	dev_info(&instance->phy->dev, "type_sw - reg %#x, index %d\n",
		 instance->type_sw_reg, instance->type_sw_index);

	return PTR_ERR_OR_ZERO(instance->type_sw);
}

static int phy_type_set(struct xsphy_instance *instance)
{
	int type;
	u32 offset;

	if (!instance->type_sw)
		return 0;

	switch (instance->type) {
	case PHY_TYPE_USB3:
		type = RG_PHY_SW_USB3;
		break;
	case PHY_TYPE_PCIE:
		type = RG_PHY_SW_PCIE;
		break;
	case PHY_TYPE_SGMII:
		type = RG_PHY_SW_SGMII;
		break;
	case PHY_TYPE_USB2:
	default:
		return 0;
	}

	offset = instance->type_sw_index * BITS_PER_BYTE;
	regmap_update_bits(instance->type_sw, instance->type_sw_reg,
			   RG_PHY_SW_TYPE << offset, type << offset);

	return 0;
}

static int mtk_phy_init(struct phy *phy)
{
	struct xsphy_instance *inst = phy_get_drvdata(phy);
	struct mtk_xsphy *xsphy = dev_get_drvdata(phy->dev.parent);
	int ret;

	ret = clk_prepare_enable(inst->ref_clk);
	if (ret) {
		dev_err(xsphy->dev, "failed to enable ref_clk\n");
		return ret;
	}

	switch (inst->type) {
	case PHY_TYPE_USB2:
		if (xsphy->mt6989) {
			mt6989_u2_phy_instance_init(xsphy, inst);
			mt6989_u2_phy_props_set(xsphy, inst);
		} else {
			u2_phy_instance_init(xsphy, inst);
			u2_phy_props_set(xsphy, inst);
		}
		break;
	case PHY_TYPE_USB3:
		if (xsphy->mt6989)
			mt6989_u3_phy_props_set(xsphy, inst);
		else
			u3_phy_props_set(xsphy, inst);
		break;
	case PHY_TYPE_PCIE:
	case PHY_TYPE_SGMII:
		/* nothing to do, only used to set type */
		break;
	default:
		dev_err(xsphy->dev, "incompatible phy type\n");
		clk_disable_unprepare(inst->ref_clk);
		return -EINVAL;
	}

	return 0;
}

static int mtk_phy_power_on(struct phy *phy)
{
	struct xsphy_instance *inst = phy_get_drvdata(phy);
	struct mtk_xsphy *xsphy = dev_get_drvdata(phy->dev.parent);

	if (inst->type == PHY_TYPE_USB2) {
		if (xsphy->mt6989)
			mt6989_u2_phy_instance_power_on(xsphy, inst);
		else
			u2_phy_instance_power_on(xsphy, inst);
		u2_phy_slew_rate_calibrate(xsphy, inst);
		if (xsphy->mt6989) {
			if (phy->attrs.mode == PHY_MODE_USB_HOST)
				mt6989_u2_phy_host_props_set(xsphy, inst);
			else
				mt6989_u2_phy_props_set(xsphy, inst);
		}
	} else if (xsphy->mt6989 && inst->type == PHY_TYPE_USB3) {
		mt6989_u3_phy_instance_power_on(xsphy, inst);
		mt6989_u3_phy_props_set(xsphy, inst);
	}
	if (xsphy->mt6989 && xsphy->tx_chirpK_disable && inst->type == PHY_TYPE_USB2)
		mtk_phy_set_bits(inst->port_base + XSP_USBPHYA_RESERVEA1,
				 P2ARA_RG_TX_CHIRPK);

	return 0;
}

static int mtk_phy_power_off(struct phy *phy)
{
	struct xsphy_instance *inst = phy_get_drvdata(phy);
	struct mtk_xsphy *xsphy = dev_get_drvdata(phy->dev.parent);

	if (xsphy->mt6989 && xsphy->tx_chirpK_disable && inst->type == PHY_TYPE_USB2)
		mtk_phy_clear_bits(inst->port_base + XSP_USBPHYA_RESERVEA1,
				   P2ARA_RG_TX_CHIRPK);
	if (inst->type == PHY_TYPE_USB2) {
		if (xsphy->mt6989)
			mt6989_u2_phy_instance_power_off(xsphy, inst);
		else
			u2_phy_instance_power_off(xsphy, inst);
	} else if (xsphy->mt6989 && inst->type == PHY_TYPE_USB3) {
		mt6989_u3_phy_instance_power_off(xsphy, inst);
	}

	return 0;
}

static int mtk_phy_exit(struct phy *phy)
{
	struct xsphy_instance *inst = phy_get_drvdata(phy);

	clk_disable_unprepare(inst->ref_clk);
	return 0;
}

static int mtk_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct xsphy_instance *inst = phy_get_drvdata(phy);
	struct mtk_xsphy *xsphy = dev_get_drvdata(phy->dev.parent);

	if (inst->type == PHY_TYPE_USB2) {
		if (xsphy->mt6989)
			mt6989_u2_phy_instance_set_mode(xsphy, inst, mode, submode);
		else
			u2_phy_instance_set_mode(xsphy, inst, mode);
	}

	return 0;
}

static struct phy *mtk_phy_xlate(struct device *dev,
				 const struct of_phandle_args *args)
{
	struct mtk_xsphy *xsphy = dev_get_drvdata(dev);
	struct xsphy_instance *inst = NULL;
	struct device_node *phy_np = args->np;
	int index;

	if (args->args_count != 1) {
		dev_err(dev, "invalid number of cells in 'phy' property\n");
		return ERR_PTR(-EINVAL);
	}

	for (index = 0; index < xsphy->nphys; index++)
		if (phy_np == xsphy->phys[index]->phy->dev.of_node) {
			inst = xsphy->phys[index];
			break;
		}

	if (!inst) {
		dev_err(dev, "failed to find appropriate phy\n");
		return ERR_PTR(-EINVAL);
	}

	inst->type = args->args[0];
	if (!(inst->type == PHY_TYPE_USB2 ||
	      inst->type == PHY_TYPE_USB3 ||
	      inst->type == PHY_TYPE_PCIE ||
	      inst->type == PHY_TYPE_SGMII)) {
		dev_err(dev, "unsupported phy type: %d\n", inst->type);
		return ERR_PTR(-EINVAL);
	}

	if (xsphy->mt6989) {
		int ret;

		mt6989_phy_parse_property(inst);
		ret = mt6989_phy_parse_efuse(inst);
		if (ret)
			return ERR_PTR(ret);
	} else {
		phy_parse_property(xsphy, inst);
	}
	phy_type_set(inst);

	return inst->phy;
}

static const struct phy_ops mtk_xsphy_ops = {
	.init		= mtk_phy_init,
	.exit		= mtk_phy_exit,
	.power_on	= mtk_phy_power_on,
	.power_off	= mtk_phy_power_off,
	.set_mode	= mtk_phy_set_mode,
	.owner		= THIS_MODULE,
};

static const struct of_device_id mtk_xsphy_id_table[] = {
	{ .compatible = "mediatek,xsphy", },
	{ },
};
MODULE_DEVICE_TABLE(of, mtk_xsphy_id_table);

static int mtk_xsphy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct phy_provider *provider;
	struct resource *glb_res;
	struct mtk_xsphy *xsphy;
	struct resource res;
	int port;

	xsphy = devm_kzalloc(dev, sizeof(*xsphy), GFP_KERNEL);
	if (!xsphy)
		return -ENOMEM;

	xsphy->nphys = of_get_child_count(np);
	xsphy->phys = devm_kcalloc(dev, xsphy->nphys,
				       sizeof(*xsphy->phys), GFP_KERNEL);
	if (!xsphy->phys)
		return -ENOMEM;

	xsphy->dev = dev;
	xsphy->mt6989 = of_machine_is_compatible("mediatek,MT6989");
	xsphy->tx_chirpK_disable = device_property_read_bool(dev, "tx-chirpk-capable");
	xsphy->bc11_switch_disable = device_property_read_bool(dev, "bc11-switch-disable");
	platform_set_drvdata(pdev, xsphy);

	glb_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	/* optional, may not exist if no u3 phys */
	if (glb_res) {
		/* get banks shared by multiple u3 phys */
		xsphy->glb_base = devm_ioremap_resource(dev, glb_res);
		if (IS_ERR(xsphy->glb_base)) {
			dev_err(dev, "failed to remap glb regs\n");
			return PTR_ERR(xsphy->glb_base);
		}
	}

	xsphy->src_ref_clk = XSP_REF_CLK;
	xsphy->src_coef = XSP_SLEW_RATE_COEF;
	/* update parameters of slew rate calibrate if exist */
	device_property_read_u32(dev, "mediatek,src-ref-clk-mhz",
				 &xsphy->src_ref_clk);
	device_property_read_u32(dev, "mediatek,src-coef", &xsphy->src_coef);

	port = 0;
	for_each_child_of_node_scoped(np, child_np) {
		struct xsphy_instance *inst;
		struct phy *phy;
		int retval;

		inst = devm_kzalloc(dev, sizeof(*inst), GFP_KERNEL);
		if (!inst)
			return -ENOMEM;

		xsphy->phys[port] = inst;

		phy = devm_phy_create(dev, child_np, &mtk_xsphy_ops);
		if (IS_ERR(phy)) {
			dev_err(dev, "failed to create phy\n");
			return PTR_ERR(phy);
		}

		retval = of_address_to_resource(child_np, 0, &res);
		if (retval) {
			dev_err(dev, "failed to get address resource(id-%d)\n",
				port);
			return retval;
		}

		inst->port_base = devm_ioremap_resource(&phy->dev, &res);
		if (IS_ERR(inst->port_base)) {
			dev_err(dev, "failed to remap phy regs\n");
			return PTR_ERR(inst->port_base);
		}

		inst->phy = phy;
		inst->index = port;
		phy_set_drvdata(phy, inst);
		port++;

		inst->ref_clk = devm_clk_get(&phy->dev, "ref");
		if (IS_ERR(inst->ref_clk)) {
			dev_err(dev, "failed to get ref_clk(id-%d)\n", port);
			return PTR_ERR(inst->ref_clk);
		}

		retval = phy_type_syscon_get(inst, child_np);
		if (retval)
			return retval;
	}

	provider = devm_of_phy_provider_register(dev, mtk_phy_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static struct platform_driver mtk_xsphy_driver = {
	.probe		= mtk_xsphy_probe,
	.driver		= {
		.name	= "mtk-xsphy",
		.of_match_table = mtk_xsphy_id_table,
	},
};

module_platform_driver(mtk_xsphy_driver);

MODULE_AUTHOR("Chunfeng Yun <chunfeng.yun@mediatek.com>");
MODULE_DESCRIPTION("MediaTek USB XS-PHY driver");
MODULE_LICENSE("GPL v2");
