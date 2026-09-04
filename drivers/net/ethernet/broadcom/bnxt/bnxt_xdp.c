/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2016-2017 Broadcom Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/bpf.h>
#include <linux/bpf_trace.h>
#include <linux/filter.h>
#include <net/netdev_lock.h>
#include <net/page_pool/helpers.h>
#include <linux/bnxt/hsi.h>
#include "bnxt.h"
#include "bnxt_xdp.h"

DEFINE_STATIC_KEY_FALSE(bnxt_xdp_locking_key);

struct bnxt_sw_tx_bd *bnxt_xmit_bd(struct bnxt *bp,
				   struct bnxt_tx_ring_info *txr,
				   dma_addr_t mapping, u32 len,
				   struct xdp_buff *xdp)
{
	struct skb_shared_info *sinfo;
	struct bnxt_sw_tx_bd *tx_buf;
	struct tx_bd *txbd;
	int num_frags = 0;
	u32 flags;
	u16 prod;
	int i;

	if (xdp && xdp_buff_has_frags(xdp)) {
		sinfo = xdp_get_shared_info_from_buff(xdp);
		num_frags = sinfo->nr_frags;
	}

	/* fill up the first buffer */
	prod = txr->tx_prod;
	tx_buf = &txr->tx_buf_ring[RING_TX(bp, prod)];
	tx_buf->nr_frags = num_frags;
	if (xdp)
		tx_buf->netmem = xdp_buff_get_netmem(xdp);

	txbd = &txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];
	flags = (len << TX_BD_LEN_SHIFT) | TX_BD_CNT(num_frags + 1) |
		bnxt_lhint_arr[len >> 9];
	txbd->tx_bd_len_flags_type = cpu_to_le32(flags);
	txbd->tx_bd_opaque = SET_TX_OPAQUE(bp, txr, prod, 1 + num_frags);
	txbd->tx_bd_haddr = cpu_to_le64(mapping);

	/* now let us fill up the frags into the next buffers */
	for (i = 0; i < num_frags ; i++) {
		skb_frag_t *frag = &sinfo->frags[i];
		struct bnxt_sw_tx_bd *frag_tx_buf;
		dma_addr_t frag_mapping;
		int frag_len;

		prod = NEXT_TX(prod);
		WRITE_ONCE(txr->tx_prod, prod);

		/* first fill up the first buffer */
		frag_tx_buf = &txr->tx_buf_ring[RING_TX(bp, prod)];
		frag_tx_buf->netmem = skb_frag_netmem(frag);

		txbd = &txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];

		frag_len = skb_frag_size(frag);
		flags = frag_len << TX_BD_LEN_SHIFT;
		txbd->tx_bd_len_flags_type = cpu_to_le32(flags);
		frag_mapping =
			page_pool_get_dma_addr_netmem(frag_tx_buf->netmem);
		frag_mapping += skb_frag_off(frag);
		dma_sync_single_for_device(&bp->pdev->dev, frag_mapping,
					   frag_len, bp->rx_dir);
		txbd->tx_bd_haddr = cpu_to_le64(frag_mapping);

		len = frag_len;
	}

	flags &= ~TX_BD_LEN;
	txbd->tx_bd_len_flags_type = cpu_to_le32(((len) << TX_BD_LEN_SHIFT) | flags |
			TX_BD_FLAGS_PACKET_END);
	/* Sync TX BD */
	wmb();
	prod = NEXT_TX(prod);
	WRITE_ONCE(txr->tx_prod, prod);

	return tx_buf;
}

static void __bnxt_xmit_xdp(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			    dma_addr_t mapping, u32 len, u16 rx_prod,
			    struct xdp_buff *xdp)
{
	struct bnxt_sw_tx_bd *tx_buf;

	tx_buf = bnxt_xmit_bd(bp, txr, mapping, len, xdp);
	tx_buf->rx_prod = rx_prod;
	tx_buf->action = BNXT_XDP_TX;
	txr->xdp_tx_pending++;

}

static void __bnxt_xmit_xdp_redirect(struct bnxt *bp,
				     struct bnxt_tx_ring_info *txr,
				     dma_addr_t mapping, u32 len,
				     struct xdp_frame *xdpf)
{
	struct bnxt_sw_tx_bd *tx_buf;

