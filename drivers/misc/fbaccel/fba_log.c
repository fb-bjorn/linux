// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/sched/signal.h>

#include "fba_log.h"
#include "fba_device.h"

#define DRV_NAME "fbalogger"

/* Device buffer header offsets */
#define READ_PTR_OFFSET 0
#define WRITE_PTR_OFFSET 4
#define DROP_COUNT_OFFSET 8
#define DATA_OFFSET 12

/* Global variable for the &fba_logger_devt device number */
static dev_t fba_logger_devt;
/*
 * Global variable for the character device structure cdev is the kernel’s
 * internal structure that represents char devices.
 */
static struct cdev fba_logger_cdev;
/* Global variable for the device class */
static struct class *fba_logger_class;
static int driver_ringbuf_size = 0x20000;

static DEFINE_MUTEX(fba_logger_mtx);
static struct fba_logger_dev *fba_logger_devices[FBA_MAX_DEVS];

module_param(driver_ringbuf_size, int, 0444);
MODULE_PARM_DESC(driver_ringbuf_size,
		 "Size of the kernel logger ring buffer, (default: 128KB)");

struct fba_log_source {
	const struct logger_params *params;
	struct work_struct logger_work;
	struct fba_logger_dev *ldev;
	uint32_t ringbuf_off;
	uint32_t ringbuf_size;
	uint32_t logger_data_off;
	char log_header[32];
	uint32_t log_header_length;
	uint32_t rd_ptr;
};

struct fba_logger_dev {
	wait_queue_head_t read_q;
	int dev_id;
	struct kref ref;
	struct fba_device *fba_dev;
	struct device *dev; /* char device */
	struct pci_dev *pci_dev;

	size_t wr_ptr; /* Offset of write pointer in circular buffer */
	/*
	 * offset that indicates the amount of data that was overwritten
	 * from the beginning in driver ring buffer
	 */
	loff_t offset;
	rwlock_t rwlock;
	char *data; /* The circular buffer data */

	struct fba_log_source srcs[MAX_LOGGER_SRCS];
	int num_srcs;

	int logger_bar_num;
	void __iomem *logger_bar_offset;

	atomic_t exiting;
	struct workqueue_struct *logger_thread;
};
static size_t fba_logger_rd_ptr(struct fba_logger_dev *ldev)
{
	loff_t offset = ldev->offset;

	return do_div(offset, driver_ringbuf_size);
}

static size_t driver_queue_length(struct fba_logger_dev *ldev)
{
	size_t rd_ptr = fba_logger_rd_ptr(ldev);

	lockdep_assert_held(&ldev->rwlock);

	if (ldev->wr_ptr >= rd_ptr)
		return ldev->wr_ptr - rd_ptr;
	else
		return driver_ringbuf_size - rd_ptr + ldev->wr_ptr;
}

static bool driver_has_data(struct fba_logger_dev *ldev, loff_t *offset)
{
	bool retval;

	read_lock_irq(&ldev->rwlock);
	retval = *offset < ldev->offset + driver_queue_length(ldev);
	read_unlock_irq(&ldev->rwlock);
	return retval;
}

static inline size_t driver_first_empty_byte(struct fba_logger_dev *ldev)
{
	lockdep_assert_held(&ldev->rwlock);

	return ldev->offset + driver_queue_length(ldev);
}

static size_t src_queue_length(struct fba_log_source *src, uint32_t rd_ptr,
			       uint32_t wr_ptr)
{
	lockdep_assert_held(&src->ldev->rwlock);

	if (wr_ptr >= rd_ptr)
		return wr_ptr - rd_ptr;
	else
		return src->ringbuf_size - rd_ptr + wr_ptr;
}

static uint32_t fba_logger_src_read32(struct fba_log_source *src, int offset)
{
	struct fba_logger_dev *ldev = src->ldev;
	struct fba_device *fba_dev = ldev->fba_dev;

	return fba_read_bar(fba_dev, ldev->logger_bar_num,
			    src->ringbuf_off + offset);
}

static uint32_t fba_logger_src_ring_read32(struct fba_log_source *src,
					   int offset)
{
	struct fba_logger_dev *ldev = src->ldev;
	struct fba_device *fba_dev = ldev->fba_dev;

	return fba_read_bar(fba_dev, ldev->logger_bar_num,
			    src->logger_data_off + offset);
}

