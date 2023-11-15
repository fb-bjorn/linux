// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/percpu-refcount.h>
#include <linux/version.h>
#include <linux/pci-p2pdma.h>

#include "fba_device.h"

/*
 * Freya P2P support using dmabufs with the standard dmabuf flow.
 * The exporter allocates a resource from P2P DRAM and
 * exports it as a dmabuf file descriptor. The importer can then
 * do DMA to a portion of the exported resource specifying an offset
 * and a length.
 *
 * An exported dmabuf is freed only after userland closes the corresponding fd. Hwoever,
 * we want to allow the exporter to be able to free device resources independently,
 * either from user process initiated free or because of a device remove. In the
 * first case, we do not want a new dmabuf importer to be able to access the freed
 * device buffer.
 *
 * The fba_dmabuf structure is used to connect the dmabuf with the device resource
 * allocation represented by an alloc structure.
 *
 * struct fba_dmabuf {
 *	struct list_head list;
 *	struct fba_alloc *alloc;
 *	struct kref kref;
 *	struct percpu_ref attach_ref;
 *	...
 *
 * Exporting an allocated resource puts the corresponding fba_dmabuf on the alloc's
 * dma buf list. The fba_dmabuf does not hold an alloc reference but it stores
 * a pointer to alloc. A dmabuf can be imported only if attach_ref is live.
 *
 * The percpu_ref attach_ref tracks all active attachments. When a resource is freed, it
 * is marked dead and this prevents new importers from grabbing a reference to the fba_dmabuf.
 *
 * This design assumes that all active mappings will be freed within some bounded time interval
 * to prevent an indefinite sleep at this point. If this is a concern, we can time the wait and
 * print an error if the wait is taking too long. The final extreme step may be to send a
 * SIGKILL to the process.
 *
 * Note that a dmabuf attachment is only active for the lifetime of a job that uses P2P transfers.
 *
 * Finally, fba_dmbuf is freed when kref goes to zero.
 */

#ifdef CONFIG_DMA_SHARED_BUFFER

static const struct dma_buf_ops fba_dmabuf_ops;

static void fba_dmabuf_free(struct kref *kref)
{
	struct fba_dmabuf *fba_dmabuf =
		container_of(kref, struct fba_dmabuf, kref);

	percpu_ref_exit(&fba_dmabuf->attach_ref);
	kfree(fba_dmabuf);
}

static void fba_dmabuf_percpu_release(struct percpu_ref *ref)
{
	struct fba_dmabuf *fba_dmabuf =
		container_of(ref, struct fba_dmabuf, attach_ref);

	complete(&fba_dmabuf->detaches_done);
}

/*
 * Attach and map an instance of a dmabuf. The mapped instance is tracked in
 * the job state and is freed in fba_job_release_resources.
 */
struct fba_dmabuf_attach *fba_translate_p2p_setup(struct fba_ctx *ctx, int fd,
						  size_t offset, size_t size)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct fba_dmabuf_attach *fba_dmabuf_attach;
	unsigned long end;
	int ret = -EINVAL;
	struct sg_table *sgt;
	struct scatterlist *sg;

	if (check_add_overflow((unsigned long)offset, (unsigned long)size,
			       &end)) {
		fba_dev_err(fba_dev,
			    "%s: check_add_overflow err: offset=0x%x size=0x%x",
			    __func__, offset, size);
		return ERR_PTR(ret);
	}

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf)) {
		fba_dev_err(fba_dev, "%s: dma_buf_get err: fd=%d ret=%d",
			    __func__, fd, PTR_ERR(dmabuf));
		return (struct fba_dmabuf_attach *)dmabuf;
	}
	if (dmabuf->ops != &fba_dmabuf_ops)
		goto out_release_dmabuf;

	if (dmabuf->size < end)
		goto out_release_dmabuf;

	attach = dma_buf_attach(dmabuf, &fba_dev->pdev->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		fba_dev_err(fba_dev, "%s: dma_buf_attach err: ret=%d", __func__,
			    ret);
		goto out_release_dmabuf;
	}

	fba_dmabuf_attach = attach->priv;
	fba_dmabuf_attach->offset = offset;
	fba_dmabuf_attach->size = size;

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		fba_dev_err(fba_dev, "%s: dma_buf_map_attachment err: ret=%d",
			    __func__, ret);
		goto out_free_detach;
	}

	if (sgt->nents > 1)
		goto out_free_attachment;

	/* modify the sg list in-place to match offset and length */

	sg = sgt->sgl;
	sg_dma_address(sg) += offset;
	fba_dmabuf_attach->orig_size = sg_dma_len(sg);
	sg_dma_len(sg) = size;

	return fba_dmabuf_attach;

