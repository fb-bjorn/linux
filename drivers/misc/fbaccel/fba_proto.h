/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/* SAIL firmware communication */

/*
 * SAIL Negotiation Registers
 *
 * Registers are used to negotiate setup of the channel(s) prior to standard
 * inband Client/Server.  The Server defines the main channel properties,
 * which the Client must confirm it can use prior to activation.
 *
 * Client Rd registers must be provided by Server prior to setting SAIL_RDY.
 * The Client will read the values, confirm compatibility, then program any
 * Client Wr registers. Once programmed, client will request activation by
 * setting ACTIVE bit of SAIL_CTRL.  The Server will detect the change and
 * read Client Wr registers to fetch remaing config data.  Server will finish
 * any setup and when done it will clear SAIL_RDY and set ACTIVE bit in
 * SAIL_STAT to indicate setup is finished.
 *
 * While ACTIVE bit is set, Client is free to use the channels.
 */
enum sail_regs {
	SAIL_CTRL = 0x00, // Control Register (Client Wr)
	SAIL_STAT = 0x01, // Status Register (Client Rd)
	SAIL_BDES = 0x02, // Buffer Descriptor (Client Rd)
	SAIL_CDES = 0x03, // Channel Descriptor (Client Rd)
	SAIL_HDES = 0x04, // Host Descriptor (Client Wr)
	SAIL_RDY = 0x05, // Ready Marker (Client Rd)
	SAIL_SQAL = 0x06, // SQ Addr Low (BDES determines ownership)
	SAIL_SQAH = 0x07, // SQ Addr High (BDES determines ownership)
	SAIL_CQAL = 0x08, // CQ Addr Low (BDES determines ownership)
	SAIL_CQAH = 0x09, // CQ Addr High (BDES determines ownership)
	SAIL_SCAL = 0x0A, // Scratch Area Low (Client Wr)
	SAIL_SCAH = 0x0B, // Scratch Area Low (Client Wr)
	SAIL_STAL = 0x0C, // Status Addr Low (Client Wr)
	SAIL_STAH = 0x0D, // Status Addr High (Client Wr)
	SAIL_VERS = 0x0E, // Version Info (Client Rd)
	SAIL_STRM = 0x10, // Streaming channel configuration
	SAIL_ADMIN_CMD_TIMEOUT = 0x11, // Admin Command Timeout time
	SAIL_PKT_SIZE = 0x14, // Packet size configuration
	SAIL_DCMD = 0x18, // Debug Command Registers (Client Wr)
	SAIL_DA0L = 0x1A, // Debug ARG0 Low Registers (Client Wr)
	SAIL_DA0H = 0x1B, // Debug ARG0 High Registers (Client Wr)
	SAIL_DA1L = 0x1C, // Debug ARG0 Low Registers (Client Wr)
	SAIL_DA1H = 0x1D, // Debug ARG0 High Registers (Client Wr)
	SAIL_DA2L = 0x1E, // Debug ARG0 Low Registers (Client Wr)
	SAIL_DA2H = 0x1F, // Debug ARG0 High Registers (Client Wr)
	SAIL_MAILBOX_END,
};

// Assert that SAIL mailboxes do not collide with doorbell mailboxes.
static_assert(SAIL_MAILBOX_END <= 32);

/* Values for SAIL_CTRL and SAIL_STAT */
enum {
	SAIL_DISABLED = 0,
	SAIL_ENABLED = 1,
};

// The magic value the Server writes to SAIL_RDY after populating Client Rd
// registers.
#define SAIL_RDY_MAGIC 0xFACEB00C

/* Value written to status page when IATU setup is completed. */
#define IATU_CONFIG_OFS 0x0
#define IATU_CONFIGURED 0xFACEB00C

/*
 * SAIL Channel Descriptor
 * [00:07] CNUM - Number of channels.
 * [11:08] CQES - CQ Entry Size (2^N)
 * [15:12] SQES - SQ Entry Size (2^N)
 * [31:16] ENTR - Number of entries per channel.
 */
#define SAIL_CDES_CNUM(cdes) ((cdes)&0xFF)
#define SAIL_CDES_CQES(cdes) (1 << (((cdes) >> 8) & 0xF))
#define SAIL_CDES_SQES(cdes) (1 << (((cdes) >> 12) & 0xF))
#define SAIL_CDES_PQEC(cdes) (((cdes) >> 16) & 0xFFFF)
#define SAIL_CDES_ENTS(cdes) (SAIL_CDES_CNUM(cdes) * SAIL_CDES_PQEC(cdes))

