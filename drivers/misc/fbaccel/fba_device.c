// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#include <linux/module.h>
#include <linux/aer.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/async.h>
#include <linux/version.h>

#include "fba_device.h"
#include "fba_admin.h"
#include "fba_uart.h"
#include "fba_usoc.h"

#define FBA_MAX_DEVS 32

static dev_t fba_devt;
static struct class *fba_class;
static struct cdev fba_cdev;
static DEFINE_MUTEX(fba_devices_mtx);
static struct fba_device *fba_devices[FBA_MAX_DEVS];

static bool auto_load = true;
module_param(auto_load, bool, 0664);
MODULE_PARM_DESC(auto_load, "If true, automatically load firmware on probe");

static bool pcie_boot_en = true;
module_param(pcie_boot_en, bool, 0664);
MODULE_PARM_DESC(pcie_boot_en, "Bootflow enablement boot parameter");

/* Module parameter for forcing PRP list usage */
bool test_force_prp;
module_param(test_force_prp, bool, 0664);
MODULE_PARM_DESC(test_force_prp,
		 "If true, force PRP list even when buffer is contiguous");

static void fba_async_probe(void *data, async_cookie_t cookie);

static ssize_t boot_file_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct fba_device *fba_dev = dev_get_drvdata(dev->parent);

	if (strcmp(attr->attr.name, "boot1_filename") == 0)
		return sysfs_emit(buf, "%s\n", fba_dev->boot1_filename);
	else
		return sysfs_emit(buf, "%s\n", fba_dev->boot2_filename);
}

static int set_bootfile_name(struct fba_device *fba_dev, char *dst,
			     const char *src, size_t len)
{
	if (len > NAME_MAX || len <= 0) {
		fba_dev_err(fba_dev, "Bootfile not updated, check length: %zu",
			    len);
		return -EINVAL;
	}

	strscpy(dst, src, NAME_MAX + 1);
	if (dst[len - 1] == '\n')
		dst[len - 1] = '\0';

	return len;
}

static ssize_t boot_file_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct fba_device *fba_dev = dev_get_drvdata(dev->parent);
	char *dst;
	int ret;

	if (strcmp(attr->attr.name, "boot1_filename") == 0)
		dst = fba_dev->boot1_filename;
	else
		dst = fba_dev->boot2_filename;

	ret = set_bootfile_name(fba_dev, dst, buf, count);
	if (ret > 0)
		fba_dev_info(fba_dev, "%s updated to %s", attr->attr.name, dst);

	return ret;
}

static DEVICE_ATTR(boot1_filename, 0644, boot_file_show, boot_file_store);
static DEVICE_ATTR(boot2_filename, 0644, boot_file_show, boot_file_store);

static ssize_t boot_mode_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct fba_device *fba_dev = dev_get_drvdata(dev->parent);

	if (fba_dev->cfg->impl_ops->get_boot_mode != NULL) {
		const char *boot_mode =
			fba_dev->cfg->impl_ops->get_boot_mode(fba_dev);
		return sysfs_emit(buf, "%s\n", boot_mode);
	}

	return sysfs_emit(buf, "normal\n");
}

static ssize_t boot_mode_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct fba_device *fba_dev = dev_get_drvdata(dev->parent);
	int err;

	buf = skip_spaces(buf);

	/* Use device private method if we have one */
	if (fba_dev->cfg->impl_ops->set_boot_mode != NULL) {
		err = fba_dev->cfg->impl_ops->set_boot_mode(fba_dev, buf);
		return (err == 0) ? count : err;
	}

	/* Otherwise accept only "normal" setting */
	if (strncmp(buf, "normal", 6) == 0)
		return count;
	else
		return -EINVAL;
}

static DEVICE_ATTR_RW(boot_mode);

/* Triggers firmware load upon write (any value.) */
static ssize_t load_firmware_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct fba_device *fba_dev = dev_get_drvdata(dev->parent);

	if (atomic_cmpxchg(&fba_dev->waiting_for_load, true, false) != true) {
		/* Not waiting for load */
		return -EINVAL;
	}

	async_schedule(fba_async_probe, fba_dev);
	/* Block until firmware load completes, making scripting simpler. */
	async_synchronize_full();

	return count;
}

static DEVICE_ATTR_WO(load_firmware);

static ssize_t secure_boot_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fba_device *fba_dev = dev_get_drvdata(dev->parent);

	if (fba_dev->cfg->impl_ops->is_secure_boot != NULL) {
		if (fba_dev->cfg->impl_ops->is_secure_boot(fba_dev))
			return sysfs_emit(buf, "secure\n");
	}

	return sysfs_emit(buf, "nonsecure\n");
}

static DEVICE_ATTR_RO(secure_boot);

static ssize_t reset_ok_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct fba_device *fba_dev = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%d\n", atomic_read(&fba_dev->reset_ok));
}

static DEVICE_ATTR_RO(reset_ok);

static struct attribute *fba_device_attrs[] = {
	&dev_attr_boot1_filename.attr,
	&dev_attr_boot2_filename.attr,
	&dev_attr_boot_mode.attr,
	&dev_attr_load_firmware.attr,
	&dev_attr_reset_ok.attr,
	&dev_attr_secure_boot.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fba_device);

struct fba_channel *fba_get_channel(struct fba_device *dev, int i)
{
	return &dev->channels[i];
}

