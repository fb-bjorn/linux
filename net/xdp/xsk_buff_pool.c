// SPDX-License-Identifier: GPL-2.0

#include <linux/bitmap.h>
#include <linux/netdevice.h>
#include <net/netdev_lock.h>
#include <net/netdev_queues.h>
#include <net/netdev_rx_queue.h>
#include <net/page_pool/helpers.h>
#include <net/page_pool/memory_provider.h>
#include <net/xsk_buff_pool.h>
#include <net/xdp_sock.h>
#include <net/xdp_sock_drv.h>

#include "xsk_queue.h"
#include "xdp_umem.h"
#include "xsk.h"

#define ETH_PAD_LEN (ETH_HLEN + 2 * VLAN_HLEN  + ETH_FCS_LEN)
#define XSK_PAGE_POOL_DMA_ATTR	(DMA_ATTR_WEAK_ORDERING | \
				 DMA_ATTR_SKIP_CPU_SYNC)
#define XSK_POOL_RELEASE_RETRY	HZ

/* Keep page_pool descriptors independent from direct-driver XSK buffers. */
struct xsk_pp {
	struct xsk_pp_info info;
	struct xsk_queue *fq;
	struct device *dev;
	u64 chunk_mask;
	u64 addrs_cnt;
	unsigned long *in_use;
	/* Protects the reuse stack and ownership bitmap. */
	spinlock_t lock;
	u32 reuse_cnt;
	bool dma_need_sync;
	u32 reuse[];
};

void xp_add_xsk(struct xsk_buff_pool *pool, struct xdp_sock *xs)
{
	if (!xs->tx)
		return;

	spin_lock(&pool->xsk_tx_list_lock);
	list_add_rcu(&xs->tx_list, &pool->xsk_tx_list);
	spin_unlock(&pool->xsk_tx_list_lock);
}

void xp_del_xsk(struct xsk_buff_pool *pool, struct xdp_sock *xs)
{
	if (!xs->tx)
		return;

	spin_lock(&pool->xsk_tx_list_lock);
	list_del_rcu(&xs->tx_list);
	spin_unlock(&pool->xsk_tx_list_lock);
}

void xp_destroy(struct xsk_buff_pool *pool)
{
	if (!pool)
		return;

	kvfree(pool->tx_descs);
	kvfree(pool->heads);
	kvfree(pool);
}

int xp_alloc_tx_descs(struct xsk_buff_pool *pool, struct xdp_sock *xs,
		      u32 max_segs)
{
	u32 nentries = max(xs->tx->nentries, max_segs);

	pool->tx_descs = kvzalloc_objs(*pool->tx_descs, nentries);
	if (!pool->tx_descs)
		return -ENOMEM;

	pool->tx_descs_nentries = nentries;
	return 0;
}

struct xsk_buff_pool *xp_create_and_assign_umem(struct xdp_sock *xs,
						struct xdp_umem *umem,
						u32 max_segs)
{
	bool unaligned = umem->flags & XDP_UMEM_UNALIGNED_CHUNK_FLAG;
	struct xsk_buff_pool *pool;
	struct xdp_buff_xsk *xskb;
	u32 i, entries;

	entries = unaligned ? umem->chunks : 0;
	pool = kvzalloc_flex(*pool, free_heads, entries);
	if (!pool)
		goto out;

	pool->heads = kvzalloc_objs(*pool->heads, umem->chunks);
	if (!pool->heads)
		goto out;

	if (xs->tx)
		if (xp_alloc_tx_descs(pool, xs, max_segs))
			goto out;

	pool->chunk_mask = ~((u64)umem->chunk_size - 1);
	pool->addrs_cnt = umem->size;
	pool->heads_cnt = umem->chunks;
	pool->free_heads_cnt = umem->chunks;
	pool->headroom = umem->headroom;
	pool->chunk_size = umem->chunk_size;
	pool->chunk_shift = ffs(umem->chunk_size) - 1;
	pool->unaligned = unaligned;
	pool->frame_len = umem->chunk_size - umem->headroom -
		XDP_PACKET_HEADROOM;
	pool->umem = umem;
	pool->addrs = umem->addrs;
	pool->tx_metadata_len = umem->tx_metadata_len;
	pool->tx_sw_csum = umem->flags & XDP_UMEM_TX_SW_CSUM;
	spin_lock_init(&pool->rx_lock);
	INIT_LIST_HEAD(&pool->free_list);
	INIT_LIST_HEAD(&pool->xskb_list);
	INIT_LIST_HEAD(&pool->xsk_tx_list);
	spin_lock_init(&pool->xsk_tx_list_lock);
	spin_lock_init(&pool->cq_prod_lock);
	spin_lock_init(&xs->cq_tmp->cq_cached_prod_lock);
	refcount_set(&pool->users, 1);

	pool->fq = xs->fq_tmp;
	pool->cq = xs->cq_tmp;

	for (i = 0; i < pool->free_heads_cnt; i++) {
		xskb = &pool->heads[i];
		xskb->pool = pool;
		xskb->xdp.frame_sz = umem->chunk_size - umem->headroom;
		INIT_LIST_HEAD(&xskb->list_node);
		if (pool->unaligned)
			pool->free_heads[i] = xskb;
		else
			xp_init_xskb_addr(xskb, pool, (u64)i * pool->chunk_size);
	}

	return pool;

out:
	xp_destroy(pool);
	return NULL;
}

