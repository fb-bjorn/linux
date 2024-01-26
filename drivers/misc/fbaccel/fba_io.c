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
	const char *rwstr;
	uint32_t nwords;
	uint32_t owords;
	uint32_t i;
	uint32_t val;
	uint32_t __iomem *bar_words;
	uint32_t __user *data;
	uint64_t bar_len;
	int ret;

	data = u64_to_user_ptr(access->data_ptr);
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
			ret = put_user(val, data + i);
			if (ret)
				return ret;
		}
		return 0;
	}
	if (access->type == FBA_ACCESS_WRITE) {
		for (i = 0; i < nwords; i++) {
			ret = get_user(val, data + i);
			if (ret)
				return ret;
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

	/* TODO: T84471228 check return value of eventfd_signal */
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
	const struct fba_submission __user *subs;
	struct fba_job *job = NULL;
	int ret;

	trace_fba_post_cmd(fba_dev->name, post_req->count, post_req->user_data);

	/* Allocate the job. */
	subs = u64_to_user_ptr(post_req->subs_ptr);
	job = fba_job_alloc(ctx, subs, post_req->count, post_req->user_data);
	if (IS_ERR(job)) {
		fba_dev_err(fba_dev, "Failed job alloc ERR=%d.", PTR_ERR(job));
		return PTR_ERR(job);
	}

	if (post_req->eventfd >= 0) {
		ret = fba_setup_eventfd_cb(job, post_req->eventfd);
		if (ret != 0) {
			fba_dev_err(fba_dev, "Failed setup_eventfd_cb ERR=%d.",
				    ret);
			goto err_put_job;
		}
	}
	fba_job_trace_post(fba_dev->name, job);

	ret = fba_ctx_submit_job(ctx, job);
	if (ret < 0) {
		fba_dev_err(fba_dev, "Failed add to ctx, ERR=%d.", ret);
		goto err_put_efd;
	}

	return ret;

err_put_efd:
	if (post_req->eventfd >= 0) {
		struct eventfd_ctx *ectx = fba_job_get_job_key(job);

		eventfd_ctx_put(ectx);
	}
err_put_job:
	fba_job_release_resources(job);
	fba_job_put(job);
	return ret;
}

static_assert((sizeof(struct result_info) % sizeof(uint64_t)) == 0);

static long fba_ioctl_read_results(struct fba_ctx *ctx, int eventfd,
				   struct result_info __user *results,
				   size_t count)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	struct eventfd_ctx *ectx = NULL;
	int ret = 0;
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

		ret = fba_ctx_read(ctx, &result, ectx);
		if (ret != 0) {
			fba_dev_dbg(fba_dev,
				    "Failed to read result from channel.");
			goto out_put_ectx;
		}

		memset(&ri, 0, sizeof(ri));
		ri.user_data = result.user_id;
		ri.status = result.status;
		ri.flags_index = result.flags;

		ret = copy_to_user(&results[cnt], &ri, sizeof(ri));
		if (ret) {
			ret = -EFAULT;
			break;
		}
	}

out_put_ectx:
	if (ectx)
		eventfd_ctx_put(ectx);

	if (cnt)
		return cnt;

	return ret;
}