	tx_buf = bnxt_xmit_bd(bp, txr, mapping, len, NULL);
	tx_buf->action = BNXT_XDP_REDIRECT;
	tx_buf->xdpf = xdpf;
	dma_unmap_addr_set(tx_buf, mapping, mapping);
	dma_unmap_len_set(tx_buf, len, len);
}

void bnxt_tx_int_xdp(struct bnxt *bp, struct bnxt_napi *bnapi, int budget)
{
	struct bnxt_tx_ring_info *txr = bnapi->tx_ring[0];
	struct bnxt_rx_ring_info *rxr = bnapi->rx_ring;
	struct xsk_buff_pool *xsk_pool = NULL;
	u16 tx_hw_cons = txr->tx_hw_cons;
	bool rx_doorbell_needed = false;
	struct bnxt_sw_tx_bd *tx_buf;
	u16 tx_cons = txr->tx_cons;
	u16 last_tx_cons = tx_cons;
	u16 sw_cons;
	u32 xsk_descs = 0;
	int j, frags;

	if (!budget ||
	    READ_ONCE(txr->dev_state) == BNXT_DEV_STATE_CLOSING)
		return;

	while (RING_TX(bp, tx_cons) != tx_hw_cons) {
		tx_buf = &txr->tx_buf_ring[RING_TX(bp, tx_cons)];

		if (tx_buf->action == BNXT_XDP_REDIRECT) {
			struct pci_dev *pdev = bp->pdev;

			dma_unmap_single(&pdev->dev,
					 dma_unmap_addr(tx_buf, mapping),
					 dma_unmap_len(tx_buf, len),
					 DMA_TO_DEVICE);
			xdp_return_frame(tx_buf->xdpf);
			tx_buf->action = BNXT_XDP_TX_NONE;
			tx_buf->xdpf = NULL;
		} else if (tx_buf->action == BNXT_XDP_TX) {
			tx_buf->action = BNXT_XDP_TX_NONE;
			txr->xdp_tx_pending--;
			rx_doorbell_needed = true;
			last_tx_cons = tx_cons;

			frags = tx_buf->nr_frags;
			for (j = 0; j < frags; j++) {
				tx_cons = NEXT_TX(tx_cons);
				sw_cons = RING_TX(bp, tx_cons);
				tx_buf = &txr->tx_buf_ring[sw_cons];
				page_pool_recycle_direct_netmem(rxr->page_pool,
								tx_buf->netmem);
				tx_buf->netmem = 0;
			}
		} else if (tx_buf->action == BNXT_XDP_TX_XSK) {
			xsk_pool = tx_buf->xsk_pool;
			frags = tx_buf->nr_frags;
			xsk_descs += frags + 1;
			tx_buf->action = BNXT_XDP_TX_NONE;
			tx_buf->xsk_pool = NULL;

			for (j = 0; j < frags; j++) {
				tx_cons = NEXT_TX(tx_cons);
				sw_cons = RING_TX(bp, tx_cons);
				tx_buf = &txr->tx_buf_ring[sw_cons];
				tx_buf->action = BNXT_XDP_TX_NONE;
			}
		} else {
			bnxt_sched_reset_txr(bp, txr, tx_cons);
			return;
		}
		tx_cons = NEXT_TX(tx_cons);
	}

	bnapi->events &= ~BNXT_TX_CMP_EVENT;
	WRITE_ONCE(txr->tx_cons, tx_cons);
	if (xsk_descs)
		xsk_tx_completed(xsk_pool, xsk_descs);
	if (rx_doorbell_needed) {
		tx_buf = &txr->tx_buf_ring[RING_TX(bp, last_tx_cons)];
		bnxt_db_write(bp, &rxr->rx_db, tx_buf->rx_prod);

	}
}

