// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/eventfd.h>
#include <linux/idr.h>

#include "fba_device.h"
#include "fba_admin.h"

static unsigned int chan_hb_period_ms = CHAN_HEARTBEAT_PERIOD_MS;
module_param(chan_hb_period_ms, int, 0644);
MODULE_PARM_DESC(
	chan_hb_period_ms,
	"SAIL Channel heartbeat polling period in milliseconds (default: 60000). "
	"Set to 0 to disable.");

/*
 * The primary host/device interface consists of a set of independent
 * communication channels. Each channel has its own submission queue (squeue or
 * sq) for work submitted by the host, and its own completion queue (cqueue or
 * cq) for completions returned by the device.
 *
 * Each of these queues have a head and tail pointer, which provide an index
 * into the queue's ring buffer. The producer (which would be the host for the
 * submission queue, and the device for the completion queue) adds the next
 * entry at the tail, and the consumer (which would be the device for the
 * submission queue, and the host for the completion queue) reads next entry
 * from the head.
 *
 * The host writes to device mailboxes to signal when new entries are available
 * in the submission queue.  The device signals that it has consumed submission
 * queue entries by updating the sq_head field in returned completions.
 *
 * The device signals that there are new completion queue entries by updating
 * the phase tag in the entries themselves and then signalling an interrupt to
 * the host.  The host writes to another device mailbox to signal when it has
 * consumed completion queue entries from the device.
 *
 * A set of submission entries are provided to the device in one batch, and the
 * whole batch is associated with a single command ID assigned at this layer.
 * The device generates a single completion for that batch of submission
 * entries which includes the matching command ID.
 */

static uint32_t queue_get_used(struct fba_queue *queue)
{
	if (queue->tail >= queue->head)
		return queue->tail - queue->head;

	return (queue->entries - queue->head) + queue->tail;
}

static uint32_t queue_get_space(struct fba_queue *queue)
{
	/*
	 * Capacity of the queue will always be queue->entries-1. We can't let the
	 * queue hold queue->entries items, because it would make the queue empty
	 * and full conditions be the same.
	 */
	return (queue->entries - 1) - queue_get_used(queue);
}

static void queue_write_entries(struct fba_queue *queue,
				struct fba_submission *entries, size_t count,
				uint16_t cmd_id)
{
	uint32_t used;
	size_t i, entry_size;

	entry_size = sizeof(struct fba_submission);

	for (i = 0; i < count; i++) {
		entries[i].header.command_id = cpu_to_le16(cmd_id);
		fba_shared_memcpy_to(queue->area,
				     queue->offset + queue->tail * entry_size,
				     &entries[i], entry_size);
		queue->tail = (queue->tail + 1) % queue->entries;
	}

	used = queue_get_used(queue);
	if (used > queue->fill_watermark)
		queue->fill_watermark = used;
}

/* Signal to the device that new submission queue entries are available */
static void fba_channel_set_squeue_tail(struct fba_device *fba_dev, int chan,
					uint32_t new_tail)
{
	/* Submission doorbells are even mailboxes. */
	set_mailbox(fba_dev, (chan * 2) + fba_dev->cfg->db_mbox_base + 0,
		    new_tail);
}

/* Signal to the device that we have consumed completion queue entries */
static void fba_channel_set_cqueue_head(struct fba_device *fba_dev, int chan,
					uint32_t new_head)
{
	/* Completion doorbells are odd mailboxes. */
	set_mailbox(fba_dev, (chan * 2) + fba_dev->cfg->db_mbox_base + 1,
		    new_head);
}

#define MAX_CMD_ID 0xFFFF

/* Allocate and return a cmd_id in *cmd_id.
 *
 * Return: 0 on success, -ENOMEM if memory allocation failed, or -ENOSPC if
 * we run out of ids.
 */
static int fba_channel_alloc_cmd_id(struct fba_channel *chan,
				    struct fba_channel_req *req)
{
	int ret;

	WARN_ON(!mutex_is_locked(&chan->pending_mutex));

	ret = idr_alloc(&chan->cmd_ids, req, 1, MAX_CMD_ID, GFP_KERNEL);
	if (ret < 0)
		return ret;

	req->cmd_id = ret;
	return 0;
}

/*
 * Try to fill the queue with the head of pending_reqs.  Returns false if
 * nothing left to do.
 */
