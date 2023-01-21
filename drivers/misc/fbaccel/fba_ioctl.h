/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

#include <linux/compiler.h>
#include <linux/ioctl.h>

// Description of a single result.
//
// High bit of flag indicates an error code.  If set, status field indicates
// a completion status value, and the low 7 bits indicates the index of the
// submission entry failed.  On success, the high bit of flags is clear and
// the lower 7 bits of flag and all of status are application defined.
struct result_info {
	uint16_t status; // App defined, or CS code if high bit if flag set.
	uint8_t flags_index; // App defined, or index of failed entry.
	uint8_t reserved[5];
	uint64_t user_data; // User data provided on post.
};

struct resource_info {
	// Type of resources such as memory, computer etc.
	uint16_t type;
	// Subtype of resources as SRAM, DRAM in memory etc.
	uint16_t subtype;
	// Mainly to ensure 8 byte alignment, can be used for misc. purposes.
	uint16_t alloc;
	// Mainly to ensure 8 byte alignment, can be used for misc. purposes.
	uint16_t misc;
	// Start address of the resource.
	uint64_t start;
	// Size of the resource available for use.
	uint64_t size;
	// Granularity of resources e.g. for memory this could imply alignment.
	uint64_t avail;
};

// IOCTL struct to retreive Resource Info from device.
struct res_info_ioctl {
	size_t count; // Number of resources wanted
	size_t offset; // Starting index of resources wanted
	struct resource_info __user *info; // Where resource info should be
		// copied to.
};

// IOCTL struct to allocate resrouce and retrieve RID.
struct res_alloc_ioctl {
	uint16_t index;
	uint16_t type;
	uint16_t subtype;
	uint16_t reserved;
	uint64_t count; // Number of resources wanted
	uint64_t rid; // Returned RID
	uint64_t start; // Returned START
};

// IOCTL struct to read or write data directly to device BARs.
#define FBA_ACCESS_READ 0
#define FBA_ACCESS_WRITE 1
struct bar_access_ioctl {
	uint32_t __user *data; // Read destination, or write source.
	uint64_t offset; // Word-aligned byte offset into bar to access.
	uint32_t len; // Word-multiple number of bytes to read/write.
	uint8_t bar; // Driver bar index.
	uint8_t type; // Read or write.
};

struct scratch_access_ioctl {
	void __user *data; // Read destination, or write source.
	uint64_t rid; // RID of the scratch area.
	uint32_t len; // Length of the transfer.
	uint32_t type; // Read or write.
};

// IOCTL struct to post one packet (array of submission entries) keyed by
// eventfd.
struct post_ioctl {
	int eventfd; // The eventfd associated with this post.
	const struct fba_submission __user *subs; // Array of submission entries.
	size_t count; // Number of submission entries in the packet.
	uint64_t user_data; // User data associated with packet.
};

// IOCTL struct to read one or more results keyed by eventfd.
struct read_ioctl {
	int eventfd; // The eventfd associated with this read.
	size_t count; // Max number of results to drain.
	struct result_info __user *results; // Array of results.
};

// IOCTL struct to read or write data into DMA buffer.
struct dma_access_ioctl {
	void __user *data; // Read destination, or write source
	uint64_t offset; // Word-aligned byte offset into dma area.
	uint32_t len; // Number of bytes to read/write
	uint8_t type; // Read or write command (FBA_ACCESS_xxx)
	uint64_t addr; // Read-only physical address of DMA buffer.
};

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
	uint32_t max_entries_per_context; // Max number of entries per context
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

// Return resource status.
//
// Fills the array of struct resource_info objects provided in the
// struct.  Returns up to a PAGE worth of data.  If the number of
// count + index is greater than the number of resources, the
// result will be truncated.
//
// RETURNS:
//   +N - On success returns number of structs actually returned.
//   -EINVAL - Starting index out of range.
//   -EACCES - Unable to read struct fba_access from userspace.
//   -EFAULT - If IOCTL struct or pointers in struct are invalid.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_RESOURCE_INFO _IOW('f', 1, struct res_info_ioctl)