void xp_set_rxq_info(struct xsk_buff_pool *pool, struct xdp_rxq_info *rxq)
{
	u32 i;

	for (i = 0; i < pool->heads_cnt; i++)
		pool->heads[i].xdp.rxq = rxq;
}
EXPORT_SYMBOL(xp_set_rxq_info);

void xp_fill_cb(struct xsk_buff_pool *pool, struct xsk_cb_desc *desc)
{
	u32 i;

	for (i = 0; i < pool->heads_cnt; i++) {
		struct xdp_buff_xsk *xskb = &pool->heads[i];

		memcpy(xskb->cb + desc->off, desc->src, desc->bytes);
	}
}
EXPORT_SYMBOL(xp_fill_cb);

static int xp_disable_drv_zc(struct xsk_buff_pool *pool)
{
	struct netdev_bpf bpf = {};

	ASSERT_RTNL();

	if (!pool->umem->zc)
		return 0;

	bpf.command = XDP_SETUP_XSK_POOL;
	bpf.xsk.pool = NULL;
	bpf.xsk.queue_id = pool->queue_id;

	return pool->netdev->netdev_ops->ndo_bpf(pool->netdev, &bpf);
}

int xp_assign_dev(struct xsk_buff_pool *pool,
		  struct net_device *netdev, u16 queue_id, u16 flags)
{
	u32 needed = netdev->mtu + ETH_PAD_LEN;
	u32 segs = netdev->xdp_zc_max_segs;
	bool mbuf = flags & XDP_USE_SG;
	bool force_zc, force_copy;
	struct netdev_bpf bpf = {};
	u32 frame_size;
	int err = 0;

	ASSERT_RTNL();

	force_zc = flags & XDP_ZEROCOPY;
	force_copy = flags & XDP_COPY;

	if (force_zc && force_copy)
		return -EINVAL;

	if (pool->tx_sw_csum && (netdev->priv_flags & IFF_TX_SKB_NO_LINEAR))
		return -EOPNOTSUPP;

	if (xsk_get_pool_from_qid(netdev, queue_id))
		return -EBUSY;

	pool->netdev = netdev;
	pool->queue_id = queue_id;
	err = xsk_reg_pool_at_qid(netdev, pool, queue_id);
	if (err)
		return err;

	if (mbuf)
		pool->umem->flags |= XDP_UMEM_SG_FLAG;

	if (flags & XDP_USE_NEED_WAKEUP)
		pool->uses_need_wakeup = true;
	/* Tx needs to be explicitly woken up the first time.  Also
	 * for supporting drivers that do not implement this
	 * feature. They will always have to call sendto() or poll().
	 */
	pool->cached_need_wakeup = XDP_WAKEUP_TX;

	dev_hold(netdev);

	if (force_copy)
		/* For copy-mode, we are done. */
		return 0;

	if ((netdev->xdp_features & NETDEV_XDP_ACT_XSK) != NETDEV_XDP_ACT_XSK) {
		err = -EOPNOTSUPP;
		goto err_unreg_pool;
	}

	if (mbuf) {
		if (segs == 1) {
			err = -EOPNOTSUPP;
			goto err_unreg_pool;
		}
	} else {
		segs = 1;
	}

	/* open-code xsk_pool_get_rx_frame_size() as pool->dev is not
	 * set yet at this point; we are before getting down to driver
	 */
	frame_size = __xsk_pool_get_rx_frame_size(pool) -
		     xsk_pool_get_tailroom(mbuf);
	frame_size = ALIGN_DOWN(frame_size, 128);

	if (needed > frame_size * segs) {
		err = -EINVAL;
		goto err_unreg_pool;
	}

	if (netif_mp_unsupported(netdev, MEM_PROVIDER_CAP_XDP)) {
		err = -EBUSY;
		goto err_unreg_pool;
	}

	bpf.command = XDP_SETUP_XSK_POOL;
	bpf.xsk.pool = pool;
	bpf.xsk.queue_id = queue_id;

	netdev_assert_locked_ops_compat(netdev);
	err = netdev->netdev_ops->ndo_bpf(netdev, &bpf);
	if (err)
		goto err_unreg_pool;