out_free_attachment:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);

out_free_detach:
	dma_buf_detach(dmabuf, attach);

out_release_dmabuf:
	dma_buf_put(dmabuf);

	return ERR_PTR(ret);
}

void fba_translate_p2p_teardown(struct fba_ctx *ctx,
				struct fba_dmabuf_attach *fba_dmabuf_attach)
{
	struct scatterlist *sg;
	struct dma_buf *dmabuf = fba_dmabuf_attach->attach->dmabuf;

	sg = fba_dmabuf_attach->sgt.sgl;
	sg_dma_address(sg) -= fba_dmabuf_attach->offset;
	sg_dma_len(sg) = fba_dmabuf_attach->orig_size;

	dma_buf_unmap_attachment(fba_dmabuf_attach->attach,
				 &fba_dmabuf_attach->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, fba_dmabuf_attach->attach);
	dma_buf_put(dmabuf);
}

static int fba_dmabuf_ops_attach(struct dma_buf *dmabuf,
				 struct dma_buf_attachment *dmabuf_attach)
{
	struct fba_dmabuf *fba_dmabuf = dmabuf->priv;
	struct fba_dmabuf_attach *fba_dmabuf_attach;
	struct fba_device *peer_dev;
	int ret = -ENOMEM;

	fba_dmabuf_attach = kzalloc(sizeof(*fba_dmabuf_attach), GFP_KERNEL);
	if (!fba_dmabuf_attach)
		return ret;

	/*
	 * atomically verify dmabuf is live and grab a reference.
	 */
	if (!percpu_ref_tryget_live(&fba_dmabuf->attach_ref)) {
		ret = -ENOENT;
		goto out_free_attach;
	}

#ifdef CONFIG_PCI_P2PDMA
	if (pci_p2pdma_distance_many(fba_dmabuf->pdev, &dmabuf_attach->dev, 1,
				     true) < 0) {
		ret = -ENODEV;
		goto out_percpu_ref_put;
	}
#endif
	/*
	 * check if exporting device is alive; this can
	 * be racy but stops the imported from issuing
	 * I/O to the exporting device after the driver
	 * has merked it failed.
	 */
	peer_dev = pci_get_drvdata(fba_dmabuf->pdev);
	if (atomic_read(&peer_dev->has_failed)) {
		ret = -ENXIO;
		goto out_percpu_ref_put;
	}

	dmabuf_attach->priv = fba_dmabuf_attach;
	fba_dmabuf_attach->attach = dmabuf_attach;
	fba_dmabuf_attach->size = dmabuf->size;

	return 0;

out_percpu_ref_put:
	percpu_ref_put(&fba_dmabuf->attach_ref);
out_free_attach:
	kfree(fba_dmabuf_attach);

	return ret;
}

static void fba_dmabuf_ops_detach(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *dmabuf_attach)
{
	struct fba_dmabuf *fba_dmabuf = dmabuf->priv;
	struct fba_dmabuf_attach *fba_dmabuf_attach = dmabuf_attach->priv;

	percpu_ref_put(&fba_dmabuf->attach_ref);
	dmabuf_attach->priv = NULL;
	kfree(fba_dmabuf_attach);
}