// Allocate a resource, and return unique ID.
//
// Allocates based on type, subtype, index and returns a 64b ID which can
// be used to access the object.  On success the rid field will be filled.
//
// RETURNS:
//   0 - Success.
//   -EINVAL - Bad input params.
//   -ENOMEM - Insufficent memory to track or resources to allocate.
//   -EFAULT - If IOCTL struct or pointers in struct are invalid.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_RESOURCE_ALLOC _IOWR('f', 2, struct res_alloc_ioctl)

// Frees a resource by ID.
//
// The RID is passed in as the third parameter to the ioctl.
//
// RETURNS:
//   0 - Success. Allocation freed back to the driver.
//   -EINVAL - Bad RID provided.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_RESOURCE_FREE _IOWR('f', 3, uint64_t)

// Read or write 32-bit words at the given driver bar index and offset.
//
// RETURNS:
//   0 - Success. Read or write completed.
//   -EINVAL - Bad input params (unaligned offset or length, out of range bar).
//   -EFAULT - If IOCTL struct or pointers in struct are invalid.
//   -EACCES - Unable to read struct fba_access from userspace.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_ACCESS_BAR _IOW('f', 5, struct bar_access_ioctl)

// Post one or more packets to a submission queue.
//
// A non-blocking post of one or more packets onto the default channel.
// On success, one or more packets are posted. On failure none posted.
// Results for these packets can be retrieved by using the same eventfd
// on the read function.  The associated eventfd if provided will be
// incremeted once per completed packet.   Packet results may return
// out of order, so a unique user data field should be provided to enable
// the application to distinguish results.
//
// NOTE:
//   On success, a seperate result per packet will be delievered, and should
//   be drained to avoid exhausting per file context resources.
//
// RETURNS:
//   +N - On success returns number of packets posted (in order).
//   -EBADF - If a bad device FD was provided.
//   -EINVAL - If a bad argument was provided.
//   -EFAULT - If IOCTL struct or pointers in struct are invalid.
//   -ENOMEM - If insufficent memory is available to track the packets.
//   -EAGAIN - If insufficent space available in the queue.
//   -EFAILED - For all other failure cases.
#define FBA_IOCTL_POST_ID _IOW('f', 6, struct post_ioctl)

// Read one or more results from the completion queue.
//
// A non-blocking read of one or more results from the default channel.
// On success, a maximum of count results are copied into the result array.
// Only results matching the provided eventfd will be returned.
//
// RETURNS:
//   +N - On success returns number of results retrieved.
//   -EBADF - If a bad device FD was provided.
//   -EINVAL - If a bad argument was provided.
//   -EFAULT - If IOCTL struct or pointers in struct are invalid.
//   -EAGAIN - If no matching results are currently available.
//   -EFAILED - For all other failure cases.
#define FBA_IOCTL_READ_ID _IOW('f', 7, struct read_ioctl)

// Read or write DMA mapped area
//
// Reads from or writes into generic driver DMA mapped area.  The actual
// physical address of the DMA area is returned in the IOCTL.  A transfer
// size of zero allows using this to query for the physical address.
//
// RETURNS:
//   0 - Success. Read or write completed.
//   -EINVAL - Bad input params (unaligned offset or length, out of range bar).
//   -EFAULT - If IOCTL struct or pointers in struct are invalid.
//   -EACCES - Unable to read struct fba_access from userspace.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_ACCESS_DMA _IOWR('f', 8, struct dma_access_ioctl)

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

// Read or write 32-bit words at the given driver bar index and offset.
//
// RETURNS:
//   0 - Success. Read or write completed.
//   -EINVAL - Bad input params (unaligned offset or length, out of range bar).
//   -EFAULT - If IOCTL struct or pointers in struct are invalid.
//   -EACCES - Unable to read struct fba_access from userspace.
//   non-zero - Other, lower level errors.
#define FBA_IOCTL_ACCESS_SCRATCH _IOW('f', 11, struct scratch_access_ioctl)

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
