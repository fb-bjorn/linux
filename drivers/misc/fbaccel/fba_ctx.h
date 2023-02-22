/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/*
 * FBA Context
 *
 * A context represents a single file object openned by the application.  All
 * user space IO is filtered through this context object. The context is ref
 * counted, by the file object and any jobs allocated on it's behalf.  The
 * context is deref on job destruction and when disassociated from the OS
 * file object.
 *
 * Contexts are created in a VALID state, and must be promoted to OPEN
 * via a call to fba_ctx_open in order to post messages to the firmware.  It
 * must be demoted to CLOSED via a call to fba_ctx_close prior to being
 * derefed by the file object.
 *
 * Calling close will also release any remaining resources allocated on
 * behalf of the context (as the context may not be used again).  Any allocs
 * used by jobs will persist until those specific jobs complete.
 */

struct fba_job;
struct fba_job_result;

// Allocate and initialize a dynamic context object.
struct fba_ctx *fba_ctx_alloc(struct fba_device *fba_dev,
			      struct fba_ctx *parent);

// Send open admin message and promote the state to allow FW transactions.
int fba_ctx_open(struct fba_ctx *ctx);

// Send a close admin message (if appropriate) and release any resources
// associated with this context as it will no longer be valid.
// NOTE:  This does not deref the ctx.
int fba_ctx_close(struct fba_ctx *ctx);

// Check state of ctx.
bool fba_ctx_is_valid(struct fba_ctx *ctx);
bool fba_ctx_is_open(struct fba_ctx *ctx);

/* Get device associated with ctx. */
struct fba_device *fba_ctx_to_fba_device(struct fba_ctx *ctx);

/* Get parent context (or NULL if no parent.) */
struct fba_ctx *fba_ctx_get_parent(struct fba_ctx *ctx);

/* Add alloc to context, and return unique id (or -ENOSPC if limit reached.) */
int fba_ctx_add_alloc(struct fba_ctx *ctx, struct fba_alloc *alloc);

/* remove alloc with specified id from context. */
int fba_ctx_remove_alloc(struct fba_ctx *ctx, int id);

/* Find and reference an alloc by the given id */
struct fba_alloc *fba_ctx_find_alloc(struct fba_ctx *ctx, int id);

/* Get id associated with ctx */
int fba_ctx_get_id(struct fba_ctx *ctx);

/* Helper to keep fba_device from needing to reach into ctx */
struct fba_ctx *fba_dev_item_to_fba_ctx(struct list_head *item);

/* Returns true if associated channel has space remaining */
bool fba_ctx_has_space(struct fba_ctx *ctx);

/* Returns max number of submission entries per job for this ctx */
uint64_t fba_ctx_max_job_subs(struct fba_ctx *ctx);

// Complete a job associated with this ctx.  Returns true if callback called.
void fba_ctx_complete_job(struct fba_ctx *ctx, struct fba_job *job);

// Returns true if there are any completed jobs.
bool fba_ctx_has_completions(struct fba_ctx *ctx);

// Add a job and submit to the associated channel
int fba_ctx_submit_job(struct fba_ctx *ctx, struct fba_job *job);

// Attempt to read one matching result from the context.
int fba_ctx_read(struct fba_ctx *ctx, struct fba_job_result *res,
		 void *job_key);

// Increment reference count of a context
struct fba_ctx *fba_ctx_get(struct fba_ctx *ctx);
// Decrement reference count of a context.
void fba_ctx_put(struct fba_ctx *ctx);
// Post an admin command through the admin channel
int fba_ctx_post_admin(struct fba_ctx *ctx, struct fba_submission *sub,
		       uint16_t *stat);

// Prevent new jobs from being submitted to associated channel.
void fba_ctx_stop(struct fba_ctx *ctx);

/* After ctx has been stopped, wait for all jobs to complete (or timeout) */
void fba_ctx_wait_idle(struct fba_ctx *ctx);
