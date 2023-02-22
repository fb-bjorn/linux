// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#include <linux/device.h>
#include <linux/errno.h>

#include "fba_device.h"

#define JOB_ABORT 0
#define JOB_COMPLETE 1

static void fba_job_channel_cb(bool completed, struct fba_channel_req *req);

// Allocate a job and its resource usage array
struct fba_job *fba_job_alloc(struct fba_ctx *ctx,
			      const struct fba_submission __user *subs,
			      size_t n_subs, uint64_t user_data)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	int ctx_id = fba_ctx_get_id(ctx);
	struct fba_job *job;
	size_t size;
	int rc = -ENOMEM;
	int i;

	if (n_subs < 1)
		return ERR_PTR(-EINVAL);

	if (n_subs > fba_ctx_max_job_subs(ctx))
		return ERR_PTR(-EINVAL);

	size = n_subs * sizeof(struct fba_submission);
	job = kzalloc(sizeof(struct fba_job) + size, GFP_KERNEL);
	if (job == NULL) {
		fba_dev_err(fba_dev, "Failed to allocate job, n_subs=%zu.",
			    n_subs);
		return ERR_PTR(-ENOMEM);
	}

	kref_init(&job->ref);
	job->fba_dev = fba_dev;
	/* Job references the context */
	job->ctx = fba_ctx_get(ctx);

	INIT_LIST_HEAD(&job->ctx_item);
	INIT_LIST_HEAD(&job->dma_list);
	INIT_LIST_HEAD(&job->dma_buf_list);

	// User submitted packets must be copied in.
	if (copy_from_user(job->sub_array, subs, size)) {
		rc = -EFAULT;
		goto err_put_job;
	}

	fba_channel_init_req(&job->req, n_subs, job->sub_array,
			     fba_job_channel_cb);
	job->user_data = user_data;

	fba_alloc_translate_setup(fba_dev, job, job->req.sub_cnt);
	for (i = 0; i < job->req.sub_cnt; i++) {
		rc = fba_alloc_translate_sub(ctx, job, &job->sub_array[i]);
		if (rc < 0)
			goto err_put_job;
	}
	// Setup tail, and assign context ID.
	for (i = 0; i < job->req.sub_cnt; i++) {
		job->sub_array[i].header.context_id = ctx_id;
		job->sub_array[i].header.packet_tail = job->req.sub_cnt - i - 1;
	}

	fba_dev_dbg(fba_dev, "Created job size %d.", job->req.sub_cnt);
	return job;

err_put_job:
	fba_job_put(job);
	return ERR_PTR(rc);
}

// Dereference all allocs used by the job.
// Free all dma associated resources.
void fba_job_release_resources(struct fba_job *job)
{
	struct fba_dma *dma, *tmp;
	struct fba_dmabuf_attach *dmabuf, *tmp2;

	/*
	 * release dmabufs before releasing allocs
	 */
	list_for_each_entry_safe(dmabuf, tmp2, &job->dma_buf_list, list) {
		list_del_init(&dmabuf->list);
		fba_translate_p2p_teardown(job->ctx, dmabuf);
	}

	if (job->allocs)
		fba_alloc_translate_teardown(job);

	list_for_each_entry_safe(dma, tmp, &job->dma_list, item) {
		list_del_init(&dma->item);
		fba_dma_free(job->ctx, dma);
	}
}

void fba_job_set_callback(struct fba_job *job, fba_job_cb_t cb, void *cb_data)
{
	WARN_ON_ONCE(job->callback != NULL);
	job->callback = cb;
	job->callback_data = cb_data;
}

// Set user provided key
void fba_job_set_job_key(struct fba_job *job, void *job_key)
{
	job->user_job_key = job_key;
}

// Get user provded key
void *fba_job_get_job_key(struct fba_job *job)
{
	return job->user_job_key;
}

// Add an alloc to the job
void fba_job_add_alloc(struct fba_job *job, struct fba_alloc *alloc)
{
	job->allocs[job->alloc_cnt++] = alloc;
}

static void fba_job_channel_cb(bool completed, struct fba_channel_req *req)
{
	struct fba_job *job = container_of(req, struct fba_job, req);

	fba_job_complete(job);
}

// Submit a job to the specified channel
int fba_job_channel_post(struct fba_channel *chan, struct fba_job *job)
{
	int rc;

	fba_job_get(job);
	rc = fba_channel_post_req(chan, &job->req);
	if (rc < 0)
		fba_job_put(job);

	return rc;
}

// Notify anyone interested in the job and move it to
// ctx completed list. If no one is interested, free it.
void fba_job_complete(struct fba_job *job)
{
	struct fba_ctx *ctx = job->ctx;
	struct fba_device *fba_dev = job->fba_dev;
	uint16_t cmd_id = job->req.cmp.command_id;
	uint32_t sub_cnt = job->req.sub_cnt;
	size_t alloc_cnt = job->alloc_cnt;
	void *user_job_key = job->user_job_key;

	int ctx_id = fba_ctx_get_id(ctx);

	// Release all the resources for the job once completed.
	// We dont have to wait for the application to read the
	// Job before we release the resources used by the job.
	// This provides resources immediately for other jobs.
	fba_job_release_resources(job);

	fba_ctx_complete_job(ctx, job);

	if (job->callback)
		job->callback(JOB_COMPLETE, job->callback_data);

	trace_fba_job_done(fba_dev->name, job, user_job_key, sub_cnt,
			   alloc_cnt);
	fba_dev_dbg(fba_dev, "Job %04Xh Completed. ctx ID %d items: %d", cmd_id,
		    ctx_id, sub_cnt);

	/* Drop reference from fba_job_channel_post() */
	fba_job_put(job);
}

// NEVER to be called outside of fba_job_put
static void fba_job_free(struct kref *ref)
{
	struct fba_job *job = container_of(ref, struct fba_job, ref);

	fba_ctx_put(job->ctx);
	kfree(job);
}

// Increment reference count to job

struct fba_job *fba_job_get(struct fba_job *job)
{
	kref_get(&job->ref);
	return job;
}

// Dereference the job.
// The calling function must not re-use the job ptr once it
// called deref. The job could have been freed.
void fba_job_put(struct fba_job *job)
{
	kref_put(&job->ref, fba_job_free);
}

int fba_job_inc_usage(struct fba_device *fba_dev, const struct fba_limits *max,
		      struct fba_limits *src, struct fba_job *job)
{
	return fba_util_inc_usage(fba_dev, max, src, job->req.sub_cnt);
}

void fba_job_dec_usage(struct fba_device *fba_dev, struct fba_limits *src,
		       struct fba_job *job)
{
	fba_util_dec_usage(fba_dev, src, job->req.sub_cnt);
}

// Map a DMA buffer and add it to the list of DMAs tracked by the job
struct fba_dma *fba_job_dma_map(struct fba_ctx *ctx, struct fba_job *job,
				void *data, size_t size, int dir,
				bool force_prp)
{
	struct fba_dma *dma;

	dma = fba_dma_map(ctx, data, size, dir, force_prp);
	if (IS_ERR(dma))
		return dma;

	// Success, append dma object
	job->dma_cnt++;
	list_add_tail(&dma->item, &job->dma_list);

	return dma;
}