static size_t fba_logger_get_entry_length(struct fba_log_source *src)
{
	return fba_logger_src_ring_read32(src, src->rd_ptr);
}

static void fba_logger_src_update_rdptr(struct fba_log_source *src)
{
	struct fba_logger_dev *ldev = src->ldev;
	struct fba_device *fba_dev = ldev->fba_dev;

	if (src->params->use_rd_ptr_mbox) {
		set_mailbox(fba_dev, src->params->rd_ptr_mbox, src->rd_ptr);
	} else {
		fba_write_bar(fba_dev, src->rd_ptr, ldev->logger_bar_num,
			      src->ringbuf_off + READ_PTR_OFFSET);
	}
}

static void fba_logger_device_free(struct kref *kref)
{
	struct fba_logger_dev *ldev =
		container_of(kref, struct fba_logger_dev, ref);

	kvfree(ldev->data);
	kfree(ldev);
}

static void fba_logger_device_put(struct fba_logger_dev *ldev)
{
	kref_put(&ldev->ref, fba_logger_device_free);
}

static struct fba_logger_dev *fba_logger_device_get(struct fba_logger_dev *ldev)
{
	kref_get(&ldev->ref);
	WARN_ON(kref_read(&ldev->ref) == 1);

	return ldev;
}

static struct fba_logger_dev *fba_logger_device_lookup(int minor)
{
	struct fba_logger_dev *ldev = NULL;

	mutex_lock(&fba_logger_mtx);
	if (fba_logger_devices[minor] != NULL)
		ldev = fba_logger_device_get(fba_logger_devices[minor]);
	mutex_unlock(&fba_logger_mtx);

	return ldev;
}

/*
 * Attach the logger src name to the buffer so it will be easy
 * to distinguish the logs
 */
static void fba_logger_copy_log_header(struct fba_log_source *src)
{
	struct fba_logger_dev *ldev = src->ldev;
	int bytes_copied = 0;
	int n;

	while (bytes_copied < src->log_header_length) {
		/*
		 * How many contiguous bytes are available from the write point to
		 * the end of the circular buffer?
		 */
		n = min_t(size_t, src->log_header_length - bytes_copied,
			  driver_ringbuf_size - ldev->wr_ptr);

		memcpy(ldev->data + ldev->wr_ptr,
		       src->log_header + bytes_copied, n);
		bytes_copied += n;
		ldev->wr_ptr = (ldev->wr_ptr + n) % driver_ringbuf_size;
	}
}

static void fba_logger_copy_entry(struct fba_log_source *src, size_t entry_len)
{
	struct fba_logger_dev *ldev = src->ldev;
	void __iomem *src_addr;
	int bytes_copied = 0;
	int n, word_bytes, tail_bytes;

	src_addr = ldev->logger_bar_offset + src->logger_data_off;
	/*
	 * The read ptr address has to be increased by sizeof(uint32_t) because
	 * the first offset it was pointing to is the offset that indicated the
	 * log length and we dont copy that to the driver ringbuffer
	 */
	src->rd_ptr = (src->rd_ptr + sizeof(uint32_t)) % src->ringbuf_size;

	while (bytes_copied < entry_len) {
		/*
		 * How many contiguous bytes are available from the write point to
		 * the end of the circular buffer?
		 */
		n = min_t(size_t, entry_len - bytes_copied,
			  driver_ringbuf_size - ldev->wr_ptr);
		n = min_t(size_t, n, src->ringbuf_size - src->rd_ptr);

		tail_bytes = n % sizeof(uint32_t);
		word_bytes = n - tail_bytes;
		if (word_bytes != 0) {
			memcpy_fromio(ldev->data + ldev->wr_ptr,
				      (void __iomem *)(src_addr + src->rd_ptr),
				      word_bytes);
		}
		if (tail_bytes) {
			uint8_t bytes[sizeof(uint32_t)];

			memcpy_fromio(&bytes,
				      (void __iomem *)(src_addr + src->rd_ptr +
						       word_bytes),
				      sizeof(bytes));
			memcpy(ldev->data + ldev->wr_ptr + word_bytes, &bytes,
			       tail_bytes);
		}
		bytes_copied += n;
		src->rd_ptr = (src->rd_ptr + n) % src->ringbuf_size;
		ldev->wr_ptr = (ldev->wr_ptr + n) % driver_ringbuf_size;
	}
	src->rd_ptr = ALIGN(src->rd_ptr, sizeof(uint32_t)) % src->ringbuf_size;
}

