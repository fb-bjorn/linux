// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/list.h>

#include "fba_device.h"

/* page size used by device */
#define FBA_PAGE_SHIFT 12
#define FBA_PAGE_SIZE (1UL << FBA_PAGE_SHIFT)
#define FBA_PAGE_MASK (~(FBA_PAGE_SIZE - 1))
#define FBA_MAX_SEGMENT_SIZE (FBA_PAGE_SIZE * ((1 << (FBA_PAGE_SHIFT - 1))))

#define FBA_NEXT_PRP_PAGE_INDEX (FBA_PAGE_SIZE / sizeof(uint64_t) - 1)

static void free_prp_list(struct device *dev, struct list_head *head)
{
	struct fba_dma_prp_node *cur = NULL;
	struct fba_dma_prp_node *tmp = NULL;

	if (!head || !head->next)
		return;

	list_for_each_entry_safe(cur, tmp, head, list) {
		dma_free_coherent(dev, FBA_PAGE_SIZE, cur->prp, cur->paddr);
		list_del(&cur->list);
		kfree(cur);
	}
}

static inline uint64_t build_entry(dma_addr_t addr, size_t s)
{
	uint64_t pages = ((s + ~FBA_PAGE_MASK) >> FBA_PAGE_SHIFT) - 1;

	return (addr | (pages << 1ULL));
}

static struct fba_dma_prp_node *build_prp_node(struct device *dev)
{
	struct fba_dma_prp_node *node = NULL;
	dma_addr_t paddr = 0;
	dma_addr_t *prp = NULL;

	prp = dma_alloc_coherent(dev, FBA_PAGE_SIZE, &paddr, GFP_KERNEL);
	if (!prp)
		return NULL;

	node = kzalloc(sizeof(struct fba_dma_prp_node), GFP_KERNEL);
	if (!node) {
		dma_free_coherent(dev, FBA_PAGE_SIZE, prp, paddr);
		return NULL;
	}

	node->prp = prp;
	node->paddr = paddr;
	return node;
}

/*
 * Generate PRP entries:
 * The PRP entry format is as follows -
 * Bit 0 is used to indicate if an offset is preset. Strictly speaking,
 * this is only needed in the first entry. If bit 0 is 1, the PRP entry
 * holds the full address with offset. Note this assumes the address
 * here cannot be odd which is checked beforehand.
 * Bits 1-11 is the number of adjacent FBA_PAGE_SIZE pages transferred by
 * this corresponding entry.
 * Bits 12-63 hold the corresponding bits of the host address.
 * Note that the last entry in the current PRP is reserved for the address
 * of the next PRP.
 */
static int generate_prp_list(struct device *dev, struct scatterlist *sg,
			     struct list_head *head_list)
{
	uint32_t entry = 0;
	struct scatterlist *sg_cur = sg;
	dma_addr_t sg_paddr = 0;
	size_t sg_size = 0;
	struct fba_dma_prp_node *cur_node = NULL;
	size_t offset = 0;

	/* We need at least 1 PRP page */
	cur_node = build_prp_node(dev);
	if (!cur_node)
		return -ENOMEM;
	list_add_tail(&cur_node->list, head_list);

	/* Handle the first sg entry since it can be at offset. */
	sg_paddr = sg_dma_address(sg_cur);
	sg_size = sg_dma_len(sg_cur);
	offset = sg_paddr & ~FBA_PAGE_MASK;
	if (offset) {
		sg_size -= min(sg_size, (size_t)(FBA_PAGE_SIZE - offset));
		cur_node->prp[entry++] = sg_paddr | 1ULL;
		sg_paddr += (sg_dma_len(sg_cur) - sg_size);
	}

	if (sg_size)
		cur_node->prp[entry++] = build_entry(sg_paddr, sg_size);
	sg_cur = sg_next(sg_cur);

	/* Handle all other sg entries. */
	while (sg_cur) {
		if (unlikely(entry == FBA_NEXT_PRP_PAGE_INDEX)) {
			struct fba_dma_prp_node *next_node =
				build_prp_node(dev);
			if (!next_node)
				goto error;

			cur_node->prp[entry] = next_node->paddr;
			list_add_tail(&next_node->list, head_list);
			cur_node = next_node;
			entry = 0;
		}

		sg_paddr = sg_dma_address(sg_cur);
		sg_size = sg_dma_len(sg_cur);
		cur_node->prp[entry++] = build_entry(sg_paddr, sg_size);

		sg_cur = sg_next(sg_cur);
	}

	return 0;

error:
	free_prp_list(dev, head_list);
	return -ENOMEM;
}

