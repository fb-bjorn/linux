/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/ioctl.h>

/*
 * Description of a single result.
 *
 * High bit of flag indicates an error code.  If set, status field indicates
 * a completion status value, and the low 7 bits indicates the index of the
 * submission entry failed.  On success, the high bit of flags is clear and
 * the lower 7 bits of flag and all of status are application defined.
 */
struct result_info {
	__u16 status; /* App defined, or CS code if high bit if flag set. */
	__u8 flags_index; /* App defined, or index of failed entry. */
	__u8 reserved[5];
	__u64 user_data; /* User data provided on post. */
};

struct resource_info {
	/* Type of resources such as memory, computer etc. */
	__u16 type;
	/* Subtype of resources as SRAM, DRAM in memory etc. */
	__u16 subtype;
	/* Mainly to ensure 8 byte alignment, can be used for misc. purposes. */
	__u16 alloc;
	/* Mainly to ensure 8 byte alignment, can be used for misc. purposes. */
	__u16 misc;
	/* Start address of the resource. */
	__u64 start;
	/* Size of the resource available for use. */
	__u64 size;
	/* Granularity of resources e.g. for memory this could imply alignment. */
	__u64 avail;
};

/* IOCTL struct to retrieve Resource Info from device. */
struct res_info_ioctl {
	__u64 count; /* Number of resources wanted */
	__u64 offset; /* Starting index of resources wanted */
	__u64 info_ptr; /* Array for returned struct resource_info */
};

/* IOCTL struct to allocate resrouce and retrieve RID. */
struct res_alloc_ioctl {
	__u16 index;
	__u16 type;
	__u16 subtype;
	__u16 reserved;
	__u64 count; /* Number of resources wanted */
	__u64 rid; /* Returned RID */
	__u64 start; /* Returned START */
};

/* IOCTL struct to map a P2P memory RID to an fd */
struct res_p2p_export_ioctl {
	__u64 source_rid; /* Returned RID */
	__s32 fd; /* Returned dmabuf fd */
	__u32 padding;
};

/* IOCTL struct to read or write data directly to device BARs. */
#define FBA_ACCESS_READ 0
#define FBA_ACCESS_WRITE 1
struct bar_access_ioctl {
	__u64 data_ptr; /* Read destination, or write source. */
	__u64 offset; /* Word-aligned byte offset into bar to access. */
	__u32 len; /* Word-multiple number of bytes to read/write. */
	__u8 bar; /* Driver bar index. */
	__u8 type; /* Read or write. */
	__u16 padding;
};

struct scratch_access_ioctl {
	__u64 data_ptr; /* Read destination, or write source. */
	__u64 rid; /* RID of the scratch area. */
	__u32 len; /* Length of the transfer. */
	__u32 type; /* Read or write. */
};

/*
 * IOCTL struct to post one packet (array of submission entries) keyed by
 * eventfd.
 */
struct post_ioctl {
	__s32 eventfd; /* The eventfd associated with this post. */
	__u32 padding;
	__u64 subs_ptr; /* Array of struct fba_submission entries */
	__u64 count; /* Number of submission entries in the packet. */
	__u64 user_data; /* User data associated with packet. */
};

/* IOCTL struct to read one or more results keyed by eventfd. */
struct read_ioctl {
	__s32 eventfd; /* The eventfd associated with this read. */
	__u32 padding;
	__u64 count; /* Max number of results to drain. */
	__u64 results_ptr; /* Array of struct result-info */
};

/* IOCTL struct to read or write data into DMA buffer. */
struct dma_access_ioctl {
	__u64 data_ptr; /* Read destination, or write source */
	__u64 offset; /* Word-aligned byte offset into dma area. */
	__u32 len; /* Number of bytes to read/write */
	__u8 type; /* Read or write command (FBA_ACCESS_xxx) */
	__u8 padding[3];
	__u64 addr; /* Read-only physical address of DMA buffer. */
};

/* IOCTL struct to update and report driver state. */
struct set_state_ioctl {
	__u32 oldstate; /* Read only old state value. */
	__u32 newstate; /* Write only new state value. */
};

