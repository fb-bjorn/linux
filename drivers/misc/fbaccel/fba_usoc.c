// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates
 *
 * Driver for the Ultrasoc AXI Communicator.
 * The driver maps a portion of BAR1 (AXI communicator register space)
 * and hooks up the interrupts.
 *
 * There is one interrupt vector each for upstream and downstream messages.
 * Upstream messages flow through the AXI commimicator to the Ulrasoc domain.
 * Downstream messages flow from the Ulrasoc domain to the AXI communicator.
 * Interrupts are signaled to userspace via eventfds that are configured
 * via ioctls. Everything else is handled by the userpsace driver.
 * The driver only allows a single open at a time.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nospec.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "fba_device.h"
#include "fba_util.h"
#include "fba_usoc.h"

#define USOC_BAR 1
#define USOC_OFFSET 0x2000

#define USOC_AXI_US_INT_CTL_REG 4
#define USOC_AXI_US_INT_STS_REG 8
#define USOC_AXI_DS_INT_CTL_REG 0x84
#define USOC_AXI_DS_INT_STS_REG 0x88
#define USOC_AXI_DS_CFG_MGI_REG 0x30
#define USOC_AXI_DS_CFG_MGI_VAL 0xb01

#define DRIVER_DESC "PCI driver for Ultrasoc AXI Communicator"

#define USOC_SET_US_EFD _IOR('U', 0x1, int)
#define USOC_SET_DS_EFD _IOR('U', 0x2, int)

struct fba_usoc_dev {
	spinlock_t lock;
	int open_cnt;

	struct pci_dev *pci_dev;
	void __iomem *regs;
	unsigned long map_pfn;
	int msix_irq_ds;
	int msix_irq_us;

	struct device char_dev;
	struct device *device; /* char device */
	struct cdev cdev;
	struct eventfd_ctx *us_efd_ctx;
	struct eventfd_ctx *ds_efd_ctx;
	int minor;
	int id;
	const char *fmt;
};

static struct class *fba_usoc_class;
static dev_t fba_usoc_devt;

static const struct pci_device_id fba_usoc_id_table[] = {
	{ PCI_DEVICE(0x16c3, 0xabcd) },
	{
		0,
	}
};

static int fba_usoc_open(struct inode *inode, struct file *file)
{
	struct fba_usoc_dev *dev =
		container_of(inode->i_cdev, struct fba_usoc_dev, cdev);

	spin_lock_irq(&dev->lock);
	if (dev->open_cnt) {
		spin_unlock_irq(&dev->lock);
		return -EBUSY;
	}
	dev->open_cnt++;
	spin_unlock_irq(&dev->lock);

	file->private_data = dev;
	return 0;
}

static int fba_usoc_release(struct inode *inode, struct file *file)
{
	struct fba_usoc_dev *dev = file->private_data;
	struct eventfd_ctx *us_ctx = NULL, *ds_ctx = NULL;

	spin_lock_irq(&dev->lock);

	dev->open_cnt--;
	if (dev->open_cnt == 0) {
		us_ctx = dev->us_efd_ctx;
		ds_ctx = dev->ds_efd_ctx;

		dev->us_efd_ctx = NULL;
		dev->ds_efd_ctx = NULL;
	}

	spin_unlock_irq(&dev->lock);

	if (us_ctx != NULL)
		eventfd_ctx_put(us_ctx);
	if (ds_ctx != NULL)
		eventfd_ctx_put(ds_ctx);

	return 0;
}

static long fba_usoc_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct fba_usoc_dev *dev = file->private_data;
	int fd;
	struct eventfd_ctx *ctx, *octx;

	switch (cmd) {
	case USOC_SET_US_EFD:
		fd = (int)arg;
		ctx = eventfd_ctx_fdget(fd);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);
		spin_lock_irq(&dev->lock);
		octx = dev->us_efd_ctx;
		dev->us_efd_ctx = ctx;
		spin_unlock_irq(&dev->lock);
		if (octx != NULL)
			eventfd_ctx_put(octx);
		break;
	case USOC_SET_DS_EFD:
		fd = (int)arg;
		ctx = eventfd_ctx_fdget(fd);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);
		spin_lock_irq(&dev->lock);
		octx = dev->ds_efd_ctx;
		dev->ds_efd_ctx = ctx;
		spin_unlock_irq(&dev->lock);
		if (octx != NULL)
			eventfd_ctx_put(octx);
		break;
	default:
		return -ENOTTY;
	}
	return 0;
}

