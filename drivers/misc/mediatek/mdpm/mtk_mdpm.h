/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MTK_MDPM_DBM_H_
#define _MTK_MDPM_DBM_H_

#include <linux/types.h>

int mtk_mdpm_init_dbm(void __iomem *base, size_t size);
void mtk_mdpm_reset_dbm(void);

#endif /* _MTK_MDPM_DBM_H_ */
