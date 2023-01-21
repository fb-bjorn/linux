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

/* Map the provided addr (in resource space) to its virtual address offset. */
void *fba_alloc_map_to_va(struct fba_alloc *alloc, uint64_t addr)
{
	ptrdiff_t offs = addr - alloc->resource->start;

	return (alloc->resource->va + offs);
}
