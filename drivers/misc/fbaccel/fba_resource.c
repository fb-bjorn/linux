// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#include <linux/errno.h>
#include <linux/genalloc.h>
#include <linux/idr.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/types.h>

#include "fba_ioctl.h"
#include "fba_device.h"
#include "fba_admin.h"

static void fba_resource_free(struct kref *kref)
{
	struct fba_resource *res = container_of(kref, struct fba_resource, ref);

	gen_pool_destroy(res->pool);
	kfree(res);
}

void fba_resource_put(struct fba_resource *res)
{
	kref_put(&res->ref, fba_resource_free);
}

struct fba_resource *fba_resource_get(struct fba_resource *res)
{
	kref_get(&res->ref);

	return res;
}

static int granularity_to_order(struct fba_device *fba_dev, uint64_t size)
{
	/* For debugging only. */
	if (size == 0) {
		fba_dev_err(fba_dev, "Unexpected size 0 granularity.");
		return 0;
	}
	if ((size & (size - 1)) != 0)
		fba_dev_err(fba_dev, "Non 2^n granularity of %llu", size);

	return 63 - __builtin_clzl(size);
}

/*
 * The gen_pool allocator does not allow a zero start, so we xor FBA_ZERO_BASE
 * into the start address if the resource starts at zero.
 */
#define FBA_ZERO_BASE 0xFBA0000000000000

static int fba_resource_pool_add(struct fba_resource *res, uint64_t addr,
				 size_t size)
{
	/* Special handling for resources that start at address zero */
	if (res->start == 0)
		addr = addr ^ FBA_ZERO_BASE;

	return gen_pool_add(res->pool, addr, size, -1);
}

static int fba_resource_pool_alloc(struct fba_resource *res, uint64_t size,
				   uint64_t *ret_addr)
{
	uint64_t addr;

	spin_lock(&res->pool_lock);
	addr = gen_pool_alloc(res->pool, size);
	spin_unlock(&res->pool_lock);

	if (!addr)
		return -ENOMEM;

	/* Special handling for resources that start at address zero */
	if (res->start != 0)
		*ret_addr = addr;
	else
		*ret_addr = addr ^ FBA_ZERO_BASE;

	return 0;
}

void fba_resource_pool_free(struct fba_resource *res, uint64_t addr,
			    uint64_t size)
{
	/* Special handling for resources that start at address zero */
	if (res->start == 0)
		addr = addr ^ FBA_ZERO_BASE;

	spin_lock(&res->pool_lock);
	gen_pool_free(res->pool, addr, size);
	spin_unlock(&res->pool_lock);
}

struct fba_resource *fba_resource_find(struct fba_device *fba_dev,
				       uint16_t index, uint16_t type,
				       uint16_t subtype)
{
	struct fba_resource *res;

	spin_lock(&fba_dev->resources_lock);
	if (index >= fba_dev->resource_cnt) {
		fba_dev_dbg(fba_dev, "Resource index out of range.");
		res = ERR_PTR(-EINVAL);
		goto out_unlock;
	}

	res = fba_resource_get(fba_dev->resources[index]);
	if ((res->type != type) || (res->subtype != subtype)) {
		fba_dev_dbg(fba_dev, "Resource request type mismatch.");
		fba_resource_put(res);
		res = ERR_PTR(-EINVAL);
	}

out_unlock:
	spin_unlock(&fba_dev->resources_lock);

	return res;
}

struct fba_alloc *fba_resource_allocate(struct fba_resource *res, uint64_t size)
{
	struct fba_alloc *alloc;
	uint64_t addr = 0;
	int ret;

	ret = fba_resource_pool_alloc(res, size, &addr);
	if (ret != 0)
		return ERR_PTR(-ENOMEM);

	alloc = fba_alloc_new(res, addr, size);
	if (!alloc)
		fba_resource_pool_free(res, addr, size);

	return alloc;
}

int fba_resource_info(struct fba_device *fba_dev, struct resource_info *info,
		      size_t offs)
{
	struct fba_resource *res;

	spin_lock(&fba_dev->resources_lock);
	if (offs >= fba_dev->resource_cnt) {
		spin_unlock(&fba_dev->resources_lock);
		return -EINVAL;
	}
	res = fba_resource_get(fba_dev->resources[offs]);
	spin_unlock(&fba_dev->resources_lock);

	info->type = res->type;
	info->subtype = res->subtype;
	info->start = res->start;
	info->misc = 0;

	spin_lock(&res->pool_lock);
	info->avail = gen_pool_avail(res->pool);
	info->size = gen_pool_size(res->pool);
	spin_unlock(&res->pool_lock);

	fba_resource_put(res);

	return 0;
}

static struct fba_resource *build_resource(struct fba_device *fba_dev,
					   struct resource_def *def, void *va)
{
	int order = granularity_to_order(fba_dev, def->gran);
	struct fba_resource *res;
	int ret;

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (res == NULL)
		return NULL;

	spin_lock_init(&res->pool_lock);
	kref_init(&res->ref);

	res->pool = gen_pool_create(order, -1);
	if (res->pool == NULL) {
		fba_dev_err(fba_dev, "Create pool w/gran %#llx failed.",
			    def->gran);
		ret = -ENOMEM;
		goto err_free_res;
	}

	res->type = def->type;
	res->subtype = def->subtype;
	res->start = def->start;
	res->va = va;

	ret = fba_resource_pool_add(res, def->start, def->count);
	if (ret != 0) {
		fba_dev_err(fba_dev, "Add to pool failed.");
		ret = -EINVAL;
		goto err_destroy_pool;
	}
	return res;

err_destroy_pool:
	gen_pool_destroy(res->pool);
err_free_res:
	kfree(res);