static long fba_ioctl_resource_info(struct fba_device *fba_dev,
				    struct resource_info __user *dest,
				    size_t offs, size_t count)
{
	size_t i;
	long ret = 0;
	struct resource_info info;

	memset(&info, 0, sizeof(info));
	for (i = 0; i < count; i++) {
		ret = fba_resource_info(fba_dev, &info, offs + i);
		if (ret)
			return ret;

		ret = copy_to_user(&dest[i], &info, sizeof(info));
		if (ret)
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
	long ret;

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

	ret = fba_ctx_add_alloc(ctx, alloc);
	if (ret < 0) {
		fba_alloc_put(alloc);
		return ret;
	}

	/* Convert ID to RID. */
	*rid = ((uint64_t)ret << RID_ID_SHIFT);
	*addr = fba_alloc_get_addr(alloc);

	fba_alloc_put(alloc);

	return 0;
}

static long fba_ioctl_resource_free(struct fba_ctx *ctx, uint64_t rid)
{
	/* RID should only contain ID not offset. */
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
	detail->host_ipc_stride = fba_dev->cfg->host_ipc_entry_stride;
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
	void __user *data = u64_to_user_ptr(dma_req->data_ptr);
	uint8_t *ptr;

	ptr = (uint8_t *)fba_dev->debug_area.data;
	if (dma_req->offset >= SAIL_DEBUG_SIZE)
		return -EINVAL;
	if (dma_req->len > SAIL_DEBUG_SIZE - dma_req->offset)
		return -EINVAL;

	ptr += dma_req->offset;
	if (dma_req->type == FBA_ACCESS_READ) {
		if (copy_to_user(data, ptr, dma_req->len))
			return -EFAULT;
	} else {
		if (copy_from_user(ptr, data, dma_req->len))
			return -EFAULT;
	}
	dma_req->addr = fba_dev->debug_area.addr;

	return 0;
}

static long fba_ioctl_access_dma(struct fba_device *fba_dev,
				 struct dma_access_ioctl *dma_req)
{
	long ret;

	mutex_lock(&fba_dev->area_mutex);
	if (!fba_shared_is_valid(&fba_dev->debug_area)) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = __fba_ioctl_access_dma(fba_dev, dma_req);

out_unlock:
	mutex_unlock(&fba_dev->area_mutex);

	return ret;
}

static long __fba_ioctl_access_scratch(struct fba_ctx *ctx,
				       struct scratch_access_ioctl *scratch_req)
{
	void __user *user = u64_to_user_ptr(scratch_req->data_ptr);
	struct fba_alloc *alloc;
	__le64 scratch;
	void *scratch_va;
	long ret;

	/* Ref RID and return address as 64b value; */
	alloc = fba_alloc_convert(ctx, scratch_req->rid, scratch_req->len,
				  &scratch);
	if (IS_ERR(alloc))
		return PTR_ERR(alloc);

	/* Convert from DMA addr to VA */
	scratch_va = fba_alloc_map_to_va(alloc, scratch);

	if (scratch_req->type == FBA_ACCESS_READ)
		ret = copy_to_user(user, scratch_va, scratch_req->len);
	else
		ret = copy_from_user(scratch_va, user, scratch_req->len);
	if (ret)
		ret = -EFAULT;

	/* Release the alloc */
	fba_alloc_put(alloc);

	return ret;
}

static long fba_ioctl_access_scratch(struct fba_ctx *ctx,
				     struct scratch_access_ioctl *scratch_req)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	long ret;

	mutex_lock(&fba_dev->area_mutex);
	if (!fba_shared_is_valid(&fba_dev->scratch_area)) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = __fba_ioctl_access_scratch(ctx, scratch_req);

out_unlock:
	mutex_unlock(&fba_dev->area_mutex);

	return ret;
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
	int ret;

	if (copy_from_user(&mon_defs_req, data, sizeof(mon_defs_req)))
		return -EFAULT;

	ret = fba_mon_get_defs(fba_dev, &mon_defs_req);
	if (ret)
		return ret;

	if (copy_to_user(data, &mon_defs_req, sizeof(mon_defs_req)))
		return -EFAULT;

	return 0;
}