static uint32_t fba_logger_seek_newline(struct fba_logger_dev *ldev)
{
	uint32_t i = 0;
	char prev = '\0';
	size_t rd_ptr = fba_logger_rd_ptr(ldev);

	/* Stop once previous character was newline */
	while (rd_ptr != ldev->wr_ptr && prev != '\n') {
		prev = *(ldev->data + rd_ptr);
		rd_ptr = (rd_ptr + 1) % (driver_ringbuf_size);
		ldev->offset++;
		i++;
	}

	return i;
}

static void fba_logger_make_space(struct fba_logger_dev *ldev, size_t entry_len)
{
	uint64_t space;

	space = driver_ringbuf_size - driver_queue_length(ldev) - 1;

	while (space < entry_len) {
		/* Keep removing lines of log until we have enough space */
		space += fba_logger_seek_newline(ldev);
	}
}

static bool fba_logger_entry_is_valid(struct fba_log_source *src,
				      uint32_t entry_len, uint32_t src_wr_ptr)
{
	if (entry_len == 0 || entry_len > src->ringbuf_size ||
	    entry_len > src_queue_length(src, src->rd_ptr, src_wr_ptr)) {
		return false;
	}
	return true;
}

static void fba_logger_handler(struct work_struct *work)
{
	struct fba_log_source *src =
		container_of(work, struct fba_log_source, logger_work);
	struct fba_logger_dev *ldev = src->ldev;
	struct fba_device *fba_dev = ldev->fba_dev;
	size_t entry_len;
	uint32_t src_wr_ptr;
	int n = 0;

	write_lock(&ldev->rwlock);

	src_wr_ptr = fba_logger_src_read32(src, WRITE_PTR_OFFSET);

	while (src_wr_ptr != src->rd_ptr && n++ <= 10) {
		/* we should check if the length of the log is valid */
		entry_len = fba_logger_get_entry_length(src);
		if (fba_logger_entry_is_valid(src, entry_len, src_wr_ptr) !=
		    true) {
			write_unlock(&ldev->rwlock);
			dev_err(&fba_dev->pdev->dev,
				"%s: %s device logger has provided wrong log length 0x%llx with rd_ptr = 0x%x and wr_ptr = 0x%x",
				fba_dev->name, src->params->name,
				(uint64_t)entry_len, src->rd_ptr, src_wr_ptr);
			disable_irq(pci_irq_vector(fba_dev->pdev,
						   src->params->msix_int));

			return;
		}
		fba_logger_make_space(ldev, entry_len + src->log_header_length);
		fba_logger_copy_log_header(src);
		fba_logger_copy_entry(src, entry_len);
	}

	fba_logger_src_update_rdptr(src);

	src_wr_ptr = fba_logger_src_read32(src, WRITE_PTR_OFFSET);

	if (src_wr_ptr != src->rd_ptr)
		queue_work(ldev->logger_thread, &src->logger_work);
	wake_up_all(&(ldev)->read_q);

	write_unlock(&ldev->rwlock);
}

static irqreturn_t fba_logger_isr(int irq, void *data)
{
	struct fba_log_source *src = data;

	queue_work(src->ldev->logger_thread, &src->logger_work);
	return IRQ_HANDLED;
}

