/* SPDX-License-Identifier: GPL-2.0 */
/* MT6989 register layout and hardware sequences from MediaTek's XSPHY driver. */
#ifndef __PHY_MTK_XSPHY_MT6989_H__
#define __PHY_MTK_XSPHY_MT6989_H__

#define XSP_MISC_REG0		((SSUSB_SIFSLV_MISC) + 0x00)
#define MR0_RG_MISC_TIMER_1US		GENMASK(15, 8)
#define MR0_RG_MISC_STABLE_TIME		GENMASK(23, 16)
#define MR0_RG_MISC_BANDGAP_STABLE_TIME		GENMASK(31, 24)
#define XSP_MISC_REG1		((SSUSB_SIFSLV_MISC) + 0x04)
#define MR1_RG_MISC_PLL_STABLE_SEL	BIT(16)
#define P2A0_RG_USB20_TX_PH_ROT_SEL		GENMASK(26, 24)
#define P2A6_RG_U2_PHY_REV6		GENMASK(31, 30)
#define P2A6_RG_U2_PHY_REV6_VAL(x)	((0x3 & (x)) << 30)
#define P2A6_RG_U2_PHY_REV1		BIT(25)
#define P2A6_RG_U2_DISCTH		GENMASK(7, 4)
#define P2A6_RG_U2_SQTH			GENMASK(3, 0)
#define XSP_USBPHYACR3		((SSUSB_SIFSLV_U2PHY_COM) + 0x01c)
#define P2A3_RG_USB20_PUPD_BIST_EN	BIT(12)
#define P2A3_RG_USB20_EN_PU_DP		BIT(9)
#define XSP_USBPHYACR4		((SSUSB_SIFSLV_U2PHY_COM) + 0x020)
#define P2A4_RG_USB20_GPIO_CTL		BIT(9)
#define P2A4_USB20_GPIO_MODE		BIT(8)
#define P2A4_U2_GPIO_CTR_MSK (P2A4_RG_USB20_GPIO_CTL | P2A4_USB20_GPIO_MODE)
#define XSP_USBPHYA_RESERVE	((SSUSB_SIFSLV_U2PHY_COM) + 0x030)
#define P2AR_RG_INTR_CAL		GENMASK(29, 24)
#define P2AR_RG_INTR_CAL_MASK		(0x3f)
#define P2AR_RG_INTR_CAL_OFET		(24)
#define XSP_USBPHYA_RESERVEA	((SSUSB_SIFSLV_U2PHY_COM) + 0x034)
#define P2ARA_RG_TERM_CAL		GENMASK(11, 8)
#define P2ARA_RG_TERM_CAL_MASK          (0xf)
#define P2ARA_RG_TERM_CAL_OFET		(8)
#define XSP_USBPHYA_RESERVEA1	((SSUSB_SIFSLV_U2PHY_COM) + 0x038)
#define P2ARA_RG_TX_CHIRPK		BIT(8)
#define XSP_U2PHYA_RESERVE0	((SSUSB_SIFSLV_U2PHY_COM) + 0x040)
#define P2A2R0_RG_PLL_FBKSEL         BIT(31)
#define XSP_U2PHYA_RESERVE1	((SSUSB_SIFSLV_U2PHY_COM) + 0x044)
#define P2A2R1_RG_PLL_POSDIV    GENMASK(2, 0)
#define XSP_U2PHYDCR1		((SSUSB_SIFSLV_U2PHY_COM) + 0x064)
#define P2C_RG_USB20_SW_PLLMODE	GENMASK(19, 18)
#define XSP_U2PHYDTM0		((SSUSB_SIFSLV_U2PHY_COM) + 0x068)
#define P2D_FORCE_UART_EN		BIT(26)
#define P2D_FORCE_DATAIN		BIT(23)
#define P2D_FORCE_DM_PULLDOWN		BIT(21)
#define P2D_FORCE_DP_PULLDOWN		BIT(20)
#define P2D_FORCE_XCVRSEL		BIT(19)
#define P2D_FORCE_SUSPENDM		BIT(18)
#define P2D_FORCE_TERMSEL		BIT(17)
#define P2D_RG_DATAIN			GENMASK(13, 10)
#define P2D_RG_DMPULLDOWN		BIT(7)
#define P2D_RG_DPPULLDOWN		BIT(6)
#define P2D_RG_XCVRSEL			GENMASK(5, 4)
#define P2D_RG_XCVRSEL_VAL(x)		((0x3 & (x)) << 4)
#define P2D_RG_SUSPENDM			BIT(3)
#define P2D_RG_TERMSEL			BIT(2)
#define P2D_DTM0_PART_MASK \
		(P2D_FORCE_DATAIN | P2D_FORCE_DM_PULLDOWN | \
		P2D_FORCE_DP_PULLDOWN | P2D_FORCE_XCVRSEL | \
		P2D_FORCE_SUSPENDM | P2D_FORCE_TERMSEL | \
		P2D_RG_DMPULLDOWN | P2D_RG_DPPULLDOWN | \
		P2D_RG_TERMSEL)
