// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
//
// Generic Driver Funcs
//
// Functions for ringing oorbells, changing controller state, etc...
//
#include <linux/device.h>
#include <linux/ktime.h>
#include <linux/types.h>

#include "fba_device.h"
#include "fba_ctx.h"
#include "fba_admin.h"

enum fba_ctx_state {
	FBA_CTX_CLOSED = 0, // Closed in host. Closed or Closing in device.
	FBA_CTX_VALID, // Opened in host but not device. Accepts non FW requests.
	FBA_CTX_OPEN, // Opened in host and device. Accepts FW jobs.
	FBA_CTX_STOPPED, // Reject any new work, disallow transition to open.
};

struct fba_ctx {
	// Device that this context belongs to.
	struct fba_device *fba_dev;
	// Default channel to use.
	struct fba_channel *chan;
	// Reference count.
	struct kref ref;
	// ID for this context.
	int id;

	// Each context keeps track of resource allocations by id
	struct mutex allocs_mutex;
	struct idr allocs_idr;

	// Owned by device.
	// This context in the device context list.
	struct list_head dev_item;

	// Lock for state, usage, and job lists.
	struct mutex ctx_mtx;
	// Current state of this context.
	enum fba_ctx_state ctx_state;
	// Current usage of all added jobs for this context
	struct fba_limits usage;
	// Jobs submitted to device but not yet complete
	struct list_head sub_list;
	// Jobs completed by the device
	struct list_head cmp_list;
	// Completion used to signal idle
	struct completion ctx_idle;

	// Back pointer to parent ctx that owns the streaming ctx
	struct fba_ctx *parent_ctx;
};

static void fba_ctx_init(struct fba_device *fba_dev, struct fba_ctx *ctx,
			 struct fba_channel *chan)
{
	ctx->ctx_state = FBA_CTX_VALID;
	ctx->chan = chan;

	kref_init(&ctx->ref);
	mutex_init(&ctx->ctx_mtx);
	INIT_LIST_HEAD(&ctx->dev_item);
	INIT_LIST_HEAD(&ctx->sub_list);
	INIT_LIST_HEAD(&ctx->cmp_list);
	init_completion(&ctx->ctx_idle);

	mutex_init(&ctx->allocs_mutex);
	idr_init(&ctx->allocs_idr);
}

// Allocate per-file context.
struct fba_ctx *fba_ctx_alloc(struct fba_device *fba_dev,
			      struct fba_ctx *parent)
{
	struct fba_ctx *ctx;
	struct fba_channel *chan;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx == NULL)
		return ERR_PTR(-ENOMEM);
	fba_dev_dbg(fba_dev, "Allocated context.");

	/*
	 * Callers of fba_ctx_alloc should have referenced dev,
	 * we make ctx the new owner of that reference.
	 */
	ctx->fba_dev = fba_dev;

	if (parent)
		chan = fba_allocate_streaming_channel(fba_dev);
	else
		chan = fba_get_channel(fba_dev, SAIL_CHANNEL_EXE);

	if (chan == NULL) {
		fba_dev_err(fba_dev, "Failed to allocate channel.");
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	fba_dev_dbg(fba_dev, "Allocated channel.");

	if (parent)
		ctx->parent_ctx = fba_ctx_get(parent);

	fba_ctx_init(fba_dev, ctx, chan);

	// Add to context list.
	fba_device_add_ctx(fba_dev, &ctx->dev_item);

	return ctx;
}

/*
 * Destroy the refcounted fba_ctx, which gets released when the context and any
 * children (streaming contexts) have been closed by the opening application.
 */
static void fba_ctx_free(struct kref *ref)
{
	int id;
	struct fba_alloc *alloc;
	struct fba_ctx *ctx = container_of(ref, struct fba_ctx, ref);
	struct fba_device *fba_dev = ctx->fba_dev;

	// If the application closes the context without
	// freeing all the resources, free them now.
	idr_for_each_entry(&ctx->allocs_idr, alloc, id)
		fba_alloc_put(alloc);
	idr_destroy(&ctx->allocs_idr);
	mutex_destroy(&ctx->allocs_mutex);

	if (ctx->parent_ctx) {
		fba_free_streaming_channel(ctx->chan);
		fba_ctx_put(ctx->parent_ctx);
	}

	fba_device_put(fba_dev);

	// Verify the ref & job/submission count logic is working.
	// Verify that everything was removed from submitted/completed
	WARN_ON(ctx->usage.sub_cnt != 0);
	WARN_ON(!list_empty(&ctx->cmp_list));
	WARN_ON(!list_empty(&ctx->sub_list));

	mutex_destroy(&ctx->ctx_mtx);
	kfree(ctx);
}