static bool fba_channel_fill_head(struct fba_channel *chan)
{
	struct fba_channel_req *req;
	struct fba_device *fba_dev = chan->fba_dev;
	struct fba_queue *sq = &chan->squeue;
	uint32_t space, head, tail;
	int ret;

	WARN_ON(!mutex_is_locked(&chan->pending_mutex));

	req = list_first_entry_or_null(&chan->pending_reqs,
				       struct fba_channel_req, req_item);
	if (req == NULL)
		return false;

	if (req->cmd_id == 0) {
		/* We didn't have a cmd_id before, but can do it now */
		ret = fba_channel_alloc_cmd_id(chan, req);
		if (ret < 0) {
			/* No such luck, will need to try again later */
			return false;
		}
	}

	spin_lock(&chan->lock);
	if (chan->stopped) {
		spin_unlock(&chan->lock);
		return false;
	}

	/* Check if the submission queue has enough space. */
	space = queue_get_space(sq);
	if (req->sub_cnt > space) {
		/* Not enough space in submission queue yet, bail */
		spin_unlock(&chan->lock);
		return false;
	}

	if (fba_util_inc_usage(fba_dev, chan->limits, &chan->usage,
			       req->sub_cnt) < 0) {
		/* We have too many outstanding submission entries already */
		spin_unlock(&chan->lock);
		return false;
	}

	/* Good to go, remove from pending_reqs list */
	list_del(&req->req_item);

	/* Mark that it is no longer our responsibility to free. */
	req->posted = true;

	/* We are now ready to enqueue the item. */
	queue_write_entries(sq, req->subs, req->sub_cnt, req->cmd_id);
	head = sq->head;
	tail = sq->tail;
	fba_channel_set_squeue_tail(fba_dev, chan->chan_id, tail);
	spin_unlock(&chan->lock);

	trace_fba_channel_post(fba_dev->name, chan->chan_id, req->cmd_id,
			       req->sub_cnt);
	if (trace_fba_channel_post_sub_enabled()) {
		int i;

		for (i = 0; i < req->sub_cnt; i++)
			trace_fba_channel_post_sub(
				fba_dev->name, chan->chan_id, req->cmd_id,
				le16_to_cpu(req->subs[i].header.opcode));
	}

	fba_dev_dbg(
		fba_dev,
		"Added %d items for packet %d to channel %d tail=%d head=%d.",
		req->sub_cnt, req->cmd_id, chan->chan_id, tail, head);

	return list_empty(&chan->pending_reqs);
}

/* Try to fill submission queue with pending reqs */
static void fba_channel_fill_queue(struct fba_channel *chan)
{
	mutex_lock(&chan->pending_mutex);
	while (fba_channel_fill_head(chan)) {
		/*
		 * Loop until we run out of SQ space or pending reqs.
		 * Each loop will attempt to submit the next req.
		 */
	}
	mutex_unlock(&chan->pending_mutex);
}

void fba_channel_init_req(struct fba_channel_req *req, size_t n_subs,
			  struct fba_submission *subs, fba_channel_cb_fn cb)
{
	req->sub_cnt = n_subs;
	req->subs = subs;
	req->cb = cb;
	INIT_LIST_HEAD(&req->req_item);

	/*
	 * Initialize the result to failed. This will be updated by
	 * the FW to the appropriate value when packet is completed.
	 */
	req->status = CS_FAILED;
}

int fba_channel_post_req(struct fba_channel *chan, struct fba_channel_req *req)
{
	bool was_empty;
	int ret = 0;

	mutex_lock(&chan->pending_mutex);
	spin_lock(&chan->lock);

	/* If we lost a race with stopping device, bail here. */
	if (chan->stopped) {
		spin_unlock(&chan->lock);
		fba_dev_err(chan->fba_dev, "channel stopped.");
		ret = -EIO;
		goto out_unlock_pending;
	}

	/*
	 * Double check that req fits in channel.  Job layer should never send
	 * us something too large, but we'd try forever to submit if that
	 * happens. So good to double check here as well.
	 */
	if (req->sub_cnt > chan->max_job_subs) {
		spin_unlock(&chan->lock);
		fba_dev_err(
			chan->fba_dev,
			"job exceeds size of channel (sub_cnt=%u, max=%llu)",
			req->sub_cnt, chan->max_job_subs);
		ret = -EINVAL;
		goto out_unlock_pending;
	}

	was_empty = list_empty(&chan->pending_reqs);
	list_add_tail(&req->req_item, &chan->pending_reqs);

	spin_unlock(&chan->lock);

	if (was_empty) {
		/*
		 * New job is head of queue, try to pre-allocate cmd_id here in
		 * submitter's context.  If allocation succeeds, send it off to
		 * the device.
		 */
		if (fba_channel_alloc_cmd_id(chan, req) == 0) {
			/* If we got a cmd_id, we can try to submit it to device now. */
			(void)fba_channel_fill_head(chan);
		}
	}

out_unlock_pending:
	mutex_unlock(&chan->pending_mutex);

	return ret;
}

