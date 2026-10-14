// SPDX-License-Identifier: GPL-2.0
/* AF_XDP UMEM as a page pool memory provider
 *
 * Each aligned UMEM chunk is one readable net_iov. A chunk has one owner at
 * a time, recorded in its state under the provider lock:
 *
 *   userspace --FILL--> page pool --release--> reuse --alloc--> page pool
 *       ^                   |
 *       +-----RX ring-------+
 *
 * The RX ring takes a chunk after the page pool let go of it, and before
 * userspace can see it. A packet from elsewhere is copied into chunks taken
 * from the reuse stack or FILL ring, which wait in XSK_PP_COPY until then.
 *
 * A page pool takes chunks from the reuse stack first, then from the FILL
 * ring. A chunk returns to userspace through the socket RX ring. Several page
 * pools of one queue can share the provider while the queue is replaced.
 *
 * The chunks are few. When a refill finds none, the provider is starved, and
 * the next release schedules the NAPI of the queue. With XDP_USE_NEED_WAKEUP
 * the RX flag stays set, and userspace wakes the queue after it posts FILL
 * entries; otherwise NAPI keeps polling the FILL ring.
 */

#include <linux/sizes.h>
#include <net/netdev_lock.h>
#include <net/netdev_queues.h>
#include <net/netdev_rx_queue.h>
#include <net/page_pool/helpers.h>
#include <net/page_pool/memory_provider.h>
#include <net/xdp_sock_drv.h>

#include "../core/dev.h"
#include "xsk.h"
#include "xsk_queue.h"

#define XSK_PP_DMA_ATTR	(DMA_ATTR_WEAK_ORDERING | DMA_ATTR_SKIP_CPU_SYNC)
/* Chunks of reserved RX descriptors before they are handed over */
#define XSK_PP_BATCH	64

enum xsk_pp_chunk {
	XSK_PP_USER,	/* owned by userspace */
	XSK_PP_POOL,	/* owned by a page pool */
	XSK_PP_REUSE,	/* on the reuse stack */
	XSK_PP_COPY,	/* copied into, not yet on the RX ring */
};

struct xsk_pp {
	struct net_iov_area area;
	struct xsk_buff_pool *pool;
	/* FILL consumption, chunk states, the reuse stack and removal */
	spinlock_t lock;
	struct xsk_queue *fq;
	refcount_t refs;
	u32 nr_pools;
	bool dma_need_sync;
	/* Physical device, held while the provider is installed */
	struct net_device *netdev;
	netdevice_tracker dev_tracker;
	/* Queue of netdev to wake, cleared before netdev is put */
	struct netdev_rx_queue __rcu *rxq;
	/* Removal state, see xsk_pp_uninstall() */
	bool dma_stopped;
	bool owns_pool;
	struct work_struct release_work;
	bool starved;
	/* Chunks of unpublished RX descriptors, used by the queue NAPI only */
	u32 pending_cnt;
	netmem_ref pending[XSK_PP_BATCH];
	u8 *state;
	u32 reuse_cnt;
	u32 reuse[];
};

static void xsk_pp_free(struct xsk_pp *pp)
{
	kvfree(pp->state);
	kvfree(pp->area.niovs);
	kvfree(pp);
}

void xsk_pp_put(struct xsk_pp *pp)
{
	if (refcount_dec_and_test(&pp->refs))
		xsk_pp_free(pp);
}

/* The queue stopped DMA after the socket released the pool. */
static void xsk_pp_release_work(struct work_struct *work)
{
	struct xsk_pp *pp = container_of(work, struct xsk_pp, release_work);

	xp_release_storage(pp->pool);
}

/**
 * xsk_pp_create - make a DMA-mapped buffer pool a page pool memory provider
 * @pool: buffer pool with aligned chunks of PAGE_SIZE
 *
 * The provider uses the FILL ring and the CPU and DMA mappings of @pool,
 * which must stay valid while page pools use it. It must be the only
 * consumer of the FILL ring. Its arrays cost about 70
 * bytes per chunk, charged to the memory cgroup of the caller.
 *
 * Return: new provider, or an ERR_PTR() on error.
 */
