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
					   uint16_t type, uint16_t subtype,
					   uint64_t start, uint64_t count,
					   uint64_t gran, void *va)
{
	int order = granularity_to_order(fba_dev, gran);
	struct fba_resource *res;
	int ret;

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res)
		return NULL;

	res->pool = gen_pool_create(order, -1);
	if (res->pool == NULL) {
		fba_dev_err(fba_dev, "Create pool w/gran %#llx failed.", gran);
		ret = -ENOMEM;
		goto err_free_res;
	}

	spin_lock_init(&res->pool_lock);
	kref_init(&res->ref);
	res->type = type;
	res->subtype = subtype;
	res->start = start;
	res->va = va;

	ret = fba_resource_pool_add(res, start, count);
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

/* Fetch some firmware parameters exported as "info words" */
static int fba_fetch_info_words(struct fba_device *fba_dev)
{
	__le64 *words;
	int ret;

	ret = fba_admin_get_cfg_data(fba_dev, fba_dev->scratch_area.addr,
				     ADMIN_GET_INFO_WORDS, INFO_WORD_FIRST,
				     INFO_WORD_APP_START);
	if (ret) {
		fba_dev_err(fba_dev, "Failed to get info words first.");
		return ret;
	}

	words = (__le64 *)fba_dev->scratch_area.data;

	if (le64_to_cpu(words[INFO_WORD_MAX_COUNT]) < INFO_WORD_APP_START) {
		fba_dev_err(
			fba_dev,
			"%s: info_cnt %llu is invalid, min expected value is %d.",
			__func__, le64_to_cpu(words[INFO_WORD_MAX_COUNT]),
			INFO_WORD_APP_START);
		return -EIO;
	}

	fba_dev->n_ctxs = le64_to_cpu(words[INFO_CONTEXT_MAX_COUNT]);
	fba_dev->mon.src_info[SAIL_MTS_CCP].item_cnt =
		le64_to_cpu(words[INFO_MON_ITEM_COUNT]);
	fba_dev->mon.src_info[SAIL_MTS_CCP].group_cnt =
		le64_to_cpu(words[INFO_MON_GROUP_COUNT]);
	fba_dev->max_dma_segment_size =
		le64_to_cpu(words[INFO_DMA_MAX_SEGMENT_SIZE]);
	fba_dev->platform_type = le64_to_cpu(words[INFO_PLATFORM_TYPE]);
	fba_dev->fw_resource_cnt = le64_to_cpu(words[INFO_RESOURCE_MAX_COUNT]);

	return 0;
}

static void fba_resource_add(struct fba_device *fba_dev,
			     struct fba_resource *res)
{
	res->index = fba_dev->resource_cnt++;
	fba_dev->resources[res->index] = res;
}

/* Add resource for host memory scratch area. */
static int fba_resource_add_scratch(struct fba_device *fba_dev)
{
	struct fba_resource *res;
	uint64_t start = fba_dev->scratch_area.addr;
	uint64_t count = fba_dev->scratch_area.size;
	uint64_t gran = 64; /* Cache-line sized. */

	res = build_resource(fba_dev, RT_MEMORY, MT_SCRATCH, start, count, gran,
			     fba_dev->scratch_area.data);
	if (IS_ERR(res))
		return PTR_ERR(res);

	fba_resource_add(fba_dev, res);
	return 0;
}

static int fba_resource_add_fw(struct fba_device *fba_dev)
{
	struct resource_def *defs;
	int i, ret;

	/* Query device for resource info */
	ret = fba_admin_get_cfg_data(fba_dev, fba_dev->scratch_area.addr,
				     ADMIN_GET_RESOURCES, 0,
				     fba_dev->fw_resource_cnt);
	if (ret) {
		fba_dev_err(fba_dev, "Failed to get resource data.");
		return ret;
	}
	defs = (struct resource_def *)fba_dev->scratch_area.data;

	/* Add firmware resources */
	for (i = 0; i < fba_dev->fw_resource_cnt; i++) {
		struct resource_def *def = &defs[i];
		struct fba_resource *res;

		res = build_resource(fba_dev, le16_to_cpu(def->type),
				     le16_to_cpu(def->subtype),
				     le64_to_cpu(def->start),
				     le64_to_cpu(def->count),
				     le64_to_cpu(def->gran), NULL);
		if (IS_ERR(res))
			return PTR_ERR(res);

		fba_resource_add(fba_dev, res);
	}
	return 0;
}

static int fba_init_resources(struct fba_device *fba_dev)
{
	int ret;

	/* Allocate array of resource pointers, plus 1 extra for scratch area */
	fba_dev->resources = kcalloc(fba_dev->fw_resource_cnt + 1,
				     sizeof(*fba_dev->resources), GFP_KERNEL);
	if (fba_dev->resources == NULL)
		return -ENOMEM;

	/* Add host memory scratch space as initial resource */
	ret = fba_resource_add_scratch(fba_dev);
	if (ret)
		goto err_put_resources;

	/* Query and add resources exposed by firmware. */
	ret = fba_resource_add_fw(fba_dev);
	if (ret)
		goto err_put_resources;

	return 0;

err_put_resources:
	while (fba_dev->resource_cnt > 0)
		fba_resource_put(fba_dev->resources[--fba_dev->resource_cnt]);

	kfree(fba_dev->resources);
	fba_dev->resources = NULL;

	return ret;
}

static int fba_init_max_seg_size(struct fba_device *fba_dev)
{
	if (fba_dev->max_dma_segment_size > FBA_MAX_SEGMENT_SIZE) {
		fba_dev_info(fba_dev,
			     "Clamping max DMA segment size from %llu to %llu",
			     fba_dev->max_dma_segment_size,
			     FBA_MAX_SEGMENT_SIZE);
		fba_dev->max_dma_segment_size = FBA_MAX_SEGMENT_SIZE;
	}

	if ((fba_dev->max_dma_segment_size & ~FBA_PAGE_MASK) != 0) {
		fba_dev_err(
			fba_dev,
			"max_dma_segment_size=%llu must be a multiple of %llu",
			fba_dev->max_dma_segment_size, FBA_PAGE_SIZE);
		return -EINVAL;
	}

	return 0;
}

int fba_resource_init(struct fba_device *fba_dev)
{
	int ret;

	ret = fba_fetch_info_words(fba_dev);
	if (ret < 0)
		return ret;

	ret = fba_init_max_seg_size(fba_dev);
	if (ret != 0)
		return ret;

	return fba_init_resources(fba_dev);
}

void fba_resource_exit(struct fba_device *fba_dev)
{
	struct fba_resource **rscs;
	int i, n_rscs;

	spin_lock(&fba_dev->resources_lock);
	rscs = fba_dev->resources;
	fba_dev->resources = NULL;

	n_rscs = fba_dev->resource_cnt;
	fba_dev->resource_cnt = 0;

	spin_unlock(&fba_dev->resources_lock);

	for (i = 0; i < n_rscs; i++)
		fba_resource_put(rscs[i]);

	kfree(rscs);
}
