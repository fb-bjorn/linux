// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

/*
 * fba_request_irq and fba_free_irq based almost completely on
 * pci_request_irq and pci_free_irq from drivers/pci/irq.c:
 *
 * Copyright (c) 2008 James Bottomley <James.Bottomley@HansenPartnership.com>
 * Copyright (C) 2017 Christoph Hellwig.
 */
//
// Generic Driver Funcs
//
// Functions for ringing doorbells, changing controller state, etc...
//
#include <linux/device.h>
#include <linux/types.h>

#include "fba_device.h"

/*
 * Variant of pci_request_irq convenience function which does not
 * force IRQF_SHARED (as would be suitable for MSI-X.)
 *
 * See pci_request_irq() for additional documentation.
 */
int fba_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler,
		    irq_handler_t thread_fn, void *dev_id, const char *fmt, ...)
{
	va_list ap;
	int ret;
	char *devname;
	unsigned long irqflags = 0;

	if (!handler)
		irqflags |= IRQF_ONESHOT;

	va_start(ap, fmt);
	devname = kvasprintf(GFP_KERNEL, fmt, ap);
	va_end(ap);

	ret = request_threaded_irq(pci_irq_vector(dev, nr), handler, thread_fn,
				   irqflags, devname, dev_id);
	if (ret)
		kfree(devname);
	return ret;
}

/*
 * Variant of pci_free_irq here for symmetry with fba_request_irq.
 *
 * See pci_free_irq() for additional documentation.
 */
void fba_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id)
{
	kfree(free_irq(pci_irq_vector(dev, nr), dev_id));
}

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