#define P2D_RG_UART_EN		BIT(16)
#define SSPXTP_DIG_GLB_28		((SSPXTP_SIFSLV_DIG_GLB) + 0x028)
#define RG_XTP_DAIF_GLB_TXPLL_IR		GENMASK(17, 13)
#define SSPXTP_DIG_GLB_38		((SSPXTP_SIFSLV_DIG_GLB) + 0x038)
#define RG_XTP_DAIF_GLB_SPLL_IR		GENMASK(17, 13)
#define SSPXTP_DAIG_LN_RX0_70	((SSPXTP_SIFSLV_DIG_LN_RX0) + 0x070)
#define RG_XTP0_CDR_PPATH_DVN_G2_LTD1	GENMASK(15, 8)
#define SSPXTP_DAIG_LN_DAIF_00	((SSPXTP_SIFSLV_DIG_LN_DAIF) + 0x00)
#define RG_XTP0_DAIF_FRC_LN_TX_LCTXCM1		BIT(7)
#define RG_XTP0_DAIF_FRC_LN_TX_LCTXC0		BIT(8)
#define RG_XTP0_DAIF_FRC_LN_TX_LCTXCP1		BIT(9)
#define SSPXTP_DAIG_LN_DAIF_04	((SSPXTP_SIFSLV_DIG_LN_DAIF) + 0x04)
#define RG_XTP0_DAIF_FRC_LN_RX_AEQ_ATT		BIT(17)
#define SSPXTP_DAIG_LN_DAIF_08	((SSPXTP_SIFSLV_DIG_LN_DAIF) + 0x008)
#define RG_XTP0_DAIF_LN_TX_LCTXCM1		GENMASK(12, 7)
#define RG_XTP0_DAIF_LN_TX_LCTXC0		GENMASK(18, 13)
#define RG_XTP0_DAIF_LN_TX_LCTXCP1		GENMASK(24, 19)
#define SSPXTP_DAIG_LN_DAIF_14	((SSPXTP_SIFSLV_DIG_LN_DAIF) + 0x014)
#define RG_XTP0_DAIF_LN_RX_AEQ_ATT		GENMASK(20, 18)
#define SSPXTP_DAIG_LN_DAIF_20	((SSPXTP_SIFSLV_DIG_LN_DAIF) + 0x020)
#define RG_XTP0_DAIF_LN_G1_RX_SGDT_HF		GENMASK(23, 22)
#define SSPXTP_DAIG_LN_DAIF_2C	((SSPXTP_SIFSLV_DIG_LN_DAIF) + 0x02C)
#define RG_XTP0_DAIF_LN_G2_RX_SGDT_HF		GENMASK(23, 22)
#define SSPXTP_DAIG_LN_DAIF_44	((SSPXTP_SIFSLV_DIG_LN_DAIF) + 0x044)
#define RG_XTP0_DAIF_LN_G2_CDR_IIR_GAIN		GENMASK(15, 13)
#define SSPXTP_PHYA_LN_08	((SSPXTP_SIFSLV_PHYA_LN) + 0x08)
#define RG_XTP_LN0_TX_RXDET_HZ		BIT(13)
#define SSPXTP_PHYA_LN_30	((SSPXTP_SIFSLV_PHYA_LN) + 0x030)
#define RG_XTP_LN0_RX_AEQ_ATT		BIT(14)
#define SSPXTP_PHYA_LN_3C	((SSPXTP_SIFSLV_PHYA_LN) + 0x03C)
#define RG_XTP_LN0_RX_AEQ_CTLE_ERR_TYPE		GENMASK(14, 13)