#define SAIL_SQ_BYTES(cdes) (SAIL_CDES_SQES(cdes) * SAIL_CDES_ENTS(cdes))
#define SAIL_CQ_BYTES(cdes) (SAIL_CDES_CQES(cdes) * SAIL_CDES_ENTS(cdes))

/*
 * SAIL Buffer Descriptor
 * Describes which channel buffers are provided by Host and which are provived
 * by FW.  Includes number of requested pages for the Status area, which will
 * be backed by Host memory, but sub-allocated by Firmware.
 *
 * [07:00] STPC - Status Page Count (number of 4K pages).
 * [30:30] HCQ - Host provided CQ Buffer.
 * [31:31] HSQ - Host provided SQ Buffer.
 */
#define SAIL_BDES_STPC(bdes) ((bdes)&0xFF) // Size of Status in pages.
#define SAIL_BDES_HCQ (1u << 30) // Host provided CQ
#define SAIL_BDES_HSQ (1u << 31) // Host provided SQ

/*
 * SAIL Host Descriptor
 * Describes host provided buffers and flags.
 *
 * [07:00] FLGS - Host provided flags.
 * [31:24] SCPC - Scratch Page Count (number of 4K pages).
 */
#define SAIL_HDES_DEFINE(scratch_page_cnt, flags)                              \
	((scratch_page_cnt << 24) | (flags))

/*
 * Packet configuration
 * Describes packet constraints
 *
 * [15:00] PACKET_SIZE_MAX - Max submission entries in a legacy packet
 * [31:16] STREAMING_PACKET_SIZE_MAX - Max submission entries in a streaming packet
 */
#define SAIL_PKT_SIZE_MAX(pkt) ((pkt)&0xFFFF)
#define SAIL_PKT_STREAMING_SIZE_MAX(pkt) (((pkt) >> 16) & 0xFFFF)

/*
 * SAIL BAR Index Register
 * SAIL_SQAL/SQAH can be used to determine where SQ is allocated on BAR.
 *
 * [2:0] - Bar Number.
 * [63:3] - Offset.
 */
#define BIR_TO_BAR(bir) (bir & 0x7)
#define BIR_TO_OFFSET(bir) (bir & ~0x7)

/*
 * Streaming channel parameters
 * Describes parameters used to set up streaming channels
 *
 * [15:00] CNUM - Number of channels
 * [31:16] PQEC - Number of descriptor entries in each channel
 */
#define SAIL_STRM_CNUM(strm) ((strm)&0xFFFF)
#define SAIL_STRM_PQEC(strm) (((strm) >> 16) & 0xFFFF)

#define SAIL_SCRATCH_PAGE_CNT 16
#define SAIL_STATUS_PAGE_CNT 1
#define SAIL_DEBUG_PAGE_CNT 16
#define SAIL_SCRATCH_SIZE (4096 * SAIL_SCRATCH_PAGE_CNT)
#define SAIL_STATUS_SIZE (4096 * SAIL_STATUS_PAGE_CNT)
#define SAIL_DEBUG_SIZE (4096 * SAIL_DEBUG_PAGE_CNT)

/* SAIL channel protocol */

/* Fixed channel numbers */
#define SAIL_CHANNEL_ADMIN 0
#define SAIL_CHANNEL_EXE 1

// Admin command timeouts
#define SAIL_ADMIN_CMD_MAX_TIMEOUT_MS 50000
#define SAIL_ADMIN_CMD_MIN_TIMEOUT_MS 6000

/* Possible values for the opcode field on the admin submission queue */
enum admin_opcode {
	// Do nothing / ping.
	ADMIN_NOOP = 0x100,

	// Create device-side context.
	ADMIN_CREATE_CONTEXT,

	// Delete device-side context begin.
	// store the context-id in a delete_inprogress_ctxids list
	// and don't execute callbacks for contexts that are in this list
	ADMIN_DELETE_CONTEXT_BEGIN,

	// Delete device-side context end.
	// remove the context-id from the delete_inprogress_ctxids
	ADMIN_DELETE_CONTEXT_END,

	// Get one or more device info-words.
	ADMIN_GET_INFO_WORDS,

	// Return the requested resource information.
	ADMIN_GET_RESOURCES,

	// Add an ASYNC message to the alert service.
	ADMIN_ASYNC_ALERT,

	// Add an ASYNC message to the log service.
	ADMIN_ASYNC_LOG,

	// Read monitoring item definitions.
	ADMIN_MON_READ_ITEMS,

	// Read monitoring group definitions.
	ADMIN_MON_READ_GROUPS,

	// Read monitoring data.
	ADMIN_MON_READ_DATA,