struct xsk_pp *xsk_pp_create(struct xsk_buff_pool *pool)
{
	struct net_iov *niov;
	struct xsk_pp *pp;
	dma_addr_t dma;
	u32 i;

	pp = kvzalloc_flex(*pp, reuse, pool->heads_cnt, GFP_KERNEL_ACCOUNT);
	if (!pp)
		return ERR_PTR(-ENOMEM);

	pp->area.niovs = kvzalloc_objs(*pp->area.niovs, pool->heads_cnt,
				       GFP_KERNEL_ACCOUNT);
	pp->state = kvzalloc(pool->heads_cnt, GFP_KERNEL_ACCOUNT);
	if (!pp->area.niovs || !pp->state) {
		xsk_pp_free(pp);
		return ERR_PTR(-ENOMEM);
	}

	pp->area.num_niovs = pool->heads_cnt;
	pp->area.niov_shift = pool->chunk_shift;
	pp->area.vaddr = pool->addrs;
	pp->pool = pool;
	pp->fq = pool->fq;
	pp->dma_need_sync = dma_dev_need_sync(pool->dev);
	spin_lock_init(&pp->lock);
	refcount_set(&pp->refs, 1);
	INIT_WORK(&pp->release_work, xsk_pp_release_work);

	for (i = 0; i < pp->area.num_niovs; i++) {
		niov = &pp->area.niovs[i];
		dma = pool->dma_pages[i] & ~XSK_NEXT_PG_CONTIG_MASK;
		net_iov_init(niov, &pp->area, NET_IOV_XSK);
		if (net_mp_niov_set_dma_addr(niov, dma)) {
			xsk_pp_free(pp);
			return ERR_PTR(-ERANGE);
		}
	}

	return pp;
}

static int xsk_pp_init(struct page_pool *pool)
{
	struct xsk_pp *pp = pool->mp_priv;

	if (pool->p.order || pool->p.dma_dir != DMA_BIDIRECTIONAL ||
	    pool->p.dev != pp->pool->dev)
		return -EINVAL;
	/* Device writes and syncs must stay inside the chunk. */
	if (pool->p.max_len > pp->pool->chunk_size ||
	    pool->p.offset > pp->pool->chunk_size - pool->p.max_len)
		return -EINVAL;

	refcount_inc(&pp->refs);
	spin_lock_bh(&pp->lock);
	pp->nr_pools++;
	spin_unlock_bh(&pp->lock);
	return 0;
}

static void xsk_pp_destroy(struct page_pool *pool)
{
	struct xsk_pp *pp = pool->mp_priv;

	spin_lock_bh(&pp->lock);
	pp->nr_pools--;
	spin_unlock_bh(&pp->lock);
	xsk_pp_put(pp);
}

/* Give chunk @idx, which must be in state @from, to a page pool. */
static bool xsk_pp_claim(struct xsk_pp *pp, u32 idx, enum xsk_pp_chunk from)
{
	lockdep_assert_held(&pp->lock);

	if (pp->state[idx] != from)
		return false;

	pp->state[idx] = XSK_PP_POOL;
	return true;
}

static u32 xsk_pp_alloc_reuse(struct xsk_pp *pp, netmem_ref *netmems, u32 max)
{
	u32 idx, n = 0;

	while (n < max && pp->reuse_cnt) {
		idx = pp->reuse[--pp->reuse_cnt];
		if (WARN_ON_ONCE(!xsk_pp_claim(pp, idx, XSK_PP_REUSE)))
			continue;
		netmems[n++] = net_iov_to_netmem(&pp->area.niovs[idx]);
	}

	return n;
}

/* Read at most @max FILL entries. Addresses outside the UMEM, and chunks
 * that userspace does not own, are invalid and dropped.
 */
