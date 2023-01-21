/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

// squeue
#define CONFIG_MAX_PACKETS_PER_QUEUE 4096

//
// Mailbox
//
// Number of groups.
#define HW_MBOX_GROUP_COUNT 32
// Number of entries per group.
#define HW_MBOX_GROUP_SIZE 32

#define LOGBUF_MBOX_GROUP_START (3 * HW_MBOX_GROUP_SIZE)
#define CCP_LOGBUF_SIZE_MBOX (LOGBUF_MBOX_GROUP_START + 1)
#define CCP_LOGBUF_OFFSET_MBOX (LOGBUF_MBOX_GROUP_START + 2)

// Byte stride between mailbox entries
#define HW_MBOX_ENTRY_STRIDE 4

//
// Feature BAR Settings
//
#define HOST_IPC_BAR 0
#define HOST_IPC_STRIDE HW_MBOX_ENTRY_STRIDE
#define HOST_IPC_BAR_OFF 0x2000

#define HOST_SQ_BAR 2

#define M7_RESET_BAR 0
#define M7_RESET_OFFSET 0x49c
#define M7_RESET_MAILBOX_VAL 0xffffffff

// Doorbell mailbox offset
#define DB_MAILBOX_BASE HW_MBOX_GROUP_COUNT

//
// SAIL Configuration
//
// Information about configuration of the communication between the
// device and the host.

#define COREDUMP_M7_SIZE (16 * 1024)
#define COREDUMP_CCP_SIZE (64 * 1024)
#define RISCV_BOOT_ARGS_REGION_SIZE 4096

#define TENJIN_DEF_BOOT1_FILENAME_UNSIGNED "tenjin-boot1-ecc.bin.unsigned"
#define TENJIN_DEF_BOOT1_FILENAME_SIGNED "tenjin-boot1-ecc.bin.signed"
#define TENJIN_DEF_BOOT2_FILENAME_UNSIGNED "tenjin-app.bin.unsigned"
#define TENJIN_DEF_BOOT2_FILENAME_SIGNED "tenjin-app.bin.signed"