void bnxt_free_xdp_tx_ring(struct bnxt *bp, struct bnxt_tx_ring_info *txr)
{
	struct bnxt_rx_ring_info *rxr = txr->bnapi->rx_ring;
	struct xsk_buff_pool *pool = rxr->xsk_pool;
	struct page_pool *page_pool = rxr->page_pool;
	u16 cons = txr->tx_cons;
	u32 xsk_descs = 0;

	while (RING_TX(bp, cons) != RING_TX(bp, txr->tx_prod)) {
		struct bnxt_sw_tx_bd *head_buf, *tx_buf;
		int frags, i;

		tx_buf = &txr->tx_buf_ring[RING_TX(bp, cons)];
		head_buf = tx_buf;
		frags = tx_buf->nr_frags;

		switch (tx_buf->action) {
		case BNXT_XDP_REDIRECT:
			dma_unmap_single(&bp->pdev->dev,
					 dma_unmap_addr(tx_buf, mapping),
					 dma_unmap_len(tx_buf, len),
					 DMA_TO_DEVICE);
			xdp_return_frame(tx_buf->xdpf);
			break;
		case BNXT_XDP_TX:
			for (i = 0; i < frags; i++) {
				cons = NEXT_TX(cons);
				tx_buf = &txr->tx_buf_ring[RING_TX(bp, cons)];
				page_pool_recycle_direct_netmem(page_pool,
								tx_buf->netmem);
				tx_buf->netmem = 0;
			}
			break;
		case BNXT_XDP_TX_XSK:
			pool = tx_buf->xsk_pool;
			xsk_descs += frags + 1;
			for (i = 0; i < frags; i++)
				cons = NEXT_TX(cons);
			break;
		default:
			WARN_ON_ONCE(1);
			break;
		}

		head_buf->action = BNXT_XDP_TX_NONE;
		head_buf->xdpf = NULL;
		cons = NEXT_TX(cons);
	}

	if (pool && xsk_uses_need_wakeup(pool))
		xsk_set_tx_need_wakeup(pool);
	if (xsk_descs)
		xsk_tx_completed(pool, xsk_descs);

	WRITE_ONCE(txr->tx_cons, cons);
	txr->xdp_tx_pending = 0;
}

bool bnxt_xdp_attached(struct bnxt *bp, struct bnxt_rx_ring_info *rxr)
{
	struct bpf_prog *xdp_prog = READ_ONCE(rxr->xdp_prog);

	return !!xdp_prog;
}

void bnxt_xdp_buff_init(struct bnxt_rx_ring_info *rxr, u16 cons,
			u8 *data_ptr, unsigned int len,
			netmem_ref netmem, struct xdp_buff *xdp)
{
	u32 buflen = rxr->xdp_frame_size;
	struct bnxt_sw_rx_bd *rx_buf;
	u32 dma_offset;
	u32 offset;

	rx_buf = &rxr->rx_buf_ring[cons];
	dma_offset = rx_buf->offset + rxr->rx_offset;
	offset = rxr->xdp_headroom;

	page_pool_dma_sync_netmem_for_cpu(rxr->page_pool, netmem,
					  dma_offset, len);

	xdp_init_buff(xdp, buflen, &rxr->xdp_rxq);
	xdp_prepare_buff(xdp, data_ptr - offset, offset, len, true);
	if (xsk_buff_is_netmem(netmem))
		xdp_buff_set_netmem(xdp, netmem);
}

void bnxt_xdp_buff_frags_free(struct bnxt_rx_ring_info *rxr,
			      struct xdp_buff *xdp)
{
	struct skb_shared_info *shinfo;
	int i;

	if (!xdp || !xdp_buff_has_frags(xdp))
		return;
	shinfo = xdp_get_shared_info_from_buff(xdp);
	for (i = 0; i < shinfo->nr_frags; i++) {
		netmem_ref netmem = skb_frag_netmem(&shinfo->frags[i]);

		page_pool_recycle_direct_netmem(rxr->page_pool, netmem);
	}
	shinfo->nr_frags = 0;
}

/* returns the following:
 * true    - packet consumed by XDP and new buffer is allocated.
 * false   - packet should be passed to the stack.
 */