static u32 xsk_pp_alloc_fill(struct xsk_pp *pp, netmem_ref *netmems, u32 max)
{
	struct xsk_buff_pool *pool = pp->pool;
	struct xsk_queue *fq = pp->fq;
	u32 i, entries, cons, idx, n = 0;
	u64 addr;

	if (!fq || !max)
		return 0;

	entries = xskq_cons_nb_entries(fq, max);
	if (!entries) {
		fq->queue_empty_descs++;
		return 0;
	}

	cons = fq->cached_cons;
	for (i = 0; i < entries; i++) {
		__xskq_cons_read_addr_unchecked(fq, cons++, &addr);
		addr &= pool->chunk_mask;
		idx = addr >> pp->area.niov_shift;
		if (unlikely(addr >= pool->addrs_cnt ||
			     !xsk_pp_claim(pp, idx, XSK_PP_USER))) {
			fq->invalid_descs++;
			continue;
		}
		netmems[n++] = net_iov_to_netmem(&pp->area.niovs[idx]);
	}

	xskq_cons_release_n(fq, entries);
	__xskq_cons_release(fq);
	return n;
}

static netmem_ref xsk_pp_alloc_netmems(struct page_pool *pool, gfp_t gfp)
{
	netmem_ref *netmems = pool->alloc.cache;
	struct xsk_pp *pp = pool->mp_priv;
	struct net_iov *niov;
	dma_addr_t dma;
	u32 i, n;

	if (WARN_ON_ONCE(pool->alloc.count))
		return 0;

	/* One lock round trip fills the page pool allocation cache. */
	spin_lock_bh(&pp->lock);
	n = xsk_pp_alloc_reuse(pp, netmems, PP_ALLOC_CACHE_REFILL);
	n += xsk_pp_alloc_fill(pp, netmems + n, PP_ALLOC_CACHE_REFILL - n);
	spin_unlock_bh(&pp->lock);
	if (!n)
		return 0;

	for (i = 0; i < n; i++) {
		niov = netmem_to_net_iov(netmems[i]);
		dma = page_pool_get_dma_addr_netmem(netmems[i]);
		if (pp->dma_need_sync)
			dma_sync_single_range_for_device(pool->p.dev, dma,
							 pool->p.offset,
							 pool->p.max_len,
							 pool->p.dma_dir);
		net_mp_niov_set_page_pool(pool, niov);
	}

	pool->alloc.count = n - 1;
	return netmems[n - 1];
}

/* Schedule the NAPI of the queue. Return false if the queue does not run. */
static bool xsk_pp_wake(struct xsk_pp *pp)
{
	struct netdev_rx_queue *rxq;
	bool running = false;

	rcu_read_lock();
	rxq = rcu_dereference(pp->rxq);
	if (rxq)
		running = netif_mp_wake(rxq);
	rcu_read_unlock();

	return running;
}

int xsk_pp_wakeup(struct xsk_pp *pp)
{
	return xsk_pp_wake(pp) ? 0 : -ENETDOWN;
}

static bool xsk_pp_release_netmem(struct page_pool *pool, netmem_ref netmem)
{
	struct net_iov *niov = netmem_to_net_iov(netmem);
	struct xsk_pp *pp = pool->mp_priv;
	u32 idx = net_iov_idx(niov);
	bool starved;

	spin_lock_bh(&pp->lock);
	net_mp_niov_clear_page_pool(niov);
	/* A chunk enters the stack once per claim, so it cannot overflow. */
	if (!WARN_ON_ONCE(pp->state[idx] != XSK_PP_POOL ||
			  pp->reuse_cnt == pp->area.num_niovs)) {
		pp->state[idx] = XSK_PP_REUSE;
		pp->reuse[pp->reuse_cnt++] = idx;
	}
	starved = pp->starved;
	WRITE_ONCE(pp->starved, false);
	spin_unlock_bh(&pp->lock);

	/* A late release by an old page pool may be the only one. */
	if (starved)
		xsk_pp_wake(pp);

	return false;
}

/* Without XDP_USE_NEED_WAKEUP, NAPI polls the FILL ring while it is empty. */
static bool xsk_pp_refill_done(struct page_pool *pool, enum pp_refill state)
{
	struct xsk_pp *pp = pool->mp_priv;
	struct xsk_buff_pool *xp = pp->pool;
	bool wait = xp->uses_need_wakeup;
	bool more;

	if (state == PP_REFILL_FULL && !READ_ONCE(pp->starved))
		return true;

	spin_lock_bh(&pp->lock);
	if (!pp->fq) {
		spin_unlock_bh(&pp->lock);
		return true;
	}

	if (state == PP_REFILL_FULL) {
		WRITE_ONCE(pp->starved, false);
		spin_unlock_bh(&pp->lock);
		return true;
	}

	/* With need_wakeup, the RX flag stays set, so userspace wakes the
	 * queue after it posts FILL entries.
	 */
	more = pp->reuse_cnt || xskq_cons_nb_entries(pp->fq, 1);
	WRITE_ONCE(pp->starved, !more);
	spin_unlock_bh(&pp->lock);

	return !more && wait;
}

