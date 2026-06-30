/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

void fba_shared_alloc_iomem(struct fba_device *fba_dev, struct fba_shared *mem,
			    int bar, uint64_t offset, size_t size);
int fba_shared_alloc_coherent(struct fba_device *fba_dev,
			      struct fba_shared *mem, size_t size);
bool fba_shared_is_valid(struct fba_shared *mem);
void fba_shared_free(struct fba_device *fba_dev, struct fba_shared *mem);
void fba_shared_memcpy_to(struct fba_shared *area, uint64_t offset,
			  const void *src, size_t len);
void fba_shared_memcpy_from(const struct fba_shared *area, uint64_t offset,
			    void *dst, size_t len);
void fba_shared_memset(struct fba_shared *area, uint64_t offset, int value,
		       size_t len);