/* IOCTL struct to read driver details. */
struct get_details_ioctl {
	__u64 size; /* Max bytes to transfer. */
	__u64 bar_addrs[6]; /* Physical address of BARs. */
	__u32 bar_sizes[6]; /* Sizes of the BARs. */
	__u16 host_ipc_bar; /* Bar index. */
	__u16 host_ipc_stride; /* Mailbox entry width. */
	__u32 host_ipc_offset; /* Offset into BAR for host IPC. */
	__u64 dma_addr; /* Physical address of shared DMA buffer. */
	__u32 dma_size; /* Size of shared DMA buffer. */
	__u32 state; /* Current state of driver. */
	__u64 irq_num; /* Number of IRQs acquired. */
	__u64 irq_seen; /* Deprecated, set to 0. */
	__u64 resource_cnt; /* Number of resources available. */
	__u64 context_cnt; /* Number of contexts available. */
	__u64 context_used; /* Number of contexts used. */
	__u64 mon_item_cnt; /* Number of monitoring items definitions. */
	__u64 mon_group_cnt; /* Number of monitoring group definitions. */
	__u32 admin_sq_fill_watermark; /* AdminQ SQ fill watermark. */
	__u32 admin_cq_fill_watermark; /* AdminQ CQ fill watermark. */
	__u32 exe_sq_fill_watermark; /* ExeQ SQ fill watermark. */
	__u32 exe_cq_fill_watermark; /* ExeQ CQ fill watermark. */
	__u32 streaming_channel_cnt; /* Number of streaming channels */
	__u32 padding;
};

/* IOCTL struct to read items or groups definitions into data. */
struct mon_defs_ioctl {
	__u64 items_ptr; /* Buffer to hold items defs */
	__u64 max_items_size; /* items buffer size in bytes. */
	__u64 items_bytes_written; /* Number of byte written into items buffer. */
	__u64 groups_ptr; /* Buffer to hold group defs */
	__u64 max_groups_size; /* groups buffer size in bytes. */
	__u64 groups_bytes_written; /* Number of byte written into groups buffer. */
};

#define MON_VALUE_SIZE 124

/* IOCTL struct to read monitoring data for the specified item ids. */
struct mon_data_ioctl {
	__u64 id_count; /* Number of elements in ids, data, and err arrays. */
	__u64 ids_ptr; /* input: array of __u32 ids */
	/*
	 * Data buffer destination. Buffer size is assumed to be
	 * id_count * MON_VALUE_SIZE
	 */
	__u64 data_ptr;
	/*
	 * Error buffer destination. Buffer size is assumed to be
	 * id_count * sizeof(int16_t)
	 */
	__u64 errors_ptr;
};

/* IOCTL for getting coredump status. */
struct coredump_status_ioctl {
	/*
	 * Output param which provides the trigger status,
	 * 0 - coredump not triggered, 1 - coredump triggered.
	 */
	__u32 triggered;
	/* Output param which provides coredump data size. */
	__u32 coredump_size;
	/*
	 * If wait >= 0 call will block until coredump generated.
	 * If wait < 0, call will return status immediately.
	 */
	__s32 wait;
};

/*IOCTL for getting coredump. */
struct get_coredump_ioctl {
	/* Destination buffer to read coredump data. */
	__u64 data_ptr;
	/*
	 * Size of the buffer allocated. This should be at least equal
	 * to coredump_size reported in coredump_status_ioctl.
	 */
	__u64 size;
};

/*IOCTL for creating streaming context */
struct stream_ioctl {
	__s64 flags; /* Same as flags used in open(2) */
	__u64 fd; /* Created streaming file descriptor */
};

/* Enumeration for channel_type in fba_limit_params_ioctl */
#define FBA_CHANNEL_TYPE_CONCURRENT 0U
#define FBA_CHANNEL_TYPE_STREAMING 1U

struct fba_limit_params_ioctl {
	__u32 channel_type; /* Channel type to query (generic or streaming) */
	__u32 max_channels; /* Total number of supported channels */
	__u32 max_entries_per_context; /* Max number of entries per context */
	__u32 max_entries_per_packet; /* Max allowed entries per packet */
};

