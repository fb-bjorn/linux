/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

// Utility functions
void set_mailbox(struct fba_device *fba_dev, int mbox, uint32_t value);
void set_mailbox64(struct fba_device *fba_dev, int mbox, uint64_t value);
uint32_t get_mailbox(struct fba_device *fba_dev, int mbox);
uint64_t get_mailbox64(struct fba_device *fba_dev, int mbox);
uint32_t fba_read_bar(struct fba_device *fba_dev, uint32_t bar,
		      uint64_t offset);
void fba_write_bar(struct fba_device *fba_dev, uint32_t val, uint32_t bar,
		   uint64_t offset);

int fba_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler,
		    irq_handler_t thread_fn, void *dev_id, const char *fmt,
		    ...);
void fba_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id);

void __iomem *fba_util_get_bar_ptr(struct fba_device *fba_dev, int bar,
				   uint64_t offset);

// Update usage counts
int fba_util_inc_usage(struct fba_device *fba_dev, const struct fba_limits *max,
		       struct fba_limits *src, uint32_t sub_cnt);
void fba_util_dec_usage(struct fba_device *fba_dev, struct fba_limits *src,
			uint32_t sub_cnt);

int list_count(struct list_head *list);