enum mtk_xsphy_submode {
	PHY_MODE_BC11_SW_SET = 1,
	PHY_MODE_BC11_SW_CLR,
	PHY_MODE_DPDMPULLDOWN_SET,
	PHY_MODE_DPDMPULLDOWN_CLR,
	PHY_MODE_DPPULLUP_SET,
	PHY_MODE_DPPULLUP_CLR,
};

enum mtk_xsphy_u2_lpm_parameter {
	PHY_PLL_TIMER_COUNT,
	PHY_PLL_STABLE_TIME,
	PHY_PLL_BANDGAP_TIME,
	PHY_PLL_PARA_CNT,
};

static void mt6989_u2_phy_lpm_pll_set(struct mtk_xsphy *xsphy,
	struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;
	u32 index = inst->index;

	if (!inst->lpm_quirk)
		return;

	/* enable SW PLL mode */
	mtk_phy_update_field(pbase + XSP_U2PHYDCR1, P2C_RG_USB20_SW_PLLMODE, 0x1);

	/* enable HW count PLL time */
	mtk_phy_set_bits(pbase + XSP_MISC_REG1, MR1_RG_MISC_PLL_STABLE_SEL);

	/* set 1us time count value */
	mtk_phy_update_field(pbase + XSP_MISC_REG0, MR0_RG_MISC_TIMER_1US,
			    inst->lpm_para[PHY_PLL_TIMER_COUNT]);

	/*set pll stable time*/
	mtk_phy_update_field(pbase + XSP_MISC_REG0, MR0_RG_MISC_STABLE_TIME,
			    inst->lpm_para[PHY_PLL_STABLE_TIME]);

	/* set pll bandgap stable time */
	mtk_phy_update_field(pbase + XSP_MISC_REG0, MR0_RG_MISC_BANDGAP_STABLE_TIME,
			    inst->lpm_para[PHY_PLL_BANDGAP_TIME]);

	dev_info(xsphy->dev, "%s(%d)\n", __func__, index);
}

static void mt6989_u2_phy_props_set(struct mtk_xsphy *xsphy,
			     struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;

	if (inst->efuse_intr != -EINVAL) {
		int intr_val = inst->efuse_intr + inst->intr_ofs;

		if (inst->intr_ofs < -P2AR_RG_INTR_CAL_MASK ||
			inst->intr_ofs > P2AR_RG_INTR_CAL_MASK ||
			intr_val < 0 || intr_val > P2AR_RG_INTR_CAL_MASK)
			intr_val = inst->efuse_intr;

		mtk_phy_update_field(pbase + XSP_USBPHYA_RESERVE, P2AR_RG_INTR_CAL, intr_val);
	}

	if (inst->efuse_term_cal != -EINVAL) {
		int term_val = inst->efuse_term_cal + inst->term_ofs;

		if (inst->term_ofs < -P2ARA_RG_TERM_CAL_MASK ||
			inst->term_ofs > P2ARA_RG_TERM_CAL_MASK ||
			term_val < 0 || term_val > P2ARA_RG_TERM_CAL_MASK)
			term_val = inst->efuse_term_cal;

		mtk_phy_update_field(pbase + XSP_USBPHYA_RESERVEA, P2ARA_RG_TERM_CAL, term_val);
	}

	if (inst->eye_src != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR5, P2A5_RG_HSTX_SRCTRL,
				     inst->eye_src);

	if (inst->eye_vrt != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR1, P2A1_RG_VRT_SEL,
				     inst->eye_vrt);

	if (inst->eye_term != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR1, P2A1_RG_TERM_SEL,
				     inst->eye_term);

	if (inst->discth != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR6, P2A6_RG_U2_DISCTH,
				    inst->discth);

	if (inst->rx_sqth != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR6, P2A6_RG_U2_SQTH,
				    inst->rx_sqth);

	if (inst->rev6 != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR6, P2A6_RG_U2_PHY_REV6,
				     inst->rev6);

	if (inst->pll_fbksel != -EINVAL)
		mtk_phy_update_field(pbase + XSP_U2PHYA_RESERVE0, P2A2R0_RG_PLL_FBKSEL,
				     inst->pll_fbksel);

	if (inst->pll_posdiv != -EINVAL)
		mtk_phy_update_field(pbase + XSP_U2PHYA_RESERVE1, P2A2R1_RG_PLL_POSDIV,
				     inst->pll_posdiv);

}