static struct sg_table *
fba_dmabuf_ops_map(struct dma_buf_attachment *dmabuf_attach,
		   enum dma_data_direction dma_dir)
{
	struct dma_buf *dmabuf = dmabuf_attach->dmabuf;
	struct fba_dmabuf *fba_dmabuf = dmabuf->priv;
	struct fba_dmabuf_attach *fba_dmabuf_attach = dmabuf_attach->priv;
	struct sg_table *sgt;
	struct scatterlist *sg;
#if 0
	size_t dma_max_seg_size;
#endif

	/*
	 * Exported device buffers are contiguous, so we only need 1 scatterlist entry
	 * as long as buffer size is < device max dma segment size.
	 */

	sg_init_table(&fba_dmabuf_attach->sgl, 1);
	sgt = &fba_dmabuf_attach->sgt;
	sgt->sgl = sg = &fba_dmabuf_attach->sgl;
	sgt->nents = 1;
	sgt->orig_nents = 1;

	/* disabled for now since somce lab kernels don't implement dma_max_seg_size */
#if 0
	dma_max_seg_size = dma_get_max_seg_size(dmabuf_attach->dev);
	if (dma_max_seg_size < fba_dmabuf_attach->size) {
		/*
		 * TODO: fail (handle) importers whose max DMA segment size is < dmabuf size.
		 */
		return ERR_PTR(-EINVAL);
	}
#endif

	sg_dma_address(sg) = fba_dmabuf->bus_base;
	sg_dma_len(sg) = dmabuf->size;
	sg->offset = 0;

	return sgt;
}

static void fba_dmabuf_ops_unmap(struct dma_buf_attachment *dmabuf_attach,
				 struct sg_table *sgt,
				 enum dma_data_direction dma_dir)
{
}

static void fba_dmabuf_ops_release(struct dma_buf *dmabuf)
{
	struct fba_dmabuf *fba_dmabuf = dmabuf->priv;

	kref_put(&fba_dmabuf->kref, fba_dmabuf_free);
}

static const struct dma_buf_ops fba_dmabuf_ops = {
	.attach = fba_dmabuf_ops_attach,
	.detach = fba_dmabuf_ops_detach,
	.map_dma_buf = fba_dmabuf_ops_map,
	.unmap_dma_buf = fba_dmabuf_ops_unmap,
	.release = fba_dmabuf_ops_release,
};

/*
 * Export a buffer as a dmabuf.
 */