	// Write monitoring data.
	ADMIN_MON_WRITE_DATA,

	// Set device "wall clock"
	ADMIN_TIME_SET,
};

/*
 * We only need EXE_NOOP, the other exe opcodes are solely userspace/firmware
 * contract, so we don't define an enum exe_opcode.
 */
#define EXE_NOOP 0x200

/* Argument type field definitions */
#define AT_FIELD_BIT_WIDTH 4
#define AT_FIELD_BIT_MASK ((1 << AT_FIELD_BIT_WIDTH) - 1)

enum argument_type {
	AT_RAW = 0, // Raw data.
	AT_RID, // 64b resource ID.
	AT_RID_ARG32, // 64b resource ID followed by 32b arg.
	AT_ARG32_RID, // 32b arg followed by 64b resource ID.
	AT_ARG64_RID, // 64b arg followed by 64b resource ID.
	AT_VA_DMA64, // 64b host VA followed by 64b DMA op desc.
	AT_P2P_DMA64, // 64b global RID followed by 64b DMA op desc.
};

#define AT_DMA_FLAG_SHIFT 56
#define AT_DMA_SIZE_MASK (BIT_ULL(AT_DMA_FLAG_SHIFT) - 1ULL) // Size mask.
#define AT_DMA_TO_DEVICE (0x01ULL << AT_DMA_FLAG_SHIFT) // To device op.
#define AT_DMA_FROM_DEVICE (0x02ULL << AT_DMA_FLAG_SHIFT) // From device op.
#define AT_DMA_PRP (0x40ULL << AT_DMA_FLAG_SHIFT) // Addr is PRP not Contig.
#define AT_DMA_OPSZ (0x80ULL << AT_DMA_FLAG_SHIFT) // Size as OP Flags.
#define AT_DMA_GET_SZ(x) ((x)&AT_DMA_SIZE_MASK)
#define AT_DMA_GET_OP(x) ((x) & ~AT_DMA_SIZE_MASK)
#define AT_DMA_GET_DIR(x) ((x) & (AT_DMA_TO_DEVICE | AT_DMA_FROM_DEVICE))

/* All submission queue entries start with the same 16-byte header. */
struct submission_header {
	// Fields provided by USER
	uint16_t opcode; // Identifies command and args format.
	uint16_t reserved_usr; // 16b reserved value.
	uint32_t flags; // 31:24 CMD Flags, 23:0 ARG Flags.

	// Fields filled in by DRIVER
	uint16_t command_id; // Opaque ID mirrored on completion.
	uint16_t packet_tail;
	uint8_t context_id;
	uint8_t reserved_drv[3];
};
static_assert(sizeof(struct submission_header) == 16);

// 64-byte submission queue entry.
#define SUBMISSION_ENTRY_ALIGNMENT 64

// RID Bit fields
#define RID_ID_BITS 16
#define RID_ID_SHIFT (64 - RID_ID_BITS)
#define RID_OFFS_BITS (64 - RID_ID_BITS)
#define RID_OFFS_MASK (BIT_ULL(RID_OFFS_BITS) - 1)

#define SUBMISSION_ENTRY_ARGS 6

// Payload for ADMIN_{CREATE,DELETE}_CONTEXT
struct admin_ctx {
	// Context ID to create/delete.
	uint8_t id;
};

// Platform types
#define PLATFORM_SILICON 0
#define PLATFORM_NON_SILICON 1

// Information word identifiers. These device information words can be read with
// the ADMIN_GET_INFO_WORDS command.
enum info_word {
	INFO_WORD_FIRST = 0,
	INFO_WORD_MAX_COUNT = INFO_WORD_FIRST,

	// Max concurrent contexts that are supported at once.
	INFO_CONTEXT_MAX_COUNT,

	// Count of resources that have been exposed by the target device for use
	// by the host.
	INFO_RESOURCE_MAX_COUNT,

	// Maximum DMA segment size
	INFO_DMA_MAX_SEGMENT_SIZE,

	// uint16_t count of monitoring items.
	INFO_MON_ITEM_COUNT,

	// uint16_t count of monitoring groups.
	INFO_MON_GROUP_COUNT,

	// Device platform identifier to differentiate between silicon and non-silicon
	// devices.
	INFO_PLATFORM_TYPE,

	// Number of fixed words, start of application specific words.
	INFO_WORD_APP_START,
};

// Payload for ADMIN_GET_INFO_WORDS.
//
// The requested info words are copied to host memory.
struct admin_info_words {
	// First 64-bit info word to read. See enum info_word.
	uint32_t first_word;

