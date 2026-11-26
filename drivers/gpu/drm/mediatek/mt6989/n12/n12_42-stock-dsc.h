/* SPDX-License-Identifier: GPL-2.0 */
#ifndef N12_42_STOCK_DSC_H
#define N12_42_STOCK_DSC_H

/* Include after the real mtk_panel_ext.h.  Fields and RC data recovered from stock. */
static unsigned int n12_42_stock_rc_buf_thresh[] = {
	896, 1792, 2688, 3584, 4480, 5376, 6272, 6720, 7168, 7616, 7744, 7872, 8000, 8064
};

static unsigned int n12_42_stock_range_min_qp[] = {
	0, 4, 5, 5, 7, 7, 7, 7, 7, 7, 9, 9, 9, 11, 17
};

static unsigned int n12_42_stock_range_max_qp[] = {
	8, 8, 9, 10, 11, 11, 11, 12, 13, 14, 15, 16, 17, 17, 19
};

static int n12_42_stock_range_bpg_ofs[] = {
	2, 0, 0, -2, -4, -6, -8, -8, -8, -10, -10, -12, -12, -12, -12
};
static const struct mtk_panel_dsc_params n12_42_stock_dsc = {
	.enable = 1,
	.dual_dsc_enable = 0,
	.ver = 17,
	.slice_mode = 1,
	.rgb_swap = 0,
	.dsc_cfg = 40,
	.rct_on = 1,
	.bit_per_channel = 10,
	.dsc_line_buf_depth = 11,
	.bp_enable = 1,
	.bit_per_pixel = 128,
	.pic_height = 2712,
	.pic_width = 1220,
	.slice_height = 12,
	.slice_width = 610,
	.chunk_size = 610,
	.xmit_delay = 512,
	.dec_delay = 562,
	.scale_value = 32,
	.increment_interval = 305,
	.decrement_interval = 8,
	.line_bpg_offset = 12,
	.nfl_bpg_offset = 2235,
	.slice_bpg_offset = 1915,
	.initial_offset = 6144,
	.final_offset = 4336,
	.flatness_minqp = 7,
	.flatness_maxqp = 16,
	.rc_model_size = 8192,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit0 = 15,
	.rc_quant_incr_limit1 = 15,
	.rc_tgt_offset_hi = 3,
	.rc_tgt_offset_lo = 3,
	.ext_pps_cfg = {
		.enable = 1,
		.rc_buf_thresh = n12_42_stock_rc_buf_thresh,
		.rc_buf_thresh_count = 14,
		.range_min_qp = n12_42_stock_range_min_qp,
		.range_min_qp_count = 15,
		.range_max_qp = n12_42_stock_range_max_qp,
		.range_max_qp_count = 15,
		.range_bpg_ofs = n12_42_stock_range_bpg_ofs,
		.range_bpg_ofs_count = 15,
	},
};

#endif
