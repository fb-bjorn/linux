// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/ethtool_netlink.h>
#include <linux/netdevice.h>
#include <net/netdev_lock.h>
#include <net/netdev_queues.h>
#include <net/netdev_rx_queue.h>
#include <net/page_pool/memory_provider.h>

#include "dev.h"
#include "page_pool_priv.h"

/* Attempts to remove a provider before the core waits for a device event */
#define NETIF_MP_CLOSE_TRIES	3

/* Which queue runs after netdev_rx_queue_reconfig() */
enum netdev_rxq_state {
	NETDEV_RXQ_NEW,		/* the new one, none if the device is down */
	NETDEV_RXQ_OLD,		/* the old one */
	NETDEV_RXQ_STOPPED,	/* none, the old one is stopped */
	NETDEV_RXQ_UNCERTAIN,	/* stopping the old one failed */
};

void netdev_rx_queue_lease(struct netdev_rx_queue *rxq_dst,
			   struct netdev_rx_queue *rxq_src)
{
	netdev_assert_locked(rxq_src->dev);
	netdev_assert_locked(rxq_dst->dev);

	netdev_hold(rxq_src->dev, &rxq_src->lease_tracker, GFP_KERNEL);

	WRITE_ONCE(rxq_src->lease, rxq_dst);
	WRITE_ONCE(rxq_dst->lease, rxq_src);
}

void netdev_rx_queue_unlease(struct netdev_rx_queue *rxq_dst,
			     struct netdev_rx_queue *rxq_src)
{
	netdev_assert_locked(rxq_dst->dev);
	netdev_assert_locked(rxq_src->dev);

	netif_rxq_cleanup_unlease(rxq_src);

	WRITE_ONCE(rxq_src->lease, NULL);
	WRITE_ONCE(rxq_dst->lease, NULL);

	netdev_put(rxq_src->dev, &rxq_src->lease_tracker);
}

bool netif_rxq_is_leased(struct net_device *dev, unsigned int rxq_idx)
{
	if (rxq_idx < dev->real_num_rx_queues)
		return READ_ONCE(__netif_get_rx_queue(dev, rxq_idx)->lease);
	return false;
}

/* Virtual devices eligible for leasing have no dev->dev.parent, while
 * physical devices always have one. Use this to enforce the correct
 * lease traversal direction.
 */
static bool netif_lease_dir_ok(const struct net_device *dev,
			       enum netif_lease_dir dir)
{
	if (dir == NETIF_VIRT_TO_PHYS && !dev->dev.parent)
		return true;
	if (dir == NETIF_PHYS_TO_VIRT && dev->dev.parent)
		return true;
	return false;
}

bool netif_is_queue_leasee(const struct net_device *dev)
{
	return netif_lease_dir_ok(dev, NETIF_VIRT_TO_PHYS);
}

struct netdev_rx_queue *
__netif_get_rx_queue_lease(struct net_device **dev, unsigned int *rxq_idx,
			   enum netif_lease_dir dir)
{
	struct net_device *orig_dev = *dev;
	struct netdev_rx_queue *rxq = __netif_get_rx_queue(orig_dev, *rxq_idx);

	if (rxq->lease) {
		if (!netif_lease_dir_ok(orig_dev, dir))
			return NULL;
		rxq = rxq->lease;
		*rxq_idx = get_netdev_rx_queue_index(rxq);
		*dev = rxq->dev;
	}
	return rxq;
}

/* See also page_pool_is_unreadable() */
bool netif_rxq_has_unreadable_mp(struct net_device *dev, unsigned int rxq_idx)
{
	if (rxq_idx < dev->real_num_rx_queues)
		return __netif_get_rx_queue(dev, rxq_idx)->mp_params.mp_ops;
	return false;
}
EXPORT_SYMBOL(netif_rxq_has_unreadable_mp);

bool netif_rxq_has_mp(struct net_device *dev, unsigned int rxq_idx)
{
	if (rxq_idx < dev->real_num_rx_queues)
		return __netif_get_rx_queue(dev, rxq_idx)->mp_params.mp_priv;
	return false;
}

static int netdev_rx_queue_reconfig(struct net_device *dev,
				    unsigned int rxq_idx,
				    struct netdev_queue_config *qcfg_old,
				    struct netdev_queue_config *qcfg_new,
				    enum netdev_rxq_state *state)
{
	struct netdev_rx_queue *rxq = __netif_get_rx_queue(dev, rxq_idx);
	const struct netdev_queue_mgmt_ops *qops = dev->queue_mgmt_ops;
	void *new_mem, *old_mem;
	int err;