/* XXX - add to constants.h? */
#define PACKET_FAIL (1 << 7)

/*
 * Purge all packets from a channel. This is in place of
 * channel packet completions if FW went down.
 */
static void fba_channel_purge_pending(struct fba_channel *chan)
{
	struct fba_channel_req *req;
	int cmd_id = 0;

	/*
	 * To avoid deadlocks, we need to walk through allocated cmd_ids
	 * carefully and avoid holding pending_mutex while calling the callback
	 * function.  For example, a thread posting a request may hold the same
	 * context mutex that a callback would attempt to acquire but be
	 * blocked waiting for pending_mutex.
	 */
	while (true) {
		mutex_lock(&chan->pending_mutex);

		req = idr_get_next(&chan->cmd_ids, &cmd_id);
		if (req == NULL) {
			/* Nothing more to purge from idr */
			mutex_unlock(&chan->pending_mutex);
			break;
		}
		cmd_id++;

		spin_lock(&chan->lock);
		if (req->posted) {
			fba_util_dec_usage(chan->fba_dev, &chan->usage,
					   req->sub_cnt);
		} else {
			/*
			 * Request is pending and assigned a cmd_id but never
			 * submitted to device.  We need to remove from
			 * pending_reqs list still.
			 */

			list_del(&req->req_item);
		}
		spin_unlock(&chan->lock);

		idr_remove(&chan->cmd_ids, req->cmd_id);
		mutex_unlock(&chan->pending_mutex);

		req->status = CS_ABORT;
		req->flags = PACKET_FAIL;
		req->cb(false, req);
	}

	/* Clean up any pending reqs that were never assigned a cmd_id */
	while (true) {
		mutex_lock(&chan->pending_mutex);

		req = list_first_entry_or_null(
			&chan->pending_reqs, struct fba_channel_req, req_item);
		if (req == NULL) {
			/* Nothing more to purge */
			mutex_unlock(&chan->pending_mutex);
			return;
		}

		list_del(&req->req_item);
		mutex_unlock(&chan->pending_mutex);

		req->status = CS_ABORT;
		req->flags = PACKET_FAIL;
		req->cb(false, req);
	}
}

void fba_channel_shutdown(struct fba_channel *chan)
{
	spin_lock(&chan->lock);
	chan->stopped = true;
	spin_unlock(&chan->lock);

	/*
	 * In the normal shutdown case, there should be no packets outstanding.
	 * For failure case, we may have packets stuck waiting for completion.
	 * Complete any such packets with failure.
	 */
	fba_channel_purge_pending(chan);
}

/* Complete the request matching provided cmp.  Return true if found */
static bool fba_channel_complete(struct fba_channel *chan,
				 struct fba_completion *cmp)
{
	struct fba_channel_req *req;
	uint16_t command_id;
	uint16_t status;

	command_id = le16_to_cpu(cmp->command_id);
	status = le16_to_cpu(cmp->status);

	mutex_lock(&chan->pending_mutex);
	req = idr_find(&chan->cmd_ids, command_id);
	if (req != NULL)
		idr_remove(&chan->cmd_ids, command_id);
	mutex_unlock(&chan->pending_mutex);

	if (req == NULL) {
		if (status != CS_CANCEL /* XXX - and !chan->stopped */)
			fba_dev_err(chan->fba_dev,
				    "Packet not found for completion ID=%04Xh.",
				    command_id);
		return false;
	}

	spin_lock(&chan->lock);
	fba_util_dec_usage(chan->fba_dev, &chan->usage, req->sub_cnt);
	spin_unlock(&chan->lock);
	req->status = status;
	req->flags = cmp->flags;
	req->cb(true, req);

	/* Possibly we can now submit more pending reqs */
	fba_channel_fill_queue(chan);

	return true;
}

/*
 * Fetch the next available completion into the provided completion entry.
 * Returns the position of the completion queue head if one is available,
 * -ENOENT if no new completion is available, or -EIO if invalid pointer
 *  updates are detected.
 */
