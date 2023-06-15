/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */

/* from fbia/freya/driver.h: */

#pragma once

/* Note: Trust Zone are minimum 4K in size */
#define REGION_SIDEBAND_SIZE (4 * 1024)
#define REGION_INBAND_SIZE (4 * 1024)
#define REGION_UPGRADE_MEM_SIZE (520 * 1024)

#define SRAM_UPGRADE_MEM_SIZE REGION_UPGRADE_MEM_SIZE
#define SIDEBAND_SIZE 0x100
#define SRAM_END_ADDR(cxram_base, cxram_size) (cxram_base + cxram_size)
#define SIDEBAND_BASE_ADDR(cxram_base, cxram_size)                             \
	(SRAM_END_ADDR(cxram_base, cxram_size) - SIDEBAND_SIZE)

#define SRAM_INBAND_BASE_ADDR(cxram_base, cxram_size)                          \
	(SRAM_END_ADDR(cxram_base, cxram_size) - REGION_SIDEBAND_SIZE -        \
	 REGION_INBAND_SIZE)

#define SRAM_UPGRADE_MEM_BASE_ADDR(cxram_base, cxram_size)                     \
	(SRAM_INBAND_BASE_ADDR(cxram_base, cxram_size) - SRAM_UPGRADE_MEM_SIZE)

#define SRAM_UPGRADE_OFS(cxram_base, cxram_size)                               \
	(SRAM_UPGRADE_MEM_BASE_ADDR(cxram_base, cxram_size) - cxram_base)

#define BAR_CXRAM 2

#define SB_FW_IMAGE_TARGET_CMD_OFS 0x90
#define BOOT_CMD BIT(16)
#define FW_IMAGE_DST_MASK GENMASK(15, 8)
#define FW_IMAGE_SRC_MASK GENMASK(7, 0)

/*
 * Device power cycle persistent error signals
 * For detailed sideband structure details please take a look at:
 * https://drive.google.com/file/d/1RlBzqMTK4mSNchBS2XEVeXMc1Jttbcrr/view
 */
#define SB_ASIC_ERROR_TYPE_OFS 0x78
#define ASIC_ERROR_TYPE_MASK GENMASK(31, 24)

#define SB_MODULE_ERROR_TYPE_OFS 0x7C
#define ASIC_TEMP_VALUE_MASK GENMASK(31, 24)
#define ASIC_INTERRUPT_FLAG_MASK GENMASK(23, 16)
#define ASIC_WARNING_FLAG_MASK GENMASK(15, 8)
#define MODULE_ERROR_TYPE_MASK GENMASK(7, 0)
#define ASIC_MAX_TEMP 85

/* Special case for resetting max temp and error counts in one write. */
#define SB_MAX_ASIC_TEMP_OFS 0x7F

#define SB_INT_MEM_ERR_CNT_OFS 0x80
#define ASIC_PEC_BYTE_MASK GENMASK(31, 24)
#define ASIC_SMBUS_ERROR_MASK GENMASK(23, 16)
#define ASIC_EXT_MEM_ERR_CNT_MASK GENMASK(15, 8)
#define ASIC_INT_MEM_ERR_CNT_MASK GENMASK(7, 0)

#define SB_FW_UPDATE_OFS 0x84
#define CONTROL_CLEAR_MASK GENMASK(15, 8)
#define INIT_DOWNLOAD BIT(15)
#define READY_FOR_FW_IMAGE BIT(14)
#define TRANSFER_DONE BIT(11)
#define UPDATE_COMPLETE BIT(10)
#define UPDATE_IN_PROGRESS BIT(9)

#define SB_ERROR_CODE_OFS 0x94
#define ASIC_CERT3_ERROR_CODE_MASK GENMASK(31, 24)
#define ASIC_CERT2_ERROR_CODE_MASK GENMASK(23, 16)
#define ASIC_CERT1_ERROR_CODE_MASK GENMASK(15, 8)
#define ASIC_ERROR_CODE_MASK GENMASK(7, 0)

#define SB_TRANSFER_MEM_START_OFS 0xA4
#define SB_TRANSFER_MEM_MAX_SIZE_OFS 0xA8
#define SB_CIP_CFG1_OFS 0xAC
#define HW_TYPE_MASK GENMASK(31, 28)

#define SB_BOOT_STATUS2_OFS 0xBC
#define DDR_INITIALIZED_MASK BIT(30)
#define BOOT1_COMPLETE_MASK BIT(31)
#define LCS_STATE_MASK GENMASK(26, 24)
#define LCS_STATE_SECURE 5

#define SB_TRANSFER_HEADER_MODE_OFS 0xC4
#define PAYLOAD_EXEC_FUNC_MODE BIT(1)
#define PAYLOAD_EXEC_CUSTOM_ARGS BIT(31)