static void fba_logger_srcs_init(struct fba_logger_dev *ldev)
{
	struct fba_log_source *src;
	struct fba_device *fba_dev = ldev->fba_dev;
	int i;

	for (i = 0; i < ldev->num_srcs; i++) {
		src = &ldev->srcs[i];
		src->params = &fba_dev->cfg->logger_params[i];

		src->ldev = ldev;
		src->ringbuf_off = get_mailbox(fba_dev, src->params->off_mbox);
		src->ringbuf_size =
			get_mailbox(fba_dev, src->params->size_mbox);
		src->logger_data_off = fba_logger_src_read32(src, DATA_OFFSET) +
				       src->ringbuf_off;
		if (src->params->use_rd_ptr_mbox) {
			src->rd_ptr =
				get_mailbox(fba_dev, src->params->rd_ptr_mbox);
		} else {
			src->rd_ptr =
				fba_logger_src_read32(src, READ_PTR_OFFSET);
		}
		src->log_header_length = snprintf(src->log_header,
						  sizeof(src->log_header),
						  "%s: %s: %s: ", fba_dev->name,
						  dev_name(&fba_dev->pdev->dev),
						  src->params->name);
	}
}

static int fba_logger_srcs_msix_init(struct fba_logger_dev *ldev)
{
	struct fba_log_source *src;
	struct fba_device *fba_dev = ldev->fba_dev;
	int i, ret = 0;

	for (i = 0; i < ldev->num_srcs; i++) {
		src = &ldev->srcs[i];
		src->params = &fba_dev->cfg->logger_params[i];
		ret = pci_request_irq(fba_dev->pdev, src->params->msix_int,
				      fba_logger_isr, NULL, src,
				      "fba_logger_%s", ldev->fba_dev->name);
		if (ret) {
			dev_err(&fba_dev->pdev->dev,
				"%s: pci_request_irq %d failed\n",
				fba_dev->name, src->params->msix_int);
			goto err_irq;
		}
	}
	return ret;
err_irq:
	for (--i; i >= 0; i--) {
		src = &ldev->srcs[i];
		pci_free_irq(ldev->pci_dev, src->params->msix_int, src);
	}
	return ret;
}

static int fba_logger_device_init(struct fba_logger_dev *ldev)
{
	int ret = -ENOSPC;

	mutex_lock(&fba_logger_mtx);
	if (fba_logger_devices[ldev->dev_id] == NULL) {
		fba_logger_devices[ldev->dev_id] = ldev;
		ret = 0;
	} else {
		dev_err(&ldev->fba_dev->pdev->dev,
			"%s: Device ID %d already exists.", ldev->fba_dev->name,
			ldev->dev_id);
	}
	mutex_unlock(&fba_logger_mtx);

	return ret;
}

static int fba_logger_thread_init(struct fba_logger_dev *ldev)
{
	struct fba_device *fba_dev = ldev->fba_dev;
	struct fba_log_source *src;
	int i;
	char thread_name[32];

	snprintf(thread_name, sizeof(thread_name), "%s_logger", fba_dev->name);
	ldev->logger_thread = create_singlethread_workqueue(thread_name);
	if (!ldev->logger_thread)
		return -ENOMEM;

	for (i = 0; i < ldev->num_srcs; i++) {
		src = &ldev->srcs[i];
		INIT_WORK(&src->logger_work, fba_logger_handler);
		queue_work(ldev->logger_thread, &src->logger_work);
	}

	return 0;
}

static bool fba_logger_src_ringbuff_invalid(struct fba_device *fba_dev)
{
	int i;
	uint32_t size, src_ringbuf_off, src_rd_ptr;

	for (i = 0; i < fba_dev->cfg->num_logger_srcs; i++) {
		size = get_mailbox(fba_dev,
				   fba_dev->cfg->logger_params[i].size_mbox);
		src_ringbuf_off = get_mailbox(
			fba_dev, fba_dev->cfg->logger_params[i].off_mbox);

		if (fba_dev->cfg->logger_params[i].use_rd_ptr_mbox) {
			src_rd_ptr = get_mailbox(
				fba_dev,
				fba_dev->cfg->logger_params[i].rd_ptr_mbox);
		} else {
			src_rd_ptr = fba_read_bar(
				fba_dev, fba_dev->cfg->logger_bar_num,
				src_ringbuf_off + READ_PTR_OFFSET);
		}
		if ((size > driver_ringbuf_size) || (src_rd_ptr > size))
			return true;
	}
	return false;
}

static int fba_logger_open(struct inode *inode, struct file *file)
{
	struct fba_logger_dev *ldev = fba_logger_device_lookup(iminor(inode));

	if (ldev == NULL)
		return -ENODEV;

	file->private_data = ldev;
	return 0;
}

