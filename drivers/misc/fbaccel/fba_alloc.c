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

	fba_dmabuf_exit(alloc);

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

	/* Use parent context for streaming contexts */
	if (fba_ctx_get_parent(ctx))
		ctx = fba_ctx_get_parent(ctx);

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

// Convert to alloc and add to those tracked by job
static int fba_alloc_convert_job(struct fba_job *job, uint64_t rid,
				 uint64_t size, uint64_t *val)
{
	struct fba_alloc *alloc = fba_alloc_convert(job->ctx, rid, size, val);

	if (IS_ERR(alloc))
		return PTR_ERR(alloc);

	fba_job_add_alloc(job, alloc);
	return 0;
}

int fba_alloc_translate_setup(struct fba_device *fba_dev, struct fba_job *job,
			      size_t cnt)
{
	job->allocs = kcalloc(cnt * SUBMISSION_ENTRY_ARGS, sizeof(*job->allocs),
			      GFP_KERNEL);
	if (job->allocs == NULL)
		return -ENOMEM;

	return 0;
}

void fba_alloc_translate_teardown(struct fba_job *job)
{
	int i;

	for (i = 0; i < job->alloc_cnt; i++) {
		if (job->allocs[i])
			fba_alloc_put(job->allocs[i]);
	}

	kfree(job->allocs);
	job->allocs = NULL;
	job->alloc_cnt = 0;
}

static int fba_alloc_translate_p2p(struct fba_job *job, uint64_t rid,
				   uint64_t size, uint64_t *val)
{
	int fd = (rid >> RID_ID_SHIFT);
	uint64_t offset = (rid & RID_OFFS_MASK);
	struct fba_dmabuf_attach *fba_dmabuf;

	size = size & AT_DMA_SIZE_MASK;
	fba_dmabuf = fba_translate_p2p_setup(job->ctx, fd, offset, size);
	if (IS_ERR(fba_dmabuf))
		return PTR_ERR(fba_dmabuf);

	*val = sg_dma_address(fba_dmabuf->sgt.sgl);

	job->dmabuf_cnt++;
	list_add_tail(&fba_dmabuf->list, &job->dma_buf_list);

	return 0;
}

static struct fba_dma *fba_alloc_translate_dma(struct fba_ctx *ctx,
					       struct fba_job *job,
					       uintptr_t buf_addr,
					       uint64_t enc_size)

{
	size_t size = enc_size & AT_DMA_SIZE_MASK;
	enum dma_data_direction dir;
	uint64_t enc_dir;

	enc_dir = (enc_size & (AT_DMA_TO_DEVICE | AT_DMA_FROM_DEVICE));
	switch (enc_dir) {
	case AT_DMA_TO_DEVICE:
		dir = DMA_TO_DEVICE;
		break;
	case AT_DMA_FROM_DEVICE:
		dir = DMA_FROM_DEVICE;
		break;
	case AT_DMA_TO_DEVICE | AT_DMA_FROM_DEVICE:
		dir = DMA_BIDIRECTIONAL;
		break;
	default:
		/* Cowardly refuse to map as DMA_NONE */
		return ERR_PTR(-EINVAL);
	}

	return fba_job_dma_map(ctx, job, (void *)buf_addr, size, dir, true);
}

int fba_alloc_translate_sub(struct fba_ctx *ctx, struct fba_job *job,
			    struct fba_submission *sub)
{
	int i, rc = 0;
	uint64_t *iargs = sub->args;
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);

	for (i = 0; i < SUBMISSION_ENTRY_ARGS; i++) {
		uint64_t arg_type =
			(sub->header.flags >> (i * AT_FIELD_BIT_WIDTH)) &
			AT_FIELD_BIT_MASK;
		uint64_t *ptr = &iargs[i];
		uint64_t size;

		switch (arg_type) {
		case AT_RAW:
			break;

		case AT_RID:
			rc = fba_alloc_convert_job(job, iargs[i], 0, ptr);
			break;

		case AT_RID_ARG32:
			// ARG32 not available when last arg.
			if (i == SUBMISSION_ENTRY_ARGS - 1)
				return -EINVAL;

			size = iargs[i + 1] & 0xFFFFFFFF;
			rc = fba_alloc_convert_job(job, iargs[i], size, ptr);
			break;

		case AT_ARG32_RID:
			// ARG32 not available when first arg.
			if (i == 0)
				return -EINVAL;
			size = iargs[i - 1] >> 32;
			rc = fba_alloc_convert_job(job, iargs[i], size, ptr);
			break;

		case AT_ARG64_RID:
			// ARG64 not available when first arg.
			if (i == 0)
				return -EINVAL;
			size = iargs[i - 1];
			if ((size & AT_DMA_OPSZ))
				size = size & AT_DMA_SIZE_MASK;
			rc = fba_alloc_convert_job(job, iargs[i], size, ptr);
			break;

		case AT_VA_DMA64: {
			struct fba_dma *dma;

			// VA_DMA64 not available when last arg.
			if (i == SUBMISSION_ENTRY_ARGS - 1)
				return -EINVAL;

			size = iargs[i + 1];
			dma = fba_alloc_translate_dma(ctx, job, iargs[i], size);
			if (IS_ERR(dma)) {
				rc = PTR_ERR(dma);
				break;
			}
			if (dma->map_cnt == 1 && !test_force_prp) {
				*ptr = dma->dma_addr;
				iargs[i + 1] |= AT_DMA_OPSZ;
			} else {
				*ptr = dma->prp_addr;
				iargs[i + 1] |= AT_DMA_OPSZ | AT_DMA_PRP;
			}
			break;
		}

		case AT_P2P_DMA64: {
			// P2P_DMA64 not available when last arg.
			if (i == SUBMISSION_ENTRY_ARGS - 1)
				return -EINVAL;

			size = iargs[i + 1];
			rc = fba_alloc_translate_p2p(job, iargs[i], size, ptr);
			if (!rc)
				iargs[i + 1] |= AT_DMA_OPSZ;
			break;
		}

		default:
			fba_dev_err(fba_dev, "Unknown ARG type %d.",
				    (int)arg_type);
			return -EINVAL;
		}
		if (rc != 0)
			break;
	}
	return rc;
}

/* Map the provided addr (in resource space) to its virtual address offset. */
void *fba_alloc_map_to_va(struct fba_alloc *alloc, uint64_t addr)
{
	ptrdiff_t offs = addr - alloc->resource->start;

	return (alloc->resource->va + offs);
}
