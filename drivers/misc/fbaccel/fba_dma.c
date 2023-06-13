// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/list.h>

#include "fba_device.h"

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

static uint64_t build_entry(dma_addr_t addr, size_t size)
{
	uint64_t npages = DIV_ROUND_UP(size, FBA_PAGE_SIZE) - 1;

	WARN_ON_ONCE(size == 0);
	WARN_ON_ONCE(size > FBA_MAX_SEGMENT_SIZE);
	WARN_ON_ONCE((addr & ~FBA_PAGE_MASK) != 0);

	return addr | (npages << 1ULL);
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
 * Each entry in the PRP has the following format:
 * - Bit 0 is used to indicate if an offset is preset. Strictly speaking, this
 *   is only needed in the first entry. If bit 0 is 1, the PRP entry holds the
 *   full address with offset. Note this assumes the address here cannot be odd
 *   which is checked beforehand.
 * - Bits 1-11 is the number of adjacent FBA_PAGE_SIZE pages minus 1
 *   transferred by this entry.
 * - Bits 12-63 hold the corresponding bits of the host address.
 * - Note that the last entry (index 511) in the current PRP is reserved for
 *   the address of the next PRP.
 * - The total size of the transfer is sent separately from the PRP list. The
 *   total size and the PRP list provides all the information the device needs
 *   to DMA to/from scattered host memory.
 */
static int generate_prp_list(struct fba_device *fba_dev, struct scatterlist *sg,
			     struct list_head *head_list, unsigned int map_cnt)
{
	struct device *dev = &fba_dev->pdev->dev;
	struct fba_dma_prp_node *cur_node;
	struct scatterlist *sg_cur;
	dma_addr_t sg_paddr;
	uint64_t sg_size, sg_size_rem, page_offset, page_rem;
	uint32_t prp_idx;
	unsigned int sg_idx;
	int ret;

	/* We need at least 1 PRP page */
	cur_node = build_prp_node(dev);
	if (!cur_node) {
		fba_dev_err(fba_dev, "failed to build first PRP node");
		return -ENOMEM;
	}
	list_add_tail(&cur_node->list, head_list);

	prp_idx = 0;
	sg_cur = sg;
	sg_idx = 0;
	sg_paddr = sg_dma_address(sg_cur);
	sg_size = sg_dma_len(sg_cur);

	/* Handle the first sg entry since it can be offset from a page-aligned boundary. */
	page_offset = sg_paddr & ~FBA_PAGE_MASK;
	if (page_offset) {
		page_rem = FBA_PAGE_SIZE - page_offset;
		sg_size_rem = sg_size - min(sg_size, page_rem);
		cur_node->prp[prp_idx++] = sg_paddr | 1ULL;
	} else {
		page_rem = 0;
		sg_size_rem = sg_size;
	}

	sg_paddr += page_rem;
	sg_size = sg_size_rem;

	while (sg_cur && (sg_idx < map_cnt)) {
		while (sg_size > 0) {
			uint64_t entry_size;

			if (unlikely(prp_idx == FBA_NEXT_PRP_PAGE_INDEX)) {
				struct fba_dma_prp_node *next_node;

				next_node = build_prp_node(dev);
				if (!next_node) {
					fba_dev_err(fba_dev,
						    "failed to build PRP node");
					ret = -ENOMEM;
					goto error;
				}

				cur_node->prp[prp_idx] = next_node->paddr;
				list_add_tail(&next_node->list, head_list);
				cur_node = next_node;
				prp_idx = 0;
			}

			entry_size =
				min(sg_size, fba_dev->max_dma_segment_size);
			cur_node->prp[prp_idx++] =
				build_entry(sg_paddr, entry_size);
			sg_paddr += entry_size;
			sg_size -= entry_size;
		}

		sg_cur = sg_next(sg_cur);
		if (sg_cur) {
			sg_paddr = sg_dma_address(sg_cur);
			sg_size = sg_dma_len(sg_cur);
		}
		sg_idx++;
	}

	return 0;

error:
	free_prp_list(fba_dev->dev, head_list);
	return ret;
}

struct fba_dma *fba_dma_map(struct fba_ctx *ctx, void __user *user_data,
			    uint64_t size, int dir, bool force_prp_ok)
{
	int ret = -ENOMEM;
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	struct device *dev = &fba_dev->pdev->dev;
	uintptr_t user_addr = (uintptr_t)user_data;
	uint64_t overflow;
	int npages;
	unsigned int gup_flags;
	struct fba_dma *dma;
	bool force_prp = false;
	struct fba_dma_prp_node *node;

	/* disallow odd user addresses */
	if ((uintptr_t)user_data & 1) {
		fba_dev_err(fba_dev, "Odd user addresses are not supported");
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

	gup_flags = FOLL_LONGTERM;
	if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL)
		gup_flags |= FOLL_WRITE;

	dma->get_cnt = pin_user_pages_fast(user_addr & PAGE_MASK, npages,
					   gup_flags, dma->pages);
	if (dma->get_cnt != npages) {
		fba_dev_err(fba_dev, "Only pinned %d out of expected %d pages.",
			    dma->get_cnt, npages);
		if (dma->get_cnt < 0)
			ret = dma->get_cnt;
		else
			ret = -EFAULT;
		goto err_dma_free;
	}

	ret = sg_alloc_table_from_pages_segment(
		&dma->sg, dma->pages, dma->get_cnt, user_addr & ~PAGE_MASK,
		size, dma_get_max_seg_size(dev), GFP_KERNEL);
	if (ret) {
		fba_dev_err(fba_dev, "Failed to allocate sg_table.");
		goto err_dma_free;
	}

	dma->map_cnt = dma_map_sg(dev, dma->sg.sgl, dma->sg.nents, dir);

	if (dma->map_cnt == 0) {
		fba_dev_err(fba_dev, "Failed to map sg.");
		ret = -EINVAL;
		goto err_dma_free;
	}

	if (dma->map_cnt == 1 && !force_prp) {
		dma->dma_addr = sg_dma_address(&dma->sg.sgl[0]);
		return dma;
	}

	INIT_LIST_HEAD(&dma->prp_list_head);
	ret = generate_prp_list(fba_dev, dma->sg.sgl, &dma->prp_list_head,
				dma->map_cnt);
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
	bool is_dirty;

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
	if (dma->get_cnt > 0) {
		is_dirty = dma->flags == DMA_FROM_DEVICE ||
			   dma->flags == DMA_BIDIRECTIONAL;
		unpin_user_pages_dirty_lock(dma->pages, dma->get_cnt, is_dirty);
	}

	kvfree(dma->pages);

	kfree(dma);
}
