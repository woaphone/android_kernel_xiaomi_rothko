/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef MTK_HCP_ISP7sp_H
#define MTK_HCP_ISP7sp_H

#include "mtk-hcp.h"

#if SMVR_DECOUPLE
phys_addr_t isp7sp_get_reserve_mem_phys(unsigned int id, unsigned int mode);
void *isp7sp_get_reserve_mem_virt(unsigned int id, unsigned int mode);
phys_addr_t isp7sp_get_reserve_mem_dma(unsigned int id, unsigned int mode);
phys_addr_t isp7sp_get_reserve_mem_size(unsigned int id, unsigned int mode);
uint32_t isp7sp_get_reserve_mem_fd(unsigned int id, unsigned int mode);
void *isp7sp_get_wpe_virt(unsigned int mode);
int isp7sp_get_wpe_cq_fd(unsigned int mode);
int isp7sp_get_wpe_tdr_fd(unsigned int mode);
void *isp7sp_get_dip_virt(unsigned int mode);
int isp7sp_get_dip_cq_fd(unsigned int mode);
int isp7sp_get_dip_tdr_fd(unsigned int mode);
void *isp7sp_get_traw_virt(unsigned int mode);
int isp7sp_get_traw_cq_fd(unsigned int mode);
int isp7sp_get_traw_tdr_fd(unsigned int mode);
void *isp7sp_get_pqdip_virt(unsigned int mode);
int isp7sp_get_pqdip_cq_fd(unsigned int mode);
int isp7sp_get_pqdip_tdr_fd(unsigned int mode);
int isp7sp_release_working_buffer(struct mtk_hcp *hcp_dev, unsigned int mode);
int isp7sp_release_gce_working_buffer(struct mtk_hcp *hcp_dev);
int isp7sp_allocate_working_buffer(struct mtk_hcp *hcp_dev, unsigned int mode, unsigned int gmb_en);
//int isp7s_allocate_working_buffer2(struct mtk_hcp *hcp_dev, unsigned int mode);
int isp7sp_get_init_info(struct img_init_info *info);
void *isp7sp_get_gce_virt(unsigned int mode);
void *isp7sp_get_gce_token_virt(unsigned int mode);
void *isp7sp_get_hwid_virt(unsigned int mode);
int isp7sp_partial_flush(struct mtk_hcp *hcp_dev, struct flush_buf_info *b_info);
int isp7sp_allocate_gce_working_buffer(struct mtk_hcp *hcp_dev, unsigned int mode);
int isp7sp_get_mem_info(struct img_init_info *info);
#else
int isp7sp_release_working_buffer(struct mtk_hcp *hcp_dev);
int isp7sp_allocate_working_buffer(struct mtk_hcp *hcp_dev, unsigned int mode);
int isp7sp_get_init_info(struct img_init_info *info);
void *isp7sp_get_gce_virt(void);
void *isp7sp_get_hwid_virt(void);
int isp7sp_partial_flush(struct mtk_hcp *hcp_dev, struct flush_buf_info *b_info);
#endif

extern struct mtk_hcp_data isp7sp_hcp_data;

#endif /* _MTK_HCP_ISP7sp_H */
