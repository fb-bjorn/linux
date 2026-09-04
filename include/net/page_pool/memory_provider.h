/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_PAGE_POOL_MEMORY_PROVIDER_H
#define _NET_PAGE_POOL_MEMORY_PROVIDER_H

#include <linux/bits.h>
#include <net/netmem.h>
#include <net/page_pool/types.h>

struct netdev_rx_queue;
struct netlink_ext_ack;
struct sk_buff;

/**
 * enum memory_provider_cap - memory provider capabilities
 * @MEM_PROVIDER_CAP_XDP: supports XDP
 * @MEM_PROVIDER_CAP_HDS: supports header-data split
 * @MEM_PROVIDER_CAP_PAGE_FRAG: supports page-pool fragment allocation
 */
enum memory_provider_cap {
	MEM_PROVIDER_CAP_XDP		= BIT(0),
	MEM_PROVIDER_CAP_HDS		= BIT(1),
	MEM_PROVIDER_CAP_PAGE_FRAG	= BIT(2),
};

struct memory_provider_ops {
	netmem_ref (*alloc_netmems)(struct page_pool *pool, gfp_t gfp);
	bool (*release_netmem)(struct page_pool *pool, netmem_ref netmem);
	int (*init)(struct page_pool *pool);
	void (*destroy)(struct page_pool *pool);
	int (*nl_fill)(void *mp_priv, struct sk_buff *rsp,
		       struct netdev_rx_queue *rxq);
	void (*uninstall)(void *mp_priv, struct netdev_rx_queue *rxq);
	unsigned long capabilities;
};

bool net_mp_niov_set_dma_addr(struct net_iov *niov, dma_addr_t addr);
void net_mp_niov_set_page_pool(struct page_pool *pool, struct net_iov *niov);
void net_mp_set_pool_bulk(struct page_pool *pool, netmem_ref *netmems,
			  u32 count);
void net_mp_niov_release_page_pool(struct net_iov *niov);
void net_mp_page_pool_release_bulk(struct page_pool *pool, u32 count);
void net_mp_niov_clear_page_pool(struct net_iov *niov);

int netif_mp_open_rxq(struct net_device *dev, unsigned int rxq_idx,
		      const struct pp_memory_provider_params *p,
		      struct netlink_ext_ack *extack);
#ifdef CONFIG_XDP_SOCKETS
int netif_mp_open_rxq_xsk(struct net_device *dev, unsigned int rxq_idx,
			  const struct pp_memory_provider_params *p,
			  struct netlink_ext_ack *extack);
int netif_mp_close_rxq_xsk(struct net_device *dev, unsigned int rxq_idx,
			   const struct pp_memory_provider_params *old_p);
#endif
void netif_mp_close_rxq(struct net_device *dev, unsigned int rxq_idx,
			const struct pp_memory_provider_params *old_p);

/**
  * net_mp_netmem_place_in_cache() - give a netmem to a page pool
  * @pool:      the page pool to place the netmem into
  * @netmem:    netmem to give
  *
  * Push an accounted netmem into the page pool's allocation cache. The caller
  * must ensure that there is space in the cache. It should only be called off
  * the mp_ops->alloc_netmems() path.
  */
static inline void net_mp_netmem_place_in_cache(struct page_pool *pool,
						netmem_ref netmem)
{
	pool->alloc.cache[pool->alloc.count++] = netmem;
}

#endif