static u32 xsk_pp_chunk_idx(netmem_ref netmem)
{
	return net_iov_idx(netmem_to_net_iov(netmem));
}

/**
 * xsk_pp_flush - give the chunks of reserved RX descriptors to userspace
 * @pp: provider
 *
 * Call this before the RX ring publishes the descriptors.
 *
 * Context: the NAPI of the queue, which serializes the pending chunks.
 */
void xsk_pp_flush(struct xsk_pp *pp)
{
	struct page_pool *pool;
	u32 i, j, n = 0;
	u32 idx;

	if (!pp->pending_cnt)
		return;

	spin_lock_bh(&pp->lock);
	for (i = 0; i < pp->pending_cnt; i++) {
		idx = xsk_pp_chunk_idx(pp->pending[i]);
		if (pp->state[idx] == XSK_PP_COPY) {
			pp->state[idx] = XSK_PP_USER;
			continue;
		}

		DEBUG_NET_WARN_ON_ONCE(pp->state[idx] != XSK_PP_POOL);
		pp->pending[n++] = pp->pending[i];
	}

	/* A queue restart may change the page pool within a batch. */
	for (i = 0; i < n; i = j) {
		pool = netmem_get_pp(pp->pending[i]);
		for (j = i + 1; j < n; j++)
			if (netmem_get_pp(pp->pending[j]) != pool)
				break;

		net_mp_release_page_pool_bulk(pool, pp->pending + i, j - i);
		while (i < j) {
			idx = xsk_pp_chunk_idx(pp->pending[i++]);
			pp->state[idx] = XSK_PP_USER;
		}
	}
	pp->pending_cnt = 0;
	spin_unlock_bh(&pp->lock);
}

/**
 * xsk_pp_queue_buf - is a packet in a buffer of the provider queue
 * @pp: provider
 * @xdp: packet
 *
 * The RX ring of a provider socket has one producer, the NAPI of its queue.
 * An XDP test run for the same queue brings buffers of its own.
 *
 * Return: true if the head buffer of @xdp is a net_iov from a page pool of
 * the queue that @pp is installed on.
 */
bool xsk_pp_queue_buf(struct xsk_pp *pp, struct xdp_buff *xdp)
{
	netmem_ref head = xdp_buff_head_netmem(xdp);
	struct netdev_rx_queue *rxq;
	struct page_pool *pool;
	bool ours = false;

	/* The queue copies only net_iov heads of its page pools. */
	if (!netmem_is_net_iov(head))
		return false;

	pool = netmem_get_pp(head);
	rcu_read_lock();
	rxq = rcu_dereference(pp->rxq);
	if (pool && rxq)
		ours = pool->slow.netdev == rxq->dev &&
		       pool->slow.queue_idx == get_netdev_rx_queue_index(rxq);
	rcu_read_unlock();

	return ours;
}

static bool xsk_pp_owns(struct xsk_pp *pp, netmem_ref netmem)
{
	struct page_pool *pool;

	if (!netmem_is_net_iov(netmem))
		return false;

	pool = netmem_get_pp(netmem);
	return pool && pool->mp_priv == pp;
}

/**
 * xsk_pp_rcv_zc - put a packet in provider chunks on the RX ring
 * @pp: provider of the socket
 * @xs: socket
 * @xdp: packet from the queue of @pp
 *
 * Context: the NAPI of the queue, see xsk_pp_flush().
 * Return: 0 on success, -ENOBUFS if the RX ring is full, or -EXDEV if a
 * buffer of @xdp is not a chunk of @pp, or @xdp has fragments and @xs does
 * not take them.
 */