bool bnxt_rx_xdp(struct bnxt *bp, struct bnxt_rx_ring_info *rxr, u16 cons,
		 struct xdp_buff *xdp, netmem_ref netmem, u8 **data_ptr,
		 unsigned int *len, u8 *event)
{
	struct bpf_prog *xdp_prog = READ_ONCE(rxr->xdp_prog);
	struct bnxt_tx_ring_info *txr;
	struct bnxt_sw_rx_bd *rx_buf;
	struct pci_dev *pdev;
	dma_addr_t mapping;
	u32 tx_needed = 1;
	void *orig_data;
	u32 tx_avail;
	u32 offset;
	u32 act;

	if (!xdp_prog)
		return false;

	pdev = bp->pdev;
	offset = rxr->xdp_headroom;

	txr = rxr->bnapi->tx_ring[0];
	/* BNXT_RX_PAGE_MODE(bp) when XDP enabled */
	orig_data = xdp->data;

	act = bpf_prog_run_xdp(xdp_prog, xdp);

	tx_avail = bnxt_tx_avail(bp, txr);
	/* Hold the RX producer while XDP_TX owns reposted RX buffers. */
	if (txr->xdp_tx_pending)
		*event &= ~BNXT_RX_EVENT;

	*len = xdp->data_end - xdp->data;
	if (orig_data != xdp->data) {
		offset = xdp->data - xdp->data_hard_start;
		*data_ptr = xdp->data_hard_start + offset;
	}

	switch (act) {
	case XDP_PASS:
		return false;

	case XDP_TX:
		rx_buf = &rxr->rx_buf_ring[cons];
		mapping = rx_buf->mapping - rxr->rx_dma_offset;
		mapping += rxr->rx_offset - rxr->xdp_headroom;

		if (unlikely(xdp_buff_has_frags(xdp))) {
			struct skb_shared_info *sinfo = xdp_get_shared_info_from_buff(xdp);

			tx_needed += sinfo->nr_frags;
		}

		if (tx_avail < tx_needed) {
			trace_xdp_exception(bp->dev, xdp_prog, act);
			bnxt_xdp_buff_frags_free(rxr, xdp);
			bnxt_reuse_rx_netmem(rxr, cons, netmem);
			return true;
		}

		dma_sync_single_for_device(&pdev->dev, mapping + offset, *len,
					   bp->rx_dir);

		*event &= ~BNXT_RX_EVENT;
		*event |= BNXT_TX_EVENT;
		__bnxt_xmit_xdp(bp, txr, mapping + offset, *len,
				NEXT_RX(rxr->rx_prod), xdp);
		bnxt_reuse_rx_netmem(rxr, cons, netmem);
		return true;
	case XDP_REDIRECT:
		/* if we are calling this here then we know that the
		 * redirect is coming from a frame received by the
		 * bnxt_en driver.
		 */

		/* if we are unable to allocate a new buffer, abort and reuse */
		if (bnxt_alloc_rx_data(bp, rxr, rxr->rx_prod, GFP_ATOMIC)) {
			bnxt_xsk_rx_starved(rxr);
			trace_xdp_exception(bp->dev, xdp_prog, act);
			bnxt_xdp_buff_frags_free(rxr, xdp);
			bnxt_reuse_rx_netmem(rxr, cons, netmem);
			return true;
		}

		if (xdp_do_redirect(bp->dev, xdp, xdp_prog)) {
			trace_xdp_exception(bp->dev, xdp_prog, act);
			bnxt_xdp_buff_frags_free(rxr, xdp);
			page_pool_recycle_direct_netmem(rxr->page_pool, netmem);
			return true;
		}

		*event |= BNXT_REDIRECT_EVENT;
		break;
	default:
		bpf_warn_invalid_xdp_action(bp->dev, xdp_prog, act);
		fallthrough;
	case XDP_ABORTED:
		trace_xdp_exception(bp->dev, xdp_prog, act);
		fallthrough;
	case XDP_DROP:
		bnxt_xdp_buff_frags_free(rxr, xdp);
		bnxt_reuse_rx_netmem(rxr, cons, netmem);
		break;
	}
	return true;
}

int bnxt_xdp_xmit(struct net_device *dev, int num_frames,
		  struct xdp_frame **frames, u32 flags)
{
	struct bnxt *bp = netdev_priv(dev);
	struct bpf_prog *xdp_prog = READ_ONCE(bp->xdp_prog);
	struct pci_dev *pdev = bp->pdev;
	struct bnxt_tx_ring_info *txr;
	dma_addr_t mapping;
	int nxmit = 0;
	int ring;
	int i;

	if (!test_bit(BNXT_STATE_OPEN, &bp->state) ||
	    !bp->tx_nr_rings_xdp ||
	    !xdp_prog)
		return -EINVAL;

	ring = smp_processor_id() % bp->tx_nr_rings_xdp;
	txr = &bp->tx_ring[ring];

	if (READ_ONCE(txr->dev_state) == BNXT_DEV_STATE_CLOSING)
		return -EINVAL;

	if (static_branch_unlikely(&bnxt_xdp_locking_key))
		spin_lock(&txr->xdp_tx_lock);

