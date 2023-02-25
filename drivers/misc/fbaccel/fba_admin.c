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
	uint16_t opcode = le16_to_cpu(sub->header.opcode);

	long duration;
	int ret;

	init_completion(&cb_data.completion);
	fba_channel_init_req(&cb_data.req, sub_cnt, sub, fba_admin_channel_cb);

	ret = fba_channel_post_req(chan, &cb_data.req);
	if (ret < 0)
		return ret;

	duration = msecs_to_jiffies(fba_dev->admin_cmd_timeout_ms);
	ret = wait_for_completion_timeout(&cb_data.completion, duration);
	if (ret == 0) {
		fba_device_abort(fba_dev, "admin packet timeout");
		fba_dev_err(fba_dev,
			    "Timeout waiting for admin packet. opcode=%d.",
			    opcode);
		return -ETIMEDOUT;
	}
	if (cb_data.req.status == CS_CANCEL)
		return -ETIMEDOUT;

	if (!cb_data.completed) {
		fba_dev_err(fba_dev, "Admin command did not complete.");
		return ret;
	}

	*status = cb_data.req.status;
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

int fba_admin_create_context(struct fba_device *fba_dev, int id)
{
	struct fba_submission sub;
	uint16_t status;
	int ret;

	memset(&sub, 0, sizeof(sub));
	sub.header.opcode = cpu_to_le16(ADMIN_CREATE_CONTEXT);
	sub.ctx.id = id;

	ret = fba_admin_packet_sync(fba_dev, &sub, 1, &status);
	if (ret) {
		fba_dev_err(fba_dev, "ADMIN_CREATE_CONTEXT failed %d.", ret);
		return ret;
	}
	if (status != 0) {
		fba_dev_err(fba_dev,
			    "Failed to create ctx on device, status=%d.",
			    status);
		fba_device_abort(fba_dev, "create context failed");
		return -EIO;
	}

	return 0;
}

int fba_admin_delete_context_begin(struct fba_device *fba_dev, int id)
{
	struct fba_submission sub;
	uint16_t status;
	int ret;

	memset(&sub, 0, sizeof(sub));
	sub.header.opcode = cpu_to_le16(ADMIN_DELETE_CONTEXT_BEGIN);
	sub.ctx.id = id;

	ret = fba_admin_packet_sync(fba_dev, &sub, 1, &status);
	if (ret) {
		fba_dev_err(fba_dev, "%s: failed rc %d", __func__, ret);
		return ret;
	}
	if (status != 0) {
		fba_dev_err(
			fba_dev,
			"Failed to delete ctx begin id=%d on device, status=%d.",
			id, status);
		fba_device_abort(fba_dev, "delete context failed");
		return -EIO;
	}

	return 0;
}

int fba_admin_delete_context_end(struct fba_device *fba_dev, int id)
{
	struct fba_submission sub;
	uint16_t status;
	int ret;

	memset(&sub, 0, sizeof(sub));
	sub.header.opcode = cpu_to_le16(ADMIN_DELETE_CONTEXT_END);
	sub.ctx.id = id;

	ret = fba_admin_packet_sync(fba_dev, &sub, 1, &status);
	if (ret) {
		fba_dev_err(fba_dev, "%s: failed rc %d", __func__, ret);
		return ret;
	}
	if (status != 0) {
		fba_dev_err(
			fba_dev,
			"Failed to delete ctx end id=%d on device, status=%d.",
			id, status);
		fba_device_abort(fba_dev, "delete context failed.");
		return -EIO;
	}

	return 0;
}

int fba_admin_get_cfg_data(struct fba_device *fba_dev, dma_addr_t addr,
			   uint16_t opcode, uint64_t start, uint64_t end)
{
	struct fba_submission sub;
	uint16_t status;
	int ret;

	memset(&sub, 0, sizeof(sub));

	sub.header.command_id = 0;
	sub.header.opcode = cpu_to_le16(opcode);
	sub.header.flags = 0;
	sub.header.packet_tail = 0;
	sub.header.context_id = 0;
	sub.info_words.host_addr = cpu_to_le64(addr);
	sub.info_words.first_word = cpu_to_le32(start);
	sub.info_words.num_words = cpu_to_le32(end);

	ret = fba_admin_packet_sync(fba_dev, &sub, 1, &status);
	if (ret != 0) {
		fba_dev_err(fba_dev, "Failed to %d get cfg data. rc=%d", opcode,
			    ret);
	} else if (status != CS_OK) {
		fba_dev_err(fba_dev,
			    "Failed to %d get cfg data. rc=%d, status=%u",
			    opcode, ret, status);
		ret = -EIO;
	}

	return ret;
}

