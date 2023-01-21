/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

#include <linux/compiler.h>
#include <linux/ioctl.h>

// IOCTL struct to update and report driver state.
struct set_state_ioctl {
	uint32_t oldstate; // Read only old state value.
	uint32_t newstate; // Write only new state value.
};

// IOCTL struct to read driver details.
struct get_details_ioctl {
	uint64_t size; // Max bytes to transfer.
	uint64_t bar_addrs[6]; // Physical address of BARs.
	uint32_t bar_sizes[6]; // Sizes of the BARs.
	uint16_t host_ipc_bar; // Bar index.
	uint16_t host_ipc_stride; // Mailbox entry width.
	uint32_t host_ipc_offset; // Offset into BAR for host IPC.
	uint64_t dma_addr; // Physical address of shared DMA buffer.
	uint32_t dma_size; // Size of shared DMA buffer.
	uint32_t state; // Current state of driver.
	uint64_t irq_num; // Number of IRQs aquired.
	uint64_t irq_seen; // Deprecated, set to 0.
	uint64_t resource_cnt; // Number of resources available.
	uint64_t context_cnt; // Number of contexts available.
	uint64_t context_used; // Number of contexts used.
	uint64_t reserved1;
	uint64_t reserved2;
	uint32_t admin_sq_fill_watermark; // AdminQ SQ fill watermark.
	uint32_t admin_cq_fill_watermark; // AdminQ CQ fill watermark.
	uint32_t exe_sq_fill_watermark; // ExeQ SQ fill watermark.
	uint32_t exe_cq_fill_watermark; // ExeQ CQ fill watermark.
	uint32_t reserved3;
};

// Enumeration for channel_type in fba_limit_params_ioctl
#define FBA_CHANNEL_TYPE_CONCURRENT 0U

struct fba_limit_params_ioctl {
	uint32_t channel_type; // Channel type to query
	uint32_t max_channels; // Total number of supported channels
	uint32_t reserved;
	uint32_t max_entries_per_packet; // Max allowed entries per packet
};

// IOCTL for running a payload executable binary
struct payload_exec_ioctl {
	int use_default_func;
	int use_default_args;
	uint32_t func;
	uint32_t arg1;
	uint32_t arg2;
	uint32_t arg3;
	const char __user *user_hbin_filename;
	unsigned long exec_timeout;
	uint64_t results_buf_size;
	uint64_t __user *user_results_buf;
	uint64_t __user *user_actual_results_size;
};

// Set driver state.
//
// DOWN -> PRESENT -> CONNECTED -> ACTIVE.
//
// Currently we only support moving from a lower state to a higher state.
// It's not possible to skip a state, switching to a higher state will
// always step through any previous states that haven't been activated yet.
//
// RETURNS:
//   0 - Success.
//   non-zero - Other, lower level errors.
// NOTE:  Examine driver logging to determine what happened.
#define FBA_IOCTL_SET_STATE _IOWR('f', 9, struct set_state_ioctl)

// Get Driver Info
//
// Return information about driver internals.
//
//
// RETURNS:
//   0 - Success.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_GET_DETAILS _IOR('f', 10, struct get_details_ioctl)

// Unlock Secure Debug capabilities
//
// Downloads binary with Secure Debug certificate chain to enable debugging
// features on a provisioned device
//
// RETURNS:
//   0 - Success.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_SECURE_DBG_UNLOCK _IO('f', 20)

// Load/run an executable payload HBIN
//
// Downloads & executes a payload executable HBIN. Results from the FW, if any,
// are returned in an optional user provided buffer
//
// RETURNS:
//   0 - Success.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_PAYLOAD_EXEC _IOWR('f', 25, struct payload_exec_ioctl)

// Return limit parameters
//
// Return limit parameters for the device in associated fba_limit_params_ioctl
//
// RETURNS:
//   0 - Success.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_GET_LIMIT_PARAMS _IO('f', 22)