int xsk_pp_rcv_zc(struct xsk_pp *pp, struct xdp_sock *xs,
		  struct xdp_buff *xdp)
{
	struct skb_shared_info *sinfo = NULL;
	void *umem = xs->pool->addrs;
	u32 i, nr_frags = 0, num_desc;
	netmem_ref head;
	skb_frag_t *frag;

	head = xdp_buff_head_netmem(xdp);
	if (!xsk_pp_owns(pp, head))
		return -EXDEV;

	if (unlikely(xdp_buff_has_frags(xdp))) {
		if (!xs->sg)
			return -EXDEV;

		sinfo = xdp_get_shared_info_from_buff(xdp);
		nr_frags = sinfo->nr_frags;
		for (i = 0; i < nr_frags; i++)
			if (!xsk_pp_owns(pp, skb_frag_netmem(&sinfo->frags[i])))
				return -EXDEV;
	}

	num_desc = nr_frags + 1;
	if (xskq_prod_nb_free(xs->rx, num_desc) < num_desc) {
		xs->rx_queue_full++;
		return -ENOBUFS;
	}
	if (pp->pending_cnt + num_desc > XSK_PP_BATCH)
		xsk_pp_flush(pp);

	__xskq_prod_reserve_desc(xs->rx, xdp->data - umem,
				 xdp->data_end - xdp->data,
				 nr_frags ? XDP_PKT_CONTD : 0);
	pp->pending[pp->pending_cnt++] = head;

	for (i = 0; i < nr_frags; i++) {
		frag = &sinfo->frags[i];
		__xskq_prod_reserve_desc(xs->rx, skb_frag_address(frag) - umem,
					 skb_frag_size(frag),
					 i + 1 < nr_frags ? XDP_PKT_CONTD : 0);
		pp->pending[pp->pending_cnt++] = skb_frag_netmem(frag);
	}

	return 0;
}

/**
 * xsk_pp_claim_copy - take chunks to copy a packet into
 * @pp: provider of the socket
 * @addrs: UMEM addresses of the chunks
 * @n: number of chunks, at most MAX_SKB_FRAGS + 1
 *
 * The chunks go to userspace with the next xsk_pp_flush().
 *
 * Context: the NAPI of the queue, see xsk_pp_flush().
 * Return: true on success. On failure the caller gets no chunk; chunks taken
 * from FILL stay with the provider for reuse.
 */
bool xsk_pp_claim_copy(struct xsk_pp *pp, u64 *addrs, u32 n)
{
	netmem_ref *netmems;
	u32 i, got, idx;

	BUILD_BUG_ON(MAX_SKB_FRAGS + 1 > XSK_PP_BATCH);
	if (pp->pending_cnt + n > XSK_PP_BATCH)
		xsk_pp_flush(pp);

	/* Take the chunks straight into the pending list. */
	netmems = pp->pending + pp->pending_cnt;
	spin_lock_bh(&pp->lock);
	got = xsk_pp_alloc_reuse(pp, netmems, n);
	got += xsk_pp_alloc_fill(pp, netmems + got, n - got);
	for (i = 0; i < got; i++) {
		idx = xsk_pp_chunk_idx(netmems[i]);
		if (got < n) {
			pp->state[idx] = XSK_PP_REUSE;
			pp->reuse[pp->reuse_cnt++] = idx;
			continue;
		}

		pp->state[idx] = XSK_PP_COPY;
		addrs[i] = (u64)idx << pp->area.niov_shift;
	}
	if (got == n)
		pp->pending_cnt += n;
	spin_unlock_bh(&pp->lock);

	return got == n;
}

static int xsk_pp_nl_fill(void *mp_priv, struct sk_buff *rsp,
			  struct netdev_rx_queue *rxq)
{
	return 0;
}

/* After %MP_UNINSTALL_LOGICAL_ONLY the queue may still DMA to the UMEM and
 * write TX completions. Stop taking FILL entries, and keep the pool, its
 * mapping and the queue until %MP_UNINSTALL_DMA_STOPPED. If the socket
 * released the pool meanwhile, the provider frees it then.
 */
static void xsk_pp_uninstall(void *mp_priv, struct netdev_rx_queue *rxq,
			     enum mp_uninstall_reason reason)
{
	struct xsk_pp *pp = mp_priv;
	bool owns;

