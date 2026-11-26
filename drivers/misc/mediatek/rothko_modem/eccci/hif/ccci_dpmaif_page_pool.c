// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 MediaTek Inc.
 */

#include <linux/cma.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/skbuff.h>
#include <linux/sysctl.h>

#include "ccci_debug.h"
#include "ccci_dpmaif_reg_com.h"
#include "ccci_dpmaif_page_pool.h"
#include "ccci_dpmaif_com.h"
#ifdef RX_PAGE_POOL

#define TAG "dpmaif_page_pool"
#define MAX_POOL_SIZE 32768
#define POOL_SCAN_BATCH 64

struct dpmaif_page_pool {
	struct page *pages;
	unsigned int page_count;
	unsigned int next_page;
};

static struct reserved_mem *rmem;
static struct dpmaif_page_pool resv_skb_mem[POOL_NUMBER];
struct dpmaif_page_pool *g_page_pool_arr[POOL_NUMBER];

unsigned int g_page_pool_is_on;

atomic_t   g_alloc_from_kernel;
unsigned int sysctl_devalloc_threshold = 2000;

/*
 * Register the sysctl to set threshold of allocing skb from kernel.
 */

static const struct ctl_table devalloc_threshold_table[] = {
	{
		.procname	= "devalloc_threshold",
		.data		= &sysctl_devalloc_threshold,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
	},
};

static void register_devalloc_threshold_sysctl(void)
{
	if (!register_sysctl("net", devalloc_threshold_table))
		pr_info("CCCI:dpmaif:register devalloc_threshold failed\n");
}

/*
 * The CMA range keeps its allocation references while DPMAIF is bound.
 * Each SKB owns an additional page reference through its normal lifetime.
 * The SKB refill thread is the sole consumer of this allocation cursor.
 */
static struct page *dpmaif_alloc_pool_page(void)
{
	struct dpmaif_page_pool *pool;
	struct page *page;
	unsigned int index, scanned;

	for (index = 0; index < POOL_NUMBER; index++) {
		pool = g_page_pool_arr[index];
		if (!pool)
			continue;

		for (scanned = 0; scanned < min(pool->page_count, POOL_SCAN_BATCH);
		     scanned++) {
			page = pfn_to_page(page_to_pfn(pool->pages) + pool->next_page);
			if (++pool->next_page == pool->page_count)
				pool->next_page = 0;
			if (page_ref_count(page) != 1)
				continue;

			smp_acquire__after_ctrl_dep();
			get_page(page);
			return page;
		}
	}

	return NULL;
}

int skb_alloc_from_pool(
		struct sk_buff **ppskb,
		unsigned long long *p_base_addr,
		unsigned int pkt_buf_sz)
{
	struct page *page;
	unsigned int dma_len;

	dma_len = SKB_WITH_OVERHEAD(PAGE_SIZE) - NET_SKB_PAD;
	if (unlikely(pkt_buf_sz > dma_len))
		return LOW_MEMORY_SKB;

	page = dpmaif_alloc_pool_page();
	if (!page)
		return LOW_MEMORY_SKB;

	*ppskb = build_skb(page_address(page), PAGE_SIZE);
	if (!*ppskb) {
		put_page(page);
		CCCI_ERROR_LOG(-1, TAG,
				"[%s:%d] error: alloc skb fail!\n", __func__, __LINE__);
		return LOW_MEMORY_SKB;
	}

	skb_reserve(*ppskb, NET_SKB_PAD);
	*p_base_addr = dma_map_single(dpmaif_ctl->dev, (*ppskb)->data,
				     dma_len, DMA_FROM_DEVICE);

	if (dma_mapping_error(dpmaif_ctl->dev, *p_base_addr)) {
		CCCI_ERROR_LOG(-1, TAG,
			"[%s] error: dma mapping fail: %u!\n", __func__, dma_len);

		dev_kfree_skb_any(*ppskb);
		*ppskb = NULL;

		return DMA_MAPPING_ERR;
	}

	return 0;
}


void ccci_dpmaif_create_page_pool(unsigned int index)
{
	if (resv_skb_mem[index].pages)
		g_page_pool_arr[index] = &resv_skb_mem[index];
}


static int dpmaif_cma_mem_init(void)
{
	struct device_node *rmem_node;
	struct reserved_mem *region;
	int ret;

	if (rmem)
		return 0;

	rmem_node = of_parse_phandle(dpmaif_ctl->dev->of_node, "memory-region", 0);
	if (!rmem_node) {
		CCCI_ERROR_LOG(0, TAG, "[%s] error: no node for page pool.\n", __func__);
		return -ENODEV;
	}
	region = of_reserved_mem_lookup(rmem_node);
	of_node_put(rmem_node);

	if (!region)
		return -ENODEV;

	ret = of_reserved_mem_device_init_by_idx(dpmaif_ctl->dev, dpmaif_ctl->dev->of_node, 0);
	if (ret) {
		CCCI_ERROR_LOG(0, TAG, "%s: of_reserved_mem_device_init failed(%d).",
			__func__, ret);
		return ret;
	}

	rmem = region;
	return 0;
}

void ccci_dpmaif_cma_mem_alloc(unsigned int index)
{
	struct page *pages;

	if (!dpmaif_ctl->dev->cma_area) {
		CCCI_ERROR_LOG(0, TAG, "[%s] cma_area == NULL\n", __func__);
		return;
	}

	resv_skb_mem[index].page_count =
		min_t(phys_addr_t, rmem->size / PAGE_SIZE / POOL_NUMBER, MAX_POOL_SIZE);
	pages = cma_alloc(dpmaif_ctl->dev->cma_area,
			  resv_skb_mem[index].page_count, 0, false);

	if (pages) {
		resv_skb_mem[index].pages = pages;
		CCCI_NORMAL_LOG(0, TAG,
			"[%s] resv_skb_mem[%d].pool_page_num:%d.reserv_base:%llx,\n",
			__func__, index, resv_skb_mem[index].page_count,
			(unsigned long long)rmem->base);
	} else
		CCCI_ERROR_LOG(0, TAG,
		"[%s] error: alloc failed resv_skb_mem[index].pool_page_num:%d.\n",
		__func__, resv_skb_mem[index].page_count);
}


void ccci_dpmaif_page_pool_init(void)
{
	if (g_page_pool_is_on)
		return;

	if (!dpmaif_ctl->dev->of_node) {
		CCCI_ERROR_LOG(0, TAG, "dpmaif page pool driver of_node is null, exit\n");
		return;
	}

	if (dpmaif_cma_mem_init() == 0) {
		ccci_dpmaif_cma_mem_alloc(0);
		ccci_dpmaif_create_page_pool(0);
		if (g_page_pool_arr[0]) {
			g_page_pool_is_on = 1;
			atomic_set(&g_alloc_from_kernel, 0);
			register_devalloc_threshold_sysctl();
		}
	}
}
#endif