	// Number of consecutive 64-bit info words to copy into host memory.
	uint32_t num_words;

	// Destination address in host memory where info words are written.
	uint64_t host_addr;
};

enum resource_type {
	RT_UNKNOWN = 0,
	// Memory resources (could be DRAM, SRAM etc.)
	RT_MEMORY,
	// Compute core resources
	RT_COMPUTE,
	// Synchronization primitive resources
	RT_SYNC,
};

enum allocator_type {
	AT_UNKNOWN = 0,
	AT_BLOCK, // Standard malloc/free
	AT_BITMAP, // Bitmap pattern.
};

enum mem_type {
	MT_UNKNOWN = 0,
	MT_DRAM = 1,
	MT_SRAM = 2,
	MT_SCRATCH = 3,
	MT_P2P_DRAM = 4,
	MT_ECC_DRAM = 5,
	MT_P2P_ECC_DRAM = 6,
};

struct resource_def {
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
	uint64_t count;
	// Granularity of resources e.g. for memory this could imply alignment.
	uint64_t gran;
};
// Ensure struct is 8-byte aligned.
static_assert((sizeof(struct resource_def) % sizeof(uint64_t)) == 0);

// analogous to struct timespec in time.h without with 32-bit / 64-bit ambiguity removed
struct time_spec_64 {
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct va_dma_op {
	uint64_t host_addr;
	// flags_size is used differently for prp vs. contiguous dma transfers:
	// - requests for prp dma transfers set both size and flags.
	// - requests for contiguous dma transfers set only the size.
	uint64_t flags_size;
};

struct fba_submission {
	// Common 16-byte header.
	struct submission_header header;

	// 48-byte payload depends on header.opcode as documented in the structs.
	union {
		struct admin_ctx ctx;
		struct admin_info_words info_words;
		struct time_spec_64 time_spec;
		struct va_dma_op va_dma;
		uint64_t args[SUBMISSION_ENTRY_ARGS];
	};
};
static_assert(sizeof(struct fba_submission) == 64);
static_assert(sizeof(struct fba_submission) % SUBMISSION_ENTRY_ALIGNMENT == 0);

// Responses from the device to the host are all 64 bits in size.
#define COMPLETION_ENTRY_ALIGNMENT 8

// Status codes in completion entries.
enum completion_status {
	CS_OK = 0,
	CS_BAD_OPCODE, // Unknown opcode.
	CS_INVAL_QID, // Invalid queue ID.
	CS_INVAL_ALIGN, // Invalid address alignment.
	CS_INVAL_QSIZE, // Invalid queue size.
	CS_INVAL_FUNC, // Invalid callback function.
	CS_FAILED, // Command failed for an unspecified reason.
	CS_TIMEOUT, // Operation timed out.
	CS_QUEUE_EXISTS, // Queue ID already in use.
	CS_QUEUE_MISSING, // No such queue exists.
	CS_STRM_FAILED, // Stream is in failed state.
	CS_NOMEM, // Out of memory.
	CS_BUSY, // Resource is busy.
	CS_NO_ADMINQ, // Operation not allowed for admin queue.
	CS_INVAL_IRQV, // Invalid interrupt vector.
	CS_DMA_ACCESS, // DMA access permission denied.
	CS_BAD_INDEX, // Starting index is out of range.
	CS_BAD_RANGE, // Ending index is out of range.
	CS_BAD_ARG, // Bad argument.
	CS_EIO, // I/O error.
	CS_EMPTY, // No entries available to read.
	CS_INUSE, // ID already in use.
	CS_ENOSPC, // No space left.
	CS_BAD_INFO_WORD,
	CS_CANCEL, // Command was cancelled.
	CS_BAD_FLAGS, // Command had incorrect flags
	CS_DMA_BAD_OPCODE, // DMA Unknown opcode
	CS_QUEUE_BUSY, // Queue is busy
	CS_DMA_BAD_DIRECTION_ARG, // Bad direction arg in DMA
	CS_CTX_DELETE, // Context is being deleted
	CS_INALID_CTX, // context-id is out of range
	CS_ABORT, // command aborted due to hard (non-recoverable) failure
	// Start of application specific status codes
	CS_APP_STATUS = 0x100
};

struct fba_completion {
	// Identifier for the completed command. Copied from the command's header.
	uint16_t command_id;

	// New head pointer for the squeue containing the command. This is used
	// to indicate to the host how many submission queue entries have been
	// consumed by the device so far.
	uint16_t sq_head;

	// Command completion status. See enum completion_status in constants.h.
	uint16_t status;