void fba_device_add_ctx(struct fba_device *fba_dev, struct list_head *entry)
{
	mutex_lock(&fba_dev->ctx_mtx);
	list_add_tail(entry, &fba_dev->ctx_list);
	mutex_unlock(&fba_dev->ctx_mtx);
}

void fba_device_del_ctx(struct fba_device *fba_dev, struct list_head *entry)
{
	mutex_lock(&fba_dev->ctx_mtx);
	list_del_init(entry);
	mutex_unlock(&fba_dev->ctx_mtx);
}

int fba_device_get_ctx_count(struct fba_device *fba_dev)
{
	int count;

	mutex_lock(&fba_dev->ctx_mtx);
	count = list_count(&fba_dev->ctx_list);
	mutex_unlock(&fba_dev->ctx_mtx);

	return count;
}

/*
 * Lookup device by minor id and return refcounted device ptr.
 * Returns NULL if device no longer available.
 */
struct fba_device *fba_device_lookup(unsigned int minor)
{
	struct fba_device *fba_dev = NULL;

	if (minor >= ARRAY_SIZE(fba_devices))
		return NULL;

	mutex_lock(&fba_devices_mtx);
	if (fba_devices[minor] != NULL)
		fba_dev = fba_device_get(fba_devices[minor]);
	mutex_unlock(&fba_devices_mtx);

	return fba_dev;
}

static void fba_unregister_subdevs(struct fba_device *fba_dev)
{
	if (fba_dev->usoc_dev != NULL) {
		fba_usoc_unregister(fba_dev->usoc_dev);
		fba_dev->usoc_dev = NULL;
	}

	if (fba_dev->uart_dev != NULL) {
		fba_uart_unregister(fba_dev->uart_dev);
		fba_dev->uart_dev = NULL;
	}
}

static void fba_device_unregister(struct fba_device *fba_dev)
{
	fba_unregister_subdevs(fba_dev);

	mutex_lock(&fba_devices_mtx);
	fba_devices[fba_dev->minor] = NULL;
	device_destroy(fba_class, MKDEV(MAJOR(fba_devt), fba_dev->minor));
	ida_free(fba_dev->cfg->devnode_ida, fba_dev->id);
	mutex_unlock(&fba_devices_mtx);
}

/* Stop new work from being submitted to the device. */
static void fba_device_stop(struct fba_device *fba_dev)
{
	struct list_head *item;
	int i;

	/* Stop polling requests */
	fba_mon_device_stop(&fba_dev->mon);

	/* Stop each context and wait for outstanding work to drain */
	mutex_lock(&fba_dev->ctx_mtx);
	list_for_each(item, &fba_dev->ctx_list) {
		fba_ctx_stop(fba_dev_item_to_fba_ctx(item));
	}
	mutex_unlock(&fba_dev->ctx_mtx);

	fba_channel_stop_heartbeats(fba_dev);
	fba_poller_stop(fba_dev->m7_health_poller);

	/*
	 * At this point there should be no work in channels.  Finish tearing
	 * down channels.
	 */
	for (i = 0; i < fba_dev->channel_total; i++)
		fba_channel_shutdown(fba_get_channel(fba_dev, i));
}

static int fba_device_alloc_queue(struct fba_device *dev,
				  struct fba_shared *area,
				  uint32_t host_queue_mask,
				  enum sail_regs sail_q, size_t q_size,
				  uint32_t bdes)
{
	int rc;

	if (bdes & host_queue_mask) {
		rc = fba_shared_alloc_coherent(dev, area, q_size);
		if (rc)
			return rc;

		set_mailbox64(dev, sail_q, area->addr);
	} else {
		uint64_t bir = get_mailbox64(dev, sail_q);

		fba_shared_alloc_iomem(dev, area, BIR_TO_BAR(bir),
				       BIR_TO_OFFSET(bir), q_size);
	}
	return 0;
}

static int __fba_wait_for_state(struct fba_device *dev, uint32_t reg,
				uint32_t state, uint32_t num_retries,
				bool bail_early)
{
	uint32_t val;
	int i;
	int tries = num_retries;

	if (tries < 1)
		tries = 1;

	for (i = 0; i < tries; i++) {
		/* Bail early if we're unbinding. */
		if (bail_early && READ_ONCE(dev->exiting))
			return -ETIMEDOUT;

		fba_dev_info(dev, "Scan state %d try %d.", reg, i);
		val = get_mailbox(dev, reg);
		if (val == state)
			return 0;
		set_current_state(TASK_UNINTERRUPTIBLE);
		// TODO: T85229364 T85229365 Reduce this to something more sensible, but
		// emulation can be slow.
		schedule_timeout(HZ);
	}
	fba_dev_err(dev, "Failed state %d = %x.", reg, val);
	return -ETIMEDOUT;
}

int fba_wait_for_state(struct fba_device *dev, uint32_t reg, uint32_t state,
		       uint32_t num_retries)
{
	return __fba_wait_for_state(dev, reg, state, num_retries, true);
}

int fba_wait_for_state_nobail(struct fba_device *dev, uint32_t reg,
			      uint32_t state, uint32_t num_retries)
{
	return __fba_wait_for_state(dev, reg, state, num_retries, false);
}

static int fba_wait_for_status(struct fba_device *dev, uint64_t offset,
			       uint32_t state, uint32_t num_retries)
{
	uint32_t val;
	int i;
	int tries = num_retries;

	if (tries < 1)
		tries = 1;

	for (i = 0; i < tries; i++) {
		fba_dev_info(dev, "Wait for status ready, try %d.", i);
		fba_shared_memcpy_from(&dev->status_area, offset, &val,
				       sizeof(uint32_t));
		if (val == state)
			return 0;
		set_current_state(TASK_UNINTERRUPTIBLE);
		// TODO: T85229364 T85229365 Reduce this to something more sensible, but
		// emulation can be slow.
		schedule_timeout(HZ);
	}
	fba_dev_err(dev, "Failed status ready = %x.", val);
	return -ETIMEDOUT;
}

