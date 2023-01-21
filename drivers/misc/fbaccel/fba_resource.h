/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/*
 * FBA Resource
 *
 * A Resource is a limited pool of allocatable objects managed by the
 * driver but exits on the device (except for scratch).  During activation
 * the driver reads the table of resources from the device and creates
 * pools to manage them.  User space applications then allocate portions
 * of the pool which they reference w/ an handle + offset (aka RID).
 *
 * The allocation RIDs are converted back to device address space during
 * translataion.
 */

struct fba_resource {
	int index;
	uint16_t type;
	uint16_t subtype;
	// Start of generic address (Device Address).
	uint64_t start;
	// Host virtual address (where applicable)
	void *va;
	// While the genpool subsystem is a lock-free structure which can be
	// safely called concurrently, for our specific use case though there
	// are instances when multiple parallel invocations to gen_pool_add can
	// result in unexpected fragmentation which can prevent later
	// alloctions from being satisfied.
	// To avoid this we serialize operations by ensuring that all calls to
	// gen_pool_create and gen_pool_free are locked in our driver.
	spinlock_t pool_lock;
	struct gen_pool *pool;
	struct kref ref;
};

void fba_resource_pool_free(struct fba_resource *res, uint64_t addr,
			    uint64_t size);

struct fba_resource *fba_resource_get(struct fba_resource *res);
void fba_resource_put(struct fba_resource *res);

// Resource Operations
int fba_resource_init(struct fba_device *fba_dev);
void fba_resource_exit(struct fba_device *fba_dev);
