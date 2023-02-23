/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

#include "fba_proto.h"
#include "fba_trace.h"

/*
 * FBA Job
 *
 * A Job is a shared object which job tracks a progress of a Packet as it
 * works through the system.  User submits the packet which is a group of
 * submissions with the same user id processed atomically.  Submissions
 * are translated (e.g. VA -> PA, user ID -> sys ID) based on the submitting
 * context and store here prior to queuing on a channel..  The completion is
 * copied here on arrival.
 *
 */

typedef void (*fba_job_cb_t)(int completed, void *data);

struct fba_job {
	struct fba_device *fba_dev;
	struct fba_ctx *ctx; /* Owner context. */
	struct list_head
		ctx_item; /* This job in the context (submitted or completed) list. */
	struct list_head dma_list; /* List of DMA transactions for this job. */
	struct list_head dma_buf_list; /* List of dma_bufs for this job. */
	struct kref ref; /* Reference count. */
	uint64_t user_data;

	fba_job_cb_t callback;
	void *callback_data; /* Freed on deref. */

	/* TODO - make this as responsibility of the job owner */
	void *user_job_key; /* Used to find matching job */

	uint32_t dma_cnt; /* Number of DMA ops in this job. */
	uint32_t dmabuf_cnt; /* Number of DMABUF ops in this job. */
	uint16_t sys_cmd_id; /* Channel unique ID for this packet. */

	/* Owned by job_alloc */
	size_t alloc_cnt;
	struct fba_alloc **allocs; /* allocs used by the job */

	struct fba_channel_req req; /* Req used by channel */
	struct fba_submission sub_array[]; /* Post conversion commands. */
};

struct fba_job *fba_job_alloc(struct fba_ctx *ctx,
			      const struct fba_submission __user *subs,
			      size_t n_subs, uint64_t user_data);

/* Pick notify method prior to posting to channel. */
struct completion *fba_job_get_completion(struct fba_job *job);
int fba_job_set_eventfd(struct fba_job *job, int eventfd);
void fba_job_set_callback(struct fba_job *job, fba_job_cb_t cb, void *cb_data);

/* Map a DMA buffer and add it to the list of DMAs tracked by the job */
struct fba_dma *fba_job_dma_map(struct fba_ctx *ctx, struct fba_job *job,
				void __user *data, size_t size, int dir,
				bool force_prp_ok);

/* Increment reference count of a job */
struct fba_job *fba_job_get(struct fba_job *job);
/* Decrement reference count of a job. */
void fba_job_put(struct fba_job *job);

/* Release any resources associated with job */
void fba_job_release_resources(struct fba_job *job);

/* Update usage counts for job */
int fba_job_inc_usage(struct fba_device *fba_dev, const struct fba_limits *max,
		      struct fba_limits *src, struct fba_job *job);
void fba_job_dec_usage(struct fba_device *fba_dev, struct fba_limits *src,
		       struct fba_job *job);

/* Generic Submission/Completion work (DMA) */
int fba_job_translate(struct fba_ctx *ctx, struct fba_job *job,
		      struct fba_submission *sub);

/* Submit a job to the specified channel */
int fba_job_channel_post(struct fba_channel *chan, struct fba_job *job);

/* Notify that a job completed */
void fba_job_complete(struct fba_job *job);

/* Set user provided job key */
void fba_job_set_job_key(struct fba_job *job, void *job_key);

/* Get user provided job key */
void *fba_job_get_job_key(struct fba_job *job);

/*
 * Tracing helpers.  These live here so that consumers outside of fba_job.c can
 * generate trace events including internal job state, but still have the
 * tracepoint dynamically patched out when disabled.
 */
static inline void fba_job_trace_post(const char *name, struct fba_job *job)
{
	trace_fba_job_post(name, job, job->user_job_key, job->req.sub_cnt,
			   job->alloc_cnt);
}

static inline void fba_job_trace_admin_post(const char *name,
					    struct fba_job *job)
{
	trace_fba_job_admin_post(name, job, job->user_job_key, job->req.sub_cnt,
				 job->alloc_cnt);
}

static inline void fba_job_trace_read_results(const char *name,
					      struct fba_job *job)
{
	trace_fba_job_read_results(name, job, job->user_job_key,
				   job->req.sub_cnt, job->alloc_cnt);
}

/* Add an alloc to the job */
void fba_job_add_alloc(struct fba_job *job, struct fba_alloc *alloc);