static void mt6989_u2_phy_host_props_set(struct mtk_xsphy *xsphy,
			     struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;

	if (inst->efuse_intr != -EINVAL) {
		int host_intr_val = inst->efuse_intr + inst->host_intr_ofs;

		if (inst->host_intr_ofs < -P2AR_RG_INTR_CAL_MASK ||
			inst->host_intr_ofs > P2AR_RG_INTR_CAL_MASK ||
			host_intr_val < 0 || host_intr_val > P2AR_RG_INTR_CAL_MASK)
			host_intr_val = inst->efuse_intr;

		mtk_phy_update_field(pbase + XSP_USBPHYA_RESERVE, P2AR_RG_INTR_CAL, host_intr_val);
	}

	if (inst->efuse_term_cal != -EINVAL) {
		int host_term_val = inst->efuse_term_cal + inst->host_term_ofs;

		if (inst->host_term_ofs < -P2ARA_RG_TERM_CAL_MASK ||
			inst->host_term_ofs > P2ARA_RG_TERM_CAL_MASK ||
			host_term_val < 0 || host_term_val > P2ARA_RG_TERM_CAL_MASK)
			host_term_val = inst->efuse_term_cal;

		mtk_phy_update_field(pbase + XSP_USBPHYA_RESERVEA, P2ARA_RG_TERM_CAL, host_term_val);
	}

	if (inst->host_rx_sqth != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR6, P2A6_RG_U2_SQTH,
				    inst->host_rx_sqth);

	if (inst->eye_src_host != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR5, P2A5_RG_HSTX_SRCTRL,
				     inst->eye_src_host);

	if (inst->eye_vrt_host != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR1, P2A1_RG_VRT_SEL,
				     inst->eye_vrt_host);

	if (inst->eye_term_host != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR1, P2A1_RG_TERM_SEL,
				     inst->eye_term_host);

	if (inst->rev6_host != -EINVAL)
		mtk_phy_update_field(pbase + XSP_USBPHYACR6, P2A6_RG_U2_PHY_REV6,
				    inst->rev6_host);
}

static void mt6989_u3_phy_props_set(struct mtk_xsphy *xsphy,
			     struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;

	if (inst->efuse_intr != -EINVAL)
		mtk_phy_update_field(xsphy->glb_base + SSPXTP_PHYA_GLB_00,
				     RG_XTP_GLB_BIAS_INTR_CTRL, inst->efuse_intr);

	if (inst->efuse_tx_imp != -EINVAL)
		mtk_phy_update_field(pbase + SSPXTP_PHYA_LN_04,
				     RG_XTP_LN0_TX_IMPSEL, inst->efuse_tx_imp);

	if (inst->efuse_rx_imp != -EINVAL)
		mtk_phy_update_field(pbase + SSPXTP_PHYA_LN_14,
				     RG_XTP_LN0_RX_IMPSEL, inst->efuse_rx_imp);

	if (inst->tx_lctxcm1 != -EINVAL) {
		mtk_phy_set_bits(pbase + SSPXTP_DAIG_LN_DAIF_00,
				 RG_XTP0_DAIF_FRC_LN_TX_LCTXCM1);

		mtk_phy_update_field(pbase + SSPXTP_DAIG_LN_DAIF_08,
				    RG_XTP0_DAIF_LN_TX_LCTXCM1, inst->tx_lctxcm1);
	}

	if (inst->tx_lctxc0 != -EINVAL) {
		mtk_phy_set_bits(pbase + SSPXTP_DAIG_LN_DAIF_00,
				 RG_XTP0_DAIF_FRC_LN_TX_LCTXC0);

		mtk_phy_update_field(pbase + SSPXTP_DAIG_LN_DAIF_08,
				    RG_XTP0_DAIF_LN_TX_LCTXC0, inst->tx_lctxc0);
	}

	if (inst->tx_lctxcp1 != -EINVAL) {
		mtk_phy_set_bits(pbase + SSPXTP_DAIG_LN_DAIF_00,
				 RG_XTP0_DAIF_FRC_LN_TX_LCTXCP1);

		mtk_phy_update_field(pbase + SSPXTP_DAIG_LN_DAIF_08,
				    RG_XTP0_DAIF_LN_TX_LCTXCP1, inst->tx_lctxcp1);
	}
}