	for (i = 0; i < num_frames; i++) {
		struct xdp_frame *xdp = frames[i];

		if (!bnxt_tx_avail(bp, txr))
			break;

		mapping = dma_map_single(&pdev->dev, xdp->data, xdp->len,
					 DMA_TO_DEVICE);

		if (dma_mapping_error(&pdev->dev, mapping))
			break;

		__bnxt_xmit_xdp_redirect(bp, txr, mapping, xdp->len, xdp);
		nxmit++;
	}

	if (flags & XDP_XMIT_FLUSH) {
		/* Sync BD data before updating doorbell */
		wmb();
		bnxt_db_write(bp, &txr->tx_db, txr->tx_prod);
	}

	if (static_branch_unlikely(&bnxt_xdp_locking_key))
		spin_unlock(&txr->xdp_tx_lock);

	return nxmit;
}

static void bnxt_xsk_tx_packet(struct bnxt *bp,
			       struct bnxt_tx_ring_info *txr,
			       struct xsk_buff_pool *pool,
			       struct xdp_desc *descs, u32 count)
{
	struct bnxt_sw_tx_bd *head_buf;
	u16 head, prod = txr->tx_prod;
	u32 i;

	head = prod;
	head_buf = &txr->tx_buf_ring[RING_TX(bp, head)];
	head_buf->xsk_pool = pool;
	head_buf->nr_frags = count - 1;
	head_buf->action = BNXT_XDP_TX_XSK;

	for (i = 0; i < count; i++) {
		struct xdp_desc_ctx ctx;
		struct bnxt_sw_tx_bd *tx_buf;
		struct xdp_desc *desc = &descs[i];
		struct tx_bd *txbd;
		u32 flags;

		ctx = xsk_buff_raw_get_ctx(pool, desc->addr, desc->options);
		xsk_buff_raw_dma_sync_for_device(pool, ctx.dma, desc->len);

		tx_buf = &txr->tx_buf_ring[RING_TX(bp, prod)];
		if (i)
			tx_buf->action = BNXT_XDP_TX_NONE;

		txbd = &txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];
		flags = desc->len << TX_BD_LEN_SHIFT;
		if (!i)
			flags |= TX_BD_CNT(count) |
				 bnxt_lhint_arr[desc->len >> 9];
		if (xsk_is_eop_desc(desc))
			flags |= TX_BD_FLAGS_PACKET_END;

		txbd->tx_bd_len_flags_type = cpu_to_le32(flags);
		txbd->tx_bd_haddr = cpu_to_le64(ctx.dma);
		if (!i)
			txbd->tx_bd_opaque =
				SET_TX_OPAQUE(bp, txr, head, count);

		prod = NEXT_TX(prod);
	}

	WRITE_ONCE(txr->tx_prod, prod);
}

static void bnxt_xsk_tx(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			struct xsk_buff_pool *pool)
{
	struct xdp_desc *descs = pool->tx_descs;
	bool need_wakeup;
	u32 count, first, i;
	u32 budget;

	if (READ_ONCE(txr->dev_state) == BNXT_DEV_STATE_CLOSING)
		return;
	need_wakeup = xsk_uses_need_wakeup(pool);
	if (need_wakeup && xsk_tx_need_wakeup(pool))
		return;
	if (static_branch_unlikely(&bnxt_xdp_locking_key))
		spin_lock(&txr->xdp_tx_lock);

	if (need_wakeup && txr->tx_cons == txr->tx_prod)
		xsk_set_tx_need_wakeup(pool);

	budget = min_t(u32, bnxt_tx_avail(bp, txr), BNXT_XSK_TX_BUDGET);
	count = xsk_tx_peek_release_desc_batch(pool, budget);

	for (i = 0; i < count;) {
		first = i;
		while (++i < count && !xsk_is_eop_desc(&descs[i - 1]))
			;

		bnxt_xsk_tx_packet(bp, txr, pool, &descs[first], i - first);
	}

	if (count) {
		/* Publish descriptors before notifying the device. */
		wmb();
		bnxt_db_write(bp, &txr->tx_db, txr->tx_prod);
	}

	if (need_wakeup && txr->tx_cons != txr->tx_prod)
		xsk_clear_tx_need_wakeup(pool);

	if (static_branch_unlikely(&bnxt_xdp_locking_key))
		spin_unlock(&txr->xdp_tx_lock);
}

