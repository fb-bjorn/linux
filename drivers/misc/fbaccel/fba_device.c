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
#include <linux/version.h>

#include "fba_device.h"
#include "fba_proto.h"

#define FBA_MAX_DEVS 32

static dev_t fba_devt;
static struct class *fba_class;
static DEFINE_MUTEX(fba_devices_mtx);
static struct fba_device *fba_devices[FBA_MAX_DEVS];

static void fba_device_unregister(struct fba_device *fba_dev)
{
	mutex_lock(&fba_devices_mtx);
	fba_devices[fba_dev->minor] = NULL;
	device_destroy(fba_class, MKDEV(MAJOR(fba_devt), fba_dev->minor));
	ida_free(fba_dev->cfg->devnode_ida, fba_dev->id);
	mutex_unlock(&fba_devices_mtx);
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

	pci_disable_pcie_error_reporting(pdev);
	pci_clear_master(pdev);
	pci_release_regions(pdev);

	if (pci_is_enabled(pdev))
		pci_disable_device(pdev);
}

static int fba_pci_init(struct fba_device *fba_dev)
{
	struct pci_dev *pdev = fba_dev->pdev;
	int rc;

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

	return 0;

err_disable_reporting:
	pci_disable_pcie_error_reporting(pdev);
	pci_clear_master(pdev);
err_release_regions:
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);

	return rc;
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
	fba_dev->dev = device_create(fba_class, &fba_dev->pdev->dev,
				     MKDEV(MAJOR(fba_devt), fba_dev->minor),
				     NULL, "%s", fba_dev->name);

	return 0;

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
	fba_device_unregister(fba_dev);

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

	return fba_device_init(dev);
}

static void fbaccel_remove(struct pci_dev *pdev)
{
	struct fba_device *dev;

	dev_info(&pdev->dev, "Remove.");

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