static void mt6989_u2_phy_instance_init(struct mtk_xsphy *xsphy,
				 struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;
	u32 tmp;

	/* read u2 intr default value from register if dts/efuse no proper config */
	if (inst->efuse_intr == -EINVAL) {
		tmp = readl(pbase + XSP_USBPHYA_RESERVE);
		tmp >>= P2AR_RG_INTR_CAL_OFET;
		inst->efuse_intr = tmp & P2AR_RG_INTR_CAL_MASK;
	}

	/* read u2 term_cal default value from register if dts/efuse no proper config */
	if (inst->efuse_term_cal == -EINVAL) {
		tmp = readl(pbase + XSP_USBPHYA_RESERVEA);
		tmp >>= P2ARA_RG_TERM_CAL_OFET;
		inst->efuse_term_cal = tmp & P2ARA_RG_TERM_CAL_MASK;
	}

	/* DP/DM BC1.1 path Disable */
	mtk_phy_clear_bits(pbase + XSP_USBPHYACR6, P2A6_RG_BC11_SW_EN);

	mtk_phy_set_bits(pbase + XSP_USBPHYACR0, P2A0_RG_INTR_EN);
}

static void mt6989_u2_phy_instance_power_on(struct mtk_xsphy *xsphy,
				     struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;
	u32 index = inst->index;

	mtk_phy_set_bits(pbase + XSP_U2PHYDTM0, P2D_FORCE_SUSPENDM);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0, P2D_RG_SUSPENDM);

	mtk_phy_set_bits(pbase + XSP_U2PHYDTM0, P2D_RG_SUSPENDM);

	udelay(30);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0, P2D_FORCE_SUSPENDM);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0, P2D_RG_SUSPENDM);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0, P2D_FORCE_UART_EN);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM1, P2D_RG_UART_EN);

	mtk_phy_clear_bits(pbase + XSP_USBPHYACR4, P2A4_U2_GPIO_CTR_MSK);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0, P2D_FORCE_SUSPENDM);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0,
			   (P2D_RG_XCVRSEL | P2D_RG_DATAIN | P2D_DTM0_PART_MASK));

	mtk_phy_clear_bits(pbase + XSP_USBPHYACR6, P2A6_RG_BC11_SW_EN);

	mtk_phy_set_bits(pbase + XSP_USBPHYACR6, P2A6_RG_OTG_VBUSCMP_EN);

	mtk_phy_update_bits(pbase + XSP_U2PHYDTM1,
			    P2D_RG_VBUSVALID | P2D_RG_AVALID | P2D_RG_SESSEND,
			    P2D_RG_VBUSVALID | P2D_RG_AVALID);

	mtk_phy_clear_bits(pbase + XSP_USBPHYACR0, P2A0_RG_USB20_TX_PH_ROT_SEL);

	mtk_phy_clear_bits(pbase + XSP_USBPHYACR6,
			   (P2A6_RG_U2_PHY_REV6 | P2A6_RG_U2_PHY_REV1));
	mtk_phy_set_bits(pbase + XSP_USBPHYACR6, P2A6_RG_U2_PHY_REV6_VAL(1));

	udelay(800);

	mt6989_u2_phy_lpm_pll_set(xsphy, inst);

	dev_info(xsphy->dev, "%s(%d)\n", __func__, index);
}

