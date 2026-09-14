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

DECLARE_STATIC_KEY_FALSE(bnxt_xdp_locking_key);

#define BNXT_XDP_TAILROOM	SKB_DATA_ALIGN(sizeof(struct skb_shared_info))

static inline u32 bnxt_xdp_rx_size(void)
{
	return PAGE_SIZE - ALIGN(max(NET_SKB_PAD, XDP_PACKET_HEADROOM), 8) -
		BNXT_XDP_TAILROOM;
}

enum bnxt_xdp_tx_action {
	BNXT_XDP_TX_NONE,
	BNXT_XDP_TX,
	BNXT_XDP_REDIRECT,
	BNXT_XDP_TX_LOOPBACK,
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
		 struct xdp_buff *xdp, struct bpf_prog *xdp_prog,
		 netmem_ref netmem, u8 **data_ptr, unsigned int *len,
		 u8 *event);
int bnxt_xdp(struct net_device *dev, struct netdev_bpf *xdp);
int bnxt_xdp_xmit(struct net_device *dev, int num_frames,
		  struct xdp_frame **frames, u32 flags);

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
void bnxt_free_xdp_tx_ring(struct bnxt *bp,
			   struct bnxt_tx_ring_info *txr);

#endif