static int fba_device_msix_init(struct fba_device *fba_dev)
{
	struct pci_dev *pdev = fba_dev->pdev;
	int rc, msix_cnt;

	msix_cnt = CHANNEL_MSIX_VECTOR_BASE + fba_dev->channel_total;
	rc = pci_alloc_irq_vectors(pdev, msix_cnt, msix_cnt, PCI_IRQ_MSIX);
	if (rc < 0 || rc != msix_cnt) {
		fba_dev_err(fba_dev, "Failed to allocate IRQ vectors.");
		if (rc >= 0) {
			rc = -ENOSPC;
			pci_free_irq_vectors(pdev);
		}
		return rc;
	}

	return 0;
}

static void fba_device_msix_exit(struct fba_device *fba_dev)
{
	pci_free_irq_vectors(fba_dev->pdev);
}

/* Register UART sub-device */
static void fba_device_uart_register(struct fba_device *fba_dev)
{
	fba_dev->uart_dev = fba_uart_register(
		fba_dev, fba_dev->dev, fba_dev->minor, fba_dev->cfg->name,
		fba_dev->id, fba_dev->cfg->uart_mbox_base,
		fba_dev->cfg->uart_msi_irq_rx, fba_dev->cfg->uart_msi_irq_tx);
	if (IS_ERR(fba_dev->uart_dev)) {
		/* Null so we don't try to unregister an ERR_PTR later on */
		fba_dev->uart_dev = NULL;
	}
}

/* Register UltraSoC sub-device */
static int fba_device_usoc_init(struct fba_device *fba_dev)
{
	void __iomem *usoc_regs;
	int rv = 0;

	if (fba_dev->cfg->usoc_bar >= 0) {
		usoc_regs = fba_util_get_bar_ptr(
			fba_dev, fba_dev->cfg->usoc_bar,
			fba_dev->cfg->usoc_pfn_ofs * PAGE_SIZE);
		fba_dev->usoc_dev = fba_usoc_register(
			fba_dev->pdev, fba_dev->dev, fba_dev->minor,
			fba_dev->cfg->name, fba_dev->id, usoc_regs,
			(pci_resource_start(fba_dev->pdev,
					    fba_dev->cfg->usoc_bar) >>
			 PAGE_SHIFT) +
				fba_dev->cfg->usoc_pfn_ofs,
			fba_dev->cfg->usoc_msi_irq_ds,
			fba_dev->cfg->usoc_msi_irq_us);
		if (IS_ERR(fba_dev->usoc_dev)) {
			rv = PTR_ERR(fba_dev->usoc_dev);
			fba_dev->usoc_dev = NULL;
			fba_dev_err(fba_dev, "Failed to register usoc.");
		}
	}

	return rv;
}

static void fba_device_fw_setup_ptrs(struct fba_device *dev)
{
	uint32_t hdes = SAIL_HDES_DEFINE(dev->cfg->scratch_page_cnt, 0);

	set_mailbox64(dev, SAIL_SCAL, dev->scratch_area.addr);
	set_mailbox64(dev, SAIL_STAL, dev->status_area.addr);

	set_mailbox(dev, SAIL_HDES, hdes);
}

