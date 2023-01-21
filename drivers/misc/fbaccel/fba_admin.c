// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
/* FBA Admin functions */
#include <linux/device.h>
#include <linux/types.h>

#include "fba_device.h"
#include "fba_admin.h"

struct fba_admin_cb_data {
	struct fba_channel_req req;

	struct completion completion;
	bool completed;
};

static void fba_admin_channel_cb(bool completed, struct fba_channel_req *req)
{
	struct fba_admin_cb_data *cb_data =
		container_of(req, struct fba_admin_cb_data, req);

	cb_data->completed = completed;
	complete(&cb_data->completion);
}

static int fba_admin_packet_chan_sync(struct fba_device *fba_dev,
				      struct fba_channel *chan,
				      struct fba_submission *sub,
				      uint32_t sub_cnt, uint16_t *status)
{
	struct fba_admin_cb_data cb_data = { 0 };
	uint16_t opcode = sub->header.opcode;

	long duration;
	int rc;

	init_completion(&cb_data.completion);
	fba_channel_init_req(&cb_data.req, sub_cnt, sub, fba_admin_channel_cb);

	rc = fba_channel_post_req(chan, &cb_data.req);
	if (rc < 0)
		return rc;

	duration = msecs_to_jiffies(fba_dev->admin_cmd_timeout_ms);
	rc = wait_for_completion_timeout(&cb_data.completion, duration);
	if (rc == 0) {
		fba_device_abort(fba_dev, "admin packet timeout");
		fba_dev_err(fba_dev,
			    "Timeout waiting for admin packet. opcode=%d.",
			    opcode);
		return -ETIMEDOUT;
	}
	if (cb_data.req.cmp.status == CS_CANCEL)
		return -ETIMEDOUT;

	if (!cb_data.completed) {
		fba_dev_err(fba_dev, "Admin command did not complete.");
		return rc;
	}

	*status = cb_data.req.cmp.status;
	fba_dev_dbg(fba_dev, "Admin packet completed. opcode %d status %d.",
		    opcode, *status);
	return 0;
}

static int fba_admin_packet_sync(struct fba_device *fba_dev,
				 struct fba_submission *sub, uint32_t sub_cnt,
				 uint16_t *stat)
{
	return fba_admin_packet_chan_sync(
		fba_dev, fba_get_channel(fba_dev, SAIL_CHANNEL_ADMIN), sub,
		sub_cnt, stat);
}

int fba_admin_create_context(struct fba_device *dev, int id)
{
	struct fba_submission sub;
	uint16_t status;
	int rc;

	memset(&sub, 0, sizeof(sub));
	sub.header.opcode = ADMIN_CREATE_CONTEXT;
	sub.ctx.id = id;

	rc = fba_admin_packet_sync(dev, &sub, 1, &status);
	if (rc) {
		fba_dev_err(dev, "ADMIN_CREATE_CONTEXT failed %d.", rc);
		return rc;
	}
	if (status != 0) {
		fba_dev_err(dev, "Failed to create ctx on device, status=%d.",
			    status);
		fba_device_abort(dev, "create context failed");
		return -EIO;
	}

	return 0;
}

int fba_admin_delete_context_begin(struct fba_device *dev, int id)
{
	struct fba_submission sub;
	uint16_t status;
	int rc;

	memset(&sub, 0, sizeof(sub));
	sub.header.opcode = ADMIN_DELETE_CONTEXT_BEGIN;
	sub.ctx.id = id;

	rc = fba_admin_packet_sync(dev, &sub, 1, &status);
	if (rc) {
		fba_dev_err(dev, "%s: failed rc %d", __func__, rc);
		return rc;
	}
	if (status != 0) {
		fba_dev_err(
			dev,
			"Failed to delete ctx begin id=%d on device, status=%d.",
			id, status);
		fba_device_abort(dev, "delete context failed");
		return -EIO;
	}

	return 0;
}

int fba_admin_delete_context_end(struct fba_device *dev, int id)
{
	struct fba_submission sub;
	uint16_t status;
	int rc;

	memset(&sub, 0, sizeof(sub));
	sub.header.opcode = ADMIN_DELETE_CONTEXT_END;
	sub.ctx.id = id;

	rc = fba_admin_packet_sync(dev, &sub, 1, &status);
	if (rc) {
		fba_dev_err(dev, "%s: failed rc %d", __func__, rc);
		return rc;
	}
	if (status != 0) {
		fba_dev_err(
			dev,
			"Failed to delete ctx end id=%d on device, status=%d.",
			id, status);
		fba_device_abort(dev, "delete context failed.");
		return -EIO;
	}

	return 0;
}

