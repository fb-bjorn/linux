// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#include <linux/errno.h>
#include <linux/genalloc.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>

#include "fba_ioctl.h"
#include "fba_device.h"
#include "fba_ctx.h"
#include "fba_mon.h"

#define MAX_MON_SET_DATA_SIZE 2048

static long fba_ioctl_bar_access(struct fba_device *fba_dev,
				 struct bar_access_ioctl *access)
{
	int rv;
	const char *rwstr;
	uint32_t nwords;
	uint32_t owords;
	uint32_t i;
	uint32_t val;
	uint32_t __iomem *bar_words;
	uint64_t bar_len;

	rwstr = (access->type == FBA_ACCESS_READ) ? "Read" : "Write";
	fba_dev_dbg(fba_dev, "%s bar=%u offset=%#llx len=%#x", rwstr,
		    access->bar, access->offset, access->len);

	if ((access->bar >= BAR_CNT) || (access->len & 3) ||
	    (access->offset & 3)) {
		fba_dev_dbg(fba_dev, "Invalid BAR access params.");
		return -EINVAL;
	}

	if (fba_dev->bars[access->bar] == NULL)
		return -EINVAL;
	bar_len = pci_resource_len(fba_dev->pdev, access->bar);
	if (access->offset >= bar_len || access->len > bar_len - access->offset)
		return -EINVAL;

	nwords = access->len / sizeof(uint32_t);
	owords = access->offset / sizeof(uint32_t);
	bar_words = (uint32_t __iomem *)fba_dev->bars[access->bar];

	if (access->type == FBA_ACCESS_READ) {
		for (i = 0; i < nwords; i++) {
			val = ioread32(&bar_words[i + owords]);
			rv = put_user(val, &access->data[i]);
			if (rv)
				return rv;
		}
		return 0;
	}
	if (access->type == FBA_ACCESS_WRITE) {
		for (i = 0; i < nwords; i++) {
			rv = get_user(val, &access->data[i]);
			if (rv)
				return rv;
			iowrite32(val, &bar_words[i + owords]);
		}
		return 0;
	}

	fba_dev_err(fba_dev, "Invalid access type: %u", access->type);
	return -EINVAL;
}

static void fba_eventfd_cb(int completed, void *data)
{
	struct eventfd_ctx *ectx = (struct eventfd_ctx *)data;

	// TODO: T84471228 check return value of eventfd_signal
	if (completed)
		eventfd_signal(ectx, 1);
	eventfd_ctx_put(ectx);
}

static int fba_setup_eventfd_cb(struct fba_job *job, int eventfd)
{
	struct eventfd_ctx *ectx;

	WARN_ON(eventfd < 0);

	ectx = eventfd_ctx_fdget(eventfd);
	if (IS_ERR(ectx)) {
		fba_dev_err(job->fba_dev, "Failed to get event context.");
		return PTR_ERR(ectx);
	}
	fba_job_set_callback(job, fba_eventfd_cb, ectx);
	fba_job_set_job_key(job, ectx);

	return 0;
}

static long fba_ioctl_post_id(struct fba_ctx *ctx, struct post_ioctl *post_req)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	struct fba_job *job = NULL;
	int rc;

	trace_fba_post_cmd(fba_dev->name, post_req->count, post_req->user_data);

	// Allocate the job.
	job = fba_job_alloc(ctx, post_req->subs, post_req->count,
			    post_req->user_data);
	if (IS_ERR(job))
		return PTR_ERR(job);

	if (post_req->eventfd >= 0) {
		rc = fba_setup_eventfd_cb(job, post_req->eventfd);
		if (rc != 0)
			goto err_put_job;
	}
	fba_job_trace_post(fba_dev->name, job);

	rc = fba_ctx_submit_job(ctx, job);
	if (rc < 0) {
		fba_dev_err(fba_dev, "Failed add to ctx, ERR=%d.", rc);
		goto err_put_efd;
	}

	return rc;

err_put_efd:
	if (post_req->eventfd >= 0) {
		struct eventfd_ctx *ectx = fba_job_get_job_key(job);

		eventfd_ctx_put(ectx);
	}
err_put_job:
	fba_job_release_resources(job);
	fba_job_put(job);
	return rc;
}

static_assert((sizeof(struct result_info) % sizeof(uint64_t)) == 0);

