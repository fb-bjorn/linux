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

static void fba_alloc_free(struct kref *ref)
{
	struct fba_alloc *alloc = container_of(ref, struct fba_alloc, ref);
	struct fba_resource *res = alloc->resource;
	uint64_t addr = alloc->addr;

	fba_resource_pool_free(res, addr, alloc->size);
	fba_resource_put(res);

	kfree(alloc);
}

struct fba_alloc *fba_alloc_get(struct fba_alloc *alloc)
{
	kref_get(&alloc->ref);

	return alloc;
}

// Dereference an allocation.
// The calling function must not re-use the alloc ptr once it
// called deref on it. The pointer passed could have been freed.
void fba_alloc_put(struct fba_alloc *alloc)
{
	WARN_ON(alloc == NULL);
	kref_put(&alloc->ref, fba_alloc_free);
}

struct fba_alloc *fba_alloc_new(struct fba_resource *res, uint64_t addr,
				uint64_t size)
{
	struct fba_alloc *alloc;

	alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
	if (!alloc)
		return alloc;

	alloc->resource = fba_resource_get(res);
	alloc->size = size;
	alloc->addr = addr;
	kref_init(&alloc->ref);

	mutex_init(&alloc->dma_buf_mutex);
	INIT_LIST_HEAD(&alloc->dma_buf_list);

	return alloc;
}

uint64_t fba_alloc_get_addr(struct fba_alloc *alloc)
{
	return alloc->addr;
}

struct fba_alloc *fba_alloc_convert(struct fba_ctx *ctx, uint64_t rid,
				    uint64_t size, uint64_t *val)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	int id = (rid >> RID_ID_SHIFT);
	uint64_t offs = (rid & RID_OFFS_MASK);
	uint64_t alloc_size;
	struct fba_alloc *alloc = NULL;

	// Skip NULL IDs.
	if (rid == 0)
		return NULL;

	alloc = fba_ctx_find_alloc(ctx, id);
	if (alloc == NULL) {
		fba_dev_err(fba_dev,
			    "No allocation exists for resource id: %#llx", rid);
		return ERR_PTR(-EINVAL);
	}
	alloc_size = alloc->size;

	// End location in range must be within size.
	if (offs >= alloc_size || size > alloc_size - offs) {
		fba_dev_err(
			fba_dev,
			"Offset + size exceed alloc capacity! offset: %#llx, size %#llx, alloc size %#llx.",
			offs, size, alloc_size);
		fba_alloc_put(alloc);
		return ERR_PTR(-EINVAL);
	}

	// translate the address
	*val = alloc->addr + offs;
	return alloc;
}

/* Map the provided addr (in resource space) to its virtual address offset. */
void *fba_alloc_map_to_va(struct fba_alloc *alloc, uint64_t addr)
{
	ptrdiff_t offs = addr - alloc->resource->start;

	return (alloc->resource->va + offs);
}
