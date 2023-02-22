/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock_types.h>
#include <linux/workqueue.h>

#include "fba_ioctl.h"
#include "fba_proto.h"

/*
 * FBA Monitoring
 *
 * SAIL monitoring kernel module API present the definitions and data as simple
 * arrays. To do that it consolidates the definitions and data from multiple
 * sources. The current sources are the driver itself and the CCP.
 * It periodically pools for updates from the device and from the driver.
 * The monitoring items and groups from the device and the driver are held in
 * separate per source structs witin fba_mon:
 * fba_mon.src_data[SAIL_MTS_DRIVER].items and fba_mon.src_data[SAIL_MTS_DRIVER].groups.
 * fba_mon.src_data[SAIL_MTS_CCP].items and fba_mon.src_data[SAIL_MTS_CCP].groups.
 * The driver unifies items and groups from all the sources (driver and ccp) into a single
 * array of items and groups and exposes the unified array to the user space.
 * The driver is using a mutex to protext fba_mon.src_data[].data from concurrently updating
 * the data monitoring cache, and processing fba_mon_get_data() requests. Without a mutex,
 * data may change while being copied by fba_mon_get_data().
 */

struct fba_device;

struct mon_item;
struct mon_group;
struct mon_data;

/* Monitoring source id */
enum mon_source {
	SAIL_MTS_DRIVER,
	SAIL_MTS_CCP,
	SAIL_MTS_MAX,
};

struct fba_mon_src {
	bool is_init;
	/* Count of items. */
	size_t item_cnt;
	/* Count of groups. */
	size_t group_cnt;
	/* Item offset in stacked array. */
	size_t item_offset;
	/* group offset in stacked array. */
	size_t group_offset;
	/* Array of all items. */
	struct mon_item *items;
	/* Array of all groups. */
	struct mon_group *groups;
	/* Array of all the item data. */
	struct mon_data *data;
	/* Array of all the item set data. */
	struct mon_data_set *data_set;
};

struct fba_mon {
	/*
	 * Lock guards fba_mon_get_data() data copying from concurrent
	 * update of data.
	 */
	struct mutex mutex;

	/* Used for periodic polling of the item's data. */
	struct workqueue_struct *workq;
	struct delayed_work dwork;
	struct fba_mon_src src_info[SAIL_MTS_MAX];
	unsigned int mon_poll_freq_jiffies;
};

/*
 * Stop making new requests to device, and wait for existing to complete
 * or timeout.
 */
void fba_mon_device_stop(struct fba_mon *mon);

/*
 * fba_mon_init() is called from device probe.
 * Initializes the fba_mon structure, mutext and also creates the fba_mon
 * delayed work queue.
 * fba_mon_init() also allocates and initializes driver provided items and
 * groups. After this call user can query the driver item and group data.
 * Device items and groups are not initialized as part of fba_mon_init().
 * fba_mon_device_init() needs to be called once device is active to access
 * device items and group data.
 */
int fba_mon_init(struct fba_device *fba_dev);

/*
 * Called from driver remove()
 * Free resources and kills the workqueue thread.
 */
void fba_mon_exit(struct fba_mon *mon);

/*
 * Returns -ENOMEM on failures to allocate the fields of fba_mon.
 * Returns DMA error on failure to read the defintiions from the devices.
 * Retutns zero on success.
 * See the documentation at the top for more.
 * fba_mon_device_init is to be called once device is in ACTIVE state.
 * Device provided items and groups data can be read through ioctl after calling
 * fba_mon_device_init().
 */
int fba_mon_device_init(struct fba_device *fba_dev);

/*
 * Frees device item related resources
 * fba_mon_device_exit() should be called when device is transitioned to deactive state.
 */
void fba_mon_device_exit(struct fba_mon *mon);

/*
 * Copies the values of the monitored items specified by ids into data_arr.
 * Copies the error of the monitored items specified by ids into err_arr.
 * data_arr is assumed to be of size id_count * MON_VALUE_SIZE bytes
 * and err_arr array is assumed to be of size id_count elements.
 * The returnd value at data_arr[i*MON_VALUE_SIZE] is valid when
 * err_arr[i]==0.
 *
 * Returns:
 * - zero on success
 * - -EAGAIN when data is not yet avialble. Data is available after first time
 *   that data is read for all items.
 * - -EINVAL for invalid ids.
 */
int fba_mon_get_data(struct fba_device *fba_dev, uint32_t *ids,
		     uint32_t id_count, void *data_arr, int16_t *err_arr);

/*
 * Sets the values of the monitored items specified by ids into data_arr.
 * data_arr is assumed to be of size id_count * MON_VALUE_SIZE bytes
 * Returns:
 * - zero on success
 * - -EACCES on attempt to write to a monitoring entry that has no write callback
 * - -EINVAL for invalid ids.
 */
int fba_mon_set_data(struct fba_device *fba_dev, uint32_t *ids,
		     uint32_t id_count, void *data_arr);

/* API to get the mon item defenition. This is called from FBA_IOCTL_MON_DEFS. */
int fba_mon_get_defs(struct fba_device *fba_dev,
		     struct mon_defs_ioctl *mon_defs_req);

size_t fba_mon_get_item_cnt(struct fba_mon *mon);

size_t fba_mon_get_group_cnt(struct fba_mon *mon);