static long fba_ioctl_read_results(struct fba_ctx *ctx, int eventfd,
				   struct result_info __user *results,
				   size_t count)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	struct eventfd_ctx *ectx = NULL;
	int rc = 0;
	int cnt = 0;

	if ((results == NULL) || (count == 0))
		return -EINVAL;

	if (eventfd >= 0) {
		ectx = eventfd_ctx_fdget(eventfd);
		if (IS_ERR(ectx)) {
			fba_dev_err(fba_dev, "Unable to get eventfd context.");
			return PTR_ERR(ectx);
		}
	}

	for (cnt = 0; cnt < count; cnt++) {
		struct fba_job_result result;
		struct result_info ri;

		rc = fba_ctx_read(ctx, &result, ectx);
		if (rc != 0) {
			fba_dev_dbg(fba_dev,
				    "Failed to read result from channel.");
			goto out_put_ectx;
		}

		memset(&ri, 0, sizeof(ri));
		ri.user_data = result.user_id;
		ri.status = result.cmp.status;
		ri.flags_index = result.cmp.flags;

		rc = copy_to_user(&results[cnt], &ri, sizeof(ri));
		if (rc) {
			rc = -EFAULT;
			break;
		}
	}

out_put_ectx:
	if (ectx)
		eventfd_ctx_put(ectx);

	if (cnt)
		return cnt;

	return rc;
}

static long fba_ioctl_resource_info(struct fba_device *fba_dev,
				    struct resource_info __user *dest,
				    size_t offs, size_t count)
{
	size_t i;
	long rc = 0;
	struct resource_info info;

	memset(&info, 0, sizeof(info));
	for (i = 0; i < count; i++) {
		rc = fba_resource_info(fba_dev, &info, offs + i);
		if (rc)
			return rc;

		rc = copy_to_user(&dest[i], &info, sizeof(info));
		if (rc)
			return -EFAULT;
	}
	return count;
}

static long fba_ioctl_resource_alloc(struct fba_ctx *ctx, uint16_t type,
				     uint16_t subtype, uint16_t idx,
				     uint64_t size, uint64_t *rid,
				     uint64_t *addr)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	struct fba_resource *res;
	struct fba_alloc *alloc;
	long rc;

	res = fba_resource_find(fba_dev, idx, type, subtype);
	if (IS_ERR(res))
		return PTR_ERR(res);

	alloc = fba_resource_allocate(res, size);
	fba_resource_put(res);
	if (IS_ERR(alloc)) {
		fba_dev_dbg(fba_dev, "Failed allocation size %#llx from %d.",
			    size, idx);
		return PTR_ERR(alloc);
	}

	rc = fba_ctx_add_alloc(ctx, alloc);
	if (rc < 0) {
		fba_alloc_put(alloc);
		return rc;
	}

	// Convert ID to RID.
	*rid = ((uint64_t)rc << RID_ID_SHIFT);
	*addr = fba_alloc_get_addr(alloc);

	fba_alloc_put(alloc);

	return 0;
}

static long fba_ioctl_resource_free(struct fba_ctx *ctx, uint64_t rid)
{
	// RID should only contain ID not offset.
	if (rid & RID_OFFS_MASK)
		return -EINVAL;

	return fba_ctx_remove_alloc(ctx, rid >> RID_ID_SHIFT);
}

/* For legacy reasons, always return FBA_ACTIVE */
#define FBA_ACTIVE 8

static long fba_ioctl_get_details(struct fba_device *fba_dev,
				  struct get_details_ioctl *detail)
{
	int i;

	memset(detail, 0, sizeof(*detail));
	detail->host_ipc_bar = fba_dev->cfg->host_ipc_bar;
	detail->host_ipc_stride = fba_dev->cfg->host_ipc_stride;
	detail->host_ipc_offset = fba_dev->cfg->host_ipc_offset;
	detail->irq_num = CHANNEL_MSIX_VECTOR_BASE + fba_dev->channel_total;
	for (i = 0; i < BAR_CNT; i++) {
		detail->bar_addrs[i] = (uintptr_t)fba_dev->bars[i];
		detail->bar_sizes[i] = pci_resource_len(fba_dev->pdev, i);
	}
	detail->dma_addr = fba_dev->debug_area.addr;
	detail->dma_size = SAIL_DEBUG_SIZE;
	detail->resource_cnt = fba_dev->resource_cnt;
	detail->context_cnt = fba_dev->n_ctxs;
	detail->mon_item_cnt = fba_mon_get_item_cnt(&fba_dev->mon);
	detail->mon_group_cnt = fba_mon_get_group_cnt(&fba_dev->mon);