static int fba_mon_data_ioctl(struct fba_device *fba_dev, void __user *data_req)
{
	struct mon_data_ioctl mon_data_req = {}; /* Users's request */
	uint32_t *ids;
	void *data;
	int16_t *errors;
	void __user *ids_buf;
	void __user *data_buf;
	void __user *errors_buf;
	int ret = 0;
	size_t data_size = 0;
	size_t id_size = 0;
	size_t err_size = 0;

	if (copy_from_user(&mon_data_req, data_req, sizeof(mon_data_req)))
		return -EFAULT;
	ids_buf = u64_to_user_ptr(mon_data_req.ids_ptr);
	data_buf = u64_to_user_ptr(mon_data_req.data_ptr);
	errors_buf = u64_to_user_ptr(mon_data_req.errors_ptr);

	if (mon_data_req.id_count == 0)
		return 0;

	id_size = mon_data_req.id_count * sizeof(*ids);
	data_size = mon_data_req.id_count * MON_VALUE_SIZE;
	err_size = mon_data_req.id_count * sizeof(*errors);
	ids = kzalloc(id_size, GFP_KERNEL);
	data = kzalloc(data_size, GFP_KERNEL);
	errors = kzalloc(err_size, GFP_KERNEL);
	if (ids == NULL || data == NULL || errors == NULL) {
		ret = -ENOMEM;
		goto out_free;
	}

	if (copy_from_user(ids, ids_buf, id_size)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (copy_from_user(data, data_buf, data_size)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (copy_from_user(errors, errors_buf, err_size)) {
		ret = -EFAULT;
		goto out_free;
	}

	ret = fba_mon_get_data(fba_dev, ids, mon_data_req.id_count, data,
			       errors);
	if (ret != 0)
		goto out_free;

	if (copy_to_user(ids_buf, ids, id_size)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (copy_to_user(data_buf, data, data_size)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (copy_to_user(errors_buf, errors, err_size)) {
		ret = -EFAULT;
		goto out_free;
	}

out_free:
	kfree(ids);
	kfree(data);
	kfree(errors);

	return ret;
}

static int fba_mon_data_set_ioctl(struct fba_device *fba_dev,
				  void __user *data_req)
{
	struct mon_data_ioctl mon_data_req; /* Users's request */
	uint32_t *ids;
	void *data;
	size_t data_size = 0;
	size_t id_size = 0;
	int max_id_cnt = fba_mon_get_item_cnt(&fba_dev->mon);
	void __user *ids_buf;
	void __user *data_buf;
	int ret = 0;

	if (copy_from_user(&mon_data_req, data_req, sizeof(mon_data_req)))
		return -EFAULT;
	ids_buf = u64_to_user_ptr(mon_data_req.ids_ptr);
	data_buf = u64_to_user_ptr(mon_data_req.data_ptr);

	if (mon_data_req.id_count > max_id_cnt)
		return -EINVAL;

	if (mon_data_req.id_count == 0)
		return 0;

	id_size = mon_data_req.id_count * sizeof(*ids);
	data_size = mon_data_req.id_count * MON_VALUE_SIZE;
	if (data_size > MAX_MON_SET_DATA_SIZE)
		return -EINVAL;

	ids = kzalloc(id_size, GFP_KERNEL);
	data = kzalloc(data_size, GFP_KERNEL);

	if (data == NULL) {
		ret = -ENOMEM;
		goto out_free_bufs;
	}

	if (ids == NULL) {
		ret = -ENOMEM;
		goto out_free_bufs;
	}

	if (copy_from_user(ids, ids_buf, id_size)) {
		ret = -EFAULT;
		goto out_free_bufs;
	}

	if (copy_from_user(data, data_buf, data_size)) {
		ret = -EFAULT;
		goto out_free_bufs;
	}
	ret = fba_mon_set_data(fba_dev, ids, mon_data_req.id_count, data);

out_free_bufs:
	kfree(ids);
	kfree(data);

	return ret;
}

static long fba_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	void __user *data = (void __user *)arg;
	struct fba_ctx *ctx = fp->private_data;
	struct fba_ctx *streaming_ctx, *parent_ctx;
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	long ret;

	/* All IOCTLS take a struct *, so ensure it's non-zero. */
	if (data == NULL)
		return -EINVAL;

	streaming_ctx = ctx;
	parent_ctx = fba_ctx_get_parent(ctx);
	if (parent_ctx)
		ctx = parent_ctx;

	switch (cmd) {
	case FBA_IOCTL_RESOURCE_INFO: {
		struct res_info_ioctl res_req;
		struct resource_info __user *info;

		if (copy_from_user(&res_req, data, sizeof(res_req)))
			return -EFAULT;
		info = u64_to_user_ptr(res_req.info_ptr);
		ret = fba_ioctl_resource_info(fba_dev, info, res_req.offset,
					      res_req.count);
		return ret;
	}
	case FBA_IOCTL_RESOURCE_ALLOC: {
		struct res_alloc_ioctl alloc_req;

		if (copy_from_user(&alloc_req, data, sizeof(alloc_req)))
			return -EFAULT;
		ret = fba_ioctl_resource_alloc(
			ctx, alloc_req.type, alloc_req.subtype, alloc_req.index,
			alloc_req.count, &alloc_req.rid, &alloc_req.start);
		if (ret != 0)
			return ret;
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
		ret = fba_get_dmabuf(ctx, export_p2p_req.source_rid,
				     &export_p2p_req.fd);
		if (ret != 0)
			return ret;
		if (copy_to_user(data, &export_p2p_req, sizeof(export_p2p_req)))
			return -EFAULT;
		return 0;
	}
	case FBA_IOCTL_RESOURCE_FREE:
		return fba_ioctl_resource_free(ctx, arg);
	case FBA_IOCTL_ACCESS_BAR: {
		struct bar_access_ioctl bar_req;

		if (!capable(CAP_SYS_RAWIO))
			return -EPERM;

		if (copy_from_user(&bar_req, data, sizeof(bar_req)))
			return -EFAULT;
		ret = fba_ioctl_bar_access(fba_dev, &bar_req);
		return ret;
	}
	case FBA_IOCTL_POST_ID: {
		struct post_ioctl post_req;

		if (atomic_read(&fba_dev->has_failed))
			return -EIO;

		if (!fba_ctx_is_open(streaming_ctx)) {
			fba_dev_err(fba_dev, "post_id: ctx not open\n");
			return -EACCES;
		}

		if (copy_from_user(&post_req, data, sizeof(post_req))) {
			fba_dev_err(fba_dev,
				    "post_id: copy from user failed\n");
			return -EFAULT;
		}

		ret = fba_ioctl_post_id(streaming_ctx, &post_req);
		return ret;
	}
	case FBA_IOCTL_READ_ID: {
		struct read_ioctl read_req;
		struct result_info __user *results;

		if (atomic_read(&fba_dev->has_failed))
			return -EIO;

		if (copy_from_user(&read_req, data, sizeof(read_req)))
			return -EFAULT;
		results = u64_to_user_ptr(read_req.results_ptr);

		ret = fba_ioctl_read_results(streaming_ctx, read_req.eventfd,
					     results, read_req.count);
		return ret;
	}
	case FBA_IOCTL_GET_DETAILS: {
		struct get_details_ioctl details_req;

		if (copy_from_user(&details_req, data, sizeof(details_req)))
			return -EFAULT;

		ret = fba_ioctl_get_details(fba_dev, &details_req);
		if (ret != 0)
			return ret;

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
		if (atomic_read(&fba_dev->has_failed))
			return -EIO;

		/* No need to set state anymore, but still need side effect */
		ret = fba_ctx_open(ctx);

		return ret;
	case FBA_IOCTL_ACCESS_DMA: {
		struct dma_access_ioctl dma_req;

		if (copy_from_user(&dma_req, data, sizeof(dma_req)))
			return -EFAULT;
		ret = fba_ioctl_access_dma(fba_dev, &dma_req);
		if (ret != 0)
			return ret;
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

		if (atomic_read(&fba_dev->has_failed))
			return -EIO;

		if (copy_from_user(&stream_req, data,
				   sizeof(struct stream_ioctl)))
			return -EFAULT;

		ret = fba_ioctl_stream_create(fba_dev, ctx, &stream_req);
		if (ret == 0) {
			if (copy_to_user(data, &stream_req,
					 sizeof(struct stream_ioctl))) {
				return -EFAULT;
			}
		}
		return ret;
	}
	case FBA_IOCTL_GET_LIMIT_PARAMS: {
		struct fba_limit_params_ioctl limit_params_req;

		if (copy_from_user(&limit_params_req, data,
				   sizeof(limit_params_req)))
			return -EFAULT;

		ret = fba_ioctl_get_limit_params(fba_dev, &limit_params_req);
		if (ret == 0) {
			if (copy_to_user(data, &limit_params_req,
					 sizeof(limit_params_req)))
				return -EFAULT;
		}
		return ret;
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
	int ret;

	ret = fba_ctx_close(ctx);
	fba_ctx_put(ctx);

	return ret;
}

const struct file_operations fba_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fba_ioctl,
	.poll = fba_poll,
	.open = fba_open,
	.release = fba_release,
};