static int get_completion(struct fba_channel *chan, struct fba_completion *cmp)
{
	uint32_t sq_space_before, sq_space_after;
	struct fba_completion *cmp_array =
		chan->cqueue.area->data + chan->cqueue.offset;
	struct fba_completion *tmp = NULL;
	int ret;

	spin_lock(&chan->lock);

	tmp = &cmp_array[chan->cqueue.head % chan->cqueue.entries];
	if (tmp->phase_tag != chan->phase) {
		ret = -ENOENT;
		goto out_unlock;
	}

	chan->cqueue.head++;
	if (chan->cqueue.head == chan->cqueue.entries) {
		chan->phase++;
		chan->cqueue.head = 0;
	}

	/*
	 * Update channel with new device reported submission head.  Since the
	 * reported head is 16b not 32b, we need to handle roll over.
	 */
	sq_space_before = queue_get_space(&chan->squeue);
	chan->squeue.head = le16_to_cpu(tmp->sq_head);
	sq_space_after = queue_get_space(&chan->squeue);

	/*
	 * When the device moves the SQ head (consumes submission entries),
	 * check that the free space did not decrease.
	 */
	if (sq_space_after < sq_space_before) {
		/* Device has given us an invalid sq_head value. */
		spin_unlock(&chan->lock);
		fba_device_abort(chan->fba_dev, "invalid sq_head");
		return -EIO;
	}

	*cmp = *tmp;
	ret = chan->cqueue.head;

out_unlock:
	spin_unlock(&chan->lock);
	return ret;
}

bool fba_channel_has_space(struct fba_channel *chan)
{
	bool has_space = false;

	spin_lock(&chan->lock);
	has_space = (queue_get_space(&chan->squeue) > 0);
	/* XXX - check other limits? */
	spin_unlock(&chan->lock);

	return has_space;
}

uint64_t fba_channel_max_job_subs(struct fba_channel *chan)
{
	return chan->max_job_subs;
}

static int fba_channel_handle_completions(struct fba_channel *chan)
{
	int ncmp = 0;
	int cqueue_head = -1;
	struct fba_device *fba_dev = chan->fba_dev;

	while (1) {
		struct fba_completion cmp;
		int ret;

		ret = get_completion(chan, &cmp);
		if (ret < 0)
			break;

		trace_fba_channel_done(fba_dev->name, chan->chan_id,
				       le16_to_cpu(cmp.command_id), ret);

		cqueue_head = ret;
		ncmp++;

		(void)fba_channel_complete(chan, &cmp);
	}

	if (ncmp > 0) {
		/*
		 * Every time the while loop above runs, it drains everything
		 * it available out of the completion queue. Therefore, the
		 * amount that it drained is equal to the cqueue fill level.
		 */
		spin_lock(&chan->lock);
		if (ncmp > chan->cqueue.fill_watermark)
			chan->cqueue.fill_watermark = ncmp;
		spin_unlock(&chan->lock);
		/* Let the device know we've drained the completion queue. */
		fba_channel_set_cqueue_head(fba_dev, chan->chan_id,
					    cqueue_head);
		/*
		 * Signal to any process waiting on this read wait queue that
		 * there is data available to read from this device.
		 */
		wake_up_interruptible(&fba_dev->read_q);
		/* If there's space in the squeue then wake up any process waiting on this. */
		if (fba_channel_has_space(chan))
			wake_up_interruptible(&fba_dev->write_q);
	}

	return ncmp;
}

static void work_handler(struct work_struct *work)
{
	struct fba_channel *chan = container_of(work, struct fba_channel, work);
	int cnt = fba_channel_handle_completions(chan);

	fba_dev_dbg(chan->fba_dev, "Channel %d handled %d packets.",
		    chan->chan_id, cnt);
}

static irqreturn_t fba_channel_isr(int irq, void *data)
{
	struct fba_channel *chan = data;

	queue_work(chan->fba_dev->msix_wq, &chan->work);

	return IRQ_HANDLED;
}

void fba_channel_get_fill_watermarks(struct fba_channel *chan,
				     uint32_t *sq_watermark,
				     uint32_t *cq_watermark)
{
	spin_lock(&chan->lock);
	*sq_watermark = chan->squeue.fill_watermark;
	*cq_watermark = chan->cqueue.fill_watermark;
	spin_unlock(&chan->lock);
}