	RCU_INIT_POINTER(pp->rxq, NULL);
	if (reason == MP_UNINSTALL_LOGICAL_ONLY) {
		spin_lock_bh(&pp->lock);
		pp->fq = NULL;
		spin_unlock_bh(&pp->lock);
		return;
	}

	__xsk_clear_pool_at_qid(rxq->dev, get_netdev_rx_queue_index(rxq));
	/* Recycles may still sync chunks for the device, see
	 * page_pool_scrub(), and wakeups may still use its queue.
	 */
	synchronize_net();
	xp_dma_unmap(pp->pool, XSK_PP_DMA_ATTR);
	netdev_put(pp->netdev, &pp->dev_tracker);
	pp->netdev = NULL;

	spin_lock_bh(&pp->lock);
	pp->fq = NULL;
	pp->dma_stopped = true;
	owns = pp->owns_pool;
	spin_unlock_bh(&pp->lock);

	if (owns)
		schedule_work(&pp->release_work);
}

const struct memory_provider_ops xsk_pp_ops = {
	.init		= xsk_pp_init,
	.destroy	= xsk_pp_destroy,
	.alloc_netmems	= xsk_pp_alloc_netmems,
	.release_netmem	= xsk_pp_release_netmem,
	.nl_fill	= xsk_pp_nl_fill,
	.uninstall	= xsk_pp_uninstall,
	.refill_done	= xsk_pp_refill_done,
	.caps		= MP_CAP_READABLE,
};

/**
 * xsk_pp_keep_pool - let the provider release a pool its queue still uses
 * @pp: provider of the pool
 *
 * Return: true if the provider releases the pool once the queue DMA stops.
 */
bool xsk_pp_keep_pool(struct xsk_pp *pp)
{
	bool keep;

	/* Unless DMA stopped, the queue may still use the pool. That includes
	 * a removal that failed before it uninstalled the provider.
	 */
	spin_lock_bh(&pp->lock);
	keep = !pp->dma_stopped;
	pp->owns_pool = keep;
	spin_unlock_bh(&pp->lock);

	return keep;
}

/* Give the chunks that a failed install took from FILL to the copy mode
 * allocator. Return false if a page pool still owns chunks.
 */
static bool xsk_pp_abandon(struct xsk_pp *pp)
{
	struct xsk_buff_pool *pool = pp->pool;
	bool busy;
	u32 idx;

	spin_lock_bh(&pp->lock);
	busy = pp->nr_pools;
	while (!busy && pp->reuse_cnt) {
		idx = pp->reuse[--pp->reuse_cnt];
		pp->state[idx] = XSK_PP_USER;
		xp_free(&pool->heads[idx]);
	}
	spin_unlock_bh(&pp->lock);

	return !busy;
}

static int xsk_pp_validate(struct net_device *dev, u16 queue_id,
			   struct xsk_buff_pool *pool, unsigned int mtu,
			   struct bpf_prog *prog,
			   struct netlink_ext_ack *extack)
{
	struct xsk_rx_check check = {
		.queue_id = queue_id,
		.buf_size = pool->chunk_size,
		.headroom = xsk_pool_get_headroom(pool),
		.mtu = mtu,
		.sg = pool->umem->flags & XDP_UMEM_SG_FLAG,
		.tx_metadata_len = pool->tx_metadata_len,
		.prog = prog,
	};
	struct netdev_bpf bpf = {
		.command = XDP_VALIDATE_XSK_POOL,
		.xsk_check.check = &check,
		.xsk_check.extack = extack,
	};

	return dev->netdev_ops->ndo_bpf(dev, &bpf);
}

/**
 * xsk_validate_dev - check AF_XDP page pool queues against a new setting
 * @dev: physical device
 * @mtu: MTU to check
 * @prog: XDP program to check, or NULL for the current one
 * @mode: XDP mode of @prog
 * @extack: netlink extended ack
 *
 * Context: holds the instance lock of @dev, or RTNL for a device without it.
 * Return: 0 if the driver accepts all queues of @dev that have an AF_XDP
 * memory provider, or its error.
 */
