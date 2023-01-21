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

static long fba_ioctl_get_details(struct fba_device *dev,
				  struct get_details_ioctl *detail)
{
	int i;

	memset(detail, 0, sizeof(*detail));
	detail->host_ipc_bar = dev->cfg->host_ipc_bar;
	detail->host_ipc_stride = dev->cfg->host_ipc_stride;
	detail->host_ipc_offset = dev->cfg->host_ipc_offset;
	detail->irq_num = CHANNEL_MSIX_VECTOR_BASE + dev->channel_total;
	for (i = 0; i < BAR_CNT; i++) {
		detail->bar_addrs[i] = (uintptr_t)dev->bars[i];
		detail->bar_sizes[i] = pci_resource_len(dev->pdev, i);
	}
	detail->dma_addr = dev->debug_area.addr;
	detail->dma_size = SAIL_DEBUG_SIZE;
	detail->resource_cnt = dev->resource_cnt;
	detail->context_cnt = dev->n_ctxs;

	detail->state = FBA_ACTIVE;

	detail->context_used = fba_device_get_ctx_count(dev);

	fba_channel_get_fill_watermarks(fba_get_channel(dev,
							SAIL_CHANNEL_ADMIN),
					&detail->admin_sq_fill_watermark,
					&detail->admin_cq_fill_watermark);
	fba_channel_get_fill_watermarks(fba_get_channel(dev, SAIL_CHANNEL_EXE),
					&detail->exe_sq_fill_watermark,
					&detail->exe_cq_fill_watermark);

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

static long fba_ioctl_get_limit_params(struct fba_device *dev,
				       struct fba_limit_params_ioctl *params)
{
	switch (params->channel_type) {
	/* Count the number of user available channels */
	case FBA_CHANNEL_TYPE_CONCURRENT:
		params->max_channels = dev->channel_cnt - SAIL_CHANNEL_EXE;
		params->max_entries_per_packet = dev->channel_max_packet_size;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static long fba_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	void __user *data = (void __user *)arg;
	struct fba_ctx *ctx = fp->private_data;
	struct fba_device *dev = fba_ctx_to_fba_device(ctx);
	long rc;

	// All IOCTLS take a struct *, so ensure it's non-zero.
	if (data == NULL)
		return -EINVAL;

	switch (cmd) {
	case FBA_IOCTL_RESOURCE_INFO: {
		struct res_info_ioctl res_req;

		if (copy_from_user(&res_req, data, sizeof(res_req)))
			return -EFAULT;
		rc = fba_ioctl_resource_info(dev, res_req.info, res_req.offset,
					     res_req.count);
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
	case FBA_IOCTL_RESOURCE_FREE:
		return fba_ioctl_resource_free(ctx, arg);
	case FBA_IOCTL_ACCESS_BAR: {
		struct bar_access_ioctl bar_req;

		if (copy_from_user(&bar_req, data, sizeof(bar_req)))
			return -EFAULT;
		rc = fba_ioctl_bar_access(dev, &bar_req);
		return rc;
	}
	case FBA_IOCTL_GET_DETAILS: {
		struct get_details_ioctl details_req;

		if (copy_from_user(&details_req, data, sizeof(details_req)))
			return -EFAULT;

		rc = fba_ioctl_get_details(dev, &details_req);
		if (rc != 0)
			return rc;

		if (copy_to_user(data, &details_req, sizeof(details_req)))
			return -EFAULT;

		return 0;
	}
	case FBA_IOCTL_SET_STATE:
		/* No need to set state anymore, but still need side effect */
		rc = fba_ctx_open(ctx);

		return rc;
	case FBA_IOCTL_ACCESS_DMA: {
		struct dma_access_ioctl dma_req;

		if (copy_from_user(&dma_req, data, sizeof(dma_req)))
			return -EFAULT;
		rc = fba_ioctl_access_dma(dev, &dma_req);
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
	case FBA_IOCTL_GET_LIMIT_PARAMS: {
		struct fba_limit_params_ioctl limit_params_req;

		if (copy_from_user(&limit_params_req, data,
				   sizeof(limit_params_req)))
			return -EFAULT;

		rc = fba_ioctl_get_limit_params(dev, &limit_params_req);
		if (rc == 0) {
			if (copy_to_user(data, &limit_params_req,
					 sizeof(limit_params_req)))
				return -EFAULT;
		}
		return rc;
	}
	default:
		/* Provide a hook for device-specific ioctl handling */
		if (dev->cfg->impl_ops->priv_ioctl)
			return dev->cfg->impl_ops->priv_ioctl(fp, cmd, arg);
		return -EINVAL;
	}
	return 0;
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

	ctx = fba_ctx_alloc(fba_dev);
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
	.open = fba_open,
	.release = fba_release,
};