/* Set up channels once firmware is ready */
static int fba_device_fw_channel_init(struct fba_device *dev)
{
	struct fba_queue_info sqinfo, cqinfo;
	uint32_t bdes = get_mailbox(dev, SAIL_BDES);
	uint32_t cdes = get_mailbox(dev, SAIL_CDES);
	uint32_t strm = get_mailbox(dev, SAIL_STRM);
	uint32_t pkt_size = get_mailbox(dev, SAIL_PKT_SIZE);
	size_t q_size;
	int rc, i;

	dev->channel_cnt = SAIL_CDES_CNUM(cdes);
	if (dev->channel_cnt > MAX_CHANNELS)
		dev->channel_cnt = MAX_CHANNELS;
	if (dev->channel_cnt <= SAIL_CHANNEL_EXE) {
		fba_dev_err(
			dev,
			"Device did not advertise enough channels. (advertised=%u, min=%u)",
			dev->channel_cnt, SAIL_CHANNEL_EXE + 1);
		return -EINVAL;
	}

	dev->streaming_channel_cnt = SAIL_STRM_CNUM(strm);
	dev->channel_total = dev->channel_cnt + dev->streaming_channel_cnt;
	if (dev->channel_total > MAX_CHANNELS) {
		fba_dev_warn(
			dev,
			"Limiting channels to max supported.  (advertised=%u, MAX_CHANNELS=%u)",
			dev->channel_total, MAX_CHANNELS);
		dev->streaming_channel_cnt = MAX_CHANNELS - dev->channel_cnt;
		dev->channel_total = MAX_CHANNELS;
	}

	fba_dev_dbg(dev, "using channels = %d, streaming_channels = %d.",
		    dev->channel_cnt, dev->streaming_channel_cnt);

	dev->channel_entries = SAIL_CDES_PQEC(cdes);
	dev->streaming_channel_entries = SAIL_STRM_PQEC(strm);

	dev->channel_max_packet_size = SAIL_PKT_SIZE_MAX(pkt_size);
	dev->streaming_channel_max_packet_size =
		SAIL_PKT_STREAMING_SIZE_MAX(pkt_size);

	dev->chan_limits.sub_cnt = dev->channel_entries;

	dev->streaming_chan_limits.sub_cnt = dev->streaming_channel_entries;

	// Allocate enough space for all submission queues.
	q_size = SAIL_SQ_BYTES(cdes) + dev->streaming_channel_cnt *
					       dev->streaming_channel_entries *
					       SAIL_CDES_SQES(cdes);
	rc = fba_device_alloc_queue(dev, &dev->sqs_area, SAIL_BDES_HSQ,
				    SAIL_SQAL, q_size, bdes);
	if (rc)
		return rc;

	// Allocate enough space for all completion queues.
	q_size = SAIL_CQ_BYTES(cdes) + dev->streaming_channel_cnt *
					       dev->streaming_channel_entries *
					       SAIL_CDES_CQES(cdes);
	rc = fba_device_alloc_queue(dev, &dev->cqs_area, SAIL_BDES_HCQ,
				    SAIL_CQAL, q_size, bdes);
	if (rc)
		goto err_free_sq;

	cqinfo.size = dev->channel_entries * SAIL_CDES_CQES(cdes);
	sqinfo.size = dev->channel_entries * SAIL_CDES_SQES(cdes);

	cqinfo.area = &dev->cqs_area;
	sqinfo.area = &dev->sqs_area;

	cqinfo.offset = 0;
	sqinfo.offset = 0;

	/* Allocate MSIX vectors now that we know the number of channels */
	rc = fba_device_msix_init(dev);
	if (rc != 0)
		goto err_free_cq;

	/* Initialize channels */
	for (i = 0; i < dev->channel_total; i++) {
		struct fba_limits *limits;
		uint64_t max_subs;

		if (i < dev->channel_cnt) {
			cqinfo.size =
				dev->channel_entries * SAIL_CDES_CQES(cdes);
			sqinfo.size =
				dev->channel_entries * SAIL_CDES_SQES(cdes);
			limits = &dev->chan_limits;
			max_subs = dev->channel_max_packet_size;
		} else {
			cqinfo.size = dev->streaming_channel_entries *
				      SAIL_CDES_CQES(cdes);
			sqinfo.size = dev->streaming_channel_entries *
				      SAIL_CDES_SQES(cdes);
			limits = &dev->streaming_chan_limits;
			max_subs = dev->streaming_channel_max_packet_size;
		}

		rc = fba_channel_init(dev, i, CHANNEL_MSIX_VECTOR_BASE + i,
				      &sqinfo, &cqinfo, limits, max_subs);
		if (rc)
			goto err_exit_channels;

		sqinfo.offset += sqinfo.size;
		cqinfo.offset += cqinfo.size;
	}

	fba_device_fw_setup_ptrs(dev);

	return 0;

err_exit_channels:
	while (i-- > 0)
		fba_channel_exit(fba_get_channel(dev, i));

err_free_cq:
	fba_shared_free(dev, &dev->cqs_area);

err_free_sq:
	fba_shared_free(dev, &dev->sqs_area);

	return rc;
}

/* XXX - make this into a module parameter */
#define COREDUMP_TIMEOUT_MS (30 * 1000)

/* Abort operation of device, called upon detection of fatal error. */
void fba_device_abort(struct fba_device *fba_dev, const char *reason)
{
	int i;

	if (atomic_cmpxchg(&fba_dev->has_failed, false, true) != false) {
		/*
		 * We've lost a race with another thread triggering abort.
		 * But is not safe to return until abort process is complete.
		 */
		wait_for_completion(&fba_dev->abort_done);
		return;
	}

	fba_dev_err(fba_dev, "aborting device: %s", reason);

	/*
	 * Coredump via DMA will fail after clearing master.  We need to
	 * wait here until coredump is done.  Assumes that coredump will
	 * be initiated automatically by udev rule and will complete
	 * within proscribed timeout.
	 */
	wait_for_completion_timeout(&fba_dev->coredump_done,
				    msecs_to_jiffies(COREDUMP_TIMEOUT_MS));

	/*
	 * Disable PCI bus master/DMA and wait for any in-flight PCIe
	 * transactions to complete.  After these steps, should be safe
	 * to free any DMA mappings as device can no longer write to them
	 * (assuming it honors clearing Bus Master Enable correctly.)
	 */
	pci_disable_device(fba_dev->pdev);
	pci_wait_for_pending_transaction(fba_dev->pdev);

	/* Mark channels as stopped and fail any outstanding work */
	for (i = 0; i < fba_dev->channel_total; i++)
		fba_channel_shutdown(fba_get_channel(fba_dev, i));

	/* Wake up anyone waiting for abort to complete */
	complete_all(&fba_dev->abort_done);
}

/* Signal coredump has completed, and is safe to fence DMA */
void fba_device_signal_coredump_done(struct fba_device *fba_dev)
{
	complete_all(&fba_dev->coredump_done);
}