void bnxt_xsk_poll(struct bnxt *bp, struct bnxt_napi *bnapi)
{
	struct bnxt_rx_ring_info *rxr = bnapi->rx_ring;
	struct xsk_buff_pool *pool;

	if (!rxr || bnapi->tx_fault)
		return;

	pool = READ_ONCE(rxr->xsk_pool);
	if (!pool)
		return;

	bnxt_xsk_tx(bp, bnapi->tx_ring[0], pool);
}

int bnxt_xsk_wakeup(struct net_device *dev, u32 queue_id, u32 flags)
{
	struct bnxt *bp = netdev_priv(dev);
	struct bnxt_rx_ring_info *rxr;
	struct xsk_buff_pool *pool;

	if (queue_id >= bp->rx_nr_rings)
		return -EINVAL;
	if (!test_bit(BNXT_STATE_OPEN, &bp->state))
		return -ENETDOWN;

	rxr = &bp->rx_ring[queue_id];
	pool = READ_ONCE(rxr->xsk_pool);
	if (!pool)
		return -EINVAL;

	if ((flags & XDP_WAKEUP_TX) && xsk_uses_need_wakeup(pool))
		xsk_clear_tx_need_wakeup(pool);

	napi_schedule(&rxr->bnapi->napi);

	return 0;
}

bool bnxt_xsk_active(struct bnxt *bp)
{
	unsigned int queue_id;

	for (queue_id = 0; queue_id < bp->dev->real_num_rx_queues;
	     queue_id++)
		if (bnxt_xsk_pool(bp, queue_id))
			return true;

	return false;
}

static int bnxt_xsk_validate(struct bnxt *bp, struct xsk_buff_pool *pool,
			     struct bpf_prog *prog, unsigned int mtu,
			     u32 rx_size, struct netlink_ext_ack *extack)
{
	u32 packet_size = mtu + VLAN_ETH_HLEN;

	if (!prog) {
		NL_SET_ERR_MSG_MOD(extack,
				   "AF_XDP zero-copy requires an XDP program");
		return -EINVAL;
	}

	if (!bnxt_xsk_page_size_ok() ||
	    xsk_pool_get_chunk_size(pool) != SZ_4K) {
		NL_SET_ERR_MSG_MOD(extack,
				   "AF_XDP zero-copy requires 4K pages and chunks");
		return -EOPNOTSUPP;
	}

	if (!xsk_pool_get_rx_frag_step(pool)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "AF_XDP zero-copy requires aligned UMEM");
		return -EOPNOTSUPP;
	}

	if (rx_size > NET_IP_ALIGN &&
	    packet_size <= rx_size - NET_IP_ALIGN)
		return 0;

	if (!xsk_pool_uses_sg(pool)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "UMEM headroom and MTU require XDP_USE_SG");
		return -EOPNOTSUPP;
	}

	if (!prog->aux->xdp_has_frags) {
		NL_SET_ERR_MSG_MOD(extack,
				   "UMEM headroom and MTU require XDP frags");
		return -EOPNOTSUPP;
	}

	if (!(bp->flags & BNXT_FLAG_AGG_RINGS)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "UMEM headroom requires aggregate RX rings");
		return -EOPNOTSUPP;
	}

	return 0;
}

static u32 bnxt_xsk_rx_thresh(struct bnxt *bp,
			      struct xsk_buff_pool *new_pool,
			      unsigned int new_queue,
			      struct bpf_prog *prog, unsigned int mtu)
{
	struct xsk_buff_pool *pool;
	u32 rx_size;
	unsigned int queue_id;

	rx_size = SKB_DATA_ALIGN(mtu + VLAN_ETH_HLEN + NET_IP_ALIGN +
				 ETH_FCS_LEN);
	if (prog && prog->aux->xdp_has_frags)
		rx_size = PAGE_SIZE -
			  ALIGN(max(NET_SKB_PAD, XDP_PACKET_HEADROOM), 8) -
			  BNXT_XDP_TAILROOM;

	for (queue_id = 0; queue_id < bp->dev->real_num_rx_queues;
	     queue_id++) {
		if (new_pool && queue_id == new_queue)
			pool = new_pool;
		else
			pool = bnxt_xsk_pool(bp, queue_id);
		if (pool)
			rx_size = min(rx_size, bnxt_xsk_rx_size(pool));
	}

	return rx_size;
}