static bool __fba_ctx_is_valid(struct fba_ctx *ctx)
{
	return (ctx->ctx_state >= FBA_CTX_VALID);
}

bool fba_ctx_is_valid(struct fba_ctx *ctx)
{
	bool is_valid;

	mutex_lock(&ctx->ctx_mtx);
	is_valid = __fba_ctx_is_valid(ctx);
	mutex_unlock(&ctx->ctx_mtx);

	return is_valid;
}

static bool __fba_ctx_is_open(struct fba_ctx *ctx)
{
	return (ctx->ctx_state == FBA_CTX_OPEN);
}

bool fba_ctx_is_open(struct fba_ctx *ctx)
{
	bool is_open;

	mutex_lock(&ctx->ctx_mtx);
	is_open = __fba_ctx_is_open(ctx);
	mutex_unlock(&ctx->ctx_mtx);

	return is_open;
}

int fba_ctx_open(struct fba_ctx *ctx)
{
	struct fba_device *fba_dev = ctx->fba_dev;
	int rc, id;

	mutex_lock(&ctx->ctx_mtx);

	if (__fba_ctx_is_open(ctx)) {
		/* Already open, quietly do nothing. */
		rc = 0;
		goto out_unlock;
	}

	if (ctx->ctx_state == FBA_CTX_STOPPED) {
		fba_dev_err(fba_dev,
			    "Context promotion failed: context is stopped.");
		rc = -EIO;
		goto out_unlock;
	}
	id = ida_alloc_range(&fba_dev->ctx_ids, 1, fba_dev->n_ctxs, GFP_KERNEL);
	if (id < 0) {
		fba_dev_err(fba_dev,
			    "Context promotion failed: context limit reached.");
		rc = id;
		goto out_unlock;
	}

	rc = fba_admin_create_context(fba_dev, id);
	if (rc) {
		ida_free(&fba_dev->ctx_ids, id);
		fba_dev_err(
			fba_dev,
			"Context promotion failed: unable to create context in device.");
		goto out_unlock;
	}

	ctx->id = id;
	ctx->ctx_state = FBA_CTX_OPEN;

out_unlock:
	mutex_unlock(&ctx->ctx_mtx);
	return rc;
}

// Increment reference count to context
struct fba_ctx *fba_ctx_get(struct fba_ctx *ctx)
{
	kref_get(&ctx->ref);
	return ctx;
}

// Dereference the Context.
// The calling function must not re-use the ctx ptr once it
// called deref. The ctx could have been freed.
void fba_ctx_put(struct fba_ctx *ctx)
{
	kref_put(&ctx->ref, fba_ctx_free);
}

struct fba_device *fba_ctx_to_fba_device(struct fba_ctx *ctx)
{
	return ctx->fba_dev;
}

/* Get parent context (or NULL if no parent.) */
struct fba_ctx *fba_ctx_get_parent(struct fba_ctx *ctx)
{
	return ctx->parent_ctx;
}

/* Add alloc to context, and return unique id (or -ENOSPC if limit reached.) */
int fba_ctx_add_alloc(struct fba_ctx *ctx, struct fba_alloc *alloc)
{
	struct fba_device *fba_dev = ctx->fba_dev;
	int rc;

	mutex_lock(&ctx->allocs_mutex);
	rc = idr_alloc(&ctx->allocs_idr, fba_alloc_get(alloc), 1, 65535,
		       GFP_KERNEL);
	if (rc < 0) {
		fba_dev_dbg(fba_dev, "Reached allocation limit for context");
		fba_alloc_put(alloc);
		rc = -ENOSPC;
	}
	mutex_unlock(&ctx->allocs_mutex);

	return rc;
}

/* remove alloc with specified id from context. */
int fba_ctx_remove_alloc(struct fba_ctx *ctx, int id)
{
	struct fba_device *fba_dev = ctx->fba_dev;
	struct fba_alloc *alloc;
	int rc = 0;

	mutex_lock(&ctx->allocs_mutex);
	alloc = (struct fba_alloc *)idr_remove(&ctx->allocs_idr, id);
	if (alloc == NULL) {
		fba_dev_dbg(fba_dev, "Cannot remove resource. ID not found: %d",
			    id);
		rc = -ENOENT;
	}
	mutex_unlock(&ctx->allocs_mutex);

	if (alloc)
		fba_alloc_put(alloc);

	return rc;
}

