// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
//
// Tenjin Driver
//
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/version.h>

#include "fba_device.h"
#include "fba_tenjin_priv.h"
#include "fba_proto.h"

#define DRV_NAME "tenjin"

/* Used to give each class of device it's own id space */
static DEFINE_IDA(tenjin_ida);

const struct fba_cfg tenjin_cfg = {
	.name = DRV_NAME,
	.devnode_ida = &tenjin_ida,
	.host_ipc_bar = HOST_IPC_BAR,
	.host_ipc_stride = HOST_IPC_STRIDE,
	.host_ipc_offset = HOST_IPC_BAR_OFF,
	.db_mbox_base = DB_MAILBOX_BASE,
	.host_sq_wc_bar = -1,
};