static int bnxt_xsk_validate_pools(struct bnxt *bp,
				   struct xsk_buff_pool *new_pool,
				   unsigned int new_queue,
				   struct bpf_prog *prog, unsigned int mtu,
				   struct netlink_ext_ack *extack)
{
	struct xsk_buff_pool *pool;
	u32 rx_size;
	unsigned int queue_id;
	int rc;

	rx_size = bnxt_xsk_rx_thresh(bp, new_pool, new_queue, prog, mtu);
	if (new_pool) {
		rc = bnxt_xsk_validate(bp, new_pool, prog, mtu, rx_size,
				       extack);
		if (rc)
			return rc;
	}

	for (queue_id = 0; queue_id < bp->dev->real_num_rx_queues;
	     queue_id++) {
		if (new_pool && queue_id == new_queue)
			continue;

		pool = bnxt_xsk_pool(bp, queue_id);
		if (!pool)
			continue;

		rc = bnxt_xsk_validate(bp, pool, prog, mtu, rx_size, extack);
		if (rc)
			return rc;
	}

	return 0;
}

static int bnxt_xsk_setup_pool(struct bnxt *bp, struct xsk_buff_pool *pool,
			       u16 queue_id, struct netlink_ext_ack *extack)
{
	int rc;

	if (queue_id >= bp->dev->real_num_rx_queues) {
		NL_SET_ERR_MSG_MOD(extack, "RX queue index out of range");
		return -EINVAL;
	}

	if (!pool)
		return xsk_pool_setup_page_pool(bp->dev, NULL, queue_id,
						extack);

	if (!BNXT_SUPPORTS_QUEUE_API(bp)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "AF_XDP zero-copy requires queue management");
		return -EOPNOTSUPP;
	}
	if (netif_running(bp->dev) && !bp->tph_mode) {
		NL_SET_ERR_MSG_MOD(extack,
				   "AF_XDP zero-copy requires PCIe TPH");
		return -EOPNOTSUPP;
	}

	rc = bnxt_xsk_validate_pools(bp, pool, queue_id,
				     READ_ONCE(bp->xdp_prog), bp->dev->mtu,
				     extack);
	if (rc)
		return rc;

	return xsk_pool_setup_page_pool(bp->dev, pool, queue_id, extack);
}

static int bnxt_xdp_set(struct bnxt *bp, struct bpf_prog *prog,
			struct netlink_ext_ack *extack)
{
	struct net_device *dev = bp->dev;
	int tx_xdp = 0, rc, tc;
	struct bpf_prog *old;

	netdev_assert_locked(dev);
	if (!prog && bnxt_xsk_active(bp)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "detach AF_XDP sockets before the XDP program");
		return -EBUSY;
	}

	if (prog && !prog->aux->xdp_has_frags &&
	    bp->dev->mtu > BNXT_MAX_PAGE_MODE_MTU) {
		netdev_warn(dev, "MTU %d larger than %d without XDP frag support.\n",
			    bp->dev->mtu, BNXT_MAX_PAGE_MODE_MTU);
		return -EOPNOTSUPP;
	}
	if (prog && bp->flags & BNXT_FLAG_HDS) {
		netdev_warn(dev, "XDP is disallowed when HDS is enabled.\n");
		return -EOPNOTSUPP;
	}
	if (!(bp->flags & BNXT_FLAG_SHARED_RINGS)) {
		netdev_warn(dev, "ethtool rx/tx channels must be combined to support XDP.\n");
		return -EOPNOTSUPP;
	}
	if (prog) {
		rc = bnxt_xsk_validate_pools(bp, NULL, 0, prog, dev->mtu,
					     extack);
		if (rc)
			return rc;
	}
	if (prog)
		tx_xdp = bp->rx_nr_rings;

	tc = bp->num_tc;
	if (!tc)
		tc = 1;
	rc = bnxt_check_rings(bp, bp->tx_nr_rings_per_tc, bp->rx_nr_rings,
			      true, tc, tx_xdp);
	if (rc) {
		netdev_warn(dev, "Unable to reserve enough TX rings to support XDP.\n");
		return rc;
	}
	if (netif_running(dev))
		bnxt_close_nic(bp, true, false);

	old = xchg(&bp->xdp_prog, prog);
	if (old)
		bpf_prog_put(old);

	if (prog) {
		bnxt_set_rx_skb_mode(bp, true);
		xdp_features_set_redirect_target_locked(dev, true);
	} else {
		xdp_features_clear_redirect_target_locked(dev);
		bnxt_set_rx_skb_mode(bp, false);
	}
	bp->tx_nr_rings_xdp = tx_xdp;
	bp->tx_nr_rings = bp->tx_nr_rings_per_tc * tc + tx_xdp;
	bnxt_set_cp_rings(bp, true);
	bnxt_set_tpa_flags(bp);
	bnxt_set_ring_params(bp);

	if (netif_running(dev))
		return bnxt_open_nic(bp, true, false);

	return 0;
}