	if (!pool->dma_pages) {
		WARN(1, "Driver did not DMA map zero-copy buffers");
		err = -EINVAL;
		goto err_unreg_xsk;
	}
	pool->umem->zc = true;
	pool->xdp_zc_max_segs = netdev->xdp_zc_max_segs;
	return 0;

err_unreg_xsk:
	WARN_ON_ONCE(xp_disable_drv_zc(pool));
err_unreg_pool:
	if (!force_zc)
		err = 0; /* fallback to copy mode */
	if (err) {
		xsk_clear_pool_at_qid(netdev, queue_id);
		dev_put(netdev);
	}
	return err;
}

int xp_assign_dev_shared(struct xsk_buff_pool *pool, struct xdp_sock *umem_xs,
			 struct net_device *dev, u16 queue_id)
{
	u16 flags;
	struct xdp_umem *umem = umem_xs->umem;

	flags = umem->zc ? XDP_ZEROCOPY : XDP_COPY;

	if (umem->flags & XDP_UMEM_SG_FLAG)
		flags |= XDP_USE_SG;

	if (umem_xs->pool->uses_need_wakeup)
		flags |= XDP_USE_NEED_WAKEUP;

	return xp_assign_dev(pool, dev, queue_id, flags);
}

int xp_clear_dev(struct xsk_buff_pool *pool)
{
	struct net_device *netdev = pool->netdev;
	int err;

	if (!netdev)
		return 0;

	netdev_lock_ops(netdev);
	err = xp_disable_drv_zc(pool);
	if (err)
		goto unlock;

	xsk_clear_pool_at_qid(pool->netdev, pool->queue_id);
	pool->netdev = NULL;
unlock:
	netdev_unlock_ops(netdev);
	if (err)
		return err;

	dev_put(netdev);
	return 0;
}

static void xp_release_deferred(struct work_struct *work)
{
	struct xsk_buff_pool *pool;
	int err;

	pool = container_of(to_delayed_work(work), struct xsk_buff_pool, work);

	rtnl_lock();
	err = xp_clear_dev(pool);
	rtnl_unlock();
	if (err) {
		WARN_ON_ONCE(err);
		/* Retry while the queue still owns the pool. */
		schedule_delayed_work(&pool->work, XSK_POOL_RELEASE_RETRY);
		return;
	}

	if (pool->fq) {
		xskq_destroy(pool->fq);
		pool->fq = NULL;
	}

	if (pool->cq) {
		xskq_destroy(pool->cq);
		pool->cq = NULL;
	}

	xdp_put_umem(pool->umem, false);
	xp_destroy(pool);
}

void xp_get_pool(struct xsk_buff_pool *pool)
{
	refcount_inc(&pool->users);
}

bool xp_put_pool(struct xsk_buff_pool *pool)
{
	if (!pool)
		return false;

	if (refcount_dec_and_test(&pool->users)) {
		INIT_DELAYED_WORK(&pool->work, xp_release_deferred);
		schedule_delayed_work(&pool->work, 0);
		return true;
	}

	return false;
}

static struct xsk_dma_map *xp_find_dma_map(struct xsk_buff_pool *pool)
{
	struct xsk_dma_map *dma_map;

	list_for_each_entry(dma_map, &pool->umem->xsk_dma_list, list) {
		if (dma_map->netdev == pool->netdev)
			return dma_map;
	}

	return NULL;
}

static struct xsk_dma_map *xp_create_dma_map(struct device *dev, struct net_device *netdev,
					     u32 nr_pages, struct xdp_umem *umem)
{
	struct xsk_dma_map *dma_map;

	dma_map = kzalloc_obj(*dma_map);
	if (!dma_map)
		return NULL;

	dma_map->dma_pages = kvzalloc_objs(*dma_map->dma_pages, nr_pages);
	if (!dma_map->dma_pages) {
		kfree(dma_map);
		return NULL;
	}

	dma_map->netdev = netdev;
	dma_map->dev = dev;
	dma_map->dma_pages_cnt = nr_pages;
	refcount_set(&dma_map->users, 1);
	list_add(&dma_map->list, &umem->xsk_dma_list);
	return dma_map;
}

static void xp_destroy_dma_map(struct xsk_dma_map *dma_map)
{
	list_del(&dma_map->list);
	kvfree(dma_map->dma_pages);
	kfree(dma_map);
}

static void __xp_dma_unmap(struct xsk_dma_map *dma_map, unsigned long attrs)
{
	dma_addr_t *dma;
	u32 i;

	for (i = 0; i < dma_map->dma_pages_cnt; i++) {
		dma = &dma_map->dma_pages[i];
		if (*dma) {
			*dma &= ~XSK_NEXT_PG_CONTIG_MASK;
			dma_unmap_page_attrs(dma_map->dev, *dma, PAGE_SIZE,
					     DMA_BIDIRECTIONAL, attrs);
			*dma = 0;
		}
	}

	xp_destroy_dma_map(dma_map);
}