static void fba_usoc_vm_close(struct vm_area_struct *vma)
{
	struct fba_usoc_dev *dev = vma->vm_private_data;
	struct eventfd_ctx *us_ctx = NULL, *ds_ctx = NULL;

	spin_lock_irq(&dev->lock);
	dev->open_cnt--;
	if (dev->open_cnt == 0) {
		us_ctx = dev->us_efd_ctx;
		ds_ctx = dev->ds_efd_ctx;

		dev->us_efd_ctx = NULL;
		dev->ds_efd_ctx = NULL;
	}
	spin_unlock_irq(&dev->lock);
	if (us_ctx != NULL)
		eventfd_ctx_put(us_ctx);
	if (ds_ctx != NULL)
		eventfd_ctx_put(ds_ctx);
}

static const struct vm_operations_struct fba_usoc_vm_ops = {
	.close = fba_usoc_vm_close,
};

static int fba_usoc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct fba_usoc_dev *dev = file->private_data;
	int ret;

	if (vma->vm_end < vma->vm_start || vma->vm_pgoff != 0 ||
	    vma_pages(vma) != 1)
		return -EINVAL;

	vma->vm_private_data = dev;
	vma->vm_ops = &fba_usoc_vm_ops;
	vma->vm_page_prot = pgprot_device(vma->vm_page_prot);

	ret = io_remap_pfn_range(vma, vma->vm_start, dev->map_pfn,
				 vma->vm_end - vma->vm_start,
				 vma->vm_page_prot);
	if (!ret) {
		spin_lock_irq(&dev->lock);
		dev->open_cnt++;
		spin_unlock_irq(&dev->lock);
	}
	return ret;
}

static const struct file_operations fba_usoc_fops = {
	.owner = THIS_MODULE,
	.open = fba_usoc_open,
	.release = fba_usoc_release,
	.unlocked_ioctl = fba_usoc_ioctl,
	.mmap = fba_usoc_mmap,
};

static irqreturn_t fba_usoc_ds_isr(int irq, void *dev)
{
	struct fba_usoc_dev *udev = dev;
	unsigned long flags;

	dev_dbg(&udev->pci_dev->dev, "usoc: ds interrupt received\n");

	spin_lock_irqsave(&udev->lock, flags);

	if (udev->ds_efd_ctx != NULL)
		eventfd_signal(udev->ds_efd_ctx, 1);

	spin_unlock_irqrestore(&udev->lock, flags);

	return IRQ_HANDLED;
}

static irqreturn_t fba_usoc_us_isr(int irq, void *dev)
{
	struct fba_usoc_dev *udev = dev;
	unsigned long flags;

	dev_dbg(&udev->pci_dev->dev, "usoc: us interrupt received\n");

	spin_lock_irqsave(&udev->lock, flags);

	if (udev->us_efd_ctx != NULL)
		eventfd_signal(udev->us_efd_ctx, 1);

	spin_unlock_irqrestore(&udev->lock, flags);

	return IRQ_HANDLED;
}

static void fba_usoc_free_dev(struct device *dev)
{
	struct fba_usoc_dev *udev =
		container_of(dev, struct fba_usoc_dev, char_dev);

	pci_dev_put(udev->pci_dev);
	kfree(udev);
}