int bnxt_xsk_validate_mtu(struct bnxt *bp, unsigned int mtu,
			  struct netlink_ext_ack *extack)
{
	return bnxt_xsk_validate_pools(bp, NULL, 0,
				       READ_ONCE(bp->xdp_prog), mtu, extack);
}

int bnxt_xdp(struct net_device *dev, struct netdev_bpf *xdp)
{
	struct bnxt *bp = netdev_priv(dev);
	int rc;

	switch (xdp->command) {
	case XDP_SETUP_PROG:
		rc = bnxt_xdp_set(bp, xdp->prog, xdp->extack);
		break;
	case XDP_SETUP_XSK_POOL:
		rc = bnxt_xsk_setup_pool(bp, xdp->xsk.pool,
					 xdp->xsk.queue_id, xdp->extack);
		break;
	default:
		rc = -EINVAL;
		break;
	}
	return rc;
}

struct sk_buff *
bnxt_xdp_build_skb(struct bnxt *bp, struct sk_buff *skb, u8 num_frags,
		   struct bnxt_rx_ring_info *rxr, struct xdp_buff *xdp)
{
	struct skb_shared_info *sinfo = xdp_get_shared_info_from_buff(xdp);

	if (!skb)
		return NULL;

	xdp_update_skb_frags_info(skb, num_frags, sinfo->xdp_frags_size,
				  rxr->rx_page_size * num_frags,
				  xdp_buff_get_skb_flags(xdp));
	return skb;
}

int bnxt_xdp_rx_hash(const struct xdp_md *ctx, u32 *hash,
		     enum xdp_rss_hash_type *rss_type)
{
	const struct bnxt_xdp_buff *xdp = (void *)ctx;
	const struct rx_cmp_ext *rxcmp1 = xdp->rxcmp1;
	const struct rx_cmp *rxcmp = xdp->rxcmp;
	enum xdp_rss_hash_type hash_type = 0;
	u32 itypes;

	if (!rxcmp || !RX_CMP_HASH_VALID(rxcmp))
		return -ENODATA;

	*hash = le32_to_cpu(rxcmp->rx_cmp_rss_hash);

	if (!rxcmp1) {
		*rss_type = XDP_RSS_TYPE_L2;
		return 0;
	}

	if (xdp->cmp_type == CMP_TYPE_RX_L2_CMP) {
		itypes = RX_CMP_ITYPES(rxcmp);
		if (rxcmp1->rx_cmp_flags2 &
		    cpu_to_le32(RX_CMP_FLAGS2_IP_TYPE)) {
			hash_type |= XDP_RSS_TYPE_L3_IPV6;
		} else {
			hash_type |= XDP_RSS_TYPE_L3_IPV4;
		}

		switch (itypes) {
		case RX_CMP_FLAGS_ITYPE_TCP:
			hash_type |= XDP_RSS_L4 | XDP_RSS_L4_TCP;
			break;
		case RX_CMP_FLAGS_ITYPE_UDP:
			hash_type |= XDP_RSS_L4 | XDP_RSS_L4_UDP;
			break;
		case RX_CMP_FLAGS_ITYPE_ICMP:
			hash_type |= XDP_RSS_L4 | XDP_RSS_L4_ICMP;
			break;
		default:
			break;
		}
	} else if (xdp->cmp_type == CMP_TYPE_RX_L2_V3_CMP) {
		struct bnxt *bp = netdev_priv(xdp->xdp.rxq->dev);

		if (rxcmp1->rx_cmp_flags2 & cpu_to_le32(RX_CMP_FLAGS2_IP_TYPE))
			hash_type |= XDP_RSS_TYPE_L3_IPV6;
		else
			hash_type |= XDP_RSS_TYPE_L3_IPV4;

		if (bnxt_rss_ext_op(bp, rxcmp) == PKT_HASH_TYPE_L4)
			hash_type |= XDP_RSS_L4;
	}

	*rss_type = hash_type;
	return 0;
}