	/* A queue of a closed device does no DMA. */
	*state = netif_running(dev) ? NETDEV_RXQ_OLD : NETDEV_RXQ_STOPPED;
	if (!qops || !qops->ndo_queue_stop || !qops->ndo_queue_mem_free ||
	    !qops->ndo_queue_mem_alloc || !qops->ndo_queue_start)
		return -EOPNOTSUPP;

	netdev_assert_locked(dev);

	new_mem = kvzalloc(qops->ndo_queue_mem_size, GFP_KERNEL);
	if (!new_mem)
		return -ENOMEM;

	old_mem = kvzalloc(qops->ndo_queue_mem_size, GFP_KERNEL);
	if (!old_mem) {
		err = -ENOMEM;
		goto err_free_new_mem;
	}

	err = qops->ndo_queue_mem_alloc(dev, qcfg_new, new_mem, rxq_idx);
	if (err)
		goto err_free_old_mem;

	err = page_pool_check_memory_provider(dev, rxq);
	if (err)
		goto err_free_new_queue_mem;

	if (netif_running(dev)) {
		err = qops->ndo_queue_stop(dev, old_mem, rxq_idx);
		if (err) {
			*state = NETDEV_RXQ_UNCERTAIN;
			goto err_free_new_queue_mem;
		}

		err = qops->ndo_queue_start(dev, qcfg_new, new_mem, rxq_idx);
		if (err)
			goto err_start_queue;
	} else {
		swap(new_mem, old_mem);
	}

	*state = NETDEV_RXQ_NEW;
	qops->ndo_queue_mem_free(dev, old_mem);

	kvfree(old_mem);
	kvfree(new_mem);

	return 0;

err_start_queue:
	/* Restarting the queue with old_mem should be successful as we haven't
	 * changed any of the queue configuration, and there is not much we can
	 * do to recover from a failure here.
	 *
	 * WARN if we fail to recover the old rx queue, and at least free
	 * old_mem so we don't also leak that.
	 */
	if (qops->ndo_queue_start(dev, qcfg_old, old_mem, rxq_idx)) {
		WARN(1,
		     "Failed to restart old queue in error path. RX queue %d may be unhealthy.",
		     rxq_idx);
		qops->ndo_queue_mem_free(dev, old_mem);
		*state = NETDEV_RXQ_STOPPED;
	}

err_free_new_queue_mem:
	qops->ndo_queue_mem_free(dev, new_mem);

err_free_old_mem:
	kvfree(old_mem);

err_free_new_mem:
	kvfree(new_mem);

	return err;
}

int netdev_rx_queue_restart(struct net_device *dev, unsigned int rxq_idx)
{
	struct netdev_queue_config qcfg;
	enum netdev_rxq_state state;
	int err;

	netdev_queue_config(dev, rxq_idx, &qcfg);
	err = netdev_rx_queue_reconfig(dev, rxq_idx, &qcfg, &qcfg, &state);
	netif_mp_rearm_close(dev);

	return err;
}
EXPORT_SYMBOL_NS_GPL(netdev_rx_queue_restart, "NETDEV_INTERNAL");

static int __netif_mp_open_rxq(struct net_device *dev, unsigned int rxq_idx,
			       const struct pp_memory_provider_params *p,
			       struct netlink_ext_ack *extack)
{
	const struct netdev_queue_mgmt_ops *qops = dev->queue_mgmt_ops;
	struct netdev_queue_config qcfg[2];
	enum netdev_rxq_state state;
	struct netdev_rx_queue *rxq;
	int ret;

	if (!qops)
		return -EOPNOTSUPP;

	if (dev->cfg->hds_config != ETHTOOL_TCP_DATA_SPLIT_ENABLED) {
		NL_SET_ERR_MSG(extack, "tcp-data-split is disabled");
		return -EINVAL;
	}
	if (dev->cfg->hds_thresh) {
		NL_SET_ERR_MSG(extack, "hds-thresh is not zero");
		return -EINVAL;
	}
	if (dev_xdp_prog_count(dev)) {
		NL_SET_ERR_MSG(extack, "unable to custom memory provider to device with XDP program attached");
		return -EEXIST;
	}
	if (p->rx_page_size && !(qops->supported_params & QCFG_RX_PAGE_SIZE)) {
		NL_SET_ERR_MSG(extack, "device does not support: rx_page_size");
		return -EOPNOTSUPP;
	}

	rxq = __netif_get_rx_queue(dev, rxq_idx);
	if (rxq->mp_params.mp_ops) {
		NL_SET_ERR_MSG(extack, "designated queue already memory provider bound");
		return -EEXIST;
	}
#ifdef CONFIG_XDP_SOCKETS
	if (rxq->pool) {
		NL_SET_ERR_MSG(extack, "designated queue already in use by AF_XDP");
		return -EBUSY;
	}
#endif