/* IOCTL for getting coredump region info. */
struct get_coredump_region_ioctl {
	/* Destination buffer for coredump region info. */
	__u64 data_ptr;
	/*
	 * Size of the buffer allocated.This should be at least equal
	 * to coredump_size reported in get_coredump_info_ioctl.
	 */
	__u64 size;
};

/* IOCTL for getting coredump info. */
struct get_coredump_info_ioctl {
	/*
	 * Output param which provides the validity status,
	 * 0 - coredump not valid, 1 - coredump info valid.
	 */
	__u32 validity;
	/* Output param which provides the number of core dump regions */
	__u32 num_regions;
	/* Output param which provides coredump info size. */
	__u32 coredump_info_size;
};

/* IOCTL for running a payload executable binary */
struct payload_exec_ioctl {
	__u32 use_default_func;
	__u32 use_default_args;
	__u32 func;
	__u32 arg1;
	__u32 arg2;
	__u32 arg3;
	__u64 user_hbin_filename_ptr;
	__u64 exec_timeout;
	__u64 results_buf_size;
	__u64 user_results_buf_ptr;
	__u64 user_actual_results_size_ptr;
};

/* IOCTL for getting coredump data by issuing DMA from M7 */

/*
 * Flag to signal that this will be the last coredump request
 * Used to allow driver to wait for coredump to complete
 * before fencing off DMA during fault conditions.
 */
#define GET_COREDUMP_DATA_FLAG_COREDUMP_DONE (1 << 0)

struct get_coredump_region_data_ioctl {
	/* Size of the DMA operation */
	__u64 size;
	/* Device address used for DMA */
	__u64 device_addr;
	/* Host address used for DMA */
	__u64 host_buf_ptr;
	/* flags for request (mainly signaling done) */
	__u32 flags;
	__u32 padding;
};

/*
 * Return resource status.
 *
 * Fills the array of struct resource_info objects provided in the
 * struct.  Returns up to a PAGE worth of data.  If the number of
 * count + index is greater than the number of resources, the
 * result will be truncated.
 *
 * RETURNS:
 *   +N - On success returns number of structs actually returned.
 *   -EINVAL - Starting index out of range.
 *   -EACCES - Unable to read struct fba_access from userspace.
 *   -EFAULT - If IOCTL struct or pointers in struct are invalid.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_RESOURCE_INFO _IOW('f', 1, struct res_info_ioctl)

/*
 * Allocate a resource, and return unique ID.
 *
 * Allocates based on type, subtype, index and returns a 64b ID which can
 * be used to access the object.  On success the rid field will be filled.
 *
 * RETURNS:
 *   0 - Success.
 *   -EINVAL - Bad input params.
 *   -ENOMEM - Insufficent memory to track or resources to allocate.
 *   -EFAULT - If IOCTL struct or pointers in struct are invalid.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_RESOURCE_ALLOC _IOWR('f', 2, struct res_alloc_ioctl)

/*
 * Frees a resource by ID.
 *
 * The RID is passed in as the third parameter to the ioctl.
 *
 * RETURNS:
 *   0 - Success. Allocation freed back to the driver.
 *   -EINVAL - Bad RID provided.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_RESOURCE_FREE _IOWR('f', 3, __u64)

/*
 * Read or write 32-bit words at the given driver bar index and offset.
 *
 * RETURNS:
 *   0 - Success. Read or write completed.
 *   -EINVAL - Bad input params (unaligned offset or length, out of range bar).
 *   -EFAULT - If IOCTL struct or pointers in struct are invalid.
 *   -EACCES - Unable to read struct fba_access from userspace.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_ACCESS_BAR _IOW('f', 5, struct bar_access_ioctl)

/*
 * Post one or more packets to a submission queue.
 *
 * A non-blocking post of one or more packets onto the default channel.
 * On success, one or more packets are posted. On failure none posted.
 * Results for these packets can be retrieved by using the same eventfd
 * on the read function.  The associated eventfd if provided will be
 * incremeted once per completed packet.   Packet results may return
 * out of order, so a unique user data field should be provided to enable
 * the application to distinguish results.
 *
 * NOTE:
 *   On success, a separate result per packet will be delievered, and should
 *   be drained to avoid exhausting per file context resources.
 *
 * RETURNS:
 *   +N - On success returns number of packets posted (in order).
 *   -EBADF - If a bad device FD was provided.
 *   -EINVAL - If a bad argument was provided.
 *   -EFAULT - If IOCTL struct or pointers in struct are invalid.
 *   -ENOMEM - If insufficent memory is available to track the packets.
 *   -EAGAIN - If insufficent space available in the queue.
 *   -EFAILED - For all other failure cases.
 */
