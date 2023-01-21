/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

// Utility functions
void set_mailbox(struct fba_device *dev, int mbox, uint32_t value);
void set_mailbox64(struct fba_device *dev, int mbox, uint64_t value);
uint32_t get_mailbox(struct fba_device *dev, int mbox);
uint64_t get_mailbox64(struct fba_device *dev, int mbox);
uint32_t fba_read_bar(struct fba_device *dev, uint32_t bar, uint64_t offset);
void fba_write_bar(struct fba_device *dev, uint32_t val, uint32_t bar,
		   uint64_t offset);

int fba_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler,
		    irq_handler_t thread_fn, void *dev_id, const char *fmt,
		    ...);
void fba_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id);

void __iomem *fba_util_get_bar_ptr(struct fba_device *dev, int bar,
				   uint64_t offset);