static int fba_device_fw_channel_exit(struct fba_device *fba_dev)
{
	int i, rc = 0;

	if (!atomic_read(&fba_dev->has_failed)) {
		// disable sail through mailbox registers
		set_mailbox(fba_dev, SAIL_CTRL, SAIL_DISABLED);
		/* Use _nobail so we do actually wait for state change. */
		rc = fba_wait_for_state_nobail(fba_dev, SAIL_STAT,
					       SAIL_DISABLED,
					       fba_dev->num_retries);
		if (rc) {
			fba_device_abort(fba_dev, "disable device failed");
			fba_dev_err(fba_dev, "Failed to disable device.");
		}
	} else {
		fba_dev_err(fba_dev, "Device failed, skipping disable.");
	}

	if (fba_dev->cfg->impl_ops->coredump_exit != NULL)
		fba_dev->cfg->impl_ops->coredump_exit(fba_dev);
	fba_unregister_subdevs(fba_dev);

	for (i = 0; i < fba_dev->channel_total; i++)
		fba_channel_exit(fba_get_channel(fba_dev, i));

	fba_device_msix_exit(fba_dev);

	fba_shared_free(fba_dev, &fba_dev->cqs_area);
	fba_shared_free(fba_dev, &fba_dev->sqs_area);

	return rc;
}

static uint32_t get_admin_command_timeout(struct fba_device *dev)
{
	return get_mailbox(dev, SAIL_ADMIN_CMD_TIMEOUT);
}

static int fba_device_enable_sail(struct fba_device *dev)
{
	int rc;

	/*
	 * Once we trigger enable of SAIL, we have to avoid touching the device
	 * until it has finished setting up IATU mappings (due to HW
	 * limitations.)  The device will perform a write to host memory to let
	 * us know it is safe to proceed.
	 */
	set_mailbox(dev, SAIL_CTRL, SAIL_ENABLED);
	rc = fba_wait_for_status(dev, IATU_CONFIG_OFS, IATU_CONFIGURED,
				 dev->num_retries);
	if (rc) {
		fba_dev_err(dev, "Failed waiting for status memory signature.");
		return rc;
	}
	// Resetting the ACK signal
	fba_shared_memset(&dev->status_area, IATU_CONFIG_OFS, 0,
			  sizeof(uint32_t));

	rc = fba_wait_for_state(dev, SAIL_STAT, SAIL_ENABLED, dev->num_retries);
	if (rc)
		fba_dev_err(dev, "Failed waiting for state %d.", SAIL_STAT);

	return rc;
}

/* Setup some defaults once we know platform type */
static void fba_device_fw_setup_platform(struct fba_device *dev)
{
	if (dev->platform_type == PLATFORM_SILICON)
		dev->num_retries = MIN_RETRIES;
	else
		dev->num_retries = MAX_RETRIES;

	dev->admin_cmd_timeout_ms = get_admin_command_timeout(dev);
	if (!dev->admin_cmd_timeout_ms) {
		if (dev->platform_type == PLATFORM_SILICON)
			dev->admin_cmd_timeout_ms =
				SAIL_ADMIN_CMD_MIN_TIMEOUT_MS;
		else
			dev->admin_cmd_timeout_ms =
				SAIL_ADMIN_CMD_MAX_TIMEOUT_MS;
	}
}

/* Finish device setup once firmware is ready. */
static int fba_device_fw_init(struct fba_device *dev)
{
	int rc;

	rc = fba_device_fw_channel_init(dev);
	if (rc)
		return rc;

	rc = fba_device_enable_sail(dev);
	if (rc)
		goto err_channel_exit;

	fba_device_uart_register(dev);

	/* Any error logged within fba_device_usoc_init already, but continue if it fails */
	(void)fba_device_usoc_init(dev);

	// Setup Resources
	rc = fba_resource_init(dev);
	if (rc) {
		fba_dev_err(dev, "Failed to init resources.");
		goto err_channel_exit;
	}

	fba_device_fw_setup_platform(dev);

	rc = fba_admin_time_set(dev, ktime_get_real());
	if (rc) {
		fba_dev_err(dev, "Failed to set device time.");
		goto err_resource_exit;
	}

	// coredump and mon init must be after fba_resource_init()
	if (dev->cfg->impl_ops->coredump_init)
		dev->cfg->impl_ops->coredump_init(dev);

	rc = fba_mon_device_init(dev);
	if (rc) {
		fba_dev_err(dev, "Failed initialize monitoring.");
		goto err_resource_exit;
	}

	fba_channel_start_heartbeats(dev);

	return 0;

err_resource_exit:
	fba_resource_exit(dev);
err_channel_exit:
	fba_device_fw_channel_exit(dev);

	return rc;
}

/* Tear down firmware-related device state */
static int fba_device_fw_exit(struct fba_device *fba_dev)
{
	int rc;

	/*
	 * Stop monitoring/polling and wait for any outstanding work to
	 * complete, so that we can safely tear down channels.
	 */
	fba_device_stop(fba_dev);

	fba_mon_device_exit(&fba_dev->mon);

	fba_resource_exit(fba_dev);

	rc = fba_device_fw_channel_exit(fba_dev);

	return rc;
}

static void fba_device_free(struct kref *kref)
{
	struct fba_device *fba_dev = container_of(kref, struct fba_device, ref);

	WARN_ON(fba_device_get_ctx_count(fba_dev) != 0);

	/* Free things that might be referenced by lingering user contexts */
	mutex_destroy(&fba_dev->ctx_mtx);
	ida_destroy(&fba_dev->channel_ids);
	ida_destroy(&fba_dev->ctx_ids);
	destroy_workqueue(fba_dev->msix_wq);
	kfree(fba_dev->name);

	kfree(fba_dev);
}

struct fba_device *fba_device_get(struct fba_device *fba_dev)
{
	kref_get(&fba_dev->ref);
	WARN_ON(kref_read(&fba_dev->ref) == 1);