	detail->state = FBA_ACTIVE;

	detail->context_used = fba_device_get_ctx_count(fba_dev);

	fba_channel_get_fill_watermarks(fba_get_channel(fba_dev,
							SAIL_CHANNEL_ADMIN),
					&detail->admin_sq_fill_watermark,
					&detail->admin_cq_fill_watermark);
	fba_channel_get_fill_watermarks(
		fba_get_channel(fba_dev, SAIL_CHANNEL_EXE),
		&detail->exe_sq_fill_watermark, &detail->exe_cq_fill_watermark);

	return 0;
}

static long __fba_ioctl_access_dma(struct fba_device *fba_dev,
				   struct dma_access_ioctl *dma_req)
{
	uint8_t *ptr;

	ptr = (uint8_t *)fba_dev->debug_area.data;
	if (dma_req->offset >= SAIL_DEBUG_SIZE)
		return -EINVAL;
	if (dma_req->len > SAIL_DEBUG_SIZE - dma_req->offset)
		return -EINVAL;

	ptr += dma_req->offset;
	if (dma_req->type == FBA_ACCESS_READ) {
		if (copy_to_user(dma_req->data, ptr, dma_req->len))
			return -EFAULT;
	} else {
		if (copy_from_user(ptr, dma_req->data, dma_req->len))
			return -EFAULT;
	}
	dma_req->addr = fba_dev->debug_area.addr;

	return 0;
}

static long fba_ioctl_access_dma(struct fba_device *fba_dev,
				 struct dma_access_ioctl *dma_req)
{
	long rc;

	mutex_lock(&fba_dev->area_mutex);
	if (!fba_shared_is_valid(&fba_dev->debug_area)) {
		rc = -EIO;
		goto out_unlock;
	}
	rc = __fba_ioctl_access_dma(fba_dev, dma_req);

out_unlock:
	mutex_unlock(&fba_dev->area_mutex);

	return rc;
}

static long __fba_ioctl_access_scratch(struct fba_ctx *ctx,
				       struct scratch_access_ioctl *scratch_req)
{
	void __user *user = scratch_req->data;
	struct fba_alloc *alloc;
	uint64_t scratch;
	void *scratch_va;
	long rc;

	// Ref RID and return address as 64b value;
	alloc = fba_alloc_convert(ctx, scratch_req->rid, scratch_req->len,
				  &scratch);
	if (IS_ERR(alloc))
		return PTR_ERR(alloc);

	// Convert from DMA addr to VA
	scratch_va = fba_alloc_map_to_va(alloc, scratch);

	if (scratch_req->type == FBA_ACCESS_READ)
		rc = copy_to_user(user, scratch_va, scratch_req->len);
	else
		rc = copy_from_user(scratch_va, user, scratch_req->len);
	if (rc)
		rc = -EFAULT;

	// Release the alloc
	fba_alloc_put(alloc);

	return rc;
}

static long fba_ioctl_access_scratch(struct fba_ctx *ctx,
				     struct scratch_access_ioctl *scratch_req)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	long rc;

	mutex_lock(&fba_dev->area_mutex);
	if (!fba_shared_is_valid(&fba_dev->scratch_area)) {
		rc = -EIO;
		goto out_unlock;
	}
	rc = __fba_ioctl_access_scratch(ctx, scratch_req);

out_unlock:
	mutex_unlock(&fba_dev->area_mutex);

	return rc;
}

static long fba_ioctl_stream_create(struct fba_device *fba_dev,
				    struct fba_ctx *parent,
				    struct stream_ioctl *stream_req)
{
	struct fba_ctx *ctx;
	long ret;

	ctx = fba_ctx_alloc(fba_device_get(fba_dev), parent);
	if (IS_ERR(ctx)) {
		fba_dev_err(fba_dev, "Failed to allocate streaming context.");
		fba_device_put(fba_dev);
		return PTR_ERR(ctx);
	}

	ret = fba_ctx_open(ctx);
	if (ret < 0) {
		fba_dev_err(fba_dev, "Failed to open streaming context.");
		goto err_close_ctx;
	}

	ret = anon_inode_getfd("freya_streaming", &fba_fops, ctx,
			       stream_req->flags);
	if (ret < 0) {
		fba_dev_err(fba_dev, "Failed to create streaming inode: %ld.",
			    ret);
		goto err_close_ctx;
	}

