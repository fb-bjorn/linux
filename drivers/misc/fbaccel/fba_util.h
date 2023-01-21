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

void __iomem *fba_util_get_bar_ptr(struct fba_device *dev, int bar,
				   uint64_t offset);
