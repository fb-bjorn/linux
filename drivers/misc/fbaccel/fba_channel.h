/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/*
 * FBA Channel
 *
 * Channels provides a mechanism for transfering Jobs from the host to
 * the device and gathering the result.
 */

struct fba_queue_info {
	struct fba_shared *area;
	uint64_t offset;
	uint64_t size;
};

/*
 * Structure representing a queue, whether submission queue or completion
 * queue.  Accesses to ring buffer are via the fba_shared abstraction, which
 * allows either queue to live in host or device memory as required by
 * implementation, depending on resource availability and performance.
 */
struct fba_queue {
	struct fba_shared *area; // Memory area containing ring buffer
	uint64_t offset; // Ring buffer starting offset in memory area
	uint32_t head; // Next entry for consumer to read.
	uint32_t tail; // Next entry for consumer to write.
	uint32_t entries; // Size of ring buffer, as number of entries
	uint32_t fill_watermark; // High fill level watermark.
};

/*
 * Structure representing a channel, which is an interface to the device
 * with a pair of queues for submitting work and getting back completions.
 *
 * Each channel has its own MSI-X IRQ and dedicated ring buffers for
 * submission queue and completion queue.
 *
 * Currently channels come in three flavors, an admin channel for general
 * control path operations, a shared channel with out-of-order completions,
 * and (in a future diff) a set of "streaming" channels with in-order
 * completion guarantees.
 */
struct fba_channel {
	struct work_struct work;
	struct fba_device *dev;
	struct fba_limits *limits;
	uint64_t max_job_subs;
	struct fba_limits usage; // Counts used by this channel.

	spinlock_t lock; // queue lock
	struct fba_queue squeue; // Submission queue
	struct fba_queue cqueue; // Completion queue
	uint8_t phase;
	uint8_t chan_id;
	unsigned int irq;
	bool stopped;

	struct mutex pending_mutex;
	struct list_head pending_reqs;
	struct idr cmd_ids;
};

struct fba_channel_req;

typedef void (*fba_channel_cb_fn)(bool completed, struct fba_channel_req *req);

/* Tracks info for an outstanding packet. */
struct fba_channel_req {
	struct list_head req_item;

	struct fba_submission *subs;
	uint32_t sub_cnt;

	uint16_t cmd_id;

	fba_channel_cb_fn cb;
	bool posted;

	struct fba_completion cmp;
};

//
// Channel Functions.
//
int fba_channel_init(struct fba_device *dev, int id, unsigned int irq,
		     struct fba_queue_info *sqinfo,
		     struct fba_queue_info *cqinfo, struct fba_limits *limits,
		     uint64_t max_job_subs);
void fba_channel_exit(struct fba_channel *chan);

void fba_channel_init_req(struct fba_channel_req *req, size_t n_subs,
			  struct fba_submission *subs, fba_channel_cb_fn cb);
int fba_channel_post_req(struct fba_channel *chan, struct fba_channel_req *req);

bool fba_channel_has_space(struct fba_channel *chan);
uint64_t fba_channel_max_job_subs(struct fba_channel *chan);
void fba_channel_shutdown(struct fba_channel *chan);
// Get watermarks
void fba_channel_get_fill_watermarks(struct fba_channel *chan,
				     uint32_t *sq_watermark,
				     uint32_t *cq_watermark);

// Default channel heartbeat period
#define CHAN_HEARTBEAT_PERIOD_MS 60000