	return fba_dev;
}

void fba_device_put(struct fba_device *fba_dev)
{
	kref_put(&fba_dev->ref, fba_device_free);
}

static void fba_pci_exit(struct fba_device *fba_dev)
{
	struct pci_dev *pdev = fba_dev->pdev;
	int i;

	/* Unmap whichever bars we successfully mapped. */
	for (i = 0; i < BAR_CNT; i++) {
		if (fba_dev->bars[i])
			pci_iounmap(pdev, fba_dev->bars[i]);
	}

	pci_disable_pcie_error_reporting(pdev);
	pci_clear_master(pdev);
	pci_release_regions(pdev);

	if (pci_is_enabled(pdev))
		pci_disable_device(pdev);
}

static int fba_pci_init(struct fba_device *fba_dev)
{
	struct pci_dev *pdev = fba_dev->pdev;
	int rc, i;

	rc = pci_enable_device(pdev);
	if (rc) {
		fba_dev_err(fba_dev, "Cannot enable PCI device.");
		return rc;
	}

	rc = pci_request_regions(pdev, fba_dev->cfg->name);
	if (rc) {
		fba_dev_err(fba_dev, "Cannot obtain PCI resources.");
		goto err_disable_device;
	}

	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (rc) {
		fba_dev_err(fba_dev, "Failed to set DMA mask.");
		goto err_release_regions;
	}

	dma_set_max_seg_size(&pdev->dev, UINT_MAX);
	dma_set_seg_boundary(&pdev->dev, ULONG_MAX);

	pci_set_master(pdev);
	pci_set_drvdata(pdev, fba_dev);
	pci_enable_pcie_error_reporting(pdev);

	// Disable relaxed ordering on the device.
	rc = pcie_capability_clear_word(pdev, PCI_EXP_DEVCTL,
					PCI_EXP_DEVCTL_RELAX_EN);
	if (rc) {
		fba_dev_err(
			fba_dev,
			"Failed to disable relaxed ordering on PCIe device.");
		goto err_disable_reporting;
	}

	for (i = 0; i < BAR_CNT; i++) {
		if ((pci_resource_flags(pdev, i) & IORESOURCE_MEM) == 0) {
			/* skip over unused BARs to avoid log noise */
			continue;
		}

		if (i == fba_dev->cfg->host_sq_wc_bar)
			fba_dev->bars[i] = pci_ioremap_wc_bar(pdev, i);
		else
			fba_dev->bars[i] = pci_ioremap_bar(pdev, i);

		if (fba_dev->bars[i] == NULL) {
			fba_dev_err(fba_dev, "Failed to map BAR %d.", i);
			rc = -ENOSPC;
			goto err_unmap_bars;
		}
	}

	return 0;

err_unmap_bars:
	while (i-- > 0)
		pci_iounmap(pdev, fba_dev->bars[i]);
err_disable_reporting:
	pci_disable_pcie_error_reporting(pdev);
	pci_clear_master(pdev);
err_release_regions:
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);

	return rc;
}

/* Free shared memory used to communicate with device */
static void fba_shared_area_exit(struct fba_device *fba_dev)
{
	mutex_lock(&fba_dev->area_mutex);
	fba_shared_free(fba_dev, &fba_dev->status_area);
	fba_shared_free(fba_dev, &fba_dev->scratch_area);
	fba_shared_free(fba_dev, &fba_dev->debug_area);
	mutex_unlock(&fba_dev->area_mutex);
}

/* Initialize shared memory used to communicate with device */
static int fba_shared_area_init(struct fba_device *fba_dev)
{
	int rc;

	// Add a dma_alloc_coherent() to provide some mapped in DMA space.
	// We already do something like that in fba_resource to get back
	// info words and resource data.  We could just have a generic
	// block allocate here for use by the driver, and add an IOCTL to
	// read/write into it from user space.

	// Debug Area pages.
	rc = fba_shared_alloc_coherent(fba_dev, &fba_dev->debug_area,
				       SAIL_DEBUG_SIZE);
	if (rc)
		return rc;

	// Temp Area pages.
	rc = fba_shared_alloc_coherent(fba_dev, &fba_dev->scratch_area,
				       SAIL_SCRATCH_SIZE);
	if (rc)
		goto err_free_debug;

	// Allocate Host Status pages.
	rc = fba_shared_alloc_coherent(fba_dev, &fba_dev->status_area,
				       SAIL_STATUS_SIZE);
	if (rc)
		goto err_free_scratch;

	return rc;

err_free_scratch:
	fba_shared_free(fba_dev, &fba_dev->scratch_area);
err_free_debug:
	fba_shared_free(fba_dev, &fba_dev->debug_area);

	return rc;
}

/* Detect secure boot and initialize firmware filenames */
static void fba_init_fw_filenames(struct fba_device *fba_dev)
{
	const char *boot1, *boot2;

	fba_dev->cfg->impl_ops->get_fw_filenames(fba_dev, &boot1, &boot2);
	strscpy(fba_dev->boot1_filename, boot1,
		sizeof(fba_dev->boot1_filename));
	strscpy(fba_dev->boot2_filename, boot2,
		sizeof(fba_dev->boot2_filename));

	fba_dev_info(fba_dev, "boot1_filename set to %s\n",
		     fba_dev->boot1_filename);
	fba_dev_info(fba_dev, "boot2_filename set to %s\n",
		     fba_dev->boot2_filename);
}

