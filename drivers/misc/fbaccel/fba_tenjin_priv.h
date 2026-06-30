/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/* squeue */
#define CONFIG_MAX_PACKETS_PER_QUEUE 4096

/*
 *
 * Mailbox
 *
 * Number of groups.
 */
#define HW_MBOX_GROUP_COUNT 32
/* Number of entries per group. */
#define HW_MBOX_GROUP_SIZE 32

/* PCIe UART mailbox group offset and length */
#define UART_MBOX_GROUP_OFFS 24

#define TENJIN_UART_MBOX_OFFSET (UART_MBOX_GROUP_OFFS * HW_MBOX_GROUP_SIZE)

#define CCP_COREDUMP_MBOX_GROUP_OFFS 30
#define CCP_COREDUMP_MBOX_ID (CCP_COREDUMP_MBOX_GROUP_OFFS * HW_MBOX_GROUP_SIZE)
#define HEARTBEAT_MAILBOX_ACK 0xFFFFFFFF

#define M7_COREDUMP_MBOX_GROUP_OFFS 31
#define M7_COREDUMP_MBOX_ID (M7_COREDUMP_MBOX_GROUP_OFFS * HW_MBOX_GROUP_SIZE)

#define LOGBUF_MBOX_GROUP_START (3 * HW_MBOX_GROUP_SIZE)
#define CCP_LOGBUF_SIZE_MBOX (LOGBUF_MBOX_GROUP_START + 1)
#define CCP_LOGBUF_OFFSET_MBOX (LOGBUF_MBOX_GROUP_START + 2)

/* Byte stride between mailbox entries */
#define HW_MBOX_ENTRY_STRIDE 4

/*
 * Number or bytes in each mailbox group
 */
#define HW_MBOX_GROUP_STRIDE (HW_MBOX_ENTRY_STRIDE * HW_MBOX_GROUP_SIZE)

/*
 *
 * Feature BAR Settings
 *
 */
#define HOST_IPC_BAR 0
#define HOST_IPC_ENTRY_STRIDE HW_MBOX_ENTRY_STRIDE
#define HOST_IPC_GROUP_STRIDE HW_MBOX_GROUP_STRIDE
#define HOST_IPC_BAR_OFF 0x2000

#define HOST_USOC_BAR 0
#define HOST_USOC_BAR_OFF 0x5000

#define TENJIN_USOC_BAR HOST_USOC_BAR
#define TENJIN_USOC_PFN_OFS (HOST_USOC_BAR_OFF >> PAGE_SHIFT)

#define HOST_SQ_BAR 2

#define M7_RESET_BAR 0
#define M7_RESET_OFFSET 0x49c
#define M7_RESET_MAILBOX_VAL 0xffffffff

/* Doorbell mailbox offset */
#define DB_MAILBOX_BASE HW_MBOX_GROUP_COUNT

/*
 *
 * SAIL Configuration
 *
 * Information about configuration of the communication between the
 * device and the host.
 */

/*
 *
 * Usage Caps
 *
 * Max number of `struct fba_submission` per Context.
 */
#define CONTEXT_SUB_CAP 4096

#define COREDUMP_M7_SIZE (16 * 1024)
#define COREDUMP_CCP_SIZE (64 * 1024)
#define COREDUMP_A72_SIZE (8 * 1024)
#define RISCV_BOOT_ARGS_REGION_SIZE 4096
/*
 * Use low vector numbers for fixed functionality
 * The rest of the vectors are designated for completion queues
 */
#define USOC_DS_MSIX_VECTOR 0
#define USOC_US_MSIX_VECTOR 1
#define UART_TX_MSIX_VECTOR 2
#define UART_RX_MSIX_VECTOR 3

#define TENJIN_DEF_BOOT1_FILENAME_UNSIGNED "tenjin-boot1-ecc.bin.unsigned"
#define TENJIN_DEF_BOOT1_FILENAME_SIGNED "tenjin-boot1-ecc.bin.signed"
#define TENJIN_DEF_BOOT2_FILENAME_UNSIGNED "tenjin-app.bin.unsigned"
#define TENJIN_DEF_BOOT2_FILENAME_SIGNED "tenjin-app.bin.signed"