#define SB_TRANSFER_DATA_PACKET_DATA_OFS 0xC8
#define SB_TRANSFER_DATA_PACKET_UID 0x08E83A39
#define SB_TRANSFER_DATA_PACKET_VERSION 1

#define SB_LPDDR_ECC_CONFIG_OFS 0xF0
#define LPDDR_INLINE_ECC_ENABLED_MASK GENMASK(25, 24)

#define SB_EXEC_STATUS_OFS 0xE8
#define EXEC_STATUS_MASK GENMASK(31, 24)
#define EXEC_STATUS_IDLE 0x00
#define EXEC_STATUS_RUNNING 0x01
#define EXEC_STATUS_COMPLETE 0x02

#define SB_EXEC_RESULT_OFS 0xEC
#define EXEC_RESULT_MASK GENMASK(15, 8)
#define EXEC_RESULT_PASS 0x00
#define EXEC_RESULT_ABORTED 0x01
#define EXEC_RESULT_FAIL 0x02

#define SB_LPDDR_ECC_CONFIG_OFS 0xF0
#define LPDDR_INLINE_ECC_ENABLED_MASK GENMASK(25, 24)

#define FREYA_DEF_MBIST_FILENAME "mbist.hbin"
#define FREYA_DEF_SECURE_DBG_FILENAME "secdbg.hbin"
#define FREYA_DEF_PROVISIONING_FILENAME "provision.hbin"

/* timeouts conservatively set based on measured ASIC boot times */
#define RDY_FOR_IMG_XFER_TIMEOUT_MS 1000
#define IMAGE_RECEIVED_TIMEOUT_MS 100
#define BOOT1_LOAD_TIMEOUT_MS 1000
#define BOOT1_READY_TIMEOUT_MS 30000
#define CCP_LOAD_TIMEOUT_MS 1000
#define M7_HEARTBEAT_TIMEOUT_MS 100
#define ASIC_POLLING_PERIOD_MS 100

#define MBIST_EXEC_TIMEOUT_MS 0x7FFFFFF

#define EMU_DEFAULT_TIMEOUT_MS 60000
#define EMU_CCP_BOOT_RETRIES 500
#define EMU_LONG_DEFAULT_TIMEOUT_MS 120000
#define EMU_LONG_CCP_BOOT_RETRIES 1000
#define SIM_DEFAULT_TIMEOUT_MS 60000
#define SIM_CCP_BOOT_RETRIES 500
#define NON_ASIC_POLLING_PERIOD_MS 1000

#define WAIT_FOR_CCP_BOOT_RETRIES 20
#define WAIT_FOR_M7_DMA_COMPLETION 100

/* Sideband Definitions */
enum image_location {
	LOCATION_SRAM = 0,
	LOCATION_ITCM = 1,
	LOCATION_DTCM = 2,
	LOCATION_A55 = 3,
	LOCATION_RISCV = 4,
	LOCATION_D6_FLASH = 5,
	LOCATION_FW_FLASH = 6,
	LOCATION_PHANTOM_FLASH = 7,
	LOCATION_MBIST = 8,
	LOCATION_A55_CORE0 = 9,
	LOCATION_A55_CORE1 = 10,
	LOCATION_DEBUG_CERTIFICATE = 11,
	LOCATION_UKNOWN = 12,
};

struct payload_exec_data {
	__le32 uid;
	__le32 version;
	__le32 func;
	__le32 arg1;
	__le32 arg2;
	__le32 arg3;
	__le32 packet_xor;
	__le32 unused;
};

struct payload_exec_result {
	__le32 uid;
	__le32 version;
	__le32 ret_buf_addr_low;
	__le32 ret_buf_addr_high;
	__le32 ret_buf_size_low;
	__le32 ret_buf_size_high;
	__le32 packet_xor;
	__le32 unused;
};

/* from fbia/common/includes/cfg.h */

/*
 *
 * Mailbox
 */

/*
 * Currently Freya is using the first mailbox group for SAIL communication
 * and second mailbox group is used for sq/cq doorbells and last mailbox
 * group is dedicated to M7 which is used for signalling M7 to reset CCP and CIP
 *
 * Number of total mailbox groups.
 */
#define HW_MBOX_GROUP_COUNT 32
/* Number of entries per group. */
#define HW_MBOX_GROUP_SIZE 32

/* CCP mailbox group offset and length */
#define CPP_MBOX_GROUP_OFFS 0
#define CCP_MBOX_GROUP_COUNT 31

/* PCIe UART mailbox group offset and length */
#define UART_MBOX_GROUP_OFFS 30
#define UART_MBOX_GROUP_COUNT 1