static void mt6989_u2_phy_instance_power_off(struct mtk_xsphy *xsphy,
				      struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;
	u32 index = inst->index;
	enum phy_mode mode = inst->phy->attrs.mode;

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0, P2D_FORCE_UART_EN);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM1, P2D_RG_UART_EN);

	mtk_phy_clear_bits(pbase + XSP_USBPHYACR4, P2A4_U2_GPIO_CTR_MSK);

	mtk_phy_clear_bits(pbase + XSP_USBPHYACR6, P2A6_RG_BC11_SW_EN);

	mtk_phy_clear_bits(pbase + XSP_USBPHYACR6, P2A6_RG_OTG_VBUSCMP_EN);

	mtk_phy_update_bits(pbase + XSP_U2PHYDTM1,
			    P2D_RG_VBUSVALID | P2D_RG_AVALID | P2D_RG_SESSEND,
			    P2D_RG_SESSEND);

	mtk_phy_set_bits(pbase + XSP_U2PHYDTM0, (P2D_RG_SUSPENDM | P2D_FORCE_SUSPENDM));

	mdelay(2);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0, P2D_RG_DATAIN);
	mtk_phy_set_bits(pbase + XSP_U2PHYDTM0, (P2D_RG_XCVRSEL_VAL(1) | P2D_DTM0_PART_MASK));

	mtk_phy_set_bits(pbase + XSP_USBPHYACR6, P2A6_RG_U2_PHY_REV6_VAL(1));

	if (mode == PHY_MODE_INVALID)
		mtk_phy_set_bits(pbase + XSP_USBPHYACR6, P2A6_RG_U2_PHY_REV1);

	udelay(800);

	mtk_phy_clear_bits(pbase + XSP_U2PHYDTM0, P2D_RG_SUSPENDM);

	udelay(1);

	/* set BC11_SW_EN to enter L2 power off mode */
	if (mode == PHY_MODE_INVALID)
		mtk_phy_set_bits(inst->port_base + XSP_USBPHYACR6, P2A6_RG_BC11_SW_EN);

	dev_info(xsphy->dev, "%s(%d)\n", __func__, index);
}