int xsk_validate_dev(struct net_device *dev, unsigned int mtu,
		     struct bpf_prog *prog, enum bpf_xdp_mode mode,
		     struct netlink_ext_ack *extack)
{
	struct netdev_rx_queue *rxq, *lease;
	struct xsk_pp *pp;
	unsigned int i;
	int err;

	netdev_assert_locked_ops_compat(dev);

	for (i = 0; i < dev->real_num_rx_queues; i++) {
		rxq = __netif_get_rx_queue(dev, i);
		lease = READ_ONCE(rxq->lease);
		/* Only the NAPI of the queue feeds the socket. A leased queue
		 * has the provider on the physical side.
		 */
		if (mode == XDP_MODE_SKB &&
		    (rxq->mp_params.mp_ops == &xsk_pp_ops ||
		     (lease &&
		      READ_ONCE(lease->mp_params.mp_ops) == &xsk_pp_ops))) {
			NL_SET_ERR_MSG(extack,
				       "generic XDP cannot feed an AF_XDP page pool socket");
			return -EBUSY;
		}
		if (rxq->mp_params.mp_ops != &xsk_pp_ops)
			continue;

		pp = rxq->mp_params.mp_priv;
		err = xsk_pp_validate(dev, i, pp->pool, mtu, prog, extack);
		if (err)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xsk_validate_dev);

/**
 * xsk_rxq_pool - get the AF_XDP pool that provides memory to an RX queue
 * @dev: physical device
 * @queue_id: RX queue
 *
 * The pool is set while the provider is installed, also while the queue is
 * being allocated. A copy mode pool has no provider. The pool stays valid
 * while the queue the caller is allocating runs.
 *
 * Context: holds the instance lock of @dev.
 * Return: the buffer pool, or NULL.
 */
struct xsk_buff_pool *xsk_rxq_pool(struct net_device *dev, u16 queue_id)
{
	struct netdev_rx_queue *rxq;
	struct xsk_pp *pp;

	netdev_assert_locked(dev);

	if (queue_id >= dev->real_num_rx_queues)
		return NULL;

	rxq = __netif_get_rx_queue(dev, queue_id);
	if (rxq->mp_params.mp_ops != &xsk_pp_ops)
		return NULL;

