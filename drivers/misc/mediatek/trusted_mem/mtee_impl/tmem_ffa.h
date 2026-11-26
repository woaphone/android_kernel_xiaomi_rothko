/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef TMEM_CARVEOUT_HEAP_H_
#define TMEM_CARVEOUT_HEAP_H_

#include <linux/scatterlist.h>

int tmem_carveout_create(int idx, phys_addr_t heap_base, size_t heap_size);
int tmem_carveout_destroy(int idx);
phys_addr_t get_mem_pool_pa(u32 mchunk_id);
unsigned long get_mem_pool_size(u32 mchunk_id);
int tmem_register_ffa_module(void);
bool tmem_ffa_is_ready(void);
int tmem_query_ffa_handle_to_pa(u64 handle, uint64_t *phy_addr);
int tmem_run_ffa_test(int test_id);

int tmem_ffa_region_alloc(enum MTEE_MCHUNKS_ID mchunk_id,
		unsigned long size, unsigned long alignment, u64 *ffa_handle);
int tmem_ffa_region_free(enum MTEE_MCHUNKS_ID mchunk_id,
		u64 ffa_handle);

int tmem_ffa_page_alloc(enum MTEE_MCHUNKS_ID mchunk_id,
		struct sg_table *table, u64 *ffa_handle);
int tmem_ffa_page_free(enum MTEE_MCHUNKS_ID mchunk_id, u64 ffa_handle);

#endif /* TMEM_CARVEOUT_HEAP_H_ */
