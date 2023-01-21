/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
/*
 * FBA Driver
 *
 * This is driver provides generic functionality for a channel based interace
 * between the host and device across PCIe.  Command flow:
 *
 *  1- Application opens a file handle creating a context.
 *  2- Application writes a packet (1 or more submissions) to file handle.
 *  3- Driver creates a job, translating submission data (e.g. VA -> PA).
 *  4- Driver writes submission to out submission queue and adds job to pending list.
 *  5- Driver signals device that new submission objects are available.
 *  7- Device copies submissions locally, possibly processing them out of order.
 *  8- Device signals job is done by writing back completion information.
 *  9- Driver finds matching job, stores status, moves job to the complete list.
 * 10- Application reads back result, freeing job.
 */

#pragma once

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/stddef.h>
#include <uapi/linux/limits.h>
#include <linux/types.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>

#define FBACCEL_VERSION "0.34.0"

#define BAR_CNT 6

// Retry count while waiting for device to change state
#define MAX_RETRIES 50
#define MIN_RETRIES 5

struct fba_device;
struct fba_impl_ops;

// Per-application configuration parameters
struct fba_cfg {
	// Driver name prefix
	const char *name;
	struct ida *devnode_ida;

	// Mailbox access params
	int host_ipc_bar;
	int host_ipc_stride;
	uint64_t host_ipc_offset;

	// Starting index of doorbell mailboxes
	int db_mbox_base;

	// Host SQ BAR -- SQ bar is mapped in WC mode if not set to -1
	int host_sq_wc_bar;

	// Implementation-defined operations
	const struct fba_impl_ops *impl_ops;
};

extern const struct fba_cfg freya_cfg;
extern const struct fba_cfg tenjin_cfg;

struct fba_impl_ops {
	/* Implementation defined init */
	int (*impl_init)(struct fba_device *dev);
	/* Implementation defined exit */
	void (*impl_exit)(struct fba_device *dev);

	// Reset CCP and SCP
	int (*reset_controller)(struct fba_device *dev);
};

#include "fba_util.h"

/*
 * FBA Device Object
 *
 * Main object for this device.  The driver is registered as a misc device
 * which is passed back into the driver as private data in the file object.
 * This object is device generic.  Device/Application specific functionality
 * is provided via the function pointers below.
 */
struct fba_device {
	struct device *dev;
	struct pci_dev *pdev;
	struct kref ref;
	const char *name; // Driver name
	int minor;
	int id;

	// Device-specific implementation details
	const struct fba_cfg *cfg;
	void *impl_priv;

	void __iomem *bars[BAR_CNT];

	atomic_t has_failed;
	bool exiting;
	struct completion abort_done;

	// Number of retries for state update check
	int num_retries;
};

struct fba_device *fba_device_get(struct fba_device *fba_dev);
void fba_device_put(struct fba_device *fba_dev);

/* Called to quiesce device on fatal error */
void fba_device_abort(struct fba_device *fba_dev, const char *reason);

//
// State functions
//
enum sail_regs;
int fba_wait_for_state(struct fba_device *dev, enum sail_regs reg,
		       uint32_t state, uint32_t num_retries);
int fba_wait_for_state_nobail(struct fba_device *dev, enum sail_regs reg,
			      uint32_t state, uint32_t num_retries);

#define fba_dev_err(fba_dev, fmt, ...)                                         \
	dev_err(&fba_dev->pdev->dev, "%s: " fmt "\n", fba_dev->name,           \
		##__VA_ARGS__)
#define fba_dev_info(fba_dev, fmt, ...)                                        \
	dev_info(&fba_dev->pdev->dev, "%s: " fmt "\n", fba_dev->name,          \
		 ##__VA_ARGS__)
#define fba_dev_dbg(fba_dev, fmt, ...)                                         \
	dev_dbg(&fba_dev->pdev->dev, "%s: " fmt "\n", fba_dev->name,           \
		##__VA_ARGS__)
#define fba_dev_warn(fba_dev, fmt, ...)                                        \
	dev_warn(&fba_dev->pdev->dev, "%s: " fmt "\n", fba_dev->name,          \
		 ##__VA_ARGS__)
