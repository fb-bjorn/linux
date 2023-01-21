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
#include "fba_proto.h"

#define FBA_MAX_DEVS 32

static dev_t fba_devt;
static struct class *fba_class;
static DEFINE_MUTEX(fba_devices_mtx);
static struct fba_device *fba_devices[FBA_MAX_DEVS];

static bool auto_load = true;
module_param(auto_load, bool, 0664);
MODULE_PARM_DESC(auto_load, "If true, automatically load firmware on probe");

static bool pcie_boot_en = true;
module_param(pcie_boot_en, bool, 0664);
MODULE_PARM_DESC(pcie_boot_en, "Bootflow enablement boot parameter");

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

static void fba_device_unregister(struct fba_device *fba_dev)
{
	mutex_lock(&fba_devices_mtx);
	fba_devices[fba_dev->minor] = NULL;
	device_destroy(fba_class, MKDEV(MAJOR(fba_devt), fba_dev->minor));
	ida_free(fba_dev->cfg->devnode_ida, fba_dev->id);
	mutex_unlock(&fba_devices_mtx);
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

/* Abort operation of device, called upon detection of fatal error. */
void fba_device_abort(struct fba_device *fba_dev, const char *reason)
{
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
	 * Disable PCI bus master/DMA and wait for any in-flight PCIe
	 * transactions to complete.  After these steps, should be safe
	 * to free any DMA mappings as device can no longer write to them
	 * (assuming it honors clearing Bus Master Enable correctly.)
	 */
	pci_disable_device(fba_dev->pdev);
	pci_wait_for_pending_transaction(fba_dev->pdev);

	/* Wake up anyone waiting for abort to complete */
	complete_all(&fba_dev->abort_done);
}

static void fba_device_free(struct kref *kref)
{
	struct fba_device *fba_dev = container_of(kref, struct fba_device, ref);

	/* Free things that might be referenced by lingering user contexts */
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

	fba_dev_info(fba_dev, "Done setting up device, ready for use.");
}

static int fba_device_init(struct fba_device *fba_dev)
{
	int i, rc;

	kref_init(&fba_dev->ref);
	init_completion(&fba_dev->abort_done);

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

	rc = fba_pci_init(fba_dev);
	if (rc)
		goto err_free_id;

	// Driver is now available for IOCTL communication.
	fba_dev_info(fba_dev, "Done connecting BARs.");

	if (fba_dev->cfg->impl_ops->impl_init != NULL) {
		rc = fba_dev->cfg->impl_ops->impl_init(fba_dev);
		if (rc != 0)
			goto err_pci_exit;
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

err_pci_exit:
	fba_pci_exit(fba_dev);
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
	WRITE_ONCE(fba_dev->exiting, true);

	fba_device_unregister(fba_dev);

	if (fba_dev->cfg->impl_ops->impl_exit != NULL)
		fba_dev->cfg->impl_ops->impl_exit(fba_dev);

	if (!atomic_read(&fba_dev->has_failed)) {
		if (fba_dev->cfg->impl_ops->reset_controller)
			(void)fba_dev->cfg->impl_ops->reset_controller(fba_dev);
	}

	fba_pci_exit(fba_dev);

	fba_device_put(fba_dev);

	return 0;
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

#define PCI_VENDOR_ID_BROADCOM 0x14e4
#define PCI_DEVICE_ID_BROADCOM_VIPER 0x5e88

static const struct pci_device_id fbaccel_ids[] = {
	{ PCI_DEVICE_DATA(FACEBOOK, FREYA, &freya_cfg) },
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

	return ret;

err_class_destroy:
	class_destroy(fba_class);
	return ret;
}

static void __exit fbaccel_driver_exit(void)
{
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