	netdev_queue_config(dev, rxq_idx, &qcfg[0]);
	rxq->mp_params = *p;
	ret = netdev_queue_config_validate(dev, rxq_idx, &qcfg[1], extack);
	if (ret)
		goto err_clear_mp;

	/* On failure the new queue memory is freed, so the provider is not
	 * installed. A failed ndo_queue_start() must leave its queue stopped.
	 */
	ret = netdev_rx_queue_reconfig(dev, rxq_idx, &qcfg[0], &qcfg[1],
				       &state);
	if (ret)
		goto err_clear_mp;

	return 0;

err_clear_mp:
	memset(&rxq->mp_params, 0, sizeof(rxq->mp_params));
	return ret;
}

int netif_mp_open_rxq(struct net_device *dev, unsigned int rxq_idx,
		      const struct pp_memory_provider_params *p,
		      struct netlink_ext_ack *extack)
{
	int ret;

	if (!netdev_need_ops_lock(dev))
		return -EOPNOTSUPP;

	if (rxq_idx >= dev->real_num_rx_queues) {
		NL_SET_ERR_MSG(extack, "rx queue index out of range");
		return -ERANGE;
	}
	rxq_idx = array_index_nospec(rxq_idx, dev->real_num_rx_queues);

	if (!netif_rxq_is_leased(dev, rxq_idx))
		return __netif_mp_open_rxq(dev, rxq_idx, p, extack);

	if (!__netif_get_rx_queue_lease(&dev, &rxq_idx, NETIF_VIRT_TO_PHYS)) {
		NL_SET_ERR_MSG(extack, "rx queue leased to a virtual netdev");
		return -EBUSY;
	}
	if (!dev->dev.parent) {
		NL_SET_ERR_MSG(extack, "rx queue belongs to a virtual netdev");
		return -EOPNOTSUPP;
	}

	netdev_lock(dev);
	/* Unregister may have removed the providers already */
	if (dev->reg_state != NETREG_REGISTERED)
		ret = -ENODEV;
	else
		ret = __netif_mp_open_rxq(dev, rxq_idx, p, extack);
	netdev_unlock(dev);
	return ret;
}

static bool netif_mp_try_close(struct net_device *dev, unsigned int rxq_idx)
{
	struct netdev_rx_queue *rxq = __netif_get_rx_queue(dev, rxq_idx);
	struct pp_memory_provider_params p = rxq->mp_params;
	struct netdev_queue_config qcfg[2];
	enum netdev_rxq_state state;

	/* A driver may lower real_num_rx_queues on resume; queues past it
	 * do not run.
	 */
	if (rxq_idx >= dev->real_num_rx_queues) {
		memset(&rxq->mp_params, 0, sizeof(rxq->mp_params));
		goto uninstall;
	}

	netdev_queue_config(dev, rxq_idx, &qcfg[0]);
	memset(&rxq->mp_params, 0, sizeof(rxq->mp_params));
	netdev_queue_config(dev, rxq_idx, &qcfg[1]);

	netdev_rx_queue_reconfig(dev, rxq_idx, &qcfg[0], &qcfg[1], &state);
	if (state == NETDEV_RXQ_OLD || state == NETDEV_RXQ_UNCERTAIN) {
		rxq->mp_params = p;
		return false;
	}

uninstall:
	__netif_mp_uninstall_rxq(rxq, &p, MP_UNINSTALL_DMA_STOPPED);
	return true;
}

static void __netif_mp_close_rxq(struct net_device *dev, unsigned int rxq_idx,
				 const struct pp_memory_provider_params *old_p)
{
	struct netdev_rx_queue *rxq;

	rxq = __netif_get_rx_queue(dev, rxq_idx);

	/* Callers holding a netdev ref may get here after we already
	 * went thru shutdown via dev_memory_provider_uninstall().
	 */
	if (dev->reg_state > NETREG_REGISTERED &&
	    !rxq->mp_params.mp_ops)
		return;

	if (WARN_ON_ONCE(rxq->mp_params.mp_ops != old_p->mp_ops ||
			 rxq->mp_params.mp_priv != old_p->mp_priv))
		return;

	/* The core already retries the removal. */
	if (rxq->mp_close_pending)
		return;

	if (netif_mp_try_close(dev, rxq_idx))
		return;

	netdev_warn(dev, "rx queue %u still uses its memory provider, retrying\n",
		    rxq_idx);
	rxq->mp_close_pending = true;
	rxq->mp_close_tries = NETIF_MP_CLOSE_TRIES;
	__netif_mp_uninstall_rxq(rxq, old_p, MP_UNINSTALL_LOGICAL_ONLY);
	__netdev_work_core_sched(dev, NETDEV_WORK_MP_CLOSE);
}

