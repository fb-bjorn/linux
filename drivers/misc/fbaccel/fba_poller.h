/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#pragma once

#include <linux/spinlock_types.h>
#include <linux/workqueue.h>

/*
 * FBA poller
 *
 * A poller provides an abstraction for a periodic polling operation.
 */

struct fba_device;
struct fba_poller;

typedef bool (*fba_poller_func)(void *);
typedef void (*fba_poller_set_period)(void *);

/*
 * poller object that tracks a periodic polling function
 */
struct fba_poller {
	fba_poller_func func; // polling function
	void *obj; // target used by polling function
	struct delayed_work dwork; // work item for polling
	fba_poller_set_period set_period; // set the polling period
	uint64_t period_ms; // polling period in milliseconds
	atomic_t started; // tracks whether poller has been started
	bool enabled; // poller is enabled to call its function
};

void fba_poller_init(struct fba_poller *mon, void *obj, fba_poller_func func,
		     fba_poller_set_period set_func);

// Start polling the monitor
void fba_poller_start(struct fba_poller *poller);
// Stop polling the monitor
void fba_poller_stop(struct fba_poller *poller);
