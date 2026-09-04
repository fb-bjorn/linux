/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2016-2017 Broadcom Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#ifndef BNXT_XDP_H
#define BNXT_XDP_H

#include <net/netdev_rx_queue.h>
#include <net/xdp_sock_drv.h>

DECLARE_STATIC_KEY_FALSE(bnxt_xdp_locking_key);

#define BNXT_XSK_TX_BUDGET	64
#define BNXT_XSK_MAX_SEGS	min_t(u32, MAX_SKB_FRAGS + 1, TX_MAX_BD_CNT)
#define BNXT_XDP_TAILROOM	SKB_DATA_ALIGN(sizeof(struct skb_shared_info))

enum bnxt_xdp_tx_action {
	BNXT_XDP_TX_NONE,
	BNXT_XDP_TX,
	BNXT_XDP_REDIRECT,
	BNXT_XDP_TX_XSK,
};

struct bnxt_xdp_buff {
	struct xdp_buff xdp;
	struct rx_cmp *rxcmp;
	struct rx_cmp_ext *rxcmp1;
	u8 cmp_type;
};

struct bnxt_sw_tx_bd *bnxt_xmit_bd(struct bnxt *bp,
				   struct bnxt_tx_ring_info *txr,
				   dma_addr_t mapping, u32 len,
				   struct xdp_buff *xdp);
void bnxt_tx_int_xdp(struct bnxt *bp, struct bnxt_napi *bnapi, int budget);
bool bnxt_rx_xdp(struct bnxt *bp, struct bnxt_rx_ring_info *rxr, u16 cons,
		 struct xdp_buff *xdp, netmem_ref netmem, u8 **data_ptr,
		 unsigned int *len, u8 *event);
int bnxt_xdp(struct net_device *dev, struct netdev_bpf *xdp);
int bnxt_xdp_xmit(struct net_device *dev, int num_frames,
		  struct xdp_frame **frames, u32 flags);

bool bnxt_xdp_attached(struct bnxt *bp, struct bnxt_rx_ring_info *rxr);

void bnxt_xdp_buff_init(struct bnxt_rx_ring_info *rxr, u16 cons,
			u8 *data_ptr, unsigned int len,
			netmem_ref netmem, struct xdp_buff *xdp);
void bnxt_xdp_buff_frags_free(struct bnxt_rx_ring_info *rxr,
			      struct xdp_buff *xdp);
struct sk_buff *bnxt_xdp_build_skb(struct bnxt *bp, struct sk_buff *skb,
				   u8 num_frags, struct bnxt_rx_ring_info *rxr,
				   struct xdp_buff *xdp);
int bnxt_xdp_rx_hash(const struct xdp_md *ctx, u32 *hash,
		     enum xdp_rss_hash_type *rss_type);

void bnxt_xsk_poll(struct bnxt *bp, struct bnxt_napi *bnapi);
int bnxt_xsk_wakeup(struct net_device *dev, u32 queue_id, u32 flags);
bool bnxt_xsk_active(struct bnxt *bp);
void bnxt_free_xdp_tx_ring(struct bnxt *bp,
			   struct bnxt_tx_ring_info *txr);
int bnxt_xsk_validate_mtu(struct bnxt *bp, unsigned int mtu,
			  struct netlink_ext_ack *extack);

static inline struct xsk_buff_pool *
bnxt_xsk_pool(struct bnxt *bp, unsigned int queue_id)
{
	struct xsk_buff_pool *pool;

	pool = xsk_get_pool_from_qid(bp->dev, queue_id);
	if (!netif_rxq_mp_match(bp->dev, queue_id, pool))
		return NULL;

	return pool;
}

static inline bool bnxt_xsk_page_size_ok(void)
{
	unsigned long page_size = PAGE_SIZE;

	return page_size == SZ_4K;
}

static inline u32 bnxt_xsk_rx_size(struct xsk_buff_pool *pool)
{
	u32 size;

	size = __xsk_pool_get_rx_frame_size(pool) - BNXT_XDP_TAILROOM;
	return ALIGN_DOWN(size, XSK_RX_FRAME_SIZE_ALIGN);
}

static inline void *bnxt_netmem_address(netmem_ref netmem)
{
	if (xsk_buff_is_netmem(netmem))
		return xsk_buff_netmem_get_frame(netmem);

	return netmem_address(netmem);
}

static inline void bnxt_xsk_rx_starved(struct bnxt_rx_ring_info *rxr)
{
	struct xsk_buff_pool *pool = READ_ONCE(rxr->xsk_pool);

	if (!pool)
		return;

	WRITE_ONCE(rxr->xsk_rx_starved, true);
	if (xsk_uses_need_wakeup(pool))
		xsk_set_rx_need_wakeup(pool);
}

#endif