/**
 * netif_mp_close_rxq - remove a memory provider from an RX queue
 * @dev: netdev of the queue, may lease it from a physical device
 * @rxq_idx: queue index on @dev
 * @old_p: the provider to remove
 *
 * If the old queue may keep running, the provider is uninstalled with
 * %MP_UNINSTALL_LOGICAL_ONLY, and with %MP_UNINSTALL_DMA_STOPPED once the core
 * removed it.
 *
 * Context: holds the instance lock of @dev. For a leased queue, takes the
 * instance lock of the physical device.
 */
void netif_mp_close_rxq(struct net_device *dev, unsigned int rxq_idx,
			const struct pp_memory_provider_params *old_p)
{
	if (!netif_rxq_is_leased(dev, rxq_idx)) {
		__netif_mp_close_rxq(dev, rxq_idx, old_p);
		return;
	}

	if (WARN_ON_ONCE(!__netif_get_rx_queue_lease(&dev, &rxq_idx,
						     NETIF_VIRT_TO_PHYS)))
		return;

	netdev_lock(dev);
	__netif_mp_close_rxq(dev, rxq_idx, old_p);
	netdev_unlock(dev);
}

void netif_mp_retry_close(struct net_device *dev)
{
	struct netdev_rx_queue *rxq;
	bool again = false;
	unsigned int i;

	netdev_assert_locked(dev);

	for (i = 0; i < dev->num_rx_queues; i++) {
		rxq = __netif_get_rx_queue(dev, i);
		if (!rxq->mp_close_pending || !rxq->mp_close_tries)
			continue;

		rxq->mp_close_tries--;
		if (netif_mp_try_close(dev, i))
			rxq->mp_close_pending = false;
		else if (rxq->mp_close_tries)
			again = true;
		else
			netdev_warn(dev, "rx queue %u still uses its memory provider\n",
				    i);
	}

	if (again)
		__netdev_work_core_sched(dev, NETDEV_WORK_MP_CLOSE);
}

/* The device closed, attached or restarted a queue: retry failed removals */
void netif_mp_rearm_close(struct net_device *dev)
{
	struct pp_memory_provider_params p;
	struct netdev_rx_queue *rxq;
	bool pending = false;
	unsigned int i;

	netdev_assert_locked_ops_compat(dev);

	for (i = 0; i < dev->num_rx_queues; i++) {
		rxq = __netif_get_rx_queue(dev, i);
		if (!rxq->mp_close_pending)
			continue;

		/* A closed device does no DMA. Complete now, as netdev_work
		 * skips an absent device.
		 */
		if (!netif_running(dev)) {
			p = rxq->mp_params;
			memset(&rxq->mp_params, 0, sizeof(rxq->mp_params));
			rxq->mp_close_pending = false;
			__netif_mp_uninstall_rxq(rxq, &p,
						 MP_UNINSTALL_DMA_STOPPED);
			continue;
		}

		rxq->mp_close_tries = NETIF_MP_CLOSE_TRIES;
		pending = true;
	}

	if (pending)
		__netdev_work_core_sched(dev, NETDEV_WORK_MP_CLOSE);
}

void __netif_mp_uninstall_rxq(struct netdev_rx_queue *rxq,
			      const struct pp_memory_provider_params *p,
			      enum mp_uninstall_reason reason)
{
	if (p->mp_ops && p->mp_ops->uninstall)
		p->mp_ops->uninstall(p->mp_priv, rxq, reason);
}

/* Clean up memory provider state when a queue lease is torn down. If
 * a memory provider was installed on the physical queue via the lease,
 * close it now. The memory provider is a property of the queue itself,
 * and it was _guaranteed_ to be installed on the physical queue via
 * the lease redirection. The extra __netif_mp_close_rxq is needed
 * since the physical queue can outlive the virtual queue in the lease
 * case, so it needs to be reconfigured to clear the memory provider.
 * The physical queue may keep the provider until its DMA stops.
 */
void netif_rxq_cleanup_unlease(struct netdev_rx_queue *phys_rxq)
{
	unsigned int rxq_idx = get_netdev_rx_queue_index(phys_rxq);
	struct pp_memory_provider_params p = phys_rxq->mp_params;

	if (!p.mp_ops)
		return;

	__netif_mp_close_rxq(phys_rxq->dev, rxq_idx, &p);
}