static void mt6989_u2_phy_instance_set_mode(struct mtk_xsphy *xsphy,
				     struct xsphy_instance *inst,
				     enum phy_mode mode,
				     int submode)
{
	u32 tmp;

	dev_info(xsphy->dev, "%s mode(%d), submode(%d)\n", __func__,
		mode, submode);

	if (!submode) {
		tmp = readl(inst->port_base + XSP_U2PHYDTM1);
		switch (mode) {
		case PHY_MODE_USB_DEVICE:
			mt6989_u2_phy_props_set(xsphy, inst);
			tmp |= P2D_FORCE_IDDIG | P2D_RG_IDDIG;
			break;
		case PHY_MODE_USB_HOST:
			mt6989_u2_phy_host_props_set(xsphy, inst);
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
	} else {
		switch (submode) {
		case PHY_MODE_BC11_SW_SET:
			if (xsphy->bc11_switch_disable)
				break;

			mtk_phy_set_bits(inst->port_base + XSP_USBPHYACR6,
					 P2A6_RG_BC11_SW_EN);
			break;
		case PHY_MODE_BC11_SW_CLR:
			if (xsphy->bc11_switch_disable)
				break;
			/* dont' need to switch back to usb when phy off */
			if (inst->phy->power_count > 0)
				mtk_phy_clear_bits(inst->port_base + XSP_USBPHYACR6,
						   P2A6_RG_BC11_SW_EN);
			break;
		case PHY_MODE_DPDMPULLDOWN_SET:
			mtk_phy_set_bits(inst->port_base + XSP_U2PHYDTM0,
					 (P2D_RG_DPPULLDOWN | P2D_RG_DMPULLDOWN));

			mtk_phy_clear_bits(inst->port_base + XSP_USBPHYACR6,
					   P2A6_RG_U2_PHY_REV1);

			mtk_phy_set_bits(inst->port_base + XSP_USBPHYACR6,
					 P2A6_RG_BC11_SW_EN);
			break;
		case PHY_MODE_DPDMPULLDOWN_CLR:
			mtk_phy_clear_bits(inst->port_base + XSP_U2PHYDTM0,
					   (P2D_RG_DPPULLDOWN | P2D_RG_DMPULLDOWN));

			mtk_phy_set_bits(inst->port_base + XSP_USBPHYACR6,
					 P2A6_RG_U2_PHY_REV1);

			mtk_phy_clear_bits(inst->port_base + XSP_USBPHYACR6,
					   P2A6_RG_BC11_SW_EN);
			break;
		case PHY_MODE_DPPULLUP_SET:
			mtk_phy_set_bits(inst->port_base + XSP_USBPHYACR3,
					 (P2A3_RG_USB20_PUPD_BIST_EN |
					 P2A3_RG_USB20_EN_PU_DP));
			break;
		case PHY_MODE_DPPULLUP_CLR:
			mtk_phy_clear_bits(inst->port_base + XSP_USBPHYACR3,
					   (P2A3_RG_USB20_PUPD_BIST_EN |
					   P2A3_RG_USB20_EN_PU_DP));
			break;
		default:
			return;
		}
	}
}

static void mt6989_u3_phy_instance_power_on(struct mtk_xsphy *xsphy,
				     struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;

	/* clear hz mode */
	mtk_phy_clear_bits(pbase + SSPXTP_PHYA_LN_08, RG_XTP_LN0_TX_RXDET_HZ);

	/* DA_XTP_GLB_TXPLL_IR[4:0], 5'b00100 */
	mtk_phy_update_field(xsphy->glb_base + SSPXTP_DIG_GLB_28, RG_XTP_DAIF_GLB_TXPLL_IR, 0x4);

	/* DA_XTP_GLB_SPLL_IR[4:0], 5'b00100 */
	mtk_phy_update_field(xsphy->glb_base + SSPXTP_DIG_GLB_38, RG_XTP_DAIF_GLB_SPLL_IR, 0x4);

	/* DA_XTP_LN0_RX_SGDT_HF[1:0], 2'b10 */
	mtk_phy_update_field(pbase + SSPXTP_DAIG_LN_DAIF_20, RG_XTP0_DAIF_LN_G1_RX_SGDT_HF, 0x2);

	mtk_phy_update_field(pbase + SSPXTP_DAIG_LN_DAIF_2C, RG_XTP0_DAIF_LN_G2_RX_SGDT_HF, 0x2);

	/* Deprecated since MT6989 */
	if (!inst->u3_rx_fix) {
		/* DA_XTP_LN0_RX_AEQ_OFORCE[10], 1'b1 */
		mtk_phy_set_bits(pbase + SSPXTP_PHYA_LN_30, RG_XTP_LN0_RX_AEQ_ATT);

		/* rg_sspxtp0_datf_ln_rx_aeq_att[2:0], 3'b111 */
		mtk_phy_update_field(pbase + SSPXTP_DAIG_LN_DAIF_14, RG_XTP0_DAIF_LN_RX_AEQ_ATT, 0x7);

		/* rg_sspxtp0_datf_frc_ln_rx_aeq_att, 1'b1 */
		mtk_phy_set_bits(pbase + SSPXTP_DAIG_LN_DAIF_04, RG_XTP0_DAIF_FRC_LN_RX_AEQ_ATT);
	} else
		dev_info(xsphy->dev, "%s apply u3_rx_fix.\n", __func__);

	if (inst->u3_gen2_hqa) {
		dev_info(xsphy->dev, "%s apply u3_gen2_hqa.\n", __func__);
		/* RG_XTP0_CDR_PPATH_DVN_G2_LTD1[15:8], 8'b10000100 */
		mtk_phy_update_field(pbase + SSPXTP_DAIG_LN_RX0_70, RG_XTP0_CDR_PPATH_DVN_G2_LTD1, 0x84);

		/* RG_XTP0_DAIF_LN_G2_CDR_IIR_GAIN[15:13], 3'b010 */
		mtk_phy_update_field(pbase + SSPXTP_DAIG_LN_DAIF_44, RG_XTP0_DAIF_LN_G2_CDR_IIR_GAIN, 0x2);

		/* RG_XTP_LN0_RX_AEQ_CTLE_ERR_TYPE[14:13], 2'b01 */
		mtk_phy_update_field(pbase + SSPXTP_PHYA_LN_3C, RG_XTP_LN0_RX_AEQ_CTLE_ERR_TYPE, 0x1);
	}


	dev_info(xsphy->dev, "%s(%d)\n", __func__, inst->index);
}

static void mt6989_u3_phy_instance_power_off(struct mtk_xsphy *xsphy,
				      struct xsphy_instance *inst)
{
	void __iomem *pbase = inst->port_base;

	/* enable hz mode */
	mtk_phy_set_bits(pbase + SSPXTP_PHYA_LN_08, RG_XTP_LN0_TX_RXDET_HZ);

	dev_info(xsphy->dev, "%s(%d)\n", __func__, inst->index);
}

#endif