	stream_req->fd = ret;
	return 0;

err_close_ctx:
	fba_ctx_close(ctx);
	fba_ctx_put(ctx);
	return ret;
}

static long fba_ioctl_get_limit_params(struct fba_device *fba_dev,
				       struct fba_limit_params_ioctl *params)
{
	switch (params->channel_type) {
	/* Count the number of user available channels */
	case FBA_CHANNEL_TYPE_CONCURRENT:
		params->max_channels = fba_dev->channel_cnt - SAIL_CHANNEL_EXE;
		params->max_entries_per_packet =
			fba_dev->channel_max_packet_size;
		break;

	case FBA_CHANNEL_TYPE_STREAMING:
		params->max_channels = fba_dev->streaming_channel_cnt;
		params->max_entries_per_packet =
			fba_dev->streaming_channel_max_packet_size;
		break;

	default:
		return -EINVAL;
	}

	params->max_entries_per_context = fba_dev->cfg->ctx_limits.sub_cnt;

	return 0;
}

static int fba_mon_defs_ioctl(struct fba_device *fba_dev, void __user *data)
{
	struct mon_defs_ioctl mon_defs_req;
	int rv;

	if (copy_from_user(&mon_defs_req, data, sizeof(mon_defs_req)))
		return -EFAULT;

	rv = fba_mon_get_defs(fba_dev, &mon_defs_req);
	if (rv)
		return rv;

	if (copy_to_user(data, &mon_defs_req, sizeof(mon_defs_req)))
		return -EFAULT;

	return 0;
}

static int fba_mon_data_ioctl(struct fba_device *fba_dev, void __user *data_req)
{
	struct mon_data_ioctl mon_data_req = {}; // Users's request
	uint32_t *ids;
	void *data;
	int16_t *errors;
	int rc = 0;
	size_t data_size = 0;
	size_t id_size = 0;
	size_t err_size = 0;

	if (copy_from_user(&mon_data_req, data_req, sizeof(mon_data_req)))
		return -EFAULT;

	if (mon_data_req.id_count == 0)
		return 0;

	id_size = mon_data_req.id_count * sizeof(*mon_data_req.ids);
	data_size = mon_data_req.id_count * MON_VALUE_SIZE;
	err_size = mon_data_req.id_count * sizeof(*mon_data_req.errors);
	ids = kzalloc(id_size, GFP_KERNEL);
	data = kzalloc(data_size, GFP_KERNEL);
	errors = kzalloc(err_size, GFP_KERNEL);
	if (ids == NULL || data == NULL || errors == NULL) {
		rc = -ENOMEM;
		goto out_free;
	}

	if (copy_from_user(ids, mon_data_req.ids, id_size)) {
		rc = -EFAULT;
		goto out_free;
	}

	if (copy_from_user(data, mon_data_req.data, data_size)) {
		rc = -EFAULT;
		goto out_free;
	}

	if (copy_from_user(errors, mon_data_req.errors, err_size)) {
		rc = -EFAULT;
		goto out_free;
	}

	rc = fba_mon_get_data(fba_dev, ids, mon_data_req.id_count, data,
			      errors);
	if (rc != 0)
		goto out_free;

	if (copy_to_user((void __user *)mon_data_req.ids, ids, id_size)) {
		rc = -EFAULT;
		goto out_free;
	}

	if (copy_to_user((void __user *)mon_data_req.data, data, data_size)) {
		rc = -EFAULT;
		goto out_free;
	}

	if (copy_to_user((void __user *)mon_data_req.errors, errors,
			 err_size)) {
		rc = -EFAULT;
		goto out_free;
	}

out_free:
	kfree(ids);
	kfree(data);
	kfree(errors);

	return rc;
}

