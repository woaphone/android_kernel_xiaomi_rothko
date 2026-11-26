// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, 2023 Linaro Limited
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/arm_ffa.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/cpumask.h>
#include <linux/rpmb.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tee_core.h>
#include <linux/types.h>
#include "optee_private.h"
#include "optee_ffa.h"
#include "optee_rpc_cmd.h"

/*
 * This file implement the FF-A ABI used when communicating with secure world
 * OP-TEE OS via FF-A.
 * This file is divided into the following sections:
 * 1. Maintain a hash table for lookup of a global FF-A memory handle
 * 2. Convert between struct tee_param and struct optee_msg_param
 * 3. Low level support functions to register shared memory in secure world
 * 4. Dynamic shared memory pool based on alloc_pages()
 * 5. Do a normal scheduled call into secure world
 * 6. Driver initialization.
 */

/*
 * 1. Maintain a hash table for lookup of a global FF-A memory handle
 *
 * FF-A assigns a global memory handle for each piece shared memory.
 * This handle is then used when communicating with secure world.
 *
 * Main functions are optee_shm_add_ffa_handle() and optee_shm_rem_ffa_handle()
 */
/* MiTEE serializes each yielding call across its RPC and resume sequence. */
static DEFINE_MUTEX(mitee_call_mutex);

struct shm_rhash {
	struct tee_shm *shm;
	u64 global_id;
	struct rhash_head linkage;
};

static void rh_free_fn(void *ptr, void *arg)
{
	kfree(ptr);
}

static const struct rhashtable_params shm_rhash_params = {
	.head_offset = offsetof(struct shm_rhash, linkage),
	.key_len     = sizeof(u64),
	.key_offset  = offsetof(struct shm_rhash, global_id),
	.automatic_shrinking = true,
};

static struct tee_shm *optee_shm_from_ffa_handle(struct optee *optee,
						 u64 global_id)
{
	struct tee_shm *shm = NULL;
	struct shm_rhash *r;

	mutex_lock(&optee->ffa.mutex);
	r = rhashtable_lookup_fast(&optee->ffa.global_ids, &global_id,
				   shm_rhash_params);
	if (r)
		shm = r->shm;
	mutex_unlock(&optee->ffa.mutex);

	return shm;
}

static int optee_shm_add_ffa_handle(struct optee *optee, struct tee_shm *shm,
				    u64 global_id)
{
	struct shm_rhash *r;
	int rc;

	r = kmalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->shm = shm;
	r->global_id = global_id;

	mutex_lock(&optee->ffa.mutex);
	rc = rhashtable_lookup_insert_fast(&optee->ffa.global_ids, &r->linkage,
					   shm_rhash_params);
	mutex_unlock(&optee->ffa.mutex);

	if (rc)
		kfree(r);

	return rc;
}

static int optee_shm_rem_ffa_handle(struct optee *optee, u64 global_id)
{
	struct shm_rhash *r;
	int rc = -ENOENT;

	mutex_lock(&optee->ffa.mutex);
	r = rhashtable_lookup_fast(&optee->ffa.global_ids, &global_id,
				   shm_rhash_params);
	if (r)
		rc = rhashtable_remove_fast(&optee->ffa.global_ids,
					    &r->linkage, shm_rhash_params);
	mutex_unlock(&optee->ffa.mutex);

	if (!rc)
		kfree(r);

	return rc;
}

/*
 * 2. Convert between struct tee_param and struct optee_msg_param
 *
 * optee_ffa_from_msg_param() and optee_ffa_to_msg_param() are the main
 * functions.
 */

static int from_msg_param_tmp_mem(struct tee_param *p, u32 attr,
				  const struct optee_msg_param *mp)
{
	struct tee_shm *shm;
	phys_addr_t pa;
	int rc;

	p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT +
		  attr - OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
	p->u.memref.size = mp->u.tmem.size;
	shm = (struct tee_shm *)(unsigned long)mp->u.tmem.shm_ref;
	if (!shm) {
		p->u.memref.shm_offs = 0;
		p->u.memref.shm = NULL;
		return 0;
	}

	rc = tee_shm_get_pa(shm, 0, &pa);
	if (rc)
		return rc;

	p->u.memref.shm_offs = mp->u.tmem.buf_ptr - pa;
	p->u.memref.shm = shm;

	return 0;
}

static int to_msg_param_tmp_mem(struct optee_msg_param *mp,
				const struct tee_param *p)
{
	int rc;
	phys_addr_t pa;

	mp->attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;

	mp->u.tmem.shm_ref = (unsigned long)p->u.memref.shm;
	mp->u.tmem.size = p->u.memref.size;

	if (!p->u.memref.shm) {
		mp->u.tmem.buf_ptr = 0;
		return 0;
	}

	rc = tee_shm_get_pa(p->u.memref.shm, p->u.memref.shm_offs, &pa);
	if (rc)
		return rc;

	mp->u.tmem.buf_ptr = pa;
	mp->attr |= OPTEE_MSG_ATTR_CACHE_PREDEFINED <<
		    OPTEE_MSG_ATTR_CACHE_SHIFT;

	return 0;
}

static void from_msg_param_ffa_mem(struct optee *optee, struct tee_param *p,
				   u32 attr, const struct optee_msg_param *mp)
{
	struct tee_shm *shm = NULL;
	u64 offs_high = 0;
	u64 offs_low = 0;

	p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT +
		  attr - OPTEE_MSG_ATTR_TYPE_FMEM_INPUT;
	p->u.memref.size = mp->u.fmem.size;

	if (mp->u.fmem.global_id != OPTEE_MSG_FMEM_INVALID_GLOBAL_ID)
		shm = optee_shm_from_ffa_handle(optee, mp->u.fmem.global_id);
	p->u.memref.shm = shm;

	if (shm) {
		offs_low = mp->u.fmem.offs_low;
		offs_high = mp->u.fmem.offs_high;
	}
	p->u.memref.shm_offs = offs_low | offs_high << 32;
}

/**
 * optee_ffa_from_msg_param() - convert from OPTEE_MSG parameters to
 *				struct tee_param
 * @optee:	main service struct
 * @params:	subsystem internal parameter representation
 * @num_params:	number of elements in the parameter arrays
 * @msg_params:	OPTEE_MSG parameters
 *
 * Returns 0 on success or <0 on failure
 */
