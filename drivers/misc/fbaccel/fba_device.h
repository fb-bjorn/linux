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

#include "fba_ioctl.h"

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

#define CHANNEL_MSIX_VECTOR_BASE 8

#define BAR_CNT 6

// Retry count while waiting for device to change state
#define MAX_RETRIES 50
#define MIN_RETRIES 5

// MAX number of channels
// This will be reduced to the number of advertised channels by the device
// and the number of MSIX interrupts available
#define MAX_CHANNELS 128

struct fba_ctx;
struct fba_channel;
struct fba_device;
struct fba_impl_ops;

// Limit counters used track usage by job and current counts, or limits
// by context and channel.
struct fba_limits {
	uint32_t sub_cnt;
};

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

	// Number of scratch pages to allocate
	int scratch_page_cnt;

	// Host SQ BAR -- SQ bar is mapped in WC mode if not set to -1
	int host_sq_wc_bar;

	// Implementation-defined operations
	const struct fba_impl_ops *impl_ops;

	/* Special boot mode type. */
	const char *boot_mode;
};

extern const struct fba_cfg freya_cfg;
extern const struct fba_cfg artemis_cfg;
extern const struct fba_cfg tenjin_cfg;

// Contains information on shared memory areas.  If addr is non-zero
// points to host allocated dma coherent.  If addr is zero, points to
// to device memory via a device BAR.
struct fba_shared {
	union {
		void *data;
		void __iomem *iodata;
	};
	dma_addr_t addr;
	size_t size;
};

struct fba_impl_ops {
	/* Implementation defined init */
	int (*impl_init)(struct fba_device *dev);
	/* Implementation defined exit */
	void (*impl_exit)(struct fba_device *dev);

	// Boot APIs
	// Get device-specific boot mode
	const char *(*get_boot_mode)(struct fba_device *dev);
	// Set device-specific boot mode
	int (*set_boot_mode)(struct fba_device *dev, const char *mode);
	// Decide whether to load signed or unsigned firmware
	bool (*is_secure_boot)(struct fba_device *dev);
	// Get firmware default boot1 and boot2 filenames
	void (*get_fw_filenames)(struct fba_device *dev, const char **boot1,
				 const char **boot2);
	// Load firmware
	int (*load_firmware)(struct fba_device *dev);
	// Reset CCP and SCP
	int (*reset_controller)(struct fba_device *dev);
};

#include "fba_channel.h"
#include "fba_ctx.h"
#include "fba_resource.h"
#include "fba_shared.h"
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

	atomic_t allow_open;

	// TEMP DMA data used by Testing and Admin for Info Words, Resources.
	struct fba_shared cqs_area; // Area for all Completion Qs.
	struct fba_shared sqs_area; // Area for all Submission Qs.

	struct mutex area_mutex; // Protects access to following
	struct fba_shared scratch_area; // Scratch Area.
	struct fba_shared status_area; // Status Area.
	struct fba_shared debug_area; // Debug Area.

	// Usage limits.
	struct fba_limits chan_limits;

	void __iomem *bars[BAR_CNT];
	uint32_t channel_cnt;
	uint32_t channel_entries;
	uint32_t channel_max_packet_size;
	struct fba_channel channels[MAX_CHANNELS];
	uint32_t channel_total;
	struct ida channel_ids;

	// Application Specific INFO and Resources
	uint64_t *info_data; // Pointer to device INFO WORDS.
	uint64_t info_cnt; // Number of INFO words.
	uint64_t resource_cnt; // Number of FW resources.
	uint64_t max_dma_segment_size; // Max DMA segment size
	uint64_t platform_type; // Device platform type
	struct fba_resource **resources; // Pointer to resouces.
	spinlock_t resources_lock; // Lock protecting access to resources

	// Context ID == 0 is reserved for the admin context.
	// Vend IDs in the range [1, n_ctxs] (inclusive).
	unsigned int n_ctxs;

	struct mutex ctx_mtx; // Mutex to protect ctx_list
	struct list_head ctx_list; // Contexts on this device.

	struct workqueue_struct *msix_wq; // queue for msix triggered work

	atomic_t has_failed;
	atomic_t reset_ok;
	bool exiting;
	struct completion abort_done;
	bool pcie_boot_en;
	atomic_t waiting_for_load;
	char boot1_filename[NAME_MAX + 1];
	char boot2_filename[NAME_MAX + 1];

	// Admin channel command timeout value in ms
	uint32_t admin_cmd_timeout_ms;
	// Number of retries for state update check
	int num_retries;
};

struct fba_device *fba_device_get(struct fba_device *fba_dev);
void fba_device_put(struct fba_device *fba_dev);
struct fba_device *fba_device_lookup(unsigned int minor);

// Get a sail channel
struct fba_channel *fba_get_channel(struct fba_device *dev, int sail_channel);

// Device Operations
void fba_device_add_ctx(struct fba_device *fba_dev, struct list_head *entry);
void fba_device_del_ctx(struct fba_device *fba_dev, struct list_head *entry);
int fba_device_get_ctx_count(struct fba_device *fba_dev);

/* Called to quiesce device on fatal error */
void fba_device_abort(struct fba_device *fba_dev, const char *reason);

// File Operations
extern const struct file_operations fba_fops;

//
// State functions
//
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
