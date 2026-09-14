/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_PAGE_POOL_MEMORY_PROVIDER_H
#define _NET_PAGE_POOL_MEMORY_PROVIDER_H

#include <net/netmem.h>
#include <net/page_pool/types.h>

struct netdev_rx_queue;
struct netlink_ext_ack;
struct sk_buff;

struct memory_provider_ops {
	netmem_ref (*alloc_netmems)(struct page_pool *pool, gfp_t gfp);
	bool (*release_netmem)(struct page_pool *pool, netmem_ref netmem);
	/* Providing a CPU address also opts into XDP and flexible HDS. @offset
	 * may be NULL; otherwise set it to the offset from the returned address
	 * to the provider frame start. XDP_PACKET_HEADROOM is not implied.
	 */
	void *(*get_netmem_addr)(netmem_ref netmem, u32 *offset);
	/* Called after RX refill. @full says that the caller reached its refill
	 * target. Return true when NAPI may complete, or false to keep it
	 * scheduled even without packet completions.
	 */
	bool (*refill_done)(struct page_pool *pool, bool full);
	int (*init)(struct page_pool *pool);
	void (*destroy)(struct page_pool *pool);
	int (*nl_fill)(void *mp_priv, struct sk_buff *rsp,
		       struct netdev_rx_queue *rxq);
	void (*uninstall)(void *mp_priv, struct netdev_rx_queue *rxq);
	bool supports_page_pool_frag;
};

bool net_mp_niov_set_dma_addr(struct net_iov *niov, dma_addr_t addr);
void net_mp_netmem_set_page_pool_bulk(struct page_pool *pool,
				      netmem_ref *netmems, u32 count);
void net_mp_release_page_pool_bulk(struct page_pool *pool,
				   netmem_ref *netmems, u32 count);
void net_mp_niov_clear_page_pool(struct net_iov *niov);
bool netif_has_unreadable_mp(struct net_device *dev);

int netif_mp_open_rxq(struct net_device *dev, unsigned int rxq_idx,
		      const struct pp_memory_provider_params *p,
		      struct netlink_ext_ack *extack);
int netif_mp_close_rxq(struct net_device *dev, unsigned int rxq_idx,
		       const struct pp_memory_provider_params *old_p);

#endif
