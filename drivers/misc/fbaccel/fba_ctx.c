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
	struct fba_device *dev;
	// Default channel to use.
	struct fba_channel *chan;
	// Reference count.
	struct kref ref;
	// ID for this context.
	int id;

	// Owned by device.
	// This context in the device context list.
	struct list_head dev_item;

	// Lock for state, usage
	struct mutex ctx_mtx;
	// Current state of this context.
	enum fba_ctx_state ctx_state;
};

static void fba_ctx_init(struct fba_device *dev, struct fba_ctx *ctx,
			 struct fba_channel *chan)
{
	ctx->ctx_state = FBA_CTX_VALID;
	ctx->chan = chan;

	kref_init(&ctx->ref);
	mutex_init(&ctx->ctx_mtx);
	INIT_LIST_HEAD(&ctx->dev_item);
}

// Allocate per-file context.
struct fba_ctx *fba_ctx_alloc(struct fba_device *dev)
{
	struct fba_ctx *ctx;
	struct fba_channel *chan;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx == NULL)
		return ERR_PTR(-ENOMEM);
	fba_dev_dbg(dev, "Allocated context.");

	/*
	 * Callers of fba_ctx_alloc should have referenced dev,
	 * we make ctx the new owner of that reference.
	 */
	ctx->dev = dev;

	chan = fba_get_channel(dev, SAIL_CHANNEL_EXE);
	if (chan == NULL) {
		fba_dev_err(dev, "Failed to allocate channel.");
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	fba_dev_dbg(dev, "Allocated channel.");

	fba_ctx_init(dev, ctx, chan);

	// Add to context list.
	fba_device_add_ctx(dev, &ctx->dev_item);

	return ctx;
}

/*
 * Destroy the refcounted fba_ctx, which gets released when the context
 * has been closed by the opening application.
 */
static void fba_ctx_free(struct kref *ref)
{
	struct fba_ctx *ctx = container_of(ref, struct fba_ctx, ref);
	struct fba_device *dev = ctx->dev;

	fba_device_put(dev);

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
	struct fba_device *dev = ctx->dev;
	int rc, id;

	mutex_lock(&ctx->ctx_mtx);

	if (__fba_ctx_is_open(ctx)) {
		/* Already open, quietly do nothing. */
		rc = 0;
		goto out_unlock;
	}

	if (ctx->ctx_state == FBA_CTX_STOPPED) {
		fba_dev_err(dev,
			    "Context promotion failed: context is stopped.");
		rc = -EIO;
		goto out_unlock;
	}
	id = ida_alloc_range(&dev->ctx_ids, 1, dev->n_ctxs, GFP_KERNEL);
	if (id < 0) {
		fba_dev_err(dev,
			    "Context promotion failed: context limit reached.");
		rc = id;
		goto out_unlock;
	}

	rc = fba_admin_create_context(dev, id);
	if (rc) {
		ida_free(&dev->ctx_ids, id);
		fba_dev_err(
			dev,
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
	return ctx->dev;
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

/* Prevent ctx from submitting new jobs, or transitioning to open. */
void fba_ctx_stop(struct fba_ctx *ctx)
{
	bool was_open;
	int rc;

	mutex_lock(&ctx->ctx_mtx);
	was_open = (ctx->ctx_state == FBA_CTX_OPEN);
	ctx->ctx_state = FBA_CTX_STOPPED;
	mutex_unlock(&ctx->ctx_mtx);

	if (was_open) {
		fba_admin_delete_context_begin(ctx->dev, ctx->id);
		rc = fba_admin_delete_context_end(ctx->dev, ctx->id);
		if (rc == 0)
			ida_free(&ctx->dev->ctx_ids, ctx->id);
	}
}

// Close the context
int fba_ctx_close(struct fba_ctx *ctx)
{
	struct fba_device *dev = ctx->dev;

	fba_device_del_ctx(dev, &ctx->dev_item);

	if (fba_ctx_is_open(ctx))
		fba_ctx_stop(ctx);

	return 0;
}
