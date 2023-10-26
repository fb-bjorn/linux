/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
// clang-format off
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fbaccel

#if !defined(_TRACE_FBA_DRIVER_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FBA_DRIVER_H

#include <linux/tracepoint.h>
#include <linux/dma-direction.h>

TRACE_EVENT(fba_post_cmd,
	TP_PROTO(const char *name, size_t n_sub, uint64_t user_data),
	TP_ARGS(name, n_sub, user_data),

	TP_STRUCT__entry(
		__string(name, name)
		__field(size_t, n_sub)
		__field(uint64_t, user_data)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->n_sub = n_sub;
	),

	TP_printk("%s: n_sub=%zu user_data=%llu", __get_str(name), __entry->n_sub, __entry->user_data)
);

TRACE_EVENT(fba_job_post,
	TP_PROTO(const char *name, void *job, void *ectx, uint32_t sub_cnt, size_t alloc_cnt),
	TP_ARGS(name, job, ectx, sub_cnt, alloc_cnt),

	TP_STRUCT__entry(
		__string(name, name)
		__field(void *, job)
		__field(void *, ectx)
		__field(uint32_t, sub_cnt)
		__field(size_t, alloc_cnt)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->job = job;
		__entry->ectx = ectx;
		__entry->sub_cnt = sub_cnt;
		__entry->alloc_cnt = alloc_cnt;
	),

	TP_printk("%s: job=%p ectx=%p sub_cnt=%u alloc_cnt=%zu",
			__get_str(name), __entry->job, __entry->ectx, __entry->sub_cnt,
			__entry->alloc_cnt)
);

TRACE_EVENT(fba_job_admin_post,
	TP_PROTO(const char *name, void *job, void *ectx, uint32_t sub_cnt, size_t alloc_cnt),
	TP_ARGS(name, job, ectx, sub_cnt, alloc_cnt),

	TP_STRUCT__entry(
		__string(name, name)
		__field(void *, job)
		__field(void *, ectx)
		__field(uint32_t, sub_cnt)
		__field(size_t, alloc_cnt)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->job = job;
		__entry->ectx = ectx;
		__entry->sub_cnt = sub_cnt;
		__entry->alloc_cnt = alloc_cnt;
	),

	TP_printk("%s: job=%p ectx=%p sub_cnt=%u alloc_cnt=%zu",
			__get_str(name), __entry->job, __entry->ectx, __entry->sub_cnt,
			__entry->alloc_cnt)
);

TRACE_EVENT(fba_job_read_results,
	TP_PROTO(const char *name, void *job, void *ectx, uint32_t sub_cnt,
		 size_t alloc_cnt),
	TP_ARGS(name, job, ectx, sub_cnt, alloc_cnt),

	TP_STRUCT__entry(
		__string(name, name)
		__field(void *, job)
		__field(void *, ectx)
		__field(uint32_t, sub_cnt)
		__field(size_t, alloc_cnt)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->job = job;
		__entry->ectx = ectx;
		__entry->sub_cnt = sub_cnt;
		__entry->alloc_cnt = alloc_cnt;
	),

	TP_printk("%s: job=%p ectx=%p sub_cnt=%u alloc_cnt=%zu",
			__get_str(name), __entry->job, __entry->ectx, __entry->sub_cnt,
			__entry->alloc_cnt)
);


TRACE_EVENT(fba_job_done,
	TP_PROTO(const char *name, void *job, void *ectx, uint32_t sub_cnt,
		 size_t alloc_cnt),
	TP_ARGS(name, job, ectx, sub_cnt, alloc_cnt),

	TP_STRUCT__entry(
		__string(name, name)
		__field(void *, job)
		__field(void *, ectx)
		__field(uint32_t, sub_cnt)
		__field(size_t, alloc_cnt)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->job = job;
		__entry->ectx = ectx;
		__entry->sub_cnt = sub_cnt;
		__entry->alloc_cnt = alloc_cnt;
	),

	TP_printk("%s: job=%p ectx=%p sub_cnt=%u alloc_cnt=%zu",
			__get_str(name), __entry->job, __entry->ectx, __entry->sub_cnt,
			__entry->alloc_cnt)
);