	// Flags used to interpret status.  If the high bit is set the command
	// failed.
	uint8_t flags;

	// Phase tag. This field is written as 1 on the first pass through the
	// circular buffer, and incremented each time the buffer wraps around.
	// It is used by the host to identify the last entry written by the
	// device.
	//
	// This field only requires 1 bit to function correctly. We use 8 bits
	// as a debugging aid and because the bits are available. We may take
	// some of these bits in the future if we need to add other flags.
	uint8_t phase_tag;
};

static_assert(sizeof(struct fba_completion) == 8);
static_assert(sizeof(struct fba_completion) % COMPLETION_ENTRY_ALIGNMENT == 0);

/*
 * FBA Result Object
 *
 * A result object represents the result for a packet returned
 * from the driver to the application, including the original ID.
 */
struct fba_job_result {
	struct fba_completion cmp;
	uint64_t user_id;
};
static_assert(sizeof(struct fba_job_result) == 16);

/* Coredump (Tenjin style) */

#define COREDUMP_COOKIE 0x43524450

enum coredump_reason {
	// CPU hit abort handler
	COREDUMP_REASON_CPU_CRASH = 1,
	// SW triggered fatal error
	COREDUMP_REASON_FATAL_ERR = 2,
	COREDUMP_REASON_FATAL_DDR_ECC_ERR = 3,
	COREDUMP_REASON_FATAL_SRAM_ECC_ERR = 4,
	COREDUMP_REASON_FATAL_RISCV_WDT_ERR = 5,
};

// Layout of coredump region 0.
// Driver polls this hdr to detect CCP coredump trigger
// 0- struct coredump_hdr
//        cookie = 0x43524450
//        version = (0 << COREDUMP_MAJOR_VERSION_SHIFT)|(1<<COREDUMP_MINOR_VERSION_SHIFT)
//	      size = Size of the coredump region.
//	      started = 1 // CPU sets this before start of coredump
//	      completed = 1 // CPU sets this to 1 after completion of core dump
//	      cpu_id =  COREDUMP_CPU_CCP
//		    coredump_reason = COREDUMP_REASON_CPU_CRASH
// .
// .
// .
// 64   -   struct cpu_coredump_data -  entry 1
//            size = sizeof(COREDUMP_TYPE_CPU_REG0 data)
//            type = COREDUMP_TYPE_CPU_REG0
//      -     data for COREDUMP_TYPE_CPU_REG0
//      -   struct cpu_coredump_data - entry 2
//            size = size of stack dump
//            type = COREDUMP_TYPE_CPU_STACK
//      -     stack[]
//      -   struct cpu_coredump_data  - entry 3
//            size = size of profile buf
//            type = COREDUMP_TYPE_TRACE
//      -     profile_data[]
// .
// Driver polls this hdr to detect SCP coredump trigger
// 1000- struct coredump_hdr
//        cookie = 0x43524450
//        version = (0 << COREDUMP_MAJOR_VERSION_SHIFT)|(1<<COREDUMP_MINOR_VERSION_SHIFT)
//	      size = Allocated size
//	      started = 1 // CPU sets this before start of coredump
//	      completed = 1 // CPU sets this to 1 after completion of core dump
//	      cpu_id =  COREDUMP_CPU_SCP
//		    coredump_reason = COREDUMP_REASON_CPU_CRASH
// 1064 -   struct cpu_coredump_data -  entry 1
//            size = sizeof(COREDUMP_TYPE_CPU_REG0 data)
//            type = COREDUMP_TYPE_CPU_REG0
//      -     data for COREDUMP_TYPE_CPU_REG0
//      -   struct cpu_coredump_data - entry 2
//            size = size of stack dump
//            type = COREDUMP_TYPE_CPU_STACK
//      -     stack[]
//
//

// Device header written by CPU for each region of coredump.
// There can be maximum of 2 regions per CPU.
// Driver always polls region 0 for each CPU to detect coredump trigger.
struct coredump_hdr {
	// Magic identifier written by the device.
	// This is to indicate device has initialized the coredump region with header
	// and driver can poll the region to detect the coredump.
	uint32_t cookie;
	// Version number.
	uint32_t version;
	// Allocated size.
	uint32_t size;
	// Device sets this before start of coredump, Only valid for region 0.
	uint32_t started;
	// Device sets this to 1 after completion of core dump, Only valid for region 0.
	uint32_t completed;
	uint32_t cpu_id;
	// Refer to enum coredump_reason_t, Only valid for region 0
	int32_t coredump_reason;
	// Align it to 32 bytes and reserve some mem for future
	uint32_t reserved[9];
};