/* Load firmware asynchronously (and in parallel) */
static void fba_async_probe(void *data, async_cookie_t cookie)
{
	struct fba_device *fba_dev = data;
	int rc;

	if (!fba_dev->pcie_boot_en) {
		rc = fba_wait_for_state(fba_dev, SAIL_RDY, SAIL_RDY_MAGIC,
					fba_dev->num_retries);
		if (rc != 0)
			return;
	} else {
		rc = fba_dev->cfg->impl_ops->load_firmware(fba_dev);
		if (rc != 0) {
			/*
			 * Mark as failed so attempts to use the device return EIO
			 * instead of EBUSY.
			 */
			fba_device_abort(fba_dev, "firmware load failed");
			return;
		}
	}

	rc = fba_device_fw_init(fba_dev);
	if (rc != 0) {
		fba_dev_err(fba_dev, "Failed to enable device.");
		return;
	}
	fba_dev_info(fba_dev, "Done setting up device, ready for use.");

	atomic_set(&fba_dev->allow_open, true);
}

static int fba_device_init(struct fba_device *fba_dev)
{
	int i, rc;

	kref_init(&fba_dev->ref);
	spin_lock_init(&fba_dev->resources_lock);
	mutex_init(&fba_dev->ctx_mtx);
	mutex_init(&fba_dev->coredump_mutex);
	mutex_init(&fba_dev->area_mutex);
	init_completion(&fba_dev->abort_done);
	init_completion(&fba_dev->coredump_done);

	ida_init(&fba_dev->ctx_ids);
	ida_init(&fba_dev->channel_ids);

	init_waitqueue_head(&fba_dev->read_q);
	init_waitqueue_head(&fba_dev->write_q);
	INIT_LIST_HEAD(&fba_dev->ctx_list);

	rc = -ENOSPC;
	mutex_lock(&fba_devices_mtx);
	for (i = 0; i < FBA_MAX_DEVS; i++) {
		if (fba_devices[i] == NULL) {
			fba_dev->minor = i;
			fba_devices[i] = fba_dev;
			rc = 0;
			break;
		}
	}
	mutex_unlock(&fba_devices_mtx);
	if (rc) {
		dev_err(&fba_dev->pdev->dev, "Failed to allocate device ID.");
		goto err_put_device;
	}

	fba_dev->id = ida_alloc(fba_dev->cfg->devnode_ida, GFP_KERNEL);
	if (fba_dev->id < 0) {
		rc = fba_dev->id;
		goto err_free_minor;
	}

	fba_dev->name =
		kasprintf(GFP_KERNEL, "%s%d", fba_dev->cfg->name, fba_dev->id);
	if (!fba_dev->name) {
		rc = -ENOMEM;
		goto err_free_id;
	}

	fba_dev->msix_wq = alloc_workqueue(fba_dev->name, 0, 0);
	if (!fba_dev->msix_wq) {
		fba_dev_err(fba_dev, "Failed to create workqueue thread.");
		rc = -ENOMEM;
		goto err_free_id;
	}

	rc = fba_pci_init(fba_dev);
	if (rc)
		goto err_free_wq;

	rc = fba_shared_area_init(fba_dev);
	if (rc)
		goto err_pci_exit;

	// Driver is now available for IOCTL communication.
	fba_dev_info(fba_dev, "Done connecting BARs.");

	rc = fba_mon_init(fba_dev);
	if (rc != 0)
		goto err_shared_area_exit;

	if (fba_dev->cfg->impl_ops->impl_init != NULL) {
		rc = fba_dev->cfg->impl_ops->impl_init(fba_dev);
		if (rc != 0)
			goto err_mon_exit;
	}

	fba_init_fw_filenames(fba_dev);

	if (auto_load)
		async_schedule(fba_async_probe, fba_dev);
	else
		atomic_set(&fba_dev->waiting_for_load, true);

	fba_dev->dev = device_create(fba_class, &fba_dev->pdev->dev,
				     MKDEV(MAJOR(fba_devt), fba_dev->minor),
				     NULL, "%s", fba_dev->name);

	return 0;

err_mon_exit:
	fba_mon_exit(&fba_dev->mon);
err_shared_area_exit:
	fba_shared_area_exit(fba_dev);
err_pci_exit:
	fba_pci_exit(fba_dev);
err_free_wq:
	destroy_workqueue(fba_dev->msix_wq);
err_free_id:
	ida_free(fba_dev->cfg->devnode_ida, fba_dev->id);
err_free_minor:
	mutex_lock(&fba_devices_mtx);
	fba_devices[fba_dev->minor] = NULL;
	mutex_unlock(&fba_devices_mtx);
err_put_device:
	fba_device_put(fba_dev);

	return rc;
}

static int fba_device_exit(struct fba_device *fba_dev)
{
	int rc;

	WRITE_ONCE(fba_dev->exiting, true);

	/*
	 * Grab coredump mutex to ensure no coredump DMA is ongoing, and set
	 * coredump_disabled flag to disable any new coredump DMA.
	 */
	mutex_lock(&fba_dev->coredump_mutex);
	fba_dev->coredump_disabled = true;
	mutex_unlock(&fba_dev->coredump_mutex);

	fba_device_unregister(fba_dev);

	if (fba_dev->cfg->impl_ops->impl_exit != NULL)
		fba_dev->cfg->impl_ops->impl_exit(fba_dev);

	rc = fba_device_fw_exit(fba_dev);
	if (rc != 0) {
		fba_dev_err(fba_dev,
			    "Error in deactivating the device. Continue");
	}

	if (!atomic_read(&fba_dev->has_failed)) {
		if (fba_dev->cfg->impl_ops->reset_controller)
			(void)fba_dev->cfg->impl_ops->reset_controller(fba_dev);
	}

	fba_poller_stop(fba_dev->m7_health_poller);
	kfree(fba_dev->m7_health_poller);

	fba_mon_exit(&fba_dev->mon);

	fba_shared_area_exit(fba_dev);
	fba_pci_exit(fba_dev);

	fba_device_put(fba_dev);

	return 0;
}