int fba_admin_dma_from_device(struct fba_device *fba_dev, uint16_t opcode,
			      void *dst, uint32_t size)
{
	struct fba_submission sub = {};
	uint16_t status;
	struct device *dev = &fba_dev->pdev->dev;
	dma_addr_t pa_dma;
	void *va_dma;
	int ret = 0;

	va_dma = dma_alloc_coherent(dev, size, &pa_dma, GFP_KERNEL);
	if (va_dma == NULL)
		return -ENOMEM;

	sub.header.opcode = cpu_to_le16(opcode);
	sub.va_dma.host_addr = cpu_to_le64(pa_dma);
	sub.va_dma.flags_size = cpu_to_le64(size);

	ret = fba_admin_packet_sync(fba_dev, &sub, 1, &status);

	if (ret == 0) {
		memcpy(dst, va_dma, size);
	} else {
		fba_dev_err(
			fba_dev,
			"%s: opcode %d failed to read from device: dst=%p size=%u err=%d.",
			__func__, opcode, dst, size, ret);
	}

	dma_free_coherent(dev, size, va_dma, pa_dma);

	return ret;
}

int fba_admin_dma_to_device(struct fba_device *fba_dev, uint16_t opcode,
			    void *dst, uint32_t size)
{
	struct fba_submission sub = {
		.header = { .opcode = cpu_to_le16(opcode) },
		.va_dma = { .flags_size = cpu_to_le64(size | AT_DMA_OPSZ |
					  AT_DMA_TO_DEVICE), },
	};
	struct device *dev = &fba_dev->pdev->dev;
	uint16_t status;
	void *va_dma;
	dma_addr_t host_addr;
	int ret;

	va_dma = dma_alloc_coherent(dev, size, &host_addr, GFP_KERNEL);
	if (va_dma == NULL)
		return -ENOMEM;
	sub.va_dma.host_addr = cpu_to_le64(host_addr);

	memcpy(va_dma, dst, size);

	ret = fba_admin_packet_sync(fba_dev, &sub, 1, &status);

	if (ret != 0) {
		fba_dev_err(
			fba_dev,
			"%s: opcode %d failed to write to device: dst=%p size=%u err=%d.",
			__func__, opcode, dst, size, ret);
	}

	dma_free_coherent(dev, size, va_dma, host_addr);

	return ret;
}

int fba_admin_time_set(struct fba_device *fba_dev, int64_t time)
{
	struct fba_submission sub = {
		.header = { .opcode = cpu_to_le16(ADMIN_TIME_SET) },
	};
	uint16_t status;
	int32_t rem;
	int ret;

	sub.time_spec.tv_sec =
		cpu_to_le64(div_s64_rem(time, NSEC_PER_SEC, &rem));
	sub.time_spec.tv_nsec = cpu_to_le64(rem);

	ret = fba_admin_packet_sync(fba_dev, &sub, 1, &status);
	if (ret || status) {
		fba_dev_err(fba_dev, "ADMIN_TIME_SET failed rc=%d status=%d",
			    ret, status);
		if (!ret)
			ret = -EIO;
	}
	trace_fba_time_sync(fba_dev->name, le64_to_cpu(sub.time_spec.tv_sec),
			    le64_to_cpu(sub.time_spec.tv_nsec));

	return ret;
}

bool fba_admin_poll_channel(void *obj)
{
	struct fba_channel *chan = (struct fba_channel *)obj;
	struct fba_device *fba_dev = chan->fba_dev;
	uint16_t status = 0;
	int ret;

	if (chan == fba_get_channel(fba_dev, SAIL_CHANNEL_ADMIN)) {
		ret = fba_admin_time_set(fba_dev, ktime_get_real());
	} else {
		struct fba_submission sub = {
			.header.opcode = cpu_to_le16(EXE_NOOP),
		};

		ret = fba_admin_packet_chan_sync(fba_dev, chan, &sub, 1,
						 &status);
	}

	if (ret || status) {
		fba_device_abort(fba_dev, "heartbeat failure");
		fba_dev_err(fba_dev,
			    "Heartbeat failed for channel %d rc=%d status=%d",
			    (uint16_t)chan->chan_id, ret, status);
		fba_dev->heart_beat_failed = true;
		return false;
	}

	return true;
}