static int fba_logger_release(struct inode *inode, struct file *file)
{
	fba_logger_device_put(file->private_data);
	return 0;
}

static void read_from_logger(struct fba_logger_dev *ldev, size_t *length,
			     loff_t *offset, char *buffer)
{
	int bytes_copied = 0, n, start_point;
	size_t remaining;

	read_lock_irq(&ldev->rwlock);

	/* is the user trying to read data that has already scrolled off? */
	if (*offset < ldev->offset)
		*offset = ldev->offset;

	/*
	 * Find the smaller of the total bytes we have available and what
	 * the user is asking for
	 */
	*length =
		min_t(size_t, *length, driver_first_empty_byte(ldev) - *offset);
	remaining = *length;

	/* figure out where to start based on user's offset */
	start_point = fba_logger_rd_ptr(ldev) + (*offset - ldev->offset);
	start_point = start_point % driver_ringbuf_size;

	/* copy the (possibly noncontiguous) data to our buffer */
	while (remaining) {
		n = min_t(size_t, remaining, driver_ringbuf_size - start_point);
		memcpy(buffer + bytes_copied, ldev->data + start_point, n);
		bytes_copied += n;
		remaining -= n;
		start_point = (start_point + n) % driver_ringbuf_size;
	}

	read_unlock_irq(&ldev->rwlock);

	/* advance user's file pointer */
	*offset += *length;
}

static ssize_t fba_logger_read(struct file *file, char __user *user_buffer,
			       size_t size, loff_t *offset)
{
	struct fba_logger_dev *ldev = file->private_data;
	int retval;
	char *data_to_return;

	if (!driver_has_data(ldev, offset)) {
		if (atomic_read(&ldev->exiting))
			return 0;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		retval = wait_event_interruptible(
			(ldev)->read_q, driver_has_data(ldev, offset) ||
						atomic_read(&ldev->exiting));
		if (retval)
			return retval;
	}

	if (atomic_read(&ldev->exiting))
		return 0;

	data_to_return =
		kvmalloc(min_t(size_t, driver_ringbuf_size, size), GFP_KERNEL);
	if (data_to_return == NULL)
		return 0;

	read_from_logger(ldev, &size, offset, data_to_return);

	if (copy_to_user(user_buffer, data_to_return, size) > 0)
		retval = -EFAULT;
	else
		retval = size;
	kvfree(data_to_return);
	return retval;
}

struct fba_logger_dev *fba_logger_register(struct fba_device *fba_dev)
{
	struct fba_logger_dev *ldev;
	struct device *pdev;
	struct device *parent = fba_dev->dev;
	int num_params = fba_dev->cfg->num_logger_srcs;
	int ret, i;

	if (num_params < 1)
		return NULL;

	if (!parent) {
		dev_err(&fba_dev->pdev->dev, "%s: Parent device was NULL",
			fba_dev->name);
		return ERR_PTR(-EINVAL);
	}

	if (fba_logger_src_ringbuff_invalid(fba_dev)) {
		dev_err(&fba_dev->pdev->dev,
			"%s: Logger buffer size is smaller than device buffers",
			fba_dev->name);
		return ERR_PTR(-EINVAL);
	}

	ldev = kzalloc(sizeof(*ldev), GFP_KERNEL);
	if (!ldev)
		return ERR_PTR(-ENOMEM);

	ldev->data = kvmalloc(driver_ringbuf_size, GFP_KERNEL);
	if (!ldev->data) {
		kfree(ldev);
		return ERR_PTR(-ENOMEM);
	}

	kref_init(&ldev->ref);
	ldev->pci_dev = pci_dev_get(fba_dev->pdev);
	ldev->dev_id = fba_dev->id;
	ldev->fba_dev = fba_dev;
	ldev->num_srcs = num_params;
	ldev->logger_bar_num = fba_dev->cfg->logger_bar_num;
	ldev->logger_bar_offset = fba_dev->bars[ldev->logger_bar_num];
	atomic_set(&ldev->exiting, false);
	init_waitqueue_head(&((ldev)->read_q));
	rwlock_init(&ldev->rwlock);
	fba_logger_srcs_init(ldev);

