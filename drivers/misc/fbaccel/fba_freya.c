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
#define FREYA_CXRAM_BASE 0x20F00000
#define FREYA_CXRAM_SIZE 0x100000

#include "fba_freya_priv.h"
#include "fba_device.h"
#include "fba_proto.h"

#define FREYA_DRV_NAME "freya"

/* Used to give each class of device it's own id space */
static DEFINE_IDA(freya_ida);

struct fbia_priv_data {
	uint64_t cxram_sideband_ofs;
};

struct freya_priv {
	const struct fbia_priv_data *data;
};

static uint32_t freya_read_cxram_sb(struct fba_device *dev, uint32_t sb_offset)
{
	struct freya_priv *priv = dev->impl_priv;
	uint64_t offset = priv->data->cxram_sideband_ofs + sb_offset;

	return fba_read_bar(dev, BAR_CXRAM, offset);
}

static void freya_write_cxram_sb(struct fba_device *dev, uint32_t val,
				 uint32_t sb_offset)
{
	struct freya_priv *priv = dev->impl_priv;
	uint64_t offset = priv->data->cxram_sideband_ofs + sb_offset;

	fba_write_bar(dev, val, BAR_CXRAM, offset);
}

static inline void cxram_sideband_set_mask(struct fba_device *dev,
					   uint32_t mask, uint32_t newval,
					   uint64_t offset)
{
	uint32_t value;

	value = freya_read_cxram_sb(dev, offset);
	value &= ~mask;
	value |= newval;
	freya_write_cxram_sb(dev, value, offset);
}

static inline void cxram_sideband_setbit(struct fba_device *dev,
					 uint32_t setbits, uint64_t offset)
{
	uint32_t value;

	value = freya_read_cxram_sb(dev, offset);
	value |= setbits;
	freya_write_cxram_sb(dev, value, offset);
}

static inline void cxram_sideband_clearbit(struct fba_device *dev,
					   uint32_t clearbits, uint64_t offset)
{
	uint32_t value;

	value = freya_read_cxram_sb(dev, offset);
	value &= ~clearbits;
	freya_write_cxram_sb(dev, value, offset);
}

// Last host IPC mailbox group (31) is assigned to Freya M7, the first entry's
// callback is set to reset CCP and CIP. The second entry serves as a status
// to let the driver know that the CCP CIP reset is done
static int reset_ccp(struct fba_device *fba_dev)
{
	int rc;

	fba_dev_info(fba_dev, "Interrupting M7 to reset CCP and CIP.");
	set_mailbox(fba_dev, M7_MBOX_CTRL, CCP_CIP_RESET);
	fba_dev_info(fba_dev, "Waiting for M7 to reset CCP and CIP.");
	rc = fba_wait_for_state_nobail(fba_dev, M7_MBOX_STATUS,
				       CCP_CIP_RESET_DONE,
				       fba_dev->num_retries);
	set_mailbox(fba_dev, M7_MBOX_CTRL, STATUS_RESET);
	rc = fba_wait_for_state_nobail(fba_dev, M7_MBOX_STATUS,
				       STATUS_RESET_DONE, fba_dev->num_retries);
	return rc;
}

static int freya_reset_controllers(struct fba_device *fba_dev)
{
	return reset_ccp(fba_dev);
}

static int freya_init_priv(struct fba_device *fba_dev,
			   const struct fbia_priv_data *data)
{
	struct freya_priv *priv;

	priv = kzalloc(sizeof(struct freya_priv), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	priv->data = data;
	fba_dev->impl_priv = priv;

	return 0;
}

static const struct fbia_priv_data freya_data = {
	.cxram_sideband_ofs = FREYA_CXRAM_SIZE - SIDEBAND_SIZE,
};

static int freya_init(struct fba_device *fba_dev)
{
	struct freya_priv *priv;
	int rc;

	rc = freya_init_priv(fba_dev, &freya_data);
	if (rc != 0)
		return rc;

	priv = fba_dev->impl_priv;

	return 0;
}

static void freya_exit(struct fba_device *fba_dev)
{
	kfree(fba_dev->impl_priv);
}

static const struct fba_impl_ops freya_impl_ops = {
	.impl_init = freya_init,
	.impl_exit = freya_exit,
	.reset_controller = freya_reset_controllers,
};

const struct fba_cfg freya_cfg = {
	.name = FREYA_DRV_NAME,
	.devnode_ida = &freya_ida,
	.host_ipc_bar = HOST_IPC_BAR,
	.host_ipc_stride = HOST_IPC_STRIDE,
	.host_ipc_offset = HOST_IPC_BAR_OFF,
	.db_mbox_base = DB_MAILBOX_BASE,
	.host_sq_wc_bar = HOST_SQ_BAR,
	.impl_ops = &freya_impl_ops,
};