/* M7 mailbox group offset and length */
#define M7_MBOX_GROUP_OFFS 31
#define M7_MBOX_GROUP_COUNT 1

#define M7_MBOX_GROUP_START (M7_MBOX_GROUP_OFFS * HW_MBOX_GROUP_SIZE)
#define M7_MBOX_CTRL M7_MBOX_GROUP_START

#define M7_MBOX_STATUS (M7_MBOX_GROUP_START + 1)
#define CCP_CIP_RESET 1
#define STATUS_RESET 0
#define CCP_CIP_RESET_DONE 1
#define STATUS_RESET_DONE 0

#define M7_MBOX_DMA_OPCODE_ENTRY (M7_MBOX_GROUP_START + 2)
#define M7_MBOX_DMA_SIZE_ENTRY (M7_MBOX_GROUP_START + 4)
#define M7_MBOX_DMA_DADDR_ENTRY (M7_MBOX_GROUP_START + 6)
#define M7_MBOX_DMA_HADDR_ENTRY (M7_MBOX_GROUP_START + 8)

#define M7_MBOX_DMA_SUBMISSION_ENTRY (M7_MBOX_GROUP_START + 10)
#define DMA_SUBMISSION 1

#define M7_MBOX_DMA_COMPLETION_ENTRY (M7_MBOX_GROUP_START + 11)
#define DMA_COMPLETION 1

#define M7_HEARTBEAT_CTRL_ENTRY (M7_MBOX_GROUP_START + 19)
#define M7_WATCHDOG 1

#define M7_HEARTBEAT_STATUS_ENTRY (M7_MBOX_GROUP_START + 20)
#define M7_HEALTHY 0x80000000

#define M7_LOGBUF_SIZE_MBOX (M7_MBOX_GROUP_START + 21)
#define M7_LOGBUF_OFFSET_MBOX (M7_MBOX_GROUP_START + 22)

/* bootargs offset and max-size */
#define M7_BOOT_ARGS_OFFSET_MBOX (M7_MBOX_GROUP_START + 23)
#define M7_BOOT_ARGS_SIZE_MBOX (M7_MBOX_GROUP_START + 24)

/* iatu status page address, size, and setup callback */
#define M7_MBOX_IATU_STS_ADDR_ENTRY (M7_MBOX_GROUP_START + 26)
#define M7_MBOX_IATU_STS_SIZE_ENTRY (M7_MBOX_GROUP_START + 28)
#define M7_MBOX_IATU_SETUP_ENTRY (M7_MBOX_GROUP_START + 29)

#define LOGBUF_MBOX_GROUP_START ((CPP_MBOX_GROUP_OFFS + 3) * HW_MBOX_GROUP_SIZE)
#define CCP_LOGBUF_SIZE_MBOX (LOGBUF_MBOX_GROUP_START + 1)
#define CCP_LOGBUF_OFFSET_MBOX (LOGBUF_MBOX_GROUP_START + 2)

/*
 * Number of total entries
 * Byte stride between mailbox entries
 */
#define HW_MBOX_ENTRY_STRIDE 8

/*
 *
 * Feature BAR Settings
 *
 */
#define HOST_IPC_BAR 1
#define HOST_IPC_STRIDE HW_MBOX_ENTRY_STRIDE
#define HOST_IPC_BAR_OFF 0
#define HOST_IPC_BAR_IDX (BAR_TO_IDX(HOST_IPC_BAR))

/* Doorbell mailbox offset */
#define DB_MAILBOX_BASE HW_MBOX_GROUP_SIZE

#define FREYA_USOC_BAR 1
#define FREYA_USOC_PFN_OFS 2

#define FREYA_UART_BAR 1
#define FREYA_UART_MBOX_OFFSET (UART_MBOX_GROUP_OFFS * HW_MBOX_GROUP_SIZE)

#define HOST_SQ_BAR 2

#define DDR_BAR 4
#define DDR_BAR_OFF 0

#define P2P_BAR 4

#define DRAM_ECC_REGION_GRANULARITY_MB 16

/* Max number of `struct fba_submission` per context. */
#define CONTEXT_SUB_CAP 4096

/* Ultrasoc AXI Communicator */

/*
 * Use low vector numbers for fixed functionality
 * The rest of the vectors are designated for completion queues
 */
#define USOC_DS_MSIX_VECTOR 0
#define USOC_US_MSIX_VECTOR 1
#define UART_TX_MSIX_VECTOR 2
#define UART_RX_MSIX_VECTOR 3
#define COREDUMP_MSIX_VECTOR 4
#define CCP_LOGGER_MSIX_VECTOR 5
#define M7_LOGGER_MSIX_VECTOR 6

#define FREYA_DEF_BOOT1_FILENAME "boot1.hbin"
#define FREYA_DEF_BOOT2_FILENAME "ccp.hbin"