static int optee_ffa_from_msg_param(struct optee *optee,
				    struct tee_param *params, size_t num_params,
				    const struct optee_msg_param *msg_params)
{
	size_t n;

	for (n = 0; n < num_params; n++) {
		struct tee_param *p = params + n;
		const struct optee_msg_param *mp = msg_params + n;
		u32 attr = mp->attr & OPTEE_MSG_ATTR_TYPE_MASK;

		switch (attr) {
		case OPTEE_MSG_ATTR_TYPE_NONE:
			p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
			memset(&p->u, 0, sizeof(p->u));
			break;
		case OPTEE_MSG_ATTR_TYPE_VALUE_INPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_INOUT:
			optee_from_msg_param_value(p, attr, mp);
			break;
		case OPTEE_MSG_ATTR_TYPE_TMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
			if (from_msg_param_tmp_mem(p, attr, mp))
				return -EINVAL;
			break;
		case OPTEE_MSG_ATTR_TYPE_FMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_FMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_FMEM_INOUT:
			from_msg_param_ffa_mem(optee, p, attr, mp);
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int to_msg_param_ffa_mem(struct optee_msg_param *mp,
				const struct tee_param *p)
{
	struct tee_shm *shm = p->u.memref.shm;

	mp->attr = OPTEE_MSG_ATTR_TYPE_FMEM_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;

	if (shm) {
		u64 shm_offs = p->u.memref.shm_offs;

		mp->u.fmem.internal_offs = shm->offset;

		mp->u.fmem.offs_low = shm_offs;
		mp->u.fmem.offs_high = shm_offs >> 32;
		/* Check that the entire offset could be stored. */
		if (mp->u.fmem.offs_high != shm_offs >> 32)
			return -EINVAL;

		mp->u.fmem.global_id = shm->sec_world_id;
	} else {
		memset(&mp->u, 0, sizeof(mp->u));
		mp->u.fmem.global_id = OPTEE_MSG_FMEM_INVALID_GLOBAL_ID;
	}
	mp->u.fmem.size = p->u.memref.size;

	return 0;
}

/**
 * optee_ffa_to_msg_param() - convert from struct tee_params to OPTEE_MSG
 *			      parameters
 * @optee:	main service struct
 * @msg_params:	OPTEE_MSG parameters
 * @num_params:	number of elements in the parameter arrays
 * @params:	subsystem itnernal parameter representation
 * Returns 0 on success or <0 on failure
 */
static int optee_ffa_to_msg_param(struct optee *optee,
				  struct optee_msg_param *msg_params,
				  size_t num_params,
				  const struct tee_param *params)
{
	size_t n;

	for (n = 0; n < num_params; n++) {
		const struct tee_param *p = params + n;
		struct optee_msg_param *mp = msg_params + n;

		switch (p->attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_NONE:
			mp->attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
			memset(&mp->u, 0, sizeof(mp->u));
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT:
			optee_to_msg_param_value(mp, p);
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT:
			if (!p->u.memref.shm ||
			    !(p->u.memref.shm->flags & TEE_SHM_DYNAMIC)) {
				if (to_msg_param_tmp_mem(mp, p))
					return -EINVAL;
			} else if (to_msg_param_ffa_mem(mp, p)) {
				return -EINVAL;
			}
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * 3. Low level support functions to register shared memory in secure world
 *
 * Functions to register and unregister shared memory both for normal
 * clients and for tee-supplicant.
 */

static int optee_ffa_do_call_with_arg(struct tee_context *ctx,
                                    struct tee_shm *shm, u_int offs,
                                    bool system_thread);

static int optee_ffa_shm_register(struct tee_context *ctx, struct tee_shm *shm,
				  struct page **pages, size_t num_pages,
				  unsigned long start)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct ffa_device *ffa_dev = optee->ffa.ffa_dev;
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;
	struct ffa_mem_region_attributes mem_attr = {
		.receiver = ffa_dev->vm_id,
		.attrs = FFA_MEM_RW,
	};
	struct ffa_mem_ops_args args = {
		.use_txbuf = true,
		.attrs = &mem_attr,
		.nattrs = 1,
	};
	struct sg_table sgt;
	int rc;

	rc = optee_check_mem_type(start, num_pages);
	if (rc)
		return rc;

	rc = sg_alloc_table_from_pages(&sgt, pages, num_pages, 0,
				       num_pages * PAGE_SIZE, GFP_KERNEL);
	if (rc)
		return rc;
	args.sg = sgt.sgl;
	rc = mem_ops->memory_share(&args);
	sg_free_table(&sgt);
	if (rc)
		return rc;

	rc = optee_shm_add_ffa_handle(optee, shm, args.g_handle);
	if (rc) {
		mem_ops->memory_reclaim(args.g_handle, 0);
		return rc;
	}

	shm->sec_world_id = args.g_handle;

	return 0;
}

static int optee_ffa_shm_unregister(struct tee_context *ctx, struct tee_shm *shm)
{
 struct optee *optee = tee_get_drvdata(ctx->teedev);
 struct optee_shm_arg_entry *entry;
 struct optee_msg_arg *arg;
 struct tee_shm *arg_shm;
 u64 handle = shm->sec_world_id;
 u_int offs;
 int rc;

 arg = optee_get_msg_arg(ctx, 1, &entry, &arg_shm, &offs);
 if (IS_ERR(arg))
  return PTR_ERR(arg);
 arg->cmd = OPTEE_MSG_CMD_UNREGISTER_SHM;
 arg->params[0].attr = OPTEE_MSG_ATTR_TYPE_FMEM_INPUT;
 arg->params[0].u.fmem.global_id = handle;
 rc = optee_ffa_do_call_with_arg(ctx, arg_shm, offs, false);
 if (!rc && arg->ret != TEEC_SUCCESS)
  rc = -EIO;
 optee_free_msg_arg(ctx, entry, offs);
 if (rc)
  return rc;
 rc = optee->ffa.ffa_dev->ops->mem_ops->memory_reclaim(handle, 0);
 if (rc)
  return rc;
 optee_shm_rem_ffa_handle(optee, handle);
 shm->sec_world_id = 0;
 return 0;
}

static int optee_ffa_shm_unregister_supp(struct tee_context *ctx,
					 struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	const struct ffa_mem_ops *mem_ops;
	u64 global_handle = shm->sec_world_id;
	int rc;

	/*
	 * We're skipping the OPTEE_FFA_YIELDING_CALL_UNREGISTER_SHM call
	 * since this is OP-TEE freeing via RPC so it has already retired
	 * this ID.
	 */

	optee_shm_rem_ffa_handle(optee, global_handle);
	mem_ops = optee->ffa.ffa_dev->ops->mem_ops;
	rc = mem_ops->memory_reclaim(global_handle, 0);
	if (rc)
		pr_err("mem_reclaim: 0x%llx %d", global_handle, rc);

	shm->sec_world_id = 0;

	return rc;
}

/*
 * 4. Dynamic shared memory pool based on alloc_pages()
 *
 * Implements an OP-TEE specific shared memory pool.
 * The main function is optee_ffa_shm_pool_alloc_pages().
 */

static int pool_ffa_op_alloc(struct tee_shm_pool *pool,
			     struct tee_shm *shm, size_t size, size_t align)
{
	return tee_dyn_shm_alloc_helper(shm, size, align,
					optee_ffa_shm_register);
}

static void pool_ffa_op_free(struct tee_shm_pool *pool,
			     struct tee_shm *shm)
{
	tee_dyn_shm_free_helper(shm, optee_ffa_shm_unregister);
}

static void pool_ffa_op_destroy_pool(struct tee_shm_pool *pool)
{
	kfree(pool);
}

static const struct tee_shm_pool_ops pool_ffa_ops = {
	.alloc = pool_ffa_op_alloc,
	.free = pool_ffa_op_free,
	.destroy_pool = pool_ffa_op_destroy_pool,
};

/**
 * optee_ffa_shm_pool_alloc_pages() - create page-based allocator pool
 *
 * This pool is used with OP-TEE over FF-A. In this case command buffers
 * and such are allocated from kernel's own memory.
 */
static struct tee_shm_pool *__maybe_unused optee_ffa_shm_pool_alloc_pages(void)
{
	struct tee_shm_pool *pool = kzalloc(sizeof(*pool), GFP_KERNEL);

	if (!pool)
		return ERR_PTR(-ENOMEM);

	pool->ops = &pool_ffa_ops;

	return pool;
}

struct mitee_shm_pool {
	struct tee_shm_pool pool;
	struct tee_shm_pool *private;
	struct tee_shm_pool *shared;
};

static struct tee_shm_pool *mitee_shm_pool_for(struct tee_shm_pool *pool,
					      const struct tee_shm *shm)
{
	struct mitee_shm_pool *mitee = container_of(pool, struct mitee_shm_pool,
						   pool);

	if (shm->flags & TEE_SHM_PRIV)
		return mitee->private;
	return mitee->shared;
}

static int mitee_shm_pool_alloc(struct tee_shm_pool *pool, struct tee_shm *shm,
				size_t size, size_t align)
{
	struct tee_shm_pool *target = mitee_shm_pool_for(pool, shm);

	return target->ops->alloc(target, shm, size,
				  shm->flags & TEE_SHM_PRIV ? 8 : align);
}

static void mitee_shm_pool_free(struct tee_shm_pool *pool, struct tee_shm *shm)
{
	struct tee_shm_pool *target = mitee_shm_pool_for(pool, shm);

	target->ops->free(target, shm);
}

static void mitee_shm_pool_destroy(struct tee_shm_pool *pool)
{
	struct mitee_shm_pool *mitee = container_of(pool, struct mitee_shm_pool,
						   pool);

	tee_shm_pool_free(mitee->shared);
	tee_shm_pool_free(mitee->private);
	kfree(mitee);
}

static const struct tee_shm_pool_ops mitee_shm_pool_ops = {
	.alloc = mitee_shm_pool_alloc,
	.free = mitee_shm_pool_free,
	.destroy_pool = mitee_shm_pool_destroy,
};

static struct tee_shm_pool *mitee_shm_pool_create(unsigned long va,
						 phys_addr_t pa, size_t size)
{
	struct mitee_shm_pool *mitee;
	int rc;

	mitee = kzalloc(sizeof(*mitee), GFP_KERNEL);
	if (!mitee)
		return ERR_PTR(-ENOMEM);

	mitee->private = tee_shm_pool_alloc_res_mem(va, pa, PAGE_SIZE, 3);
	if (IS_ERR(mitee->private)) {
		rc = PTR_ERR(mitee->private);
		goto err_free;
	}

	mitee->shared = tee_shm_pool_alloc_res_mem(va + PAGE_SIZE,
						  pa + PAGE_SIZE,
						  size - PAGE_SIZE,
						  PAGE_SHIFT);
	if (IS_ERR(mitee->shared)) {
		rc = PTR_ERR(mitee->shared);
		goto err_private;
	}

	mitee->pool.ops = &mitee_shm_pool_ops;
	return &mitee->pool;

err_private:
	tee_shm_pool_free(mitee->private);
err_free:
	kfree(mitee);
	return ERR_PTR(rc);
}

/*
 * 5. Do a normal scheduled call into secure world
 *
 * The function optee_ffa_do_call_with_arg() performs a normal scheduled
 * call into secure world. During this call may normal world request help
 * from normal world using RPCs, Remote Procedure Calls. This includes
 * delivery of non-secure interrupts to for instance allow rescheduling of
 * the current task.
 */

static void handle_ffa_rpc_func_cmd_shm_alloc(struct tee_context *ctx,
					      struct optee *optee,
					      struct optee_msg_arg *arg)
{
	struct tee_shm *shm;

	if (arg->num_params != 1 ||
	    arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	switch (arg->params[0].u.value.a) {
	case OPTEE_RPC_SHM_TYPE_APPL:
		shm = optee_rpc_cmd_alloc_suppl(ctx, arg->params[0].u.value.b);
		break;
	case OPTEE_RPC_SHM_TYPE_KERNEL:
		shm = tee_shm_alloc_priv_buf(optee->ctx,
					     arg->params[0].u.value.b);
		break;
	default:
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	if (IS_ERR(shm)) {
		arg->ret = TEEC_ERROR_OUT_OF_MEMORY;
		return;
	}

	arg->params[0] = (struct optee_msg_param){
		.attr = OPTEE_MSG_ATTR_TYPE_FMEM_OUTPUT,
		.u.fmem.size = tee_shm_get_size(shm),
		.u.fmem.global_id = shm->sec_world_id,
		.u.fmem.internal_offs = shm->offset,
	};

	arg->ret = TEEC_SUCCESS;
}

static void handle_ffa_rpc_func_cmd_shm_free(struct tee_context *ctx,
					     struct optee *optee,
					     struct optee_msg_arg *arg)
{
	struct tee_shm *shm;

	if (arg->num_params != 1 ||
	    arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)
		goto err_bad_param;

	shm = optee_shm_from_ffa_handle(optee, arg->params[0].u.value.b);
	if (!shm)
		goto err_bad_param;
	switch (arg->params[0].u.value.a) {
	case OPTEE_RPC_SHM_TYPE_APPL:
		optee_rpc_cmd_free_suppl(ctx, shm);
		break;
	case OPTEE_RPC_SHM_TYPE_KERNEL:
		tee_shm_free(shm);
		break;
	default:
		goto err_bad_param;
	}
	arg->ret = TEEC_SUCCESS;
	return;

err_bad_param:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

static void handle_ffa_rpc_func_cmd(struct tee_context *ctx,
				    struct optee *optee,
				    struct optee_msg_arg *arg)
{
	arg->ret_origin = TEEC_ORIGIN_COMMS;
	switch (arg->cmd) {
	case OPTEE_RPC_CMD_SHM_ALLOC:
		handle_ffa_rpc_func_cmd_shm_alloc(ctx, optee, arg);
		break;
	case OPTEE_RPC_CMD_SHM_FREE:
		handle_ffa_rpc_func_cmd_shm_free(ctx, optee, arg);
		break;
	default:
		optee_rpc_cmd(ctx, optee, arg);
	}
}

static void __maybe_unused optee_handle_ffa_rpc(struct tee_context *ctx, struct optee *optee,
				 u32 cmd, struct optee_msg_arg *arg)
{
	switch (cmd) {
	case OPTEE_FFA_YIELDING_CALL_RETURN_RPC_CMD:
		handle_ffa_rpc_func_cmd(ctx, optee, arg);
		break;
	case OPTEE_FFA_YIELDING_CALL_RETURN_INTERRUPT:
		/* Interrupt delivered by now */
		break;
	default:
		pr_warn("Unknown RPC func 0x%x\n", cmd);
		break;
	}
}

static int optee_ffa_yielding_call(struct tee_context *ctx,
                                 struct ffa_send_direct_data *data,
                                 struct optee_msg_arg *rpc_arg,
                                 bool system_thread)
{
 struct optee *optee = tee_get_drvdata(ctx->teedev);
 struct ffa_device *dev = optee->ffa.ffa_dev;
 struct optee_call_waiter waiter;
 struct ffa_send_direct_data request = *data;
 cpumask_var_t saved;
 bool rpc_cpu_bound = false;
 struct tee_shm *shm;
 phys_addr_t pa;
 int saved_nice;
 int rc;

 if (!alloc_cpumask_var(&saved, GFP_KERNEL))
  return -ENOMEM;
 optee_cq_wait_init(&optee->call_queue, &waiter, system_thread);
 mutex_lock(&mitee_call_mutex);
 cpumask_copy(saved, current->cpus_ptr);
 saved_nice = task_nice(current);
 if (!(current->flags & PF_NO_SETAFFINITY)) {
  rc = set_cpus_allowed_ptr(current, &optee->ffa.binding_cpus);
  if (rc)
   goto out_call;
 }
 set_user_nice(current, MIN_NICE);
 for (;;) {
  pr_info_ratelimited("mitee: pid %d cpu %d FF-A send %#lx x1 %#lx x2 %#lx x3 %#lx x4 %#lx\n",
                      current->pid, raw_smp_processor_id(), data->data0,
                      data->data1, data->data2, data->data3, data->data4);
  rc = dev->ops->msg_ops->sync_send_receive(dev, data);
  pr_info_ratelimited("mitee: pid %d cpu %d FF-A return %d status %#lx rpc %lu x2 %#lx x3 %#lx x4 %#lx\n",
                      current->pid, raw_smp_processor_id(), rc, data->data0,
                      data->data1, data->data2, data->data3, data->data4);
  if (rc)
   break;
  atomic_notifier_call_chain(&optee->call_notifier, 1, NULL);
  if ((u32)data->data0 == TEEC_ERROR_BUSY) {
   if (request.data0 == OPTEE_FFA_YIELDING_CALL_RESUME) {
    rc = -EIO;
    break;
   }
   optee_cq_wait_for_completion(&optee->call_queue, &waiter);
   *data = request;
   continue;
  }
  if ((u32)data->data0 != TEEC_SUCCESS) {
   rc = -EIO;
   break;
  }
  if (!data->data1)
   break;
  if (!rpc_cpu_bound && data->data2 < nr_cpu_ids &&
      cpu_online(data->data2)) {
   rc = set_cpus_allowed_ptr(current, cpumask_of(data->data2));
   if (rc)
    break;
   rpc_cpu_bound = true;
  }
  switch (data->data1) {
  case 3: /* Allocate RPC message buffer; return physical address and cookie. */
   shm = tee_shm_alloc_priv_buf(ctx, data->data3);
   if (IS_ERR(shm)) {
    data->data3 = 0;
    data->data4 = 0;
   } else {
    rc = tee_shm_get_pa(shm, 0, &pa);
    if (rc) {
     tee_shm_free(shm);
     break;
    }
    data->data3 = pa;
    data->data4 = (unsigned long)shm;
    pr_info_ratelimited("mitee: pid %d RPC buffer size %zu pa %pa cookie %px\n",
                        current->pid, shm->size, &pa, shm);
   }
   data->data1 = 3;
   break;
  case 4: /* Free RPC message buffer. */
   tee_shm_free((struct tee_shm *)(unsigned long)data->data3);
   data->data1 = 4;
   data->data3 = 0;
   data->data4 = 0;
   break;
  case 1: /* Execute RPC command using the shared-memory cookie. */
   shm = (struct tee_shm *)(unsigned long)data->data3;
   rpc_arg = tee_shm_get_va(shm, 0);
   if (IS_ERR(rpc_arg)) {
    rc = PTR_ERR(rpc_arg);
    break;
   }
   pr_info_ratelimited("mitee: pid %d RPC command %u enter\n",
                       current->pid, rpc_arg->cmd);
   rpc_arg->ret_origin = TEEC_ORIGIN_COMMS;
   if (rpc_arg->cmd == OPTEE_RPC_CMD_SHM_ALLOC) {
    u64 type, size;

    if (rpc_arg->num_params != 1 ||
        rpc_arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) {
     rpc_arg->ret = TEEC_ERROR_BAD_PARAMETERS;
    } else {
     type = rpc_arg->params[0].u.value.a;
     size = rpc_arg->params[0].u.value.b;
     if (!type)
      shm = optee_rpc_cmd_alloc_suppl(ctx, size);
     else if (type == 1)
      shm = tee_shm_alloc_priv_buf(ctx, size);
     else if (type == 2)
      shm = tee_shm_alloc_kernel_buf(ctx, size);
     else
      shm = ERR_PTR(-EINVAL);
     if (IS_ERR(shm)) {
      rpc_arg->ret = TEEC_ERROR_OUT_OF_MEMORY;
     } else if (tee_shm_get_pa(shm, 0, &pa)) {
      tee_shm_free(shm);
      rpc_arg->ret = TEEC_ERROR_BAD_PARAMETERS;
     } else {
      rpc_arg->params[0].attr = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
      rpc_arg->params[0].u.tmem.buf_ptr = pa;
      rpc_arg->params[0].u.tmem.size = shm->size;
      rpc_arg->params[0].u.tmem.shm_ref = (unsigned long)shm;
      rpc_arg->ret = TEEC_SUCCESS;
      pr_info_ratelimited("mitee: pid %d SHM_ALLOC type %llu requested %llu size %zu pa %pa cookie %px\n",
                          current->pid, type, size, shm->size, &pa, shm);
     }
    }
   } else if (rpc_arg->cmd == OPTEE_RPC_CMD_SHM_FREE) {
    if (rpc_arg->num_params != 1 ||
        rpc_arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) {
     rpc_arg->ret = TEEC_ERROR_BAD_PARAMETERS;
    } else {
     u64 type = rpc_arg->params[0].u.value.a;

     shm = (struct tee_shm *)(unsigned long)rpc_arg->params[0].u.value.b;
     if (!type) {
      optee_rpc_cmd_free_suppl(ctx, shm);
      rpc_arg->ret = TEEC_SUCCESS;
     } else if (type == 1 || type == 2) {
      tee_shm_free(shm);
      rpc_arg->ret = TEEC_SUCCESS;
     } else {
      rpc_arg->ret = TEEC_ERROR_BAD_PARAMETERS;
     }
    }
   } else {
    optee_rpc_cmd(ctx, optee, rpc_arg);
   }
   pr_info_ratelimited("mitee: pid %d RPC command %u return %#x\n",
                       current->pid, rpc_arg->cmd, rpc_arg->ret);
   data->data1 = 1;
   data->data3 = 0;
   data->data4 = 0;
   break;
  case 2: /* Resume after a secure-world interrupt. */
   break;
  default:
   rc = -EINVAL;
  }
  if (rc)
   break;
  data->data0 = OPTEE_FFA_YIELDING_CALL_RESUME;
  request.data0 = OPTEE_FFA_YIELDING_CALL_RESUME;
  cond_resched();
 }
out_call:
 optee_cq_wait_final(&optee->call_queue, &waiter);
 set_cpus_allowed_ptr(current, saved);
 set_user_nice(current, saved_nice);
 mutex_unlock(&mitee_call_mutex);
 free_cpumask_var(saved);
 return rc;
}

/**
 * optee_ffa_do_call_with_arg() - Do a FF-A call to enter OP-TEE in secure world
 * @ctx:	calling context
 * @shm:	shared memory holding the message to pass to secure world
 * @offs:	offset of the message in @shm
 * @system_thread: true if caller requests TEE system thread support
 *
 * Does a FF-A call to OP-TEE in secure world and handles eventual resulting
 * Remote Procedure Calls (RPC) from OP-TEE.
 *
 * Returns return code from FF-A, 0 is OK
 */

static int optee_ffa_do_call_with_arg(struct tee_context *ctx,
                                    struct tee_shm *shm, u_int offs,
                                    bool system_thread)
{
 struct ffa_send_direct_data data = {
  .data0 = OPTEE_FFA_YIELDING_CALL_WITH_ARG,
 };
 phys_addr_t pa;
 int rc = tee_shm_get_pa(shm, offs, &pa);

 if (rc)
  return rc;
 data.data1 = upper_32_bits(pa);
 data.data2 = lower_32_bits(pa);
 data.data3 = shm->offset;
 return optee_ffa_yielding_call(ctx, &data, NULL, system_thread);
}

static int do_call_lend_protmem(struct optee *optee, u64 cookie, u32 use_case)
{
	struct optee_shm_arg_entry *entry;
	struct optee_msg_arg *msg_arg;
	struct tee_shm *shm;
	u_int offs;
	int rc;

	msg_arg = optee_get_msg_arg(optee->ctx, 1, &entry, &shm, &offs);
	if (IS_ERR(msg_arg))
		return PTR_ERR(msg_arg);

	msg_arg->cmd = OPTEE_MSG_CMD_ASSIGN_PROTMEM;
	msg_arg->params[0].attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
	msg_arg->params[0].u.value.a = cookie;
	msg_arg->params[0].u.value.b = use_case;

	rc = optee->ops->do_call_with_arg(optee->ctx, shm, offs, false);
	if (rc)
		goto out;
	if (msg_arg->ret != TEEC_SUCCESS) {
		rc = -EINVAL;
		goto out;
	}

out:
	optee_free_msg_arg(optee->ctx, entry, offs);
	return rc;
}

static int optee_ffa_lend_protmem(struct optee *optee, struct tee_shm *protmem,
				  u32 *mem_attrs, unsigned int ma_count,
				  u32 use_case)
{
	struct ffa_device *ffa_dev = optee->ffa.ffa_dev;
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;
	const struct ffa_msg_ops *msg_ops = ffa_dev->ops->msg_ops;
	struct ffa_send_direct_data data;
	struct ffa_mem_region_attributes *mem_attr;
	struct ffa_mem_ops_args args = {
		.use_txbuf = true,
		.tag = use_case,
	};
	struct page *page;
	struct scatterlist sgl;
	unsigned int n;
	int rc;

	mem_attr = kcalloc(ma_count, sizeof(*mem_attr), GFP_KERNEL);
	for (n = 0; n < ma_count; n++) {
		mem_attr[n].receiver = mem_attrs[n] & U16_MAX;
		mem_attr[n].attrs = mem_attrs[n] >> 16;
	}
	args.attrs = mem_attr;
	args.nattrs = ma_count;

	page = phys_to_page(protmem->paddr);
	sg_init_table(&sgl, 1);
	sg_set_page(&sgl, page, protmem->size, 0);

	args.sg = &sgl;
	rc = mem_ops->memory_lend(&args);
	kfree(mem_attr);
	if (rc)
		return rc;

	rc = do_call_lend_protmem(optee, args.g_handle, use_case);
	if (rc)
		goto err_reclaim;

	rc = optee_shm_add_ffa_handle(optee, protmem, args.g_handle);
	if (rc)
		goto err_unreg;

	protmem->sec_world_id = args.g_handle;

	return 0;

err_unreg:
	data = (struct ffa_send_direct_data){
		.data0 = OPTEE_FFA_RELEASE_PROTMEM,
		.data1 = (u32)args.g_handle,
		.data2 = (u32)(args.g_handle >> 32),
	};
	msg_ops->sync_send_receive(ffa_dev, &data);
err_reclaim:
	mem_ops->memory_reclaim(args.g_handle, 0);
	return rc;
}

static int optee_ffa_reclaim_protmem(struct optee *optee,
				     struct tee_shm *protmem)
{
	struct ffa_device *ffa_dev = optee->ffa.ffa_dev;
	const struct ffa_msg_ops *msg_ops = ffa_dev->ops->msg_ops;
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;
	u64 global_handle = protmem->sec_world_id;
	struct ffa_send_direct_data data = {
		.data0 = OPTEE_FFA_RELEASE_PROTMEM,
		.data1 = (u32)global_handle,
		.data2 = (u32)(global_handle >> 32)
	};
	int rc;

	optee_shm_rem_ffa_handle(optee, global_handle);
	protmem->sec_world_id = 0;

	rc = msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc)
		pr_err("Release SHM id 0x%llx rc %d\n", global_handle, rc);

	rc = mem_ops->memory_reclaim(global_handle, 0);
	if (rc)
		pr_err("mem_reclaim: 0x%llx %d", global_handle, rc);

	return rc;
}

/*
 * 6. Driver initialization
 *
 * During driver inititialization is the OP-TEE Secure Partition is probed
 * to find out which features it supports so the driver can be initialized
 * with a matching configuration.
 */

static bool __maybe_unused optee_ffa_api_is_compatible(struct ffa_device *ffa_dev,
					const struct ffa_ops *ops)
{
	const struct ffa_msg_ops *msg_ops = ops->msg_ops;
	struct ffa_send_direct_data data = {
		.data0 = OPTEE_FFA_GET_API_VERSION,
	};
	int rc;

	msg_ops->mode_32bit_set(ffa_dev);

	rc = msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc) {
		pr_err("Unexpected error %d\n", rc);
		return false;
	}
	if (data.data0 != OPTEE_FFA_VERSION_MAJOR ||
	    data.data1 < OPTEE_FFA_VERSION_MINOR) {
		pr_err("Incompatible OP-TEE API version %lu.%lu",
		       data.data0, data.data1);
		return false;
	}

	data = (struct ffa_send_direct_data){
		.data0 = OPTEE_FFA_GET_OS_VERSION,
	};
	rc = msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc) {
		pr_err("Unexpected error %d\n", rc);
		return false;
	}
	if (data.data2)
		pr_info("revision %lu.%lu (%08lx)",
			data.data0, data.data1, data.data2);
	else
		pr_info("revision %lu.%lu", data.data0, data.data1);

	return true;
}

static bool __maybe_unused optee_ffa_exchange_caps(struct ffa_device *ffa_dev,
				    const struct ffa_ops *ops,
				    u32 *sec_caps,
				    unsigned int *rpc_param_count,
				    unsigned int *max_notif_value)
{
	struct ffa_send_direct_data data = {
		.data0 = OPTEE_FFA_EXCHANGE_CAPABILITIES,
	};
	int rc;

	rc = ops->msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc) {
		pr_err("Unexpected error %d", rc);
		return false;
	}
	if (data.data0) {
		pr_err("Unexpected exchange error %lu", data.data0);
		return false;
	}

	*rpc_param_count = (u8)data.data1;
	*sec_caps = data.data2;
	if (data.data3)
		*max_notif_value = data.data3;
	else
		*max_notif_value = OPTEE_DEFAULT_MAX_NOTIF_VALUE;

	return true;
}

static void notif_work_fn(struct work_struct *work)
{
	struct optee_ffa *optee_ffa = container_of(work, struct optee_ffa,
						   notif_work);
	struct optee *optee = container_of(optee_ffa, struct optee, ffa);

	optee_do_bottom_half(optee->ctx);
}

static void notif_callback(int notify_id, void *cb_data)
{
	struct optee *optee = cb_data;

	if (notify_id == optee->ffa.bottom_half_value)
		queue_work(optee->ffa.notif_wq, &optee->ffa.notif_work);
	else
		optee_notif_send(optee, notify_id);
}

static int enable_async_notif(struct optee *optee)
{
	struct ffa_device *ffa_dev = optee->ffa.ffa_dev;
	struct ffa_send_direct_data data = {
		.data0 = OPTEE_FFA_ENABLE_ASYNC_NOTIF,
		.data1 = optee->ffa.bottom_half_value,
	};
	int rc;

	rc = ffa_dev->ops->msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc)
		return rc;
	return data.data0;
}

static void optee_ffa_get_version(struct tee_device *teedev,
				  struct tee_ioctl_version_data *vers)
{
	struct optee *optee = tee_get_drvdata(teedev);
	struct tee_ioctl_version_data v = {
		.impl_id = TEE_IMPL_ID_OPTEE,
		.impl_caps = TEE_OPTEE_CAP_TZ,
		.gen_caps = TEE_GEN_CAP_GP,
	};

	if (teedev == optee->teedev)
		v.gen_caps |= TEE_GEN_CAP_REG_MEM;

	*vers = v;
}

static int optee_ffa_open(struct tee_context *ctx)
{
	return optee_open(ctx, false);
}

static const struct tee_driver_ops optee_ffa_clnt_ops = {
	.get_version = optee_ffa_get_version,
	.open = optee_ffa_open,
	.release = optee_release,
	.open_session = optee_open_session,
	.close_session = optee_close_session,
	.invoke_func = optee_invoke_func,
	.cancel_req = optee_cancel_req,
	.shm_register = optee_ffa_shm_register,
	.shm_unregister = optee_ffa_shm_unregister,
};

static const struct tee_desc optee_ffa_clnt_desc = {
	.name = DRIVER_NAME "-ffa-clnt",
	.ops = &optee_ffa_clnt_ops,
	.owner = THIS_MODULE,
};

static const struct tee_driver_ops optee_ffa_supp_ops = {
	.get_version = optee_ffa_get_version,
	.open = optee_ffa_open,
	.release = optee_release_supp,
	.supp_recv = optee_supp_recv,
	.supp_send = optee_supp_send,
	.shm_register = optee_ffa_shm_register, /* same as for clnt ops */
	.shm_unregister = optee_ffa_shm_unregister_supp,
};

static const struct tee_desc optee_ffa_supp_desc = {
	.name = DRIVER_NAME "-ffa-supp",
	.ops = &optee_ffa_supp_ops,
	.owner = THIS_MODULE,
	.flags = TEE_DESC_PRIVILEGED,
};

static const struct optee_ops optee_ffa_ops = {
	.do_call_with_arg = optee_ffa_do_call_with_arg,
	.to_msg_param = optee_ffa_to_msg_param,
	.from_msg_param = optee_ffa_from_msg_param,
	.lend_protmem = optee_ffa_lend_protmem,
	.reclaim_protmem = optee_ffa_reclaim_protmem,
};

static void optee_ffa_remove(struct ffa_device *ffa_dev)
{
	struct optee *optee = ffa_dev_get_drvdata(ffa_dev);
	u32 bottom_half_id = optee->ffa.bottom_half_value;

	if (bottom_half_id != U32_MAX) {
		ffa_dev->ops->notifier_ops->notify_relinquish(ffa_dev,
							      bottom_half_id);
		destroy_workqueue(optee->ffa.notif_wq);
	}
	void *shm_va = optee->ffa.shm_va;

	optee_remove_common(optee);
	mitee_memlog_remove(optee);
	memunmap(shm_va);

	mutex_destroy(&optee->ffa.mutex);
	rhashtable_free_and_destroy(&optee->ffa.global_ids, rh_free_fn, NULL);

	kfree(optee);
}

static int optee_ffa_async_notif_init(struct ffa_device *ffa_dev,
				      struct optee *optee)
{
	bool is_per_vcpu = false;
	u32 notif_id = 0;
	int rc;

	INIT_WORK(&optee->ffa.notif_work, notif_work_fn);
	optee->ffa.notif_wq = create_workqueue("optee_notification");
	if (!optee->ffa.notif_wq) {
		rc = -EINVAL;
		goto err;
	}

	while (true) {
		rc = ffa_dev->ops->notifier_ops->notify_request(ffa_dev,
								is_per_vcpu,
								notif_callback,
								optee,
								notif_id);
		if (!rc)
			break;
		/*
		 * -EACCES means that the notification ID was
		 * already bound, try the next one as long as we
		 * haven't reached the max. Any other error is a
		 * permanent error, so skip asynchronous
		 * notifications in that case.
		 */
		if (rc != -EACCES)
			goto err_wq;
		notif_id++;
		if (notif_id >= OPTEE_FFA_MAX_ASYNC_NOTIF_VALUE)
			goto err_wq;
	}
	optee->ffa.bottom_half_value = notif_id;

	rc = enable_async_notif(optee);
	if (rc < 0)
		goto err_rel;

	return 0;
err_rel:
	ffa_dev->ops->notifier_ops->notify_relinquish(ffa_dev, notif_id);
err_wq:
	destroy_workqueue(optee->ffa.notif_wq);
err:
	optee->ffa.bottom_half_value = U32_MAX;

	return rc;
}

static int optee_ffa_protmem_pool_init(struct optee *optee, u32 sec_caps)
{
	enum tee_dma_heap_id id = TEE_DMA_HEAP_SECURE_VIDEO_PLAY;
	struct tee_protmem_pool *pool;
	int rc = 0;

	if (sec_caps & OPTEE_FFA_SEC_CAP_PROTMEM) {
		pool = optee_protmem_alloc_dyn_pool(optee, id);
		if (IS_ERR(pool))
			return PTR_ERR(pool);

		rc = tee_device_register_dma_heap(optee->teedev, id, pool);
		if (rc)
			pool->ops->destroy_pool(pool);
	}

	return rc;
}

static int optee_ffa_probe(struct ffa_device *ffa_dev)
{
	const struct ffa_ops *ffa_ops;
	unsigned int rpc_param_count = 4;
	struct tee_shm_pool *pool;
	struct tee_device *teedev;
	struct tee_context *ctx;
	u32 arg_cache_flags = OPTEE_SHM_ARG_ALLOC_PRIV | OPTEE_SHM_ARG_NO_CACHE;
	struct optee *optee;
	u32 sec_caps = 0;
	int rc;

	ffa_ops = ffa_dev->ops;


	optee = kzalloc(sizeof(*optee), GFP_KERNEL);
	if (!optee)
		return -ENOMEM;

 {
  struct ffa_send_direct_data msg = { .data0 = 10 };
  phys_addr_t base, end;
  int i, count;
  struct device_node *node;

  rc = ffa_ops->msg_ops->sync_send_receive(ffa_dev, &msg);
  if (rc)
   goto err_free_optee;
  if (msg.data2 != 1) {
   rc = -EINVAL;
   goto err_free_optee;
  }
  base = ALIGN(msg.data0, PAGE_SIZE);
  end = round_down(msg.data0 + msg.data1, PAGE_SIZE);
  if (end <= base || end - base < 2 * PAGE_SIZE) {
   rc = -EINVAL;
   goto err_free_optee;
  }
  optee->ffa.shm_va = memremap(base, end - base, MEMREMAP_WB);
  if (!optee->ffa.shm_va) {
   rc = -ENOMEM;
   goto err_free_optee;
  }
  pool = mitee_shm_pool_create((unsigned long)optee->ffa.shm_va,
                              base, end - base);
  if (IS_ERR(pool)) {
   rc = PTR_ERR(pool);
   memunmap(optee->ffa.shm_va);
   goto err_free_optee;
  }
  cpumask_clear(&optee->ffa.binding_cpus);
  node = of_find_compatible_node(NULL, NULL, "xiaomi,mitee");
  if (!node) {
   rc = -ENODEV;
   tee_shm_pool_free(pool);
   memunmap(optee->ffa.shm_va);
   goto err_free_optee;
  }
  count = of_property_count_u32_elems(node, "binding-core");
  for (i = 0; i < count; i++) {
   u32 cpu;

   rc = of_property_read_u32_index(node, "binding-core", i, &cpu);
   if (rc || cpu >= nr_cpu_ids) {
    rc = -EINVAL;
    break;
   }
   cpumask_set_cpu(cpu, &optee->ffa.binding_cpus);
  }
  of_node_put(node);
  if (rc || cpumask_empty(&optee->ffa.binding_cpus)) {
   rc = -EINVAL;
   tee_shm_pool_free(pool);
   memunmap(optee->ffa.shm_va);
   goto err_free_optee;
  }
 }
	optee->pool = pool;

	optee->ops = &optee_ffa_ops;
	optee->ffa.ffa_dev = ffa_dev;
	optee->ffa.bottom_half_value = U32_MAX;
	optee->rpc_param_count = rpc_param_count;
	ATOMIC_INIT_NOTIFIER_HEAD(&optee->call_notifier);

	if (IS_REACHABLE(CONFIG_RPMB) &&
	    (sec_caps & OPTEE_FFA_SEC_CAP_RPMB_PROBE))
		optee->in_kernel_rpmb_routing = true;

	teedev = tee_device_alloc(&optee_ffa_clnt_desc, NULL, optee->pool,
				  optee);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		goto err_free_shm_pool;
	}
	optee->teedev = teedev;

	teedev = tee_device_alloc(&optee_ffa_supp_desc, NULL, optee->pool,
				  optee);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		goto err_unreg_teedev;
	}
	optee->supp_teedev = teedev;

	optee_set_dev_group(optee);

	rc = tee_device_register(optee->teedev);
	if (rc)
		goto err_unreg_supp_teedev;

	rc = tee_device_register(optee->supp_teedev);
	if (rc)
		goto err_unreg_supp_teedev;

	rc = rhashtable_init(&optee->ffa.global_ids, &shm_rhash_params);
	if (rc)
		goto err_unreg_supp_teedev;
	mutex_init(&optee->ffa.mutex);
	optee_cq_init(&optee->call_queue, 0);
	optee_supp_init(&optee->supp);
	optee_shm_arg_cache_init(optee, arg_cache_flags);
	mutex_init(&optee->rpmb_dev_mutex);
	ffa_dev_set_drvdata(ffa_dev, optee);
	ctx = teedev_open(optee->teedev);
	if (IS_ERR(ctx)) {
		rc = PTR_ERR(ctx);
		goto err_rhashtable_free;
	}
	optee->ctx = ctx;
	rc = optee_notif_init(optee, OPTEE_DEFAULT_MAX_NOTIF_VALUE);
	if (rc)
		goto err_close_ctx;
	if (sec_caps & OPTEE_FFA_SEC_CAP_ASYNC_NOTIF) {
		rc = optee_ffa_async_notif_init(ffa_dev, optee);
		if (rc < 0)
			pr_err("Failed to initialize async notifications: %d",
			       rc);
	}

	if (optee_ffa_protmem_pool_init(optee, sec_caps))
		pr_info("Protected memory service not available\n");

	/* MiTEE FF-A probe registers endpoints without opening the OP-TEE
	 * enumeration PTA. TA sessions are opened by the normal client path.
	 */
	rc = mitee_memlog_probe(optee);
	if (rc) {
		pr_err("Failed to initialize MiTEE log buffer: %d\n", rc);
		goto err_notif;
	}

	INIT_WORK(&optee->rpmb_scan_bus_work, optee_bus_scan_rpmb);
	optee->rpmb_intf.notifier_call = optee_rpmb_intf_rdev;
	blocking_notifier_chain_register(&optee_rpmb_intf_added,
					 &optee->rpmb_intf);
	pr_info("initialized driver\n");
	return 0;

err_notif:
	if (optee->ffa.bottom_half_value != U32_MAX) {
		ffa_dev->ops->notifier_ops->notify_relinquish(ffa_dev,
						     optee->ffa.bottom_half_value);
		destroy_workqueue(optee->ffa.notif_wq);
	}
	optee_notif_uninit(optee);
err_close_ctx:
	teedev_close_context(ctx);
err_rhashtable_free:
	rhashtable_free_and_destroy(&optee->ffa.global_ids, rh_free_fn, NULL);
	rpmb_dev_put(optee->rpmb_dev);
	mutex_destroy(&optee->rpmb_dev_mutex);
	optee_supp_uninit(&optee->supp);
	mutex_destroy(&optee->call_queue.mutex);
	mutex_destroy(&optee->ffa.mutex);
err_unreg_supp_teedev:
	tee_device_unregister(optee->supp_teedev);
err_unreg_teedev:
	tee_device_unregister(optee->teedev);
err_free_shm_pool:
	tee_shm_pool_free(pool);
err_free_optee:
	kfree(optee);
	return rc;
}

static const struct ffa_device_id optee_ffa_device_id[] = {
	/* Stock MiTEE secure partition UUID. */
	{ UUID_INIT(0x06bb3aea, 0x7b38, 0x4dad,
		    0xa7, 0x0f, 0xf5, 0x99, 0x86, 0x57, 0x26, 0x11) },
	{}
};

static struct ffa_driver optee_ffa_driver = {
	.name = "mitee",
	.probe = optee_ffa_probe,
	.remove = optee_ffa_remove,
	.id_table = optee_ffa_device_id,
};

int optee_ffa_abi_register(void)
{
	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		return ffa_register(&optee_ffa_driver);
	else
		return -EOPNOTSUPP;
}

void optee_ffa_abi_unregister(void)
{
	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		ffa_unregister(&optee_ffa_driver);
}