int fba_admin_get_cfg_data(struct fba_device *fba_dev, dma_addr_t addr,
			   uint16_t opcode, uint64_t start, uint64_t end)
{
	struct fba_submission sub;
	uint16_t status;
	int rc;

	memset(&sub, 0, sizeof(sub));

	sub.header.command_id = 0;
	sub.header.opcode = opcode;
	sub.header.flags = 0;
	sub.header.packet_tail = 0;
	sub.header.context_id = 0;
	sub.info_words.host_addr = addr;
	sub.info_words.first_word = start;
	sub.info_words.num_words = end;

	rc = fba_admin_packet_sync(fba_dev, &sub, 1, &status);
	if (rc != 0) {
		fba_dev_err(fba_dev, "Failed to %d get cfg data. rc=%d", opcode,
			    rc);
	} else if (status != CS_OK) {
		fba_dev_err(fba_dev,
			    "Failed to %d get cfg data. rc=%d, status=%u",
			    opcode, rc, status);
		rc = -EIO;
	}

	return rc;
}

int fba_admin_dma_from_device(struct fba_device *fba_dev, uint16_t opcode,
			      void *dst, uint32_t size)
{
	struct fba_submission sub = {};
	uint16_t status;
	struct device *dev = &fba_dev->pdev->dev;
	dma_addr_t pa_dma;
	void *va_dma;
	int err = 0;

	va_dma = dma_alloc_coherent(dev, size, &pa_dma, GFP_KERNEL);
	if (va_dma == NULL)
		return -ENOMEM;

	sub.header.opcode = opcode;
	sub.va_dma.host_addr = pa_dma;
	sub.va_dma.flags_size = size;

	err = fba_admin_packet_sync(fba_dev, &sub, 1, &status);

	if (err == 0) {
		memcpy(dst, va_dma, size);
	} else {
		fba_dev_err(
			fba_dev,
			"%s: opcode %d failed to read from device: dst=%p size=%u err=%d.",
			__func__, opcode, dst, size, err);
	}

	dma_free_coherent(dev, size, va_dma, pa_dma);

	return err;
}

int fba_admin_dma_to_device(struct fba_device *fba_dev, uint16_t opcode,
			    void *dst, uint32_t size)
{
	struct fba_submission sub = {
		.header = { .opcode = opcode },
		.va_dma = { .flags_size = size | AT_DMA_OPSZ |
				      AT_DMA_TO_DEVICE, },
	};
	struct device *dev = &fba_dev->pdev->dev;
	uint16_t status;
	void *va_dma;
	dma_addr_t host_addr;
	int err;

	va_dma = dma_alloc_coherent(dev, size, &host_addr, GFP_KERNEL);
	if (va_dma == NULL)
		return -ENOMEM;
	sub.va_dma.host_addr = host_addr;

	memcpy(va_dma, dst, size);

	err = fba_admin_packet_sync(fba_dev, &sub, 1, &status);

	if (err != 0) {
		fba_dev_err(
			fba_dev,
			"%s: opcode %d failed to write to device: dst=%p size=%u err=%d.",
			__func__, opcode, dst, size, err);
	}

	dma_free_coherent(dev, size, va_dma, host_addr);

	return err;
}

int fba_admin_time_set(struct fba_device *fba_dev, int64_t time)
{
	struct fba_submission sub = {
		.header = { .opcode = ADMIN_TIME_SET },
	};
	uint16_t status;
	int32_t rem;
	int rc;

	sub.time_spec.tv_sec = div_s64_rem(time, NSEC_PER_SEC, &rem);
	sub.time_spec.tv_nsec = rem;

	rc = fba_admin_packet_sync(fba_dev, &sub, 1, &status);
	if (rc || status) {
		fba_dev_err(fba_dev, "ADMIN_TIME_SET failed rc=%d status=%d",
			    rc, status);
		if (!rc)
			rc = -EIO;
	}

	return rc;
}

bool fba_admin_poll_channel(void *obj)
{
	struct fba_channel *chan = (struct fba_channel *)obj;
	struct fba_device *dev = chan->dev;
	uint16_t status = 0;
	int rc;

	if (chan == fba_get_channel(dev, SAIL_CHANNEL_ADMIN)) {
		rc = fba_admin_time_set(dev, ktime_get_real());
	} else {
		struct fba_submission sub = {
			.header.opcode = EXE_NOOP,
		};

		rc = fba_admin_packet_chan_sync(dev, chan, &sub, 1, &status);
	}

	if (rc || status) {
		fba_device_abort(dev, "heartbeat failure");
		fba_dev_err(dev,
			    "Heartbeat failed for channel %d rc=%d status=%d",
			    (uint16_t)chan->chan_id, rc, status);
		dev->heart_beat_failed = true;
		return false;
	}

	return true;
}