int fba_get_dmabuf(struct fba_ctx *ctx, uint64_t rid, int *rfd)
{
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	int id = (rid >> RID_ID_SHIFT);
	uint64_t offset = (rid & RID_OFFS_MASK);
	uint64_t alloc_size;
	struct fba_alloc *alloc = NULL;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *buf;
	struct fba_dmabuf *fba_dmabuf;
	int ret = -EINVAL;

	if (fba_dev->cfg->p2p_bar == -1)
		return -EOPNOTSUPP;

	/* Skip NULL IDs. */
	if (rid == 0)
		return ret;

	alloc = fba_ctx_find_alloc(ctx, id);
	if (alloc == NULL) {
		fba_dev_err(fba_dev,
			    "No allocation exists for resource id %#llx", rid);
		return ret;
	}

	if (alloc->resource->type != RT_MEMORY ||
	    (alloc->resource->subtype != MT_P2P_DRAM &&
	     alloc->resource->subtype != MT_P2P_ECC_DRAM)) {
		fba_dev_err(fba_dev, "Invalid resource for P2P DMA");
		goto out_alloc_put;
	}

	alloc_size = alloc->size;

	/* End location in range must be within size. */
	if (offset >= alloc_size) {
		fba_dev_err(
			fba_dev,
			"Offset exceeds alloc capacity! offset=%#llx, alloc_size=%#llx.",
			offset, alloc_size);
		goto out_alloc_put;
	}

	fba_dmabuf = kzalloc(sizeof(*fba_dmabuf), GFP_KERNEL);
	if (!fba_dmabuf) {
		ret = -ENOMEM;
		goto out_alloc_put;
	}
	exp_info.ops = &fba_dmabuf_ops;
	exp_info.size = alloc_size;
	exp_info.priv = fba_dev;
	exp_info.flags = O_RDWR;

	buf = dma_buf_export(&exp_info);
	if (IS_ERR(buf)) {
		fba_dev_err(fba_dev, "dmabuf export failed with error %ld",
			    PTR_ERR(buf));
		ret = PTR_ERR(buf);
		goto out_free_dmabuf;
	}

	ret = percpu_ref_init(&fba_dmabuf->attach_ref,
			      fba_dmabuf_percpu_release, 0, GFP_KERNEL);
	if (ret)
		goto out_dma_buf_put;

	fba_dmabuf->dmabuf = buf;

	fba_dmabuf->bus_base =
		pci_resource_start(fba_dev->pdev, fba_dev->cfg->p2p_bar) +
		alloc->addr - alloc->resource->start + offset;

	kref_init(&fba_dmabuf->kref);

	/*
	 * pdev is safe to use without grabbing a
	 * reference as long as attach_ref is live.
	 */
	fba_dmabuf->pdev = fba_dev->pdev;

	/*
	 * grab another reference for the alloc
	 */
	kref_get(&fba_dmabuf->kref);
	init_completion(&fba_dmabuf->detaches_done);

	buf->priv = fba_dmabuf;

	mutex_lock(&alloc->dma_buf_mutex);
	/*
	 * Disallow export if context is not open.
	 * This prevents new exports after the context
	 * is stopped. The check is done holding the lock to
	 * synchronize with the fba_dmabuf_exit call from
	 * fba_ctx_stop.
	 */
	if (!fba_ctx_is_open(ctx)) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = dma_buf_fd(buf, O_CLOEXEC);
	if (ret < 0)
		goto out_unlock;
	*rfd = ret;

	list_add_tail(&fba_dmabuf->list, &alloc->dma_buf_list);
	mutex_unlock(&alloc->dma_buf_mutex);

	/*
	 * Drop reference to alloc after adding to alloc dma_buf_list.
	 */
	fba_alloc_put(alloc);

	return 0;

out_unlock:
	mutex_unlock(&alloc->dma_buf_mutex);
	percpu_ref_exit(&fba_dmabuf->attach_ref);
out_dma_buf_put:
	dma_buf_put(buf);
out_free_dmabuf:
	kfree(fba_dmabuf);
out_alloc_put:
	fba_alloc_put(alloc);

	return ret;
}

/*
 * Cleanup all dmabufs for this resource allocation, called ether from fba_alloc_free
 * or from fba_ctx_stop.
 */
void fba_dmabuf_exit(struct fba_alloc *alloc)
{
	struct fba_dmabuf *fba_dmabuf, *fba_dmabuf2;
	struct list_head head;

	INIT_LIST_HEAD(&head);

	mutex_lock(&alloc->dma_buf_mutex);
	list_splice_init(&alloc->dma_buf_list, &head);
	mutex_unlock(&alloc->dma_buf_mutex);

	list_for_each_entry_safe(fba_dmabuf, fba_dmabuf2, &head, list) {
		list_del(&fba_dmabuf->list);

		/*
		 * Kill attach ref - this prevents importers
		 * from grabbing a live reference.
		 */
		percpu_ref_kill(&fba_dmabuf->attach_ref);

		/*
		 * Wait for active attachments to finish,
		 * no new valid attachments can be added to this dmabuf.
		 */
		wait_for_completion(&fba_dmabuf->detaches_done);
		kref_put(&fba_dmabuf->kref, fba_dmabuf_free);
	}
}

#else

struct fba_dmabuf_attach *fba_translate_p2p_setup(struct fba_ctx *ctx, int fd,
						  size_t offset, size_t size)
{
	return ERR_PTR(-EOPNOTSUPP);
}

void fba_translate_p2p_teardown(struct fba_ctx *ctx,
				struct fba_dmabuf_attach *fba_dmabuf_attach)
{
}

int fba_get_dmabuf(struct fba_ctx *ctx, uint64_t rid, int *rfd)
{
	return -EOPNOTSUPP;
}

void fba_dmabuf_exit(struct fba_alloc *alloc)
{
}
#endif