	pp = rxq->mp_params.mp_priv;
	return pp->pool;
}
EXPORT_SYMBOL_GPL(xsk_rxq_pool);

/* Find the physical device of an RX queue that uses a page pool provider for
 * AF_XDP zero copy.
 */
static struct net_device *xsk_pp_phys(struct net_device *dev,
				      unsigned int *queue_id)
{
	if (*queue_id >= dev->real_num_rx_queues ||
	    !__netif_get_rx_queue_lease(&dev, queue_id, NETIF_VIRT_TO_PHYS))
		return NULL;

	if (!dev->queue_mgmt_ops || !dev->queue_mgmt_ops->xsk_page_pool)
		return NULL;

	return dev;
}

/**
 * xsk_pp_supported - does AF_XDP zero copy on a queue use a page pool
 * @dev: device the socket binds to
 * @queue_id: queue id on @dev
 *
 * Return: true if the device of the queue installs the UMEM as a page pool
 * memory provider, false for driver managed zero copy.
 */
bool xsk_pp_supported(struct net_device *dev, u16 queue_id)
{
	unsigned int qid = queue_id;

	if (qid >= dev->real_num_rx_queues) {
		const struct netdev_queue_mgmt_ops *qops = dev->queue_mgmt_ops;

		return qops && qops->xsk_page_pool;
	}

	return xsk_pp_phys(dev, &qid);
}

/* The pool is not bound yet, so no TX batch uses the array. */
static int xsk_pp_grow_tx_descs(struct xsk_buff_pool *pool, u32 nentries)
{
	struct xdp_desc *descs;

	descs = kvzalloc_objs(*descs, nentries);
	if (!descs)
		return -ENOMEM;

	kvfree(pool->tx_descs);
	pool->tx_descs = descs;
	pool->tx_descs_nentries = nentries;
	return 0;
}

/**
 * xsk_pp_bind - install a buffer pool as the memory provider of its queue
 * @pool: buffer pool bound to @dev and @queue_id
 * @dev: device the socket binds to
 * @queue_id: queue id on @dev
 *
 * Validate the setup with the driver, map the UMEM for the physical queue
 * and install the provider. On error, @pool->pp stays set if an old page
 * pool still owns UMEM chunks; copy mode must not take the pool then.
 *
 * Context: holds RTNL and the instance lock of @dev. Takes the instance
 * lock of the physical device of a leased queue.
 * Return: 0 on success, or an error.
 */
int xsk_pp_bind(struct xsk_buff_pool *pool, struct net_device *dev,
		u16 queue_id)
{
	struct pp_memory_provider_params mp = { .mp_ops = &xsk_pp_ops };
	unsigned int phys_qid = queue_id;
	struct net_device *phys;
	struct device *dma_dev;
	struct xsk_pp *pp;
	int err;

	phys = xsk_pp_phys(dev, &phys_qid);
	if (!phys)
		return -EOPNOTSUPP;
	if (pool->unaligned || pool->chunk_size != SZ_4K ||
	    pool->chunk_size != PAGE_SIZE)
		return -EOPNOTSUPP;
	if ((phys->xdp_features & NETDEV_XDP_ACT_XSK) != NETDEV_XDP_ACT_XSK ||
	    (pool->umem->flags & XDP_UMEM_SG_FLAG &&
	     phys->xdp_zc_max_segs == 1))
		return -EOPNOTSUPP;
	/* Only the NAPI of the queue feeds the socket, not generic XDP. */
	if (dev_xdp_prog_id(dev, XDP_MODE_SKB) ||
	    dev_xdp_prog_id(phys, XDP_MODE_SKB))
		return -EOPNOTSUPP;
	/* The TX batch array, sized for @dev, must hold a frame of @phys. */
	if (pool->tx_descs && pool->tx_descs_nentries < phys->xdp_zc_max_segs) {
		err = xsk_pp_grow_tx_descs(pool, phys->xdp_zc_max_segs);
		if (err)
			return err;
	}

	dma_dev = netdev_queue_get_dma_dev(dev, queue_id, NETDEV_QUEUE_TYPE_RX);
	if (!dma_dev)
		return -EOPNOTSUPP;

	if (phys != dev)
		netdev_lock(phys);
	err = -EBUSY;
	if (netif_all_mps_have_cap(phys, MP_CAP_READABLE))
		err = xsk_pp_validate(phys, phys_qid, pool, phys->mtu, NULL,
				      NULL);
	if (phys != dev)
		netdev_unlock(phys);
	if (err)
		return err;

	err = xp_dma_map_dev(pool, phys, dma_dev, XSK_PP_DMA_ATTR);
	if (err)
		return err;

	pp = xsk_pp_create(pool);
	if (IS_ERR(pp)) {
		xp_dma_unmap(pool, XSK_PP_DMA_ATTR);
		return PTR_ERR(pp);
	}

	netdev_hold(phys, &pp->dev_tracker, GFP_KERNEL);
	pp->netdev = phys;
	rcu_assign_pointer(pp->rxq, __netif_get_rx_queue(phys, phys_qid));
	mp.mp_priv = pp;
	mp.rx_page_size = pool->chunk_size;
	mp.rx_headroom = xsk_pool_get_headroom(pool);
	err = netif_mp_open_rxq(dev, queue_id, &mp, NULL);
	if (!err) {
		pool->pp = pp;
		pool->xdp_zc_max_segs = phys->xdp_zc_max_segs;
		/* A max_mtu of zero means no limit */
		if (phys->max_mtu)
			pool->tx_max_frame = phys->max_mtu + ETH_HLEN;
		/* A wakeup flag cleared and set by the kernel could miss a
		 * FILL entry posted meanwhile, see xsk_pp_refill_done().
		 */
		if (pool->uses_need_wakeup)
			xsk_set_rx_need_wakeup(pool);
		return 0;
	}

	/* The queue stopped, or never ran with the provider. */
	RCU_INIT_POINTER(pp->rxq, NULL);
	synchronize_net();
	netdev_put(phys, &pp->dev_tracker);
	pp->netdev = NULL;
	xp_dma_unmap(pool, XSK_PP_DMA_ATTR);
	if (xsk_pp_abandon(pp))
		xsk_pp_put(pp);
	else
		pool->pp = pp;

	return err;
}