#define FBA_IOCTL_POST_ID _IOW('f', 6, struct post_ioctl)

/*
 * Read one or more results from the completion queue.
 *
 * A non-blocking read of one or more results from the default channel.
 * On success, a maximum of count results are copied into the result array.
 * Only results matching the provided eventfd will be returned.
 *
 * RETURNS:
 *   +N - On success returns number of results retrieved.
 *   -EBADF - If a bad device FD was provided.
 *   -EINVAL - If a bad argument was provided.
 *   -EFAULT - If IOCTL struct or pointers in struct are invalid.
 *   -EAGAIN - If no matching results are currently available.
 *   -EFAILED - For all other failure cases.
 */
#define FBA_IOCTL_READ_ID _IOW('f', 7, struct read_ioctl)

/*
 * Read or write DMA mapped area
 *
 * Reads from or writes into generic driver DMA mapped area.  The actual
 * physical address of the DMA area is returned in the IOCTL.  A transfer
 * size of zero allows using this to query for the physical address.
 *
 * RETURNS:
 *   0 - Success. Read or write completed.
 *   -EINVAL - Bad input params (unaligned offset or length, out of range bar).
 *   -EFAULT - If IOCTL struct or pointers in struct are invalid.
 *   -EACCES - Unable to read struct fba_access from userspace.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_ACCESS_DMA _IOWR('f', 8, struct dma_access_ioctl)

/*
 * Set driver state.
 *
 * DOWN -> PRESENT -> CONNECTED -> ACTIVE.
 *
 * Currently we only support moving from a lower state to a higher state.
 * It's not possible to skip a state, switching to a higher state will
 * always step through any previous states that haven't been activated yet.
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 * NOTE:  Examine driver logging to determine what happened.
 */
#define FBA_IOCTL_SET_STATE _IOWR('f', 9, struct set_state_ioctl)

/*
 * Get Driver Info
 *
 * Return information about driver internals.
 *
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_GET_DETAILS _IOR('f', 10, struct get_details_ioctl)

/*
 * Read or write 32-bit words at the given driver bar index and offset.
 *
 * RETURNS:
 *   0 - Success. Read or write completed.
 *   -EINVAL - Bad input params (unaligned offset or length, out of range bar).
 *   -EFAULT - If IOCTL struct or pointers in struct are invalid.
 *   -EACCES - Unable to read struct fba_access from userspace.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_ACCESS_SCRATCH _IOW('f', 11, struct scratch_access_ioctl)

/*
 * Get Coredump status.
 *
 * Returns the coredump status. This IOCTL can be used to check
 * if coredump is triggered by the device. If coredump is triggered
 * this IOCTL will provide the size of coredump data that will be dumped
 * by the device.
 * User space apps can pass eventfd to this ioctl which will be registered
 * with the driver and signalled when coredump trigger is detected by driver.
 * If eventfd is < 0 then this IOCTL will return coredump status.
 * If coredump polling is not yet started by the driver, this ioctl will attempt
 * to register eventfd if provided in the ioctl input param, else this will return
 * error code.
 *
 * Confusingly we have two separate flavors of coredump, this one is older and
 * supports smaller coredumps read using PIO from PCIe BAR.  Used with the
 * FBA_IOCTL_GET_COREDUMP_DATA ioctl.
 *
 * RETURNS:
 * 0 - Success.
 * -EFAULT - If IOCTL struct is invalid.
 * -EINVAL - If the device has not started polling for coredump trigger.
 * Negative value if eventfd registration fails.
 */