static int fba_mon_data_set_ioctl(struct fba_device *fba_dev,
				  void __user *data_req)
{
	struct mon_data_ioctl mon_data_req; // Users's request
	uint32_t *ids;
	void *data;
	size_t data_size = 0;
	size_t id_size = 0;
	int max_id_cnt = fba_mon_get_item_cnt(&fba_dev->mon);
	int rc = 0;

	if (copy_from_user(&mon_data_req, data_req, sizeof(mon_data_req)))
		return -EFAULT;

	if (mon_data_req.id_count > max_id_cnt)
		return -EINVAL;

	if (mon_data_req.id_count == 0)
		return 0;

	id_size = mon_data_req.id_count * sizeof(*mon_data_req.ids);
	data_size = mon_data_req.id_count * MON_VALUE_SIZE;
	if (data_size > MAX_MON_SET_DATA_SIZE)
		return -EINVAL;

	ids = kzalloc(id_size, GFP_KERNEL);
	data = kzalloc(data_size, GFP_KERNEL);

	if (data == NULL) {
		rc = -ENOMEM;
		goto out_free_bufs;
	}

	if (ids == NULL) {
		rc = -ENOMEM;
		goto out_free_bufs;
	}

	if (copy_from_user(ids, mon_data_req.ids, id_size)) {
		rc = -EFAULT;
		goto out_free_bufs;
	}

	if (copy_from_user(data, mon_data_req.data, data_size)) {
		rc = -EFAULT;
		goto out_free_bufs;
	}
	rc = fba_mon_set_data(fba_dev, ids, mon_data_req.id_count, data);

out_free_bufs:
	kfree(ids);
	kfree(data);

	return rc;
}

static long fba_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	void __user *data = (void __user *)arg;
	struct fba_ctx *ctx = fp->private_data;
	struct fba_ctx *streaming_ctx, *parent_ctx;
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	long rc;

	// All IOCTLS take a struct *, so ensure it's non-zero.
	if (data == NULL)
		return -EINVAL;

	streaming_ctx = ctx;
	parent_ctx = fba_ctx_get_parent(ctx);
	if (parent_ctx)
		ctx = parent_ctx;

	switch (cmd) {
	case FBA_IOCTL_RESOURCE_INFO: {
		struct res_info_ioctl res_req;

		if (copy_from_user(&res_req, data, sizeof(res_req)))
			return -EFAULT;
		rc = fba_ioctl_resource_info(fba_dev, res_req.info,
					     res_req.offset, res_req.count);
		return rc;
	}
	case FBA_IOCTL_RESOURCE_ALLOC: {
		struct res_alloc_ioctl alloc_req;

#ifdef NOTYET // Not needed?  If needed, alloc is racy?
		if (!fba_ctx_is_open(ctx))
			return -EACCES;
#endif

		if (copy_from_user(&alloc_req, data, sizeof(alloc_req)))
			return -EFAULT;
		rc = fba_ioctl_resource_alloc(ctx, alloc_req.type,
					      alloc_req.subtype,
					      alloc_req.index, alloc_req.count,
					      &alloc_req.rid, &alloc_req.start);
		if (rc != 0)
			return rc;
		if (copy_to_user(data, &alloc_req, sizeof(alloc_req)))
			return -EFAULT;
		return 0;
	}
	case FBA_IOCTL_P2P_RESOURCE_EXPORT: {
		struct res_p2p_export_ioctl export_p2p_req;

		if (!fba_ctx_is_open(ctx))
			return -EACCES;

		if (copy_from_user(&export_p2p_req, data,
				   sizeof(export_p2p_req)))
			return -EFAULT;
		rc = fba_get_dmabuf(ctx, export_p2p_req.source_rid,
				    &export_p2p_req.fd);
		if (rc != 0)
			return rc;
		if (copy_to_user(data, &export_p2p_req, sizeof(export_p2p_req)))
			return -EFAULT;
		return 0;
	}
	case FBA_IOCTL_RESOURCE_FREE:
		return fba_ioctl_resource_free(ctx, arg);
	case FBA_IOCTL_ACCESS_BAR: {
		struct bar_access_ioctl bar_req;

		if (copy_from_user(&bar_req, data, sizeof(bar_req)))
			return -EFAULT;
		rc = fba_ioctl_bar_access(fba_dev, &bar_req);
		return rc;
	}
	case FBA_IOCTL_POST_ID: {
		struct post_ioctl post_req;

		if (!fba_ctx_is_open(streaming_ctx))
			return -EACCES;

		if (copy_from_user(&post_req, data, sizeof(post_req)))
			return -EFAULT;

		rc = fba_ioctl_post_id(streaming_ctx, &post_req);
		return rc;
	}
	case FBA_IOCTL_READ_ID: {
		struct read_ioctl read_req;

		if (copy_from_user(&read_req, data, sizeof(read_req)))
			return -EFAULT;

		rc = fba_ioctl_read_results(streaming_ctx, read_req.eventfd,
					    read_req.results, read_req.count);
		return rc;
	}
	case FBA_IOCTL_GET_DETAILS: {
		struct get_details_ioctl details_req;

		if (copy_from_user(&details_req, data, sizeof(details_req)))
			return -EFAULT;

		rc = fba_ioctl_get_details(fba_dev, &details_req);
		if (rc != 0)
			return rc;

		if (copy_to_user(data, &details_req, sizeof(details_req)))
			return -EFAULT;

		return 0;
	}
	case FBA_IOCTL_MON_DEFS:
		return fba_mon_defs_ioctl(fba_dev, data);
	case FBA_IOCTL_MON_DATA:
		return fba_mon_data_ioctl(fba_dev, data);
	case FBA_IOCTL_MON_SET_DATA:
		return fba_mon_data_set_ioctl(fba_dev, data);
	case FBA_IOCTL_SET_STATE:
		/* No need to set state anymore, but still need side effect */
		rc = fba_ctx_open(ctx);

		return rc;
	case FBA_IOCTL_ACCESS_DMA: {
		struct dma_access_ioctl dma_req;

		if (copy_from_user(&dma_req, data, sizeof(dma_req)))
			return -EFAULT;
		rc = fba_ioctl_access_dma(fba_dev, &dma_req);
		if (rc != 0)
			return rc;
		if (copy_to_user(data, &dma_req, sizeof(dma_req)))
			return -EFAULT;

		return 0;
	}
	case FBA_IOCTL_ACCESS_SCRATCH: {
		struct scratch_access_ioctl scratch_req;

		if (copy_from_user(&scratch_req, data, sizeof(scratch_req)))
			return -EFAULT;
		return fba_ioctl_access_scratch(ctx, &scratch_req);
	}
	case FBA_IOCTL_STREAM_CREATE: {
		struct stream_ioctl stream_req;

		if (copy_from_user(&stream_req, data,
				   sizeof(struct stream_ioctl)))
			return -EFAULT;

		rc = fba_ioctl_stream_create(fba_dev, ctx, &stream_req);
		if (rc == 0) {
			if (copy_to_user(data, &stream_req,
					 sizeof(struct stream_ioctl))) {
				return -EFAULT;
			}
		}
		return rc;
	}
	case FBA_IOCTL_GET_LIMIT_PARAMS: {
		struct fba_limit_params_ioctl limit_params_req;

		if (copy_from_user(&limit_params_req, data,
				   sizeof(limit_params_req)))
			return -EFAULT;

		rc = fba_ioctl_get_limit_params(fba_dev, &limit_params_req);
		if (rc == 0) {
			if (copy_to_user(data, &limit_params_req,
					 sizeof(limit_params_req)))
				return -EFAULT;
		}
		return rc;
	}
	default:
		/* Provide a hook for device-specific ioctl handling */
		if (fba_dev->cfg->impl_ops->priv_ioctl)
			return fba_dev->cfg->impl_ops->priv_ioctl(fp, cmd, arg);
		return -EINVAL;
	}
	return 0;
}

