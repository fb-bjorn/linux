// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#include <linux/errno.h>
#include <linux/genalloc.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>

#include "fba_device.h"
#include "fba_ctx.h"

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

	fba_ctx_put(ctx);

	return 0;
}

const struct file_operations fba_fops = {
	.owner = THIS_MODULE,
	.open = fba_open,
	.release = fba_release,
};
