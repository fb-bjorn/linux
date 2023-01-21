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