	return ERR_PTR(ret);
}

int fba_resource_init(struct fba_device *fba_dev)
{
	struct fba_resource **resources = NULL;
	struct fba_resource *res;
	uint64_t *words = NULL;
	void *temp_data = fba_dev->scratch_area.data;
	dma_addr_t temp_addr = fba_dev->scratch_area.addr;
	struct resource_def scratch_def;
	size_t cnt = 0;
	int idx, r_total;
	int ret = -ENOMEM;

	/* Capture current value of INFO_WORDS */
	ret = fba_admin_get_cfg_data(fba_dev, temp_addr, ADMIN_GET_INFO_WORDS,
				     INFO_WORD_FIRST, INFO_WORD_APP_START);
	if (ret) {
		fba_dev_err(fba_dev, "Failed to get info words first.");
		goto err_put_resources;
	}
	words = (uint64_t *)temp_data;
	fba_dev->n_ctxs = words[INFO_CONTEXT_MAX_COUNT];
	fba_dev->info_cnt = words[INFO_WORD_MAX_COUNT];
	fba_dev->mon.src_info[SAIL_MTS_CCP].item_cnt =
		words[INFO_MON_ITEM_COUNT];
	fba_dev->mon.src_info[SAIL_MTS_CCP].group_cnt =
		words[INFO_MON_GROUP_COUNT];
	fba_dev->max_dma_segment_size = words[INFO_DMA_MAX_SEGMENT_SIZE];
	fba_dev->platform_type = words[INFO_PLATFORM_TYPE];

	if (fba_dev->info_cnt < INFO_WORD_APP_START) {
		fba_dev_err(
			fba_dev,
			"%s: info_cnt %llu is invalid, min expected value is %d.",
			__func__, fba_dev->info_cnt, INFO_WORD_APP_START);
		return -EFAULT;
	}

	words = kzalloc(sizeof(uint64_t) * fba_dev->info_cnt, GFP_KERNEL);
	if (words == NULL) {
		ret = -ENOMEM;
		fba_dev_err(fba_dev, "Failed to allocate info_data.");
		goto err_put_resources;
	}

	while (cnt < fba_dev->info_cnt) {
		size_t len = (PAGE_SIZE / sizeof(uint64_t));

		len = ((len + cnt) > fba_dev->info_cnt) ?
				    fba_dev->info_cnt - cnt :
				    len;
		ret = fba_admin_get_cfg_data(fba_dev, temp_addr,
					     ADMIN_GET_INFO_WORDS, cnt, len);
		if (ret != 0) {
			fba_dev_err(fba_dev,
				    "Failed to get info words %d:%d = %d.",
				    (int)cnt, (int)len, (int)ret);
			goto err_put_resources;
		}

		memcpy(&words[cnt], temp_data, sizeof(uint64_t) * len);
		cnt += len;
	}

	/* Capture resource data. */
	ret = fba_admin_get_cfg_data(fba_dev, temp_addr, ADMIN_GET_RESOURCES, 0,
				     words[INFO_RESOURCE_MAX_COUNT]);
	if (ret) {
		fba_dev_err(fba_dev, "Failed to get resource data.");
		goto err_put_resources;
	}

	/* Create array of resources, including 1 extra for scratch area. */
	r_total = words[INFO_RESOURCE_MAX_COUNT] + 1;
	resources = kcalloc(r_total, sizeof(*resources), GFP_KERNEL);
	if (resources == NULL) {
		ret = -ENOMEM;
		goto err_put_resources;
	}

	/* Add Scratch area. */
	scratch_def = (struct resource_def){
		.type = RT_MEMORY,
		.subtype = MT_SCRATCH,
		.start = (uint64_t)fba_dev->scratch_area.addr,
		.count = fba_dev->scratch_area.size,
		.gran = 64, /* Cache-line sized. */
	};
	res = build_resource(fba_dev, &scratch_def, fba_dev->scratch_area.data);
	if (IS_ERR(res)) {
		ret = PTR_ERR(res);
		goto err_put_resources;
	}
	res->index = 0;
	resources[res->index] = res;

	/* Add FW areas. */
	for (idx = 0; idx < r_total - 1; idx++) {
		struct resource_def *defs = (struct resource_def *)temp_data;

		res = build_resource(fba_dev, &defs[idx], NULL);
		if (IS_ERR(res)) {
			ret = PTR_ERR(res);
			goto err_put_resources;
		}
		res->index = idx + 1;
		resources[res->index] = res;
	}

	spin_lock(&fba_dev->resources_lock);
	fba_dev->resources = resources;
	fba_dev->info_data = words;
	fba_dev->resource_cnt = r_total;
	spin_unlock(&fba_dev->resources_lock);

	return 0;

err_put_resources:
	if (resources) {
		for (cnt = 0; cnt < r_total; cnt++) {
			if (resources[cnt] != NULL)
				fba_resource_put(resources[cnt]);
		}
		kfree(resources);
	}
	kfree(words);

	return ret;
}

void fba_resource_exit(struct fba_device *fba_dev)
{
	struct fba_resource **rscs;
	uint64_t *info_data;
	int i, n_rscs;

	spin_lock(&fba_dev->resources_lock);
	rscs = fba_dev->resources;
	fba_dev->resources = NULL;

	n_rscs = fba_dev->resource_cnt;
	fba_dev->resource_cnt = 0;

	info_data = fba_dev->info_data;
	fba_dev->info_data = NULL;
	spin_unlock(&fba_dev->resources_lock);

	for (i = 0; i < n_rscs; i++)
		fba_resource_put(rscs[i]);

	kfree(rscs);
	kfree(info_data);
}
