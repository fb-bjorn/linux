// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/types.h>

#include "fba_poller.h"
#include "fba_device.h"
#include "fba_util.h"

// delayed work function for periodic polling
static void fba_poller_work(struct work_struct *work)
{
	struct fba_poller *poller =
		container_of(to_delayed_work(work), struct fba_poller, dwork);
	bool resched = true;

	// set polling period, and set enabled/disabled.
	poller->set_period(poller->obj);

	// Only run poller function if enabled.
	if (poller->enabled)
		resched = poller->func(poller->obj);

	if (resched)
		schedule_delayed_work(&poller->dwork,
				      msecs_to_jiffies(poller->period_ms));
}

void fba_poller_init(struct fba_poller *poller, void *obj, fba_poller_func func,
		     fba_poller_set_period period_func)
{
	poller->obj = obj;
	poller->func = func;
	poller->set_period = period_func;
	poller->set_period(obj);
	INIT_DELAYED_WORK(&poller->dwork, fba_poller_work);
}

// Start a poller. Safe to call if already started
void fba_poller_start(struct fba_poller *poller)
{
	if (poller == NULL)
		return;

	if (atomic_cmpxchg(&poller->started, false, true) == false)
		schedule_delayed_work(&poller->dwork,
				      msecs_to_jiffies(poller->period_ms));
}

// Stop a poller. Safe to call if not started
void fba_poller_stop(struct fba_poller *poller)
{
	if (poller == NULL)
		return;

	cancel_delayed_work_sync(&poller->dwork);
	atomic_set(&poller->started, false);
}
