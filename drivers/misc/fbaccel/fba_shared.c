// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#include "fba_device.h"

void fba_shared_alloc_iomem(struct fba_device *dev, struct fba_shared *mem,
			    int bar, uint64_t offset, size_t size)
{
	*mem = (struct fba_shared){
		.iodata = fba_util_get_bar_ptr(dev, bar, offset),
		.addr = 0,
		.size = size,
	};
}

int fba_shared_alloc_coherent(struct fba_device *dev, struct fba_shared *mem,
			      size_t size)
{
	void *data;
	dma_addr_t addr;

	*mem = (struct fba_shared){};

	if (size == 0 || (size & (PAGE_SIZE - 1))) {
		// Expecting PAGE sized allocations, remove if we determine
		// this expectation is invalid.
		fba_dev_err(dev, "Invalid size request %zu.", size);
		return -EINVAL;
	}

	// Allocate temp pages for Admin and misc use.
	data = dma_alloc_coherent(&dev->pdev->dev, size, &addr, GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	*mem = (struct fba_shared){
		.data = data,
		.addr = addr,
		.size = size,
	};

	return 0;
}

bool fba_shared_is_valid(struct fba_shared *mem)
{
	return (mem->size > 0);
}

void fba_shared_free(struct fba_device *dev, struct fba_shared *mem)
{
	// Not dma_coherent if ADDR is not set, like points to a BAR.
	if (mem->addr == 0)
		return;

	dma_free_coherent(&dev->pdev->dev, mem->size, mem->data, mem->addr);
	*mem = (struct fba_shared){};
}

void fba_shared_memcpy_to(struct fba_shared *area, uint64_t offset,
			  const void *src, size_t len)
{
	if (area->addr)
		memcpy(area->data + offset, src, len);
	else
		memcpy_toio(area->iodata + offset, src, len);
}

void fba_shared_memcpy_from(const struct fba_shared *area, uint64_t offset,
			    void *dst, size_t len)
{
	if (area->addr)
		memcpy(dst, area->data + offset, len);
	else
		memcpy_fromio(dst, area->iodata + offset, len);
}

void fba_shared_memset(struct fba_shared *area, uint64_t offset, int value,
		       size_t len)
{
	if (area->addr)
		memset(area->data + offset, value, len);
	else
		memset_io(area->iodata + offset, value, len);
}