/* Find and reference an alloc by the given id */
struct fba_alloc *fba_ctx_find_alloc(struct fba_ctx *ctx, int id)
{
	struct fba_alloc *alloc;

	mutex_lock(&ctx->allocs_mutex);
	alloc = idr_find(&ctx->allocs_idr, id);
	if (alloc != NULL)
		fba_alloc_get(alloc);
	mutex_unlock(&ctx->allocs_mutex);

	return alloc;
}

/* Get id associated with ctx, only valid if in FBA_CTX_OPEN state */
int fba_ctx_get_id(struct fba_ctx *ctx)
{
	/* XXX - racy with respect to quiesce? */
	return ctx->id;
}

/* Helper to keep fba_device from needing to reach into ctx */
struct fba_ctx *fba_dev_item_to_fba_ctx(struct list_head *item)
{
	return list_entry(item, struct fba_ctx, dev_item);
}

// Purge the cmp jobs
static void fba_ctx_purge_cmp(struct fba_ctx *ctx)
{
	struct fba_job *job, *tmp;
	struct fba_device *fba_dev = ctx->fba_dev;
	int cnt = 0;
	LIST_HEAD(jobs_to_delete);

	mutex_lock(&ctx->ctx_mtx);
	list_for_each_entry_safe(job, tmp, &ctx->cmp_list, ctx_item) {
		fba_job_dec_usage(fba_dev, &ctx->usage, job);
		list_move(&job->ctx_item, &jobs_to_delete);
	}
	mutex_unlock(&ctx->ctx_mtx);

	list_for_each_entry_safe(job, tmp, &jobs_to_delete, ctx_item) {
		list_del_init(&job->ctx_item);
		fba_dev_dbg(ctx->fba_dev,
			    "Purged job %04Xh. ctx ID %d Num sub: %d ",
			    job->sys_cmd_id, ctx->id, job->req.sub_cnt);
		fba_job_put(job);
		cnt++;
	}

	if (cnt)
		fba_dev_dbg(ctx->fba_dev, "Purged total %d jobs", cnt);
}

/* After ctx has been stopped, wait for all jobs to complete (or timeout) */
void fba_ctx_wait_idle(struct fba_ctx *ctx)
{
	mutex_lock(&ctx->ctx_mtx);

	if (list_empty(&ctx->sub_list)) {
		mutex_unlock(&ctx->ctx_mtx);
		return;
	}

	reinit_completion(&ctx->ctx_idle);
	mutex_unlock(&ctx->ctx_mtx);

	/*
	 * Timeout after 1.2 heartbeat intervals, which should give heartbeat
	 * an opportunity to fail first.  This is more of a stopgap in case
	 * heartbeat is disabled or heartbeat is making forward progress
	 * despite stuck jobs.
	 */
	if (wait_for_completion_timeout(
		    &ctx->ctx_idle,
		    msecs_to_jiffies(CHAN_HEARTBEAT_PERIOD_MS * 6 / 5)) == 0) {
		/* Timed out, abort device and wait again for idle. */
		fba_device_abort(ctx->fba_dev,
				 "timeout waiting for context idle");
		wait_for_completion(&ctx->ctx_idle);
	}
}

/* Prevent ctx from submitting new jobs, or transitioning to open. */
void fba_ctx_stop(struct fba_ctx *ctx)
{
	bool was_open;
	int rc;
	int id;
	struct fba_alloc *alloc;

	mutex_lock(&ctx->ctx_mtx);
	was_open = (ctx->ctx_state == FBA_CTX_OPEN);
	ctx->ctx_state = FBA_CTX_STOPPED;
	mutex_unlock(&ctx->ctx_mtx);

	mutex_lock(&ctx->allocs_mutex);
	idr_for_each_entry(&ctx->allocs_idr, alloc, id)
		fba_dmabuf_exit(alloc);
	mutex_unlock(&ctx->allocs_mutex);

	if (was_open) {
		// Not checking the return here as the device is aborted, if the device
		// returns an error or the protocol fails. If the device is not aborted
		// it makes sense to fba_ctx_wait_idle to get completion notification
		// for the jobs. If the device is aborted it still makes sense to call
		// delete_context_end() as it would fail quickly because the channels
		// are stopped.
		fba_admin_delete_context_begin(ctx->fba_dev, ctx->id);
		fba_ctx_wait_idle(ctx);
		rc = fba_admin_delete_context_end(ctx->fba_dev, ctx->id);
		if (rc == 0)
			ida_free(&ctx->fba_dev->ctx_ids, ctx->id);
	}
}