TRACE_EVENT(fba_channel_post_sub,
	TP_PROTO(const char *name, uint8_t chan_id, uint16_t cmd_id, uint16_t opcode),
	TP_ARGS(name, chan_id, cmd_id, opcode),

	TP_STRUCT__entry(
		__string(name, name)
		__field(uint8_t, chan_id)
		__field(uint16_t, cmd_id)
		__field(uint16_t, opcode)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->chan_id = chan_id;
		__entry->cmd_id = cmd_id;
		__entry->opcode = opcode;
	),

	TP_printk("%s: chan_id=%u cmd_id=%u opcode=%u", __get_str(name), __entry->chan_id, __entry->cmd_id, __entry->opcode)
);

TRACE_EVENT(fba_channel_post,
	TP_PROTO(const char *name, uint8_t chan_id, uint16_t cmd_id, uint32_t sub_cnt),
	TP_ARGS(name, chan_id, cmd_id, sub_cnt),

	TP_STRUCT__entry(
		__string(name, name)
		__field(uint8_t, chan_id)
		__field(uint16_t, cmd_id)
		__field(uint32_t, sub_cnt)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->chan_id = chan_id;
		__entry->cmd_id = cmd_id;
		__entry->sub_cnt = sub_cnt;
	),

	TP_printk("%s: chan_id=%u cmd_id=%u sub_cnt=%u", __get_str(name), __entry->chan_id, __entry->cmd_id, __entry->sub_cnt)
);


TRACE_EVENT(fba_channel_done,
	TP_PROTO(const char *name, uint8_t chan_id, uint16_t cmd_id, int cqueue_head),
	TP_ARGS(name, chan_id, cmd_id, cqueue_head),

	TP_STRUCT__entry(
		__string(name, name)
		__field(uint8_t, chan_id)
		__field(uint16_t, cmd_id)
		__field(int, cqueue_head)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->chan_id = chan_id;
		__entry->cmd_id = cmd_id;
		__entry->cqueue_head = cqueue_head;
	),

	TP_printk("%s: chan_id=%u cmd_id=%u cqueue_head=%d", __get_str(name), __entry->chan_id, __entry->cmd_id, __entry->cqueue_head)
);

TRACE_EVENT(fba_time_sync,
	TP_PROTO(const char *name, uint64_t tv_sec, uint64_t tv_nsec),
	TP_ARGS(name, tv_sec, tv_nsec),

	TP_STRUCT__entry(
		__string(name, name)
		__field(uint64_t, tv_sec)
		__field(uint64_t, tv_nsec)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->tv_sec = tv_sec;
		__entry->tv_nsec = tv_nsec;
	),

	TP_printk("%s: time=%llu.%llu", __get_str(name), __entry->tv_sec, __entry->tv_nsec)
);

TRACE_EVENT(fba_dma_map_cnt,
	TP_PROTO(const char *name, uint64_t va, uint64_t sz, uint8_t frdev, uint64_t dma_a),
	TP_ARGS(name, va, sz, frdev, dma_a),

	TP_STRUCT__entry(
		__string(name, name)
		__field(uint64_t, va)
		__field(uint64_t, sz)
		__field(uint8_t, frdev)
		__field(uint64_t, dma_a)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->va = va;
		__entry->sz = sz;
		__entry->frdev = frdev;
		__entry->dma_a = dma_a;
	),

	TP_printk("%s: va=0x%llx sz=0x%llx dir=%s dma_a=0x%llx",
		__get_str(name), __entry->va, __entry->sz,
		__entry->frdev ? "D2H" : "H2D",
		__entry->dma_a)
);

TRACE_EVENT(fba_dma_map_prp,
	TP_PROTO(const char *name, uint64_t va, uint64_t sz, uint8_t frdev, uint64_t prp_a),
	TP_ARGS(name, va, sz, frdev, prp_a),

	TP_STRUCT__entry(
		__string(name, name)
		__field(uint64_t, va)
		__field(uint64_t, sz)
		__field(uint8_t, frdev)
		__field(uint64_t, prp_a)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->va = va;
		__entry->sz = sz;
		__entry->frdev = frdev;
		__entry->prp_a = prp_a;
	),

	TP_printk("%s: va=0x%llx sz=0x%llx dir=%s prp_a=0x%llx",
		__get_str(name), __entry->va, __entry->sz,
		__entry->frdev == DMA_FROM_DEVICE ? "D2H" : "H2D",
		__entry->prp_a)
);

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE fba_trace

#endif /* _TRACE_FBA_DRIVER_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