struct fba_dma *fba_dma_map(struct fba_ctx *ctx, void __user *user_data,
			    size_t size, int dir, bool force_prp_ok)
{
	int ret = -ENOMEM;
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	struct device *dev = &fba_dev->pdev->dev;
	uintptr_t user_addr = (uintptr_t)user_data;
	size_t overflow;
	int npages;
	unsigned int gup_flags;
	struct fba_dma *dma;
	uint64_t max_dma_segment_size;
	bool force_prp = false;
	struct fba_dma_prp_node *node;

	/* disallow odd user addresses */
	if ((uintptr_t)user_data & 1) {
		fba_dev_err(fba_dev, "Invalid user address");
		return ERR_PTR(-EINVAL);
	}

	/* disallow zero-length mappings */
	if (size == 0)
		return ERR_PTR(-EINVAL);

	/*
	 * Force PRP if request is permitted to force PRP and 'test_force_prp' module
	 * parameter set.
	 */
	if (force_prp_ok && test_force_prp)
		force_prp = true;

	/*
	 * We zero fill the DMA object so that we can correctly free it via
	 * fba_dma_free regardless of how far into the process we go.
	 */
	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (dma == NULL)
		return ERR_PTR(-ENOMEM);

	/* Initialize dma object. */
	INIT_LIST_HEAD(&dma->item);
	dma->flags = dir;

	/* Compute number of pages */
	overflow = size + (user_addr & ~PAGE_MASK);
	npages = DIV_ROUND_UP(overflow, PAGE_SIZE);

	fba_dev_dbg(fba_dev, "Map DMA transaction of %d pages.", npages);

	dma->pages = kvcalloc(npages, sizeof(*dma->pages), GFP_KERNEL);
	if (dma->pages == NULL)
		goto err_dma_free;

	/* Get pages, set WRITE if direction includes FROM_DEVICE. */
	gup_flags = (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL) ?
				  FOLL_WRITE :
				  0;
	dma->get_cnt = pin_user_pages_fast(user_addr & PAGE_MASK, npages,
					   gup_flags, dma->pages);
	if (dma->get_cnt != npages) {
		if (dma->get_cnt < 0)
			ret = dma->get_cnt;
		else
			ret = -EFAULT;
		goto err_dma_free;
	}
	max_dma_segment_size = min(fba_dev->max_dma_segment_size,
				   (uint64_t)FBA_MAX_SEGMENT_SIZE);
	ret = sg_alloc_table_from_pages_segment(
		&dma->sg, dma->pages, dma->get_cnt, user_addr & ~PAGE_MASK,
		size, max_dma_segment_size, GFP_KERNEL);
	if (ret) {
		fba_dev_err(fba_dev, "Failed to allocate sg_table.");
		goto err_dma_free;
	}

	ret = dma_map_sg(dev, dma->sg.sgl, dma->sg.nents, dir);
	if (!ret) {
		fba_dev_err(fba_dev, "Failed to map sg.");
		ret = -EINVAL;
		goto err_dma_free;
	}
	dma->map_cnt = ret;

	if (dma->map_cnt == 1 && !force_prp) {
		dma->dma_addr = sg_dma_address(&dma->sg.sgl[0]);
		return dma;
	}

	INIT_LIST_HEAD(&dma->prp_list_head);
	ret = generate_prp_list(dev, dma->sg.sgl, &dma->prp_list_head);
	if (ret) {
		fba_dev_err(fba_dev, "Failed to generate prp list");
		goto err_dma_free;
	}

	node = list_entry(dma->prp_list_head.next, struct fba_dma_prp_node,
			  list);
	dma->prp_addr = node->paddr;

	return dma;

err_dma_free:
	if (dma)
		fba_dma_free(ctx, dma);
	return ERR_PTR(ret);
}

void fba_dma_free(struct fba_ctx *ctx, struct fba_dma *dma)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	struct device *dev = &fba_dev->pdev->dev;

	if (dma->map_cnt)
		dma_unmap_sg(dev, dma->sg.sgl, dma->sg.nents, dma->flags);

	if (dma->sg.nents) {
		sg_free_table(&dma->sg);
		free_prp_list(dev, &dma->prp_list_head);
	}

	/*
	 * NOTE: If we are reading from device, always set pages dirty even on
	 * failure in case we want to examine whatever we managed to get.
	 */
	unpin_user_pages_dirty_lock(dma->pages, dma->get_cnt,
				    dma->flags == DMA_FROM_DEVICE ||
					    dma->flags == DMA_BIDIRECTIONAL);

	kvfree(dma->pages);

	kfree(dma);
}
