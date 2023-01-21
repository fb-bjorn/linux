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