// Close the context
int fba_ctx_close(struct fba_ctx *ctx)
{
	struct fba_device *fba_dev = ctx->fba_dev;

	fba_device_del_ctx(fba_dev, &ctx->dev_item);

	if (fba_ctx_is_open(ctx))
		fba_ctx_stop(ctx);

	fba_ctx_purge_cmp(ctx);

	return 0;
}

bool fba_ctx_has_space(struct fba_ctx *ctx)
{
	bool has_space = false;

	mutex_lock(&ctx->ctx_mtx);
	if (__fba_ctx_is_open(ctx)) {
		/* Channel has not been stopped */
		has_space = fba_channel_has_space(ctx->chan);
	}
	mutex_unlock(&ctx->ctx_mtx);

	return has_space;
}

uint64_t fba_ctx_max_job_subs(struct fba_ctx *ctx)
{
	return fba_channel_max_job_subs(ctx->chan);
}

bool fba_ctx_has_completions(struct fba_ctx *ctx)
{
	bool result;

	mutex_lock(&ctx->ctx_mtx);
	result = !list_empty(&ctx->cmp_list);
	mutex_unlock(&ctx->ctx_mtx);

	return result;
}

// Complete a job associated with this ctx.
void fba_ctx_complete_job(struct fba_ctx *ctx, struct fba_job *job)
{
	mutex_lock(&ctx->ctx_mtx);

	/* Move job to completed list */
	list_move_tail(&job->ctx_item, &ctx->cmp_list);

	/* If now idle, wake anyone waiting for idle. */
	if (list_empty(&ctx->sub_list))
		complete_all(&ctx->ctx_idle);

	mutex_unlock(&ctx->ctx_mtx);
}

int fba_ctx_submit_job(struct fba_ctx *ctx, struct fba_job *job)
{
	struct fba_device *fba_dev = ctx->fba_dev;
	int rc;

	mutex_lock(&ctx->ctx_mtx);
	if (ctx->ctx_state != FBA_CTX_OPEN) {
		rc = -EIO;
		goto out_unlock;
	}

	// Update counters and verify we don't exceed per CTX limit.
	rc = fba_job_inc_usage(fba_dev, &fba_dev->cfg->ctx_limits, &ctx->usage,
			       job);
	if (rc != 0)
		goto out_unlock;

	// Post the job to the channel.
	rc = fba_job_channel_post(ctx->chan, job);
	if (rc == 0) {
		list_add_tail(&job->ctx_item, &ctx->sub_list);
	} else {
		// Posting to the firmware failed. The firmware will not
		// dereference the job, and the application will not read it.
		// Caller will dereference to free it.
		fba_job_dec_usage(fba_dev, &ctx->usage, job);
		fba_dev_err(fba_dev, "Failed add to channel, ERR=%d.", rc);
	}

out_unlock:
	mutex_unlock(&ctx->ctx_mtx);
	return rc;
}

int fba_ctx_read(struct fba_ctx *ctx, struct fba_job_result *res, void *job_key)
{
	struct fba_device *fba_dev = ctx->fba_dev;
	struct fba_job *job, *tmp, *match = NULL;

	mutex_lock(&ctx->ctx_mtx);

	list_for_each_entry_safe(job, tmp, &ctx->cmp_list, ctx_item) {
		if (fba_job_get_job_key(job) == job_key) {
			match = job;
			fba_job_dec_usage(fba_dev, &ctx->usage, match);
			list_del_init(&match->ctx_item);
			break;
		}
	}

	mutex_unlock(&ctx->ctx_mtx);

	// If we reached the end, we didn't find a match.
	if (match == NULL)
		return -EWOULDBLOCK;

	res->cmp = match->req.cmp;
	res->user_id = match->user_data;
	fba_job_trace_read_results(fba_dev->name, match);
	fba_job_put(match);

	return 0;
}