void fba_channel_start_heartbeats(struct fba_device *fba_dev)
{
	struct fba_channel *chan;
	int i, max_channel;

	/* Start monitoring active channels */
	max_channel =
		min((uint32_t)(SAIL_CHANNEL_EXE + 1), fba_dev->channel_cnt);
	for (i = 0; i < max_channel; i++) {
		chan = fba_get_channel(fba_dev, i);
		fba_poller_start(&chan->poller);
		fba_dev_dbg(fba_dev, "Channel %d monitored with heartbeat", i);
	}
}

void fba_channel_stop_heartbeats(struct fba_device *fba_dev)
{
	int chan_num;
	struct fba_channel *chan;
	uint32_t max_monitored_channel =
		min((uint32_t)(SAIL_CHANNEL_EXE + 1), fba_dev->channel_cnt);

	for (chan_num = 0; chan_num < max_monitored_channel; chan_num++) {
		chan = fba_get_channel(fba_dev, chan_num);
		fba_poller_stop(&chan->poller);
		fba_dev_info(fba_dev, "Channel %d heartbeat stopped", chan_num);
	}
}

void fba_channel_set_hb_mon_period(void *obj)
{
	struct fba_channel *chan = obj;

	if (chan_hb_period_ms == 0) {
		/*
		 * Go back to default polling period, disable the
		 * monitor from sending the heartbeat
		 */
		chan->poller.period_ms = CHAN_HEARTBEAT_PERIOD_MS;
		if (chan->poller.enabled)
			fba_dev_err(chan->fba_dev,
				    "HB now disabled for channel %d",
				    (uint32_t)chan->chan_id);
		chan->poller.enabled = false;
	} else {
		chan->poller.period_ms = chan_hb_period_ms;
		if (!chan->poller.enabled)
			fba_dev_info(
				chan->fba_dev,
				"HB now enabled for channel %d period %d ms",
				(uint32_t)chan->chan_id, chan_hb_period_ms);
		chan->poller.enabled = true;
	}
}

/* helper function to init queue from provided queue info */
static void setup_queue(struct fba_queue *queue, struct fba_queue_info *qinfo,
			size_t entry_size)

{
	queue->head = 0;
	queue->tail = 0;
	queue->fill_watermark = 0;
	queue->offset = qinfo->offset;
	queue->area = qinfo->area;
	queue->entries = (qinfo->size / entry_size);
}

/* Initialize a channel */
int fba_channel_init(struct fba_device *fba_dev, int id, unsigned int irq,
		     struct fba_queue_info *sqinfo,
		     struct fba_queue_info *cqinfo, struct fba_limits *limits,
		     uint64_t max_job_subs)
{
	struct fba_channel *chan = fba_get_channel(fba_dev, id);
	uint32_t max_monitored_channel =
		min((uint32_t)(SAIL_CHANNEL_EXE + 1), fba_dev->channel_cnt);
	int ret;

	chan->fba_dev = fba_dev;
	chan->chan_id = id;
	chan->irq = irq;
	chan->limits = limits;
	chan->max_job_subs = max_job_subs;
	spin_lock_init(&chan->lock);
	mutex_init(&chan->pending_mutex);
	INIT_LIST_HEAD(&chan->pending_reqs);
	idr_init(&chan->cmd_ids);

	setup_queue(&chan->squeue, sqinfo, sizeof(struct fba_submission));
	setup_queue(&chan->cqueue, cqinfo, sizeof(struct fba_completion));

	chan->phase = 1;
	INIT_WORK(&chan->work, work_handler);

	if (id == SAIL_CHANNEL_ADMIN) {
		ret = fba_request_irq(fba_dev->pdev, irq, fba_channel_isr, NULL,
				      chan, "%s-admin", fba_dev->name);
	} else {
		ret = fba_request_irq(fba_dev->pdev, irq, fba_channel_isr, NULL,
				      chan, "%s-chan%d", fba_dev->name, id);
	}
	if (ret) {
		fba_dev_err(fba_dev, "Failed to request IRQ %d.", irq);
		return ret;
	}

	if (id < max_monitored_channel)
		fba_poller_init(&chan->poller, chan, fba_admin_poll_channel,
				fba_channel_set_hb_mon_period);
	return 0;
}

void fba_channel_exit(struct fba_channel *chan)
{
	struct fba_device *fba_dev = chan->fba_dev;

	fba_free_irq(fba_dev->pdev, chan->irq, chan);
	WARN_ON(!idr_is_empty(&chan->cmd_ids));
	idr_destroy(&chan->cmd_ids);
	mutex_destroy(&chan->pending_mutex);
}