#define FBA_IOCTL_GET_COREDUMP_STATUS \
	_IOWR('f', 12, struct coredump_status_ioctl)

/*
 * Get coredump data.
 *
 * Returns the coredump data provided by the device incase of a
 * crash or fatal error.
 *
 * RETURNS:
 * 0 - Success.
 * -EFAULT - If IOCTL struct is invalid or if pointers in struct are invalid.
 * -ENOMEM - If memory allocation fails.
 * -EINVAL - If size of the buffer provided by user is incorrect.
 * -ENXIO  - If reading over PCIe failed.
 */
#define FBA_IOCTL_GET_COREDUMP_DATA _IOWR('f', 13, struct get_coredump_ioctl)

/*
 * Get coredump info.
 *
 * Returns information about the number of coredump regions and the size of
 * the region table.
 *
 * Confusingly we have two separate flavors of coredump, this one is newer and
 * supports fetching coredump data using DMA from M7.  Use along with the
 * FBA_IOCTL_GET_COREDUMP_REGIONS and FBA_IOCTL_GET_COREDUMP_REGION_DATA
 * ioctls.
 *
 * RETURNS:
 * 0 - Success.
 * -EFAULT - If IOCTL struct is invalid.
 */
#define FBA_IOCTL_GET_COREDUMP_INFO \
	_IOWR('f', 14, struct get_coredump_info_ioctl)

/*
 * Get coredump regions' info.
 *
 * Returns the coredump region information provided by the device
 *
 * RETURNS:
 * 0 - Success.
 * -EFAULT - If IOCTL struct is invalid or if pointers in struct are invalid.
 * -ENOMEM - If memory allocation fails.
 * -EINVAL - If size of the buffer provided by user is less than amount needed.
 */
#define FBA_IOCTL_GET_COREDUMP_REGIONS \
	_IOWR('f', 15, struct get_coredump_region_ioctl)

/*
 * Get coredump region data.
 *
 * Returns the coredump data for a given coredump address
 *
 * RETURNS:
 * 0 - Success.
 * -EFAULT - If IOCTL struct is invalid or if pointers in struct are invalid.
 */
#define FBA_IOCTL_GET_COREDUMP_REGION_DATA \
	_IOWR('f', 16, struct get_coredump_region_data_ioctl)

/*
 * Read Monitoring Definitions
 *
 * Returns monitoring items and groups definitions.
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_MON_DEFS _IOWR('f', 17, struct mon_defs_ioctl)

/*
 * Read Monitoring current state
 *
 * Returns the state of requested monitoring items.
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_MON_DATA _IOR('f', 18, struct mon_data_ioctl)

/*
 * Create a streaming context
 *
 * Creates a streaming context for posting streaming requests
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_STREAM_CREATE _IOR('f', 19, struct stream_ioctl)

/*
 * Unlock Secure Debug capabilities
 *
 * Downloads binary with Secure Debug certificate chain to enable debugging
 * features on a provisioned device
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_SECURE_DBG_UNLOCK _IO('f', 20)

/*
 * Load/run an executable payload HBIN
 *
 * Downloads & executes a payload executable HBIN. Results from the FW, if any,
 * are returned in an optional user provided buffer
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_PAYLOAD_EXEC _IOWR('f', 25, struct payload_exec_ioctl)

/*
 * Return limit parameters
 *
 * Return limit parameters for the device in associated fba_limit_params_ioctl
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_GET_LIMIT_PARAMS _IO('f', 22)

/*
 * Export allocated resource
 *
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 */
#define FBA_IOCTL_P2P_RESOURCE_EXPORT \
	_IOWR('f', 23, struct res_p2p_export_ioctl)

/*
 * Set Monitoring data
 *
 * Sets the data of requested monitoring items.
 *
 * RETURNS:
 *   0 - Success.
 *   non-zero - Other, lower level errors.
 *   EACCES	on attempt to write to a monitoring entry that has no write callback
 *   or the values are not successfully write
 *   EINVAL for invalid ids.
 *   EFAULT failed to copy values from host
 */
#define FBA_IOCTL_MON_SET_DATA _IOR('f', 24, struct mon_data_ioctl)
