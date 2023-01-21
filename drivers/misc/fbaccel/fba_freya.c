// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
//
// Freya Driver
//

#include <asm/byteorder.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/version.h>
#include <uapi/linux/limits.h>

#include "fba_freya_priv.h"
#include "fba_device.h"

#define FREYA_DRV_NAME "freya"

/* Used to give each class of device it's own id space */
static DEFINE_IDA(freya_ida);

const struct fba_cfg freya_cfg = {
	.name = FREYA_DRV_NAME,
	.devnode_ida = &freya_ida,
};