struct fba_channel *fba_allocate_streaming_channel(struct fba_device *dev)
{
	int chan_id;
	struct fba_channel *chan = NULL;

	chan_id = ida_alloc_range(
		&dev->channel_ids, dev->channel_cnt,
		dev->channel_cnt + dev->streaming_channel_cnt - 1, GFP_KERNEL);
	if (chan_id >= 0)
		chan = fba_get_channel(dev, chan_id);

	return chan;
}

void fba_free_streaming_channel(struct fba_channel *chan)
{
	ida_free(&chan->dev->channel_ids, chan->chan_id);
}

static int fbaccel_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct fba_device *dev;

	dev_info(&pdev->dev, "Probe.\n");
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (dev == NULL)
		return -ENOMEM;

	dev->pdev = pdev;
	dev->cfg = (const struct fba_cfg *)ent->driver_data;

	dev->pcie_boot_en = pcie_boot_en;

	dev->num_retries = MAX_RETRIES;
	dev->admin_cmd_timeout_ms = SAIL_ADMIN_CMD_MAX_TIMEOUT_MS;

	return fba_device_init(dev);
}

static void fbaccel_remove(struct pci_dev *pdev)
{
	struct fba_device *dev;

	dev_info(&pdev->dev, "Remove.");

	/* Wait for any FW load in progress to complete */
	async_synchronize_full();

	dev = pci_get_drvdata(pdev);
	fba_device_exit(dev);
}

#define PCI_VENDOR_ID_FACEBOOK 0x1d9b
#define PCI_DEVICE_ID_FACEBOOK_FREYA 0x0200
#define PCI_DEVICE_ID_FACEBOOK_ARTEMIS 0x0201

#define PCI_VENDOR_ID_BROADCOM 0x14e4
#define PCI_DEVICE_ID_BROADCOM_VIPER 0x5e88

static const struct pci_device_id fbaccel_ids[] = {
	{ PCI_DEVICE_DATA(FACEBOOK, FREYA, &freya_cfg) },
	{ PCI_DEVICE_DATA(FACEBOOK, ARTEMIS, &artemis_cfg) },
	{ PCI_DEVICE_DATA(BROADCOM, VIPER, &tenjin_cfg) },
	{}
};

MODULE_DEVICE_TABLE(pci, fbaccel_ids);

static struct pci_driver fbaccel_driver = {
	.name = KBUILD_MODNAME,
	.id_table = fbaccel_ids,
	.probe = fbaccel_probe,
	.remove = fbaccel_remove,
};

static int __init fba_subdevice_driver_init(void)
{
	int ret;

	ret = fba_uart_driver_init();
	if (ret != 0)
		return ret;

	ret = fba_usoc_driver_init();
	if (ret != 0)
		goto err_uart_exit;

	return ret;

err_uart_exit:
	fba_uart_driver_exit();
	return ret;
}

static void fba_subdevice_driver_exit(void)
{
	fba_usoc_driver_exit();
	fba_uart_driver_exit();
}

static int __init fbaccel_driver_init(void)
{
	int ret;

	fba_class = class_create(THIS_MODULE, KBUILD_MODNAME);
	if (IS_ERR(fba_class))
		return PTR_ERR(fba_class);
	fba_class->dev_groups = fba_device_groups;

	ret = alloc_chrdev_region(&fba_devt, 0, FBA_MAX_DEVS, KBUILD_MODNAME);
	if (ret < 0)
		goto err_class_destroy;

	cdev_init(&fba_cdev, &fba_fops);
	ret = cdev_add(&fba_cdev, fba_devt, FBA_MAX_DEVS);
	if (ret < 0)
		goto err_free_chrdev;

	ret = fba_subdevice_driver_init();
	if (ret < 0)
		goto err_del_cdev;

	return ret;

err_del_cdev:
	cdev_del(&fba_cdev);
err_free_chrdev:
	unregister_chrdev_region(fba_devt, FBA_MAX_DEVS);
err_class_destroy:
	class_destroy(fba_class);
	return ret;
}

static void __exit fbaccel_driver_exit(void)
{
	fba_subdevice_driver_exit();
	cdev_del(&fba_cdev);
	unregister_chrdev_region(fba_devt, FBA_MAX_DEVS);
	class_destroy(fba_class);
}

static int __init fbaccel_init(void)
{
	int ret;

	ret = fbaccel_driver_init();
	if (ret != 0)
		return ret;

	ret = pci_register_driver(&fbaccel_driver);

	/* Block until firmware load completes, making scripting simpler. */
	async_synchronize_full();

	return ret;
}

static void __exit fbaccel_exit(void)
{
	pci_unregister_driver(&fbaccel_driver);
	fbaccel_driver_exit();
}

module_init(fbaccel_init);
module_exit(fbaccel_exit);

MODULE_DESCRIPTION("Meta Accelerator Host Driver");
MODULE_AUTHOR("Meta Platforms, Inc.");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(DMA_BUF);
MODULE_VERSION(FBACCEL_VERSION);