void xp_dma_unmap(struct xsk_buff_pool *pool, unsigned long attrs)
{
	struct xsk_dma_map *dma_map;

	if (!pool->dma_pages)
		return;

	dma_map = xp_find_dma_map(pool);
	if (!dma_map) {
		WARN(1, "Could not find dma_map for device");
		return;
	}

	if (refcount_dec_and_test(&dma_map->users))
		__xp_dma_unmap(dma_map, attrs);

	kvfree(pool->dma_pages);
	pool->dma_pages = NULL;
	pool->dma_pages_cnt = 0;
	pool->dev = NULL;
}
EXPORT_SYMBOL(xp_dma_unmap);

static void xp_check_dma_contiguity(struct xsk_dma_map *dma_map)
{
	u32 i;

	for (i = 0; i < dma_map->dma_pages_cnt - 1; i++) {
		if (dma_map->dma_pages[i] + PAGE_SIZE == dma_map->dma_pages[i + 1])
			dma_map->dma_pages[i] |= XSK_NEXT_PG_CONTIG_MASK;
		else
			dma_map->dma_pages[i] &= ~XSK_NEXT_PG_CONTIG_MASK;
	}
}

static int xp_init_dma_info(struct xsk_buff_pool *pool, struct xsk_dma_map *dma_map)
{
	if (!pool->unaligned) {
		u32 i;

		for (i = 0; i < pool->heads_cnt; i++) {
			struct xdp_buff_xsk *xskb = &pool->heads[i];
			u64 orig_addr;

			orig_addr = xskb->xdp.data_hard_start - pool->addrs - pool->headroom;
			xp_init_xskb_dma(xskb, pool, dma_map->dma_pages, orig_addr);
		}
	}

	pool->dma_pages = kvzalloc_objs(*pool->dma_pages,
					dma_map->dma_pages_cnt);
	if (!pool->dma_pages)
		return -ENOMEM;

	pool->dev = dma_map->dev;
	pool->dma_pages_cnt = dma_map->dma_pages_cnt;
	memcpy(pool->dma_pages, dma_map->dma_pages,
	       pool->dma_pages_cnt * sizeof(*pool->dma_pages));

	return 0;
}