struct fba_usoc_dev *
fba_usoc_register(struct pci_dev *pdev, struct device *parent, int minor,
		  const char *name, int id, void __iomem *regs,
		  unsigned long map_pfn, int msix_irq_ds, int msix_irq_us)
{
	struct fba_usoc_dev *dev;
	uint32_t val;
	int ret;

	/* Verify mapping is valid. */
	val = ioread32(regs + USOC_AXI_DS_CFG_MGI_REG);
	if (val != USOC_AXI_DS_CFG_MGI_VAL) {
		dev_warn(&pdev->dev, "usoc mapping not valid, %08x != %08x.\n",
			 val, USOC_AXI_DS_CFG_MGI_VAL);
		return ERR_PTR(-EIO);
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev->pci_dev = pci_dev_get(pdev);

	dev->minor = minor;
	dev->id = id;
	dev->regs = regs;
	dev->map_pfn = map_pfn;
	dev->msix_irq_ds = msix_irq_ds;
	dev->msix_irq_us = msix_irq_us;
	spin_lock_init(&dev->lock);

	ret = fba_request_irq(pdev, msix_irq_ds, fba_usoc_ds_isr, NULL, dev,
			      "%s%d-usoc-ds", name, dev->id);
	if (ret) {
		dev_err(&pdev->dev, "fba_request_irq %d failed\n", msix_irq_ds);
		goto err_put_device;
	}

	ret = fba_request_irq(pdev, msix_irq_us, fba_usoc_us_isr, NULL, dev,
			      "%s%d-usoc-us", name, dev->id);
	if (ret) {
		dev_err(&pdev->dev, "fba_request_irq %d failed\n", msix_irq_us);
		goto err_free_ds_irq;
	}

	device_initialize(&dev->char_dev);
	dev->device = &dev->char_dev;
	dev->device->devt = MKDEV(MAJOR(fba_usoc_devt), dev->minor);
	dev->device->class = fba_usoc_class;
	dev->device->parent = parent;
	dev->device->groups = NULL;
	dev->device->release = fba_usoc_free_dev;
	dev_set_drvdata(dev->device, dev);
	ret = dev_set_name(dev->device, "%s%dusoc", name, dev->id);
	if (ret)
		goto err_free_irqs;

	cdev_init(&dev->cdev, &fba_usoc_fops);
	dev->cdev.owner = fba_usoc_fops.owner;

	ret = cdev_device_add(&dev->cdev, dev->device);
	if (ret)
		goto err_free_name;

	dev_dbg(dev->device, "pci function %s\n", dev_name(&pdev->dev));

	return dev;

err_free_name:
	kfree_const(dev->device->kobj.name);
err_free_irqs:
	fba_free_irq(pdev, msix_irq_us, dev);
err_free_ds_irq:
	fba_free_irq(pdev, msix_irq_ds, dev);
err_put_device:
	pci_dev_put(dev->pci_dev);
	kfree(dev);

	return ERR_PTR(ret);
}

void fba_usoc_unregister(struct fba_usoc_dev *dev)
{
	cdev_del(&dev->cdev);

	/* disable device interrupts */
	writel(0, dev->regs + USOC_AXI_US_INT_CTL_REG);
	writel(0, dev->regs + USOC_AXI_DS_INT_CTL_REG);

	fba_free_irq(dev->pci_dev, dev->msix_irq_us, dev);
	fba_free_irq(dev->pci_dev, dev->msix_irq_ds, dev);

	device_del(&dev->char_dev);
}

static char *fba_usoc_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0600;

	return NULL;
}

int __init fba_usoc_driver_init(void)
{
	int ret;

	fba_usoc_class = class_create(THIS_MODULE, "fba_usoc");
	if (IS_ERR(fba_usoc_class))
		return PTR_ERR(fba_usoc_class);
	fba_usoc_class->devnode = fba_usoc_devnode;

	ret = alloc_chrdev_region(&fba_usoc_devt, 0, FBA_MAX_DEVS, "fba_usoc");
	if (ret < 0) {
		class_destroy(fba_usoc_class);
		return ret;
	}

	return ret;
}

void fba_usoc_driver_exit(void)
{
	unregister_chrdev_region(fba_usoc_devt, FBA_MAX_DEVS);
	class_destroy(fba_usoc_class);
}