	ret = fba_logger_thread_init(ldev);
	if (ret)
		goto err_put_device;

	ret = fba_logger_srcs_msix_init(ldev);
	if (ret)
		goto err_destroy_wq;

	// dev_id is used all over this file as the minor, unlike in other devices
	// so adding a sanity check to avoid confusion
	if (fba_dev->id != fba_dev->minor) {
		dev_err(&fba_dev->pdev->dev,
			"%s: inconsistent dev ID and minor (%d and %d)",
			fba_dev->name, fba_dev->id, fba_dev->minor);
		goto err_free_irq;
	}

	pdev = device_create(fba_logger_class, parent,
			     MKDEV(MAJOR(fba_logger_devt), fba_dev->id), ldev,
			     "%slog", fba_dev->name);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto err_free_irq;
	}
	ldev->dev = pdev;

	ret = fba_logger_device_init(ldev);
	if (ret) {
		dev_err(&fba_dev->pdev->dev,
			"%s: Failed to allocate device ID.", fba_dev->name);
		goto err_destroy_dev;
	}
	return ldev;

err_destroy_dev:
	device_destroy(fba_logger_class,
		       MKDEV(MAJOR(fba_logger_devt), fba_dev->id));
err_free_irq:
	for (i = 0; i < ldev->num_srcs; i++) {
		struct fba_log_source *src = &ldev->srcs[i];
		pci_free_irq(ldev->pci_dev, src->params->msix_int, src);
	}
err_destroy_wq:
	destroy_workqueue(ldev->logger_thread);
err_put_device:
	pci_dev_put(ldev->pci_dev);
	kvfree(ldev->data);
	kfree(ldev);
	return ERR_PTR(ret);
}

void fba_logger_unregister(struct fba_logger_dev *ldev)
{
	struct fba_log_source *src;
	uint64_t i;

	mutex_lock(&fba_logger_mtx);
	fba_logger_devices[ldev->dev_id] = NULL;
	mutex_unlock(&fba_logger_mtx);

	device_destroy(fba_logger_class,
		       MKDEV(MAJOR(fba_logger_devt), ldev->dev_id));

	for (i = 0; i < ldev->num_srcs; i++) {
		src = &ldev->srcs[i];
		pci_free_irq(ldev->pci_dev, src->params->msix_int, src);
	}

	destroy_workqueue(ldev->logger_thread);
	ldev->logger_thread = NULL;

	atomic_set(&ldev->exiting, true);
	wake_up_all(&(ldev)->read_q);
	fba_logger_device_put(ldev);
}

static const struct file_operations fba_logger_fops = {
	.owner = THIS_MODULE,
	.open = fba_logger_open,
	.release = fba_logger_release,
	.read = fba_logger_read,
	.llseek = noop_llseek,
};

int fba_logger_driver_init(void)
{
	int ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	fba_logger_class = class_create(THIS_MODULE, DRV_NAME);
#else
	fba_logger_class = class_create(DRV_NAME);
#endif
	if (IS_ERR(fba_logger_class))
		return PTR_ERR(fba_logger_class);

	/*
	 * Connecting the device file with the device driver involves two steps
	 * fba_logger_devt step is registering for the <major, minor> range of
	 * device files.
	 */
	ret = alloc_chrdev_region(&fba_logger_devt, 0, FBA_MAX_DEVS,
				  "fba_logger");
	if (ret < 0)
		goto err_class_destroy;

	/*
	 * Allocate and initialize cdev structure for PCIe file operations and
	 * owner
	 */
	cdev_init(&fba_logger_cdev, &fba_logger_fops);

	ret = cdev_add(&fba_logger_cdev, fba_logger_devt, FBA_MAX_DEVS);

	if (ret < 0)
		goto err_free_chrdev;

	return ret;
err_free_chrdev:
	unregister_chrdev_region(fba_logger_devt, FBA_MAX_DEVS);
err_class_destroy:
	class_destroy(fba_logger_class);
	return ret;
}

void fba_logger_driver_exit(void)
{
	cdev_del(&fba_logger_cdev);
	class_destroy(fba_logger_class);
	unregister_chrdev_region(fba_logger_devt, FBA_MAX_DEVS);
}