int xp_dma_map(struct xsk_buff_pool *pool, struct device *dev,
	       unsigned long attrs, struct page **pages, u32 nr_pages)
{
	struct xsk_dma_map *dma_map;
	dma_addr_t dma;
	int err;
	u32 i;

	dma_map = xp_find_dma_map(pool);
	if (dma_map) {
		err = xp_init_dma_info(pool, dma_map);
		if (err)
			return err;

		refcount_inc(&dma_map->users);
		return 0;
	}

	dma_map = xp_create_dma_map(dev, pool->netdev, nr_pages, pool->umem);
	if (!dma_map)
		return -ENOMEM;

	for (i = 0; i < dma_map->dma_pages_cnt; i++) {
		dma = dma_map_page_attrs(dev, pages[i], 0, PAGE_SIZE,
					 DMA_BIDIRECTIONAL, attrs);
		if (dma_mapping_error(dev, dma)) {
			__xp_dma_unmap(dma_map, attrs);
			return -ENOMEM;
		}
		dma_map->dma_pages[i] = dma;
	}

	if (pool->unaligned)
		xp_check_dma_contiguity(dma_map);

	err = xp_init_dma_info(pool, dma_map);
	if (err) {
		__xp_dma_unmap(dma_map, attrs);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL(xp_dma_map);

static void xp_pp_free(struct xsk_buff_pool *pool)
{
	struct xsk_pp *provider = pool->pp;

	if (!provider)
		return;

	kvfree(provider->in_use);
	kvfree(provider->info.area.niovs);
	kvfree(provider);
	pool->pp = NULL;
}

static struct xsk_pp *xp_pp_create(struct xsk_buff_pool *pool)
{
	struct xsk_pp *provider;
	struct net_iov *niov;
	dma_addr_t dma;
	u64 addr;
	u32 i;

	provider = kvzalloc_flex(*provider, reuse, pool->heads_cnt);
	if (!provider)
		return ERR_PTR(-ENOMEM);

	provider->info.area.niovs =
		kvzalloc_objs(*provider->info.area.niovs, pool->heads_cnt);
	if (!provider->info.area.niovs) {
		kvfree(provider);
		return ERR_PTR(-ENOMEM);
	}
	provider->in_use = kvcalloc(BITS_TO_LONGS(pool->heads_cnt),
				    sizeof(*provider->in_use), GFP_KERNEL);
	if (!provider->in_use) {
		kvfree(provider->info.area.niovs);
		kvfree(provider);
		return ERR_PTR(-ENOMEM);
	}

	provider->info.pool = pool;
	provider->fq = pool->fq;
	provider->dev = pool->dev;
	provider->info.area.num_niovs = pool->heads_cnt;
	provider->info.area.base_virtual = (unsigned long)pool->addrs;
	provider->info.addrs = pool->addrs;
	provider->chunk_mask = pool->chunk_mask;
	provider->addrs_cnt = pool->addrs_cnt;
	provider->info.chunk_shift = pool->chunk_shift;
	provider->dma_need_sync = dma_dev_need_sync(pool->dev);
	spin_lock_init(&provider->lock);

	for (i = 0; i < provider->info.area.num_niovs; i++) {
		niov = &provider->info.area.niovs[i];
		addr = (u64)i << provider->info.chunk_shift;
		dma = (pool->dma_pages[addr >> PAGE_SHIFT] &
		       ~XSK_NEXT_PG_CONTIG_MASK) + (addr & ~PAGE_MASK);

		net_iov_init(niov, &provider->info.area, NET_IOV_XSK);
		if (net_mp_niov_set_dma_addr(niov, dma))
			goto err_free_niovs;
	}

	pool->pp = provider;
	return provider;

err_free_niovs:
	kvfree(provider->in_use);
	kvfree(provider->info.area.niovs);
	kvfree(provider);
	return ERR_PTR(-ERANGE);
}

static int xp_pp_init(struct page_pool *pp)
{
	struct xsk_buff_pool *pool = pp->mp_priv;
	struct xsk_pp *provider = pool->pp;

	if (!provider || !pool->dma_pages)
		return -EINVAL;
	if (pp->p.order)
		return -E2BIG;

	return 0;
}

static void xp_pp_destroy(struct page_pool *pp)
{
}

static netmem_ref xp_pp_prepare_netmem(struct page_pool *pp,
				       struct xsk_pp *provider,
				       struct net_iov *niov)
{
	netmem_ref netmem = net_iov_to_netmem(niov);
	dma_addr_t dma = netmem_get_dma_addr(netmem);

	if (provider->dma_need_sync)
		dma_sync_single_range_for_device(provider->dev, dma,
						 pp->p.offset, pp->p.max_len,
						 pp->p.dma_dir);

	return netmem;
}

static unsigned int
xp_pp_alloc_reused(struct page_pool *pp, struct xsk_pp *provider,
		   netmem_ref *netmems, unsigned int max)
{
	struct net_iov *niov;
	unsigned int allocated;
	unsigned int i;
	u32 idx;

	spin_lock_bh(&provider->lock);
	allocated = min(max, provider->reuse_cnt);
	for (i = 0; i < allocated; i++) {
		idx = provider->reuse[--provider->reuse_cnt];
		niov = &provider->info.area.niovs[idx];
		netmems[i] = net_iov_to_netmem(niov);
	}
	spin_unlock_bh(&provider->lock);

	for (i = 0; i < allocated; i++) {
		niov = netmem_to_net_iov(netmems[i]);
		netmems[i] = xp_pp_prepare_netmem(pp, provider, niov);
	}

	return allocated;
}

static unsigned int
xp_pp_alloc_fq(struct page_pool *pp, struct xsk_pp *provider,
	       netmem_ref *netmems, unsigned int max)
{
	struct xsk_queue *fq = provider->fq;
	struct net_iov *niov;
	u64 addr;
	unsigned int allocated = 0;
	u32 cached_cons, entries;
	u32 idx;
	u32 i;

	spin_lock_bh(&provider->lock);
	if (!fq)
		goto unlock;

	entries = xskq_cons_nb_entries(fq, max);
	if (!entries) {
		fq->queue_empty_descs++;
		goto unlock;
	}

	cached_cons = fq->cached_cons;
	for (i = 0; i < entries; i++) {
		__xskq_cons_read_addr_unchecked(fq, cached_cons++, &addr);
		addr &= provider->chunk_mask;
		if (unlikely(addr >= provider->addrs_cnt)) {
			fq->invalid_descs++;
			continue;
		}

		idx = addr >> provider->info.chunk_shift;
		if (__test_and_set_bit(idx, provider->in_use)) {
			fq->invalid_descs++;
			continue;
		}
		niov = &provider->info.area.niovs[idx];
		netmems[allocated++] = net_iov_to_netmem(niov);
	}

	xskq_cons_release_n(fq, entries);
	__xskq_cons_release(fq);
	spin_unlock_bh(&provider->lock);

	for (i = 0; i < allocated; i++) {
		niov = netmem_to_net_iov(netmems[i]);
		netmems[i] = xp_pp_prepare_netmem(pp, provider, niov);
	}

	return allocated;

unlock:
	spin_unlock_bh(&provider->lock);
	return 0;
}

static netmem_ref xp_pp_alloc_netmems(struct page_pool *pp, gfp_t gfp)
{
	struct xsk_buff_pool *pool = pp->mp_priv;
	struct xsk_pp *provider = pool->pp;
	netmem_ref *netmems = pp->alloc.cache;
	unsigned int allocated;

	if (WARN_ON_ONCE(pp->alloc.count))
		return 0;

	allocated = xp_pp_alloc_reused(pp, provider, netmems,
				       PP_ALLOC_CACHE_REFILL);
	if (allocated < PP_ALLOC_CACHE_REFILL)
		allocated += xp_pp_alloc_fq(pp, provider, netmems + allocated,
					    PP_ALLOC_CACHE_REFILL - allocated);

	if (!allocated)
		return 0;

	net_mp_set_pool_bulk(pp, netmems, allocated);

	allocated--;
	pp->alloc.count = allocated;
	return netmems[allocated];
}

static bool xp_pp_release_netmem(struct page_pool *pp, netmem_ref netmem)
{
	struct xsk_buff_pool *pool = pp->mp_priv;
	struct xsk_pp *provider = pool->pp;
	struct net_iov *niov = netmem_to_net_iov(netmem);
	u32 idx = net_iov_idx(niov);

	net_mp_niov_clear_page_pool(niov);

	spin_lock_bh(&provider->lock);
	if (WARN_ON_ONCE(!test_bit(idx, provider->in_use)) ||
	    WARN_ON_ONCE(provider->reuse_cnt >= provider->info.area.num_niovs))
		goto unlock;
	provider->reuse[provider->reuse_cnt++] = idx;

unlock:
	spin_unlock_bh(&provider->lock);

	return false;
}

void xp_pp_release_to_user_bulk(netmem_ref *netmems, u32 count)
{
	struct xsk_pp_info *info = xp_netmem_to_pp(netmems[0]);
	struct xsk_pp *provider = container_of(info, struct xsk_pp, info);
	u32 i;

	spin_lock_bh(&provider->lock);
	for (i = 0; i < count; i++) {
		u32 idx = net_iov_idx(netmem_to_net_iov(netmems[i]));

		WARN_ON_ONCE(!__test_and_clear_bit(idx, provider->in_use));
	}
	spin_unlock_bh(&provider->lock);
}

static int xp_pp_nl_fill(void *mp_priv, struct sk_buff *rsp,
			 struct netdev_rx_queue *rxq)
{
	return 0;
}

static void xp_pp_uninstall(void *mp_priv, struct netdev_rx_queue *rxq)
{
	struct xsk_buff_pool *pool = mp_priv;
	struct net_device *netdev = pool->netdev;

	if (WARN_ON_ONCE(!netdev))
		goto free_provider;

	/* Device unregister supersedes a failed deferred queue close. */
	xsk_pool_dma_unmap(pool, XSK_PAGE_POOL_DMA_ATTR);
	xsk_clear_pool_at_qid(netdev, pool->queue_id);
	pool->netdev = NULL;
	dev_put(netdev);

free_provider:
	xp_pp_free(pool);

	memset(&rxq->mp_params, 0, sizeof(rxq->mp_params));
}

static const struct memory_provider_ops xsk_pp_ops = {
	.init			= xp_pp_init,
	.destroy		= xp_pp_destroy,
	.alloc_netmems		= xp_pp_alloc_netmems,
	.release_netmem		= xp_pp_release_netmem,
	.nl_fill		= xp_pp_nl_fill,
	.uninstall		= xp_pp_uninstall,
	.capabilities		= MEM_PROVIDER_CAP_XDP | MEM_PROVIDER_CAP_HDS,
};

int xsk_pool_setup_page_pool(struct net_device *dev, struct xsk_buff_pool *pool,
			     u16 queue_id, struct netlink_ext_ack *extack)
{
	struct xsk_pp *provider;
	struct pp_memory_provider_params mp = {
		.mp_ops = &xsk_pp_ops,
	};
	int err;

	ASSERT_RTNL();

	if (!pool) {
		pool = xsk_get_pool_from_qid(dev, queue_id);
		if (!pool)
			return -EINVAL;

		mp.mp_priv = pool;
		err = netif_mp_close_rxq_xsk(dev, queue_id, &mp);
		if (err)
			return err;

		xsk_pool_dma_unmap(pool, XSK_PAGE_POOL_DMA_ATTR);
		xp_pp_free(pool);
		return 0;
	}
	if (pool->unaligned)
		return -EOPNOTSUPP;
	/* Each provider object represents one complete UMEM chunk. */
	if (pool->chunk_size != PAGE_SIZE) {
		NL_SET_ERR_MSG(extack,
			       "page pool UMEM requires page-sized chunks");
		return -EOPNOTSUPP;
	}

	err = xsk_pool_dma_map(pool, dev->dev.parent, XSK_PAGE_POOL_DMA_ATTR);
	if (err)
		return err;

	provider = xp_pp_create(pool);
	if (IS_ERR(provider)) {
		err = PTR_ERR(provider);
		goto err_unmap;
	}

	mp.mp_priv = pool;
	mp.rx_page_size = pool->chunk_size;
	err = netif_mp_open_rxq_xsk(dev, queue_id, &mp, extack);
	if (err)
		goto err_free_provider;

	return 0;

err_free_provider:
	xp_pp_free(pool);
err_unmap:
	xsk_pool_dma_unmap(pool, XSK_PAGE_POOL_DMA_ATTR);
	return err;
}
EXPORT_SYMBOL_GPL(xsk_pool_setup_page_pool);

static bool xp_addr_crosses_non_contig_pg(struct xsk_buff_pool *pool,
					  u64 addr)
{
	return xp_desc_crosses_non_contig_pg(pool, addr, pool->chunk_size);
}

static bool xp_check_unaligned(struct xsk_buff_pool *pool, u64 *addr)
{
	*addr = xp_unaligned_extract_addr(*addr);
	if (*addr >= pool->addrs_cnt ||
	    *addr + pool->chunk_size > pool->addrs_cnt ||
	    xp_addr_crosses_non_contig_pg(pool, *addr))
		return false;
	return true;
}

static bool xp_check_aligned(struct xsk_buff_pool *pool, u64 *addr)
{
	*addr = xp_aligned_extract_addr(pool, *addr);
	return *addr < pool->addrs_cnt;
}

static struct xdp_buff_xsk *xp_get_xskb(struct xsk_buff_pool *pool, u64 addr)
{
	struct xdp_buff_xsk *xskb;

	if (pool->unaligned) {
		xskb = pool->free_heads[--pool->free_heads_cnt];
		xp_init_xskb_addr(xskb, pool, addr);
		if (pool->dma_pages)
			xp_init_xskb_dma(xskb, pool, pool->dma_pages, addr);
	} else {
		xskb = &pool->heads[xp_aligned_extract_idx(pool, addr)];
	}

	return xskb;
}

static struct xdp_buff_xsk *__xp_alloc(struct xsk_buff_pool *pool)
{
	struct xdp_buff_xsk *xskb;
	u64 addr;
	bool ok;

	if (pool->free_heads_cnt == 0)
		return NULL;

	for (;;) {
		if (!xskq_cons_peek_addr_unchecked(pool->fq, &addr)) {
			pool->fq->queue_empty_descs++;
			return NULL;
		}

		ok = pool->unaligned ? xp_check_unaligned(pool, &addr) :
		     xp_check_aligned(pool, &addr);
		if (!ok) {
			pool->fq->invalid_descs++;
			xskq_cons_release(pool->fq);
			continue;
		}
		break;
	}

	xskb = xp_get_xskb(pool, addr);

	xskq_cons_release(pool->fq);
	return xskb;
}

static struct xdp_buff_xsk *xp_alloc_xskb(struct xsk_buff_pool *pool)
{
	struct xdp_buff_xsk *xskb;

	if (!pool->free_list_cnt)
		return __xp_alloc(pool);

	pool->free_list_cnt--;
	xskb = list_first_entry(&pool->free_list, struct xdp_buff_xsk,
				list_node);
	list_del_init(&xskb->list_node);

	return xskb;
}

struct xdp_buff *xp_alloc(struct xsk_buff_pool *pool)
{
	struct xdp_buff_xsk *xskb;

	xskb = xp_alloc_xskb(pool);
	if (!xskb)
		return NULL;

	xskb->xdp.data = xskb->xdp.data_hard_start + XDP_PACKET_HEADROOM;
	xskb->xdp.data_meta = xskb->xdp.data;
	xskb->xdp.flags = 0;

	if (pool->dev)
		xp_dma_sync_for_device(pool, xskb->dma, pool->frame_len);

	return &xskb->xdp;
}
EXPORT_SYMBOL(xp_alloc);

static u32 xp_alloc_new_from_fq(struct xsk_buff_pool *pool, struct xdp_buff **xdp, u32 max)
{
	u32 i, cached_cons, nb_entries;

	if (max > pool->free_heads_cnt)
		max = pool->free_heads_cnt;
	max = xskq_cons_nb_entries(pool->fq, max);

	cached_cons = pool->fq->cached_cons;
	nb_entries = max;
	i = max;
	while (i--) {
		struct xdp_buff_xsk *xskb;
		u64 addr;
		bool ok;

		__xskq_cons_read_addr_unchecked(pool->fq, cached_cons++, &addr);

		ok = pool->unaligned ? xp_check_unaligned(pool, &addr) :
			xp_check_aligned(pool, &addr);
		if (unlikely(!ok)) {
			pool->fq->invalid_descs++;
			nb_entries--;
			continue;
		}

		xskb = xp_get_xskb(pool, addr);

		*xdp = &xskb->xdp;
		xdp++;
	}

	xskq_cons_release_n(pool->fq, max);
	return nb_entries;
}

static u32 xp_alloc_reused(struct xsk_buff_pool *pool, struct xdp_buff **xdp, u32 nb_entries)
{
	struct xdp_buff_xsk *xskb;
	u32 i;

	nb_entries = min_t(u32, nb_entries, pool->free_list_cnt);

	i = nb_entries;
	while (i--) {
		xskb = list_first_entry(&pool->free_list, struct xdp_buff_xsk, list_node);
		list_del_init(&xskb->list_node);

		*xdp = &xskb->xdp;
		xdp++;
	}
	pool->free_list_cnt -= nb_entries;

	return nb_entries;
}

static u32 xp_alloc_slow(struct xsk_buff_pool *pool, struct xdp_buff **xdp,
			 u32 max)
{
	int i;

	for (i = 0; i < max; i++) {
		struct xdp_buff *buff;

		buff = xp_alloc(pool);
		if (unlikely(!buff))
			return i;
		*xdp = buff;
		xdp++;
	}

	return max;
}

u32 xp_alloc_batch(struct xsk_buff_pool *pool, struct xdp_buff **xdp, u32 max)
{
	u32 nb_entries1 = 0, nb_entries2;

	if (unlikely(pool->dev && dma_dev_need_sync(pool->dev)))
		return xp_alloc_slow(pool, xdp, max);

	if (unlikely(pool->free_list_cnt)) {
		nb_entries1 = xp_alloc_reused(pool, xdp, max);
		if (nb_entries1 == max)
			return nb_entries1;

		max -= nb_entries1;
		xdp += nb_entries1;
	}

	nb_entries2 = xp_alloc_new_from_fq(pool, xdp, max);
	if (!nb_entries2)
		pool->fq->queue_empty_descs++;

	return nb_entries1 + nb_entries2;
}
EXPORT_SYMBOL(xp_alloc_batch);

bool xp_can_alloc(struct xsk_buff_pool *pool, u32 count)
{
	u32 req_count, avail_count;

	if (pool->free_list_cnt >= count)
		return true;

	req_count = count - pool->free_list_cnt;
	avail_count = xskq_cons_nb_entries(pool->fq, req_count);
	if (!avail_count)
		pool->fq->queue_empty_descs++;

	return avail_count >= req_count;
}
EXPORT_SYMBOL(xp_can_alloc);

void xp_free(struct xdp_buff_xsk *xskb)
{
	if (!list_empty(&xskb->list_node))
		return;

	xskb->pool->free_list_cnt++;
	list_add(&xskb->list_node, &xskb->pool->free_list);
}
EXPORT_SYMBOL(xp_free);

static u64 __xp_raw_get_addr(const struct xsk_buff_pool *pool, u64 addr)
{
	return pool->unaligned ? xp_unaligned_add_offset_to_addr(addr) : addr;
}

static void *__xp_raw_get_data(const struct xsk_buff_pool *pool, u64 addr)
{
	return pool->addrs + addr;
}

void *xp_raw_get_data(struct xsk_buff_pool *pool, u64 addr)
{
	return __xp_raw_get_data(pool, __xp_raw_get_addr(pool, addr));
}
EXPORT_SYMBOL(xp_raw_get_data);

static dma_addr_t __xp_raw_get_dma(const struct xsk_buff_pool *pool, u64 addr)
{
	return (pool->dma_pages[addr >> PAGE_SHIFT] &
		~XSK_NEXT_PG_CONTIG_MASK) +
		(addr & ~PAGE_MASK);
}

dma_addr_t xp_raw_get_dma(struct xsk_buff_pool *pool, u64 addr)
{
	return __xp_raw_get_dma(pool, __xp_raw_get_addr(pool, addr));
}
EXPORT_SYMBOL(xp_raw_get_dma);

/**
 * xp_raw_get_ctx - get &xdp_desc context
 * @pool: XSk buff pool desc address belongs to
 * @addr: desc address (from userspace)
 * @options: desc options (from userspace)
 *
 * Helper for getting desc's DMA address and metadata pointer, if present.
 * Saves one call on hotpath and double calculation of the actual address.
 * Metadata is validated later by xsk_tx_metadata_request().
 *
 * Return: new &xdp_desc_ctx struct containing desc's DMA address and metadata
 * pointer, if it is present (initialized to %NULL otherwise).
 */
struct xdp_desc_ctx xp_raw_get_ctx(const struct xsk_buff_pool *pool, u64 addr,
				   u32 options)
{
	struct xdp_desc_ctx ret;

	addr = __xp_raw_get_addr(pool, addr);

	ret.dma = __xp_raw_get_dma(pool, addr);
	ret.meta = __xsk_buff_get_metadata(pool, __xp_raw_get_data(pool, addr),
					   options);

	return ret;
}
EXPORT_SYMBOL(xp_raw_get_ctx);