#define ARTEMIS_DEF_BOOT1_FILENAME "artemis-boot1.hbin"
#define ARTEMIS_DEF_BOOT2_FILENAME "artemis-ccp.hbin"

/* from fbia/freya/fbia_coredump.h: */

#define MAGIC_NUMBER 0xFACEB00C

struct coredump_region {
	__le64 region_addr;
	__le64 region_size;
};

/* From fbia/freya/freya_cxram.h: */

/* Freya boot parameter offset index in CXRAM */
#define GDB_ON_BOOT_CXRAM_INDEX 0
#define LLC_CONFIG_CXRAM_INDEX 1
#define PE_PRINT_MODE_CXRAM_INDEX 2
#define ENABLE_PCIE_CONSOLE_CXRAM_INDEX 3
#define CXRAM_PE_IDCACHE_CONFIG_INDEX 4
#define ECC_REGION_SIZE_CXRAM_INDEX 5
#define P2P_ECC_SIZE_CXRAM_INDEX 6
#define CXRAM_BOOT_PARAM_MAX_INDEX 15
#define PL330_MAX_PROGBUF_SIZE 0x40

/* CXRAM offsets for storing boot parameters */
#define CXRAM_BOOT_PARAM_OFFSET PL330_MAX_PROGBUF_SIZE

/*
 * CXRAM offset for storing gdb on Zephyr start up enablement with boot
 * parameter
 */
#define CXRAM_GDB_ON_BOOT_OFFSET                                               \
	(CXRAM_BOOT_PARAM_OFFSET + GDB_ON_BOOT_CXRAM_INDEX)
/* CXRAM offset for storing llc configuration setting through boot parameter */
#define CXRAM_LLC_CONFIG_OFFSET                                                \
	(CXRAM_BOOT_PARAM_OFFSET + (LLC_CONFIG_CXRAM_INDEX * 0x4))
/*
 * CXRAM offset for storing PE lossless print mode enablement through boot
 * parameter
 */
#define CXRAM_PE_PRINT_MODE_OFFSET                                             \
	(CXRAM_BOOT_PARAM_OFFSET + (PE_PRINT_MODE_CXRAM_INDEX * 0x4))
/* CXRAM offset for storing PCIe console enable boot parameter */
#define CXRAM_ENABLE_PCIE_CONSOLE_OFFSET                                       \
	(CXRAM_BOOT_PARAM_OFFSET + (ENABLE_PCIE_CONSOLE_CXRAM_INDEX * 0x4))
/* CXRAM offset for storing PE icache dcache disable config */
#define CXRAM_PE_IDCACHE_CONFIG_OFFSET                                         \
	(CXRAM_BOOT_PARAM_OFFSET + (CXRAM_PE_IDCACHE_CONFIG_INDEX * 0x4))

/* CXRAM offset for storing ECC Region Size */
#define CXRAM_ECC_REGION_SIZE_OFFSET                                           \
	(CXRAM_BOOT_PARAM_OFFSET + (ECC_REGION_SIZE_CXRAM_INDEX * 0x4))

/* CXRAM offset for storing ECC Region Size */
#define CXRAM_P2P_ECC_SIZE_OFFSET                                              \
	(CXRAM_BOOT_PARAM_OFFSET + (P2P_ECC_SIZE_CXRAM_INDEX * 0x4))

/* CXRAM offset for storing core dump region specification */
#define CXRAM_CCP_CRASH_STATUS_OFFSET                                          \
	(CXRAM_BOOT_PARAM_OFFSET + ((CXRAM_BOOT_PARAM_MAX_INDEX + 1) * 0x4))

/* CXRAM offset for storing latest core dump CRC code */
#define CXRAM_CCP_CORE_DUMP_CRC_OFFSET (CXRAM_CCP_CRASH_STATUS_OFFSET + 0x4)

/* CXRAM offset for storing core dump region specification */
#define CXRAM_CORE_DUMP_OFFSET (CXRAM_CCP_CORE_DUMP_CRC_OFFSET + 0x4)

/* CXRAM offset for storing core dump number of regions */
#define CXRAM_CORE_DUMP_NUM_REGIONS_OFFSET (CXRAM_CORE_DUMP_OFFSET + 0x4)

/* CXRAM offset for storing core dump region info */
#define CXRAM_CORE_DUMP_REGION_INFO_OFFSET                                     \
	(CXRAM_CORE_DUMP_NUM_REGIONS_OFFSET + 0x4)

#define FREYA_LOGGER_SRCS 2

/* Offset for Freya boot arguments in CXRAM. */
#define FREYA_CXRAM_BOOT_ARGS_OFFSET 0x8000

/* Max size of Freya boot args in CXRAM. */