static __poll_t fba_poll(struct file *filp, struct poll_table_struct *wait)
{
	__poll_t mask = 0;
	struct fba_ctx *ctx = filp->private_data;
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);

	poll_wait(filp, &fba_dev->read_q, wait);
	poll_wait(filp, &fba_dev->write_q, wait);

	if (fba_ctx_has_completions(ctx))
		mask |= EPOLLIN | EPOLLRDNORM;

	if (fba_ctx_has_space(ctx))
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static int fba_open(struct inode *node, struct file *fp)
{
	struct fba_ctx *ctx;
	struct fba_device *fba_dev;

	fba_dev = fba_device_lookup(iminor(node));
	if (fba_dev == NULL)
		return -ENODEV;

	if (!atomic_read(&fba_dev->allow_open)) {
		bool has_failed = atomic_read(&fba_dev->has_failed);

		fba_device_put(fba_dev);
		if (has_failed)
			return -EIO;
		else
			return -EBUSY;
	}

	fba_dev_dbg(fba_dev, "Open.");

	ctx = fba_ctx_alloc(fba_dev, NULL /* parent */);
	if (IS_ERR(ctx)) {
		fba_dev_err(fba_dev, "Failed to allocate context.");
		fba_device_put(fba_dev);
		return PTR_ERR(ctx);
	}

	fp->private_data = ctx;

	return 0;
}

static int fba_release(struct inode *node, struct file *fp)
{
	struct fba_ctx *ctx = fp->private_data;
	int rc;

	rc = fba_ctx_close(ctx);
	fba_ctx_put(ctx);

	return rc;
}

const struct file_operations fba_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fba_ioctl,
	.poll = fba_poll,
	.open = fba_open,
	.release = fba_release,
};
