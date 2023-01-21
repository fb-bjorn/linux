// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

//
// Generic Driver Funcs
//
// Functions for ringing doorbells, changing controller state, etc...
//
#include <linux/device.h>
#include <linux/types.h>

#include "fba_device.h"

void __iomem *fba_util_get_bar_ptr(struct fba_device *dev, int bar,
				   uint64_t offset)
{
	WARN_ON(bar >= BAR_CNT || !dev->bars[bar]);
	return (void __iomem *)(dev->bars[bar] + offset);
}

static void __iomem *get_mbox_ptr(struct fba_device *dev, int mbox)
{
	uint64_t off =
		dev->cfg->host_ipc_offset + (mbox * dev->cfg->host_ipc_stride);
	return fba_util_get_bar_ptr(dev, dev->cfg->host_ipc_bar, off);
}

uint32_t get_mailbox(struct fba_device *dev, int mbox)
{
	return ioread32(get_mbox_ptr(dev, mbox));
}

void set_mailbox(struct fba_device *dev, int mbox, uint32_t val)
{
	iowrite32(val, get_mbox_ptr(dev, mbox));
}

uint64_t get_mailbox64(struct fba_device *dev, int mbox)
{
	return (uint64_t)get_mailbox(dev, mbox + 1) << 32 |
	       (uint64_t)get_mailbox(dev, mbox + 0);
}

void set_mailbox64(struct fba_device *dev, int mbox, uint64_t val)
{
	iowrite32(lower_32_bits(val), get_mbox_ptr(dev, mbox + 0));
	iowrite32(upper_32_bits(val), get_mbox_ptr(dev, mbox + 1));
}

uint32_t fba_read_bar(struct fba_device *dev, uint32_t bar, uint64_t offset)
{
	return ioread32(fba_util_get_bar_ptr(dev, bar, offset));
}

void fba_write_bar(struct fba_device *dev, uint32_t val, uint32_t bar,
		   uint64_t offset)
{
	iowrite32(val, fba_util_get_bar_ptr(dev, bar, offset));
}
