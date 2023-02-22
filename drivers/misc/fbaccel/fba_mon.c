// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/types.h>

#include "fba_mon.h"
#include "fba_device.h"
#include "fba_poller.h"
#include "fba_util.h"
#include "fba_admin.h"

static unsigned int mon_poll_freq_ms = 5000;
module_param(mon_poll_freq_ms, int, 0644);
MODULE_PARM_DESC(
	mon_poll_freq_ms,
	"SAIL monitoring polling frequency in milliseconds (default: 5000). "
	"Set to 0 to disable.");

static bool mon_disable_polling;
module_param(mon_disable_polling, bool, 0444);
MODULE_PARM_DESC(
	mon_disable_polling,
	"Bypass the Linux kernel cache for all mon queries. "
	"Note: This is not runtime modifiable (must be set at load-time). ");

#define MON_NAME_LEN (120)

// TODO: hide from user space
// The value’s buffer is sufficiently large to hold the value of any item.
// In the future we may want to optimize the value’s size for values
// that need smaller space. For simplicity, first iteration will use this
// constant size definition.
struct mon_data {
	// To simplicity, when value encodes a string, they must terminate
	// with a null.
	char value[MON_VALUE_SIZE];
	int16_t err; // sail_err_t
	int8_t is_init; // value was updated at least once.
	int8_t reserved;
};
static_assert((sizeof(struct mon_data) % 32) == 0);

struct mon_data_set {
	// When value is a string, it should be null terminated
	char value[MON_VALUE_SIZE];
	// The monitoring item with the specified index will be set to value
	int16_t index;
	int8_t reserved[2];
};
static_assert((sizeof(struct mon_data_set) % 32) == 0);

// Describe an item to be monitored.
struct mon_item {
	char name[MON_NAME_LEN]; // Name of the item.
	uint16_t group;
	uint16_t size; // data value size in bytes
	uint8_t raw_type; // enum sail_raw_type
	uint8_t unit; // sail_unit_t
	uint8_t is_static;
	uint8_t source; // enum mon_source
};
// Ensure struct is 4-byte aligned.
static_assert((sizeof(struct mon_item) % 32) == 0);

/* Describe grouping of monitoring items. */
struct mon_group {
	uint16_t id;
	uint16_t reserved1;
	uint32_t reserved2;
	char name[MON_NAME_LEN];
};
static_assert((sizeof(struct mon_group) % 32) == 0);

// Measurement's unit in the physical world.
// Currently unused. Intended to be used for pretty printing of the value by
// the application in user space.
enum mon_unit {
	SAIL_MTU_UNKNOWN = 0,
	SAIL_MTU_NONE, // An index or count.
	SAIL_MTU_AMP, // AMPs.
	SAIL_MTU_HEX, // Hex data.
	SAIL_MTU_RPM, // Revolutions per minute.
	SAIL_MTU_PERC, // Percentage.
	SAIL_MTU_TEMP_C, // Temperature in Celsius.
	SAIL_MTU_TEMP_F, // Temperature in Fahrenheit.
	SAIL_MTU_VOLT_M, // Voltage in mV.
	SAIL_MTU_TIME_S, //Time in seconds
	SAIL_MTU_FREQ_MHZ, //Freq in Mega HZ
	SAIL_MTU_MW, // Power in mW
	SAIL_MTU_OPS, // Operations per second
	SAIL_MTU_MAX
};

// The raw C type used for encoding/decoding the raw data.
enum mon_raw_type {
	SAIL_MTT_UNKNOWN = 0, // Int (8, 16, 32, 64)
	SAIL_MTT_INT, // Signed int (8, 16, 32, 64)
	SAIL_MTT_UINT, // Unsigned int (8, 16, 32, 64)
	// SAIL_MTT_FIXP ecodes fractions as fixed point value since floating point is
	// not supported on Zephyr CCP and has weak linux kernel support.
	// The format is: int32_t val[2] , such that the equivalent float
	// value is: val[0] + val[1] * 10e-6
	// See: https://docs.zephyrproject.org/latest/reference/peripherals/sensor.html#c.sensor_value
	SAIL_MTT_FIXP,
	SAIL_MTT_CSTR, // NUL terminated C string.
	SAIL_MTT_RAW, // Raw bytes.
	SAIL_MTT_DATETIME, // Timestamp time, date, month, year
	SAIL_MTT_BOOL,
	SAIL_MTT_MAX,
};

// Helper for MON_ITM_CB()
#define MON_ITM(name_, group_, source_, raw_type_, size_, unit_, is_static_)   \
	{                                                                      \
		.name = (name_), .group = (group_), .source = (source_),       \
		.size = (size_), .raw_type = (raw_type_), .unit = (unit_),     \
		.is_static = (is_static_),                                     \
	}

// Helper for populating item definitions
#define MON_ITM_CB(name_, group_, source_, raw_type_, size_, unit_,            \
		   is_static_, user_data_, get_value_cb_)                      \
	{                                                                      \
		MON_ITM(name_, group_, source_, raw_type_, size_, unit_,       \
			is_static_),                                           \
			.user_data = (user_data_),                             \
			.get_value_cb = (get_value_cb_),                       \
			.set_value_cb = (NULL)                                 \
	}

#define MON_ITM_CB_RD_WR(name_, group_, source_, raw_type_, size_, unit_,      \
			 is_static_, user_data_, get_value_cb_, set_value_cb_) \
	{                                                                      \
		MON_ITM(name_, group_, source_, raw_type_, size_, unit_,       \
			is_static_),                                           \
			.user_data = (user_data_),                             \
			.get_value_cb = (get_value_cb_),                       \
			.set_value_cb = (set_value_cb_)                        \
	}

#define MON_BOOL(name_, group_, source_, user_data_, get_value_cb_)            \
	MON_ITM_CB(name_, group_, source_, SAIL_MTT_BOOL, sizeof(uint8_t),     \
		   SAIL_MTU_NONE, MON_ITM_DYNAMIC, user_data_, get_value_cb_)

#define MON_BOOL_STATIC(name_, group_, source_, user_data_, get_value_cb_)     \
	MON_ITM_CB(name_, group_, source_, SAIL_MTT_BOOL, sizeof(uint8_t),     \
		   SAIL_MTU_NONE, MON_ITM_STATIC, user_data_, get_value_cb_)

#define MON_ITM_STATIC (1)
#define MON_ITM_DYNAMIC (0)
#define MON_FIXP_SIZE (sizeof(int32_t) * 2)
#define MON_VER_TYPE uint64_t

// Static uint32_t
#define MON_U32(name_, group_, source_, user_data_, get_value_cb_)             \
	MON_ITM_CB(name_, group_, source_, SAIL_MTT_UINT, sizeof(uint32_t),    \
		   SAIL_MTU_NONE, MON_ITM_STATIC, user_data_, get_value_cb_)

// Define a monitored item of type string
#define MON_STR(name_, group_, source_, user_data_, get_value_cb_)             \
	MON_ITM_CB(name_, group_, source_, SAIL_MTT_CSTR,                      \
		   (MON_VALUE_SIZE - 1), SAIL_MTU_NONE, MON_ITM_STATIC,        \
		   user_data_, get_value_cb_)

#define MON_CRC_U32(name_, group_, source_, user_data_, get_value_cb_)         \
	MON_ITM_CB(name_, group_, source_, SAIL_MTT_UINT, sizeof(uint32_t),    \
		   SAIL_MTU_HEX, MON_ITM_DYNAMIC, user_data_, get_value_cb_)

// Used to define a group. Example:
// const mon_group_t groups[] = {
//  MON_GRP(SAIL_MTG_xxx, "group_name"),
// }
//
// SAIL_MTG_xxx: MTG stands for MoniToring Group. It is defined by caller
// to uniqley identify groups.
#define MON_GRP(id_, name_)                                                    \
	{                                                                      \
		.id = (id_), .name = (name_),                                  \
	}

// Callback for reading monitoring values from the driver. The callback writes
// upto buffer_size bytes into the buffer.
// dev - fba_device that reads the value.
// user_data - additional context for the callback.
// buffer - points to the value field.
// buffer_size - same as item.size
typedef int16_t (*fba_mon_cb_t)(struct fba_device * /*dev*/,
				void * /*user_data*/, void * /*buf*/,
				size_t /*buf_size*/);

struct fba_mon_item_w_cb {
	struct mon_item item;
	void *user_data;
	fba_mon_cb_t get_value_cb;
	fba_mon_cb_t set_value_cb;
};

// Monitoring group ids
enum {
	MTG_ASIC_INFO,
	MTG_CARD_ALERTS,
};

enum fba_mon_alerts {
	// Alert for DDR Un-correctable error detection
	FBA_MON_ALERT_DDR_UCE = 0,
	// Alert for Internal memory Un-correctable error detection
	FBA_MON_ALERT_INTERNAL_MEM_UCE,
	// Alert for device FW fatal error/crash/hang
	FBA_MON_ALERT_DEVICE_FATAL_ERR,
	// Alert for Heart Beat failure
	FBA_MON_ALERT_HEART_BEAT_FAILURE,
	// Alert for Control core WDT failure
	FBA_MON_ALERT_CCP_WDT_FAILURE
};

static struct mon_group driver_mon_groups[] = {
	MON_GRP(MTG_ASIC_INFO, "asic_info"),
	MON_GRP(MTG_CARD_ALERTS, "card_alerts"),
};
#define N_DRIVER_MON_GROUPS ARRAY_SIZE(driver_mon_groups)

static int16_t get_pci_addr(struct fba_device *fba_dev, void *user_data,
			    void *buf, size_t buf_size)
{
	struct pci_dev *pdev = fba_dev->pdev;

	snprintf((char *)buf, buf_size, "%s", dev_name(&pdev->dev));

	return 0;
}

static int16_t get_pci_class(struct fba_device *fba_dev, void *user_data,
			     void *buf, size_t buf_size)
{
	struct pci_dev *pdev = fba_dev->pdev;

	snprintf((char *)buf, buf_size, "%04x", pdev->class >> 8);

	return 0;
}

static int16_t get_reset_supported(struct fba_device *fba_dev, void *user_data,
				   void *buf, size_t buf_size)
{
	struct pci_dev *pdev = fba_dev->pdev;
	bool *is_supported = buf;

	*is_supported = ((pdev->class >> 8) != 0);
	return 0;
}

static int16_t get_pci_device_id(struct fba_device *fba_dev, void *user_data,
				 void *buf, size_t buf_size)
{
	struct pci_dev *pdev = fba_dev->pdev;

	snprintf((char *)buf, buf_size, "%04x", pdev->device);

	return 0;
}

static int16_t get_device_id(struct fba_device *fba_dev, void *user_data,
			     void *buf, size_t buf_size)
{
	uint32_t *data = (uint32_t *)buf;

	if (buf_size < sizeof(uint32_t)) {
		fba_dev_err(fba_dev, "%s: Invalid buf size", __func__);
		return -EFAULT;
	}
	*data = fba_dev->id;

	return 0;
}

static int16_t get_pci_vendor_id(struct fba_device *fba_dev, void *user_data,
				 void *buf, size_t buf_size)
{
	struct pci_dev *pdev = fba_dev->pdev;

	snprintf((char *)buf, buf_size, "%04x", pdev->vendor);

	return 0;
}

/* based on tables in drivers/pci/probe.c */
static const char *const pcie_speeds[] = {
	"unknown", /* 0 */
	"2.5 GT/s PCIe", /* 1 */
	"5.0 GT/s PCIe", /* 2 */
	"8.0 GT/s PCIe", /* 3 */
	"16.0 GT/s PCIe", /* 4 */
	"32.0 GT/s PCIe", /* 5 */
	"64.0 GT/s PCIe", /* 6 */
	"unknown", /* 7 */
	"unknown", /* 8 */
	"unknown", /* 9 */
	"unknown", /* a */
	"unknown", /* b */
	"unknown", /* c */
	"unknown", /* d */
	"unknown", /* e */
	"unknown", /* f */
};

/* based on current_link_speed_show() in drivers/pci/pci-sysfs.c */
static int16_t get_pcie_link_speed(struct fba_device *fba_dev, void *user_data,
				   void *buf, size_t buf_size)
{
	struct pci_dev *pdev = fba_dev->pdev;
	uint16_t linkstat;
	int err;

	err = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &linkstat);
	if (err)
		return -EINVAL;

	snprintf(buf, buf_size, "%s",
		 pcie_speeds[linkstat & PCI_EXP_LNKSTA_CLS]);

	return 0;
}

/* based on current_link_width_show() in drivers/pci/pci-sysfs.c */
static int16_t get_pcie_link_width(struct fba_device *fba_dev, void *user_data,
				   void *buf, size_t buf_size)
{
	struct pci_dev *pdev = fba_dev->pdev;
	uint16_t linkstat;
	int err;

	err = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &linkstat);
	if (err)
		return -EINVAL;

	snprintf(buf, buf_size, "%u",
		 (linkstat & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT);

	return 0;
}

static int16_t get_driver_version(struct fba_device *fba_dev, void *user_data,
				  void *buf, size_t buf_size)
{
	snprintf(buf, buf_size, "%s", FBACCEL_VERSION);
	return 0;
}

static int16_t get_alert_mon_value(struct fba_device *fba_dev, void *user_data,
				   void *buf, size_t buf_size)
{
	enum fba_mon_alerts alert_type =
		(enum fba_mon_alerts)((uintptr_t)user_data);
	uint32_t *data = (uint32_t *)buf;

	if (buf_size < sizeof(uint32_t)) {
		fba_dev_err(fba_dev, "%s: Invalid buf size", __func__);
		return -EFAULT;
	}

	switch (alert_type) {
	case FBA_MON_ALERT_DDR_UCE:
		*data = fba_dev->ddr_uce_detected;
		break;
	case FBA_MON_ALERT_INTERNAL_MEM_UCE:
		*data = fba_dev->internal_mem_uce_detected;
		break;
	case FBA_MON_ALERT_DEVICE_FATAL_ERR:
		*data = fba_dev->device_fatal_err_detected;
		break;
	case FBA_MON_ALERT_HEART_BEAT_FAILURE:
		*data = fba_dev->heart_beat_failed;
		break;
	case FBA_MON_ALERT_CCP_WDT_FAILURE:
		*data = fba_dev->ccp_wdt_failed;
		break;
	default:
		fba_dev_err(fba_dev, "%s: Invalid alert type", __func__);
		return -EINVAL;
	}
	return 0;
}

static int16_t get_ready_status(struct fba_device *fba_dev, void *user_data,
				void *buf, size_t buf_size)
{
	bool *data = (bool *)buf;

	if (buf_size < sizeof(bool)) {
		fba_dev_err(fba_dev, "%s: Invalid buf size", __func__);
		return -EFAULT;
	}
	*data = false;
	if (!atomic_read(&fba_dev->has_failed))
		*data = true;

	return 0;
}

static int16_t get_m7_health(struct fba_device *fba_dev, void *user_data,
			     void *buf, size_t buf_size)
{
	int16_t err = 0;
	char *m7_health;

	if (fba_dev->cfg->impl_ops->boot_health_check != NULL) {
		m7_health = fba_dev->cfg->impl_ops->boot_health_check(fba_dev);
		if (m7_health) {
			if (buf_size < sizeof(m7_health)) {
				fba_dev_err(fba_dev, "%s: Invalid buf size",
					    __func__);
				return -EFAULT;
			}
			snprintf(buf, buf_size, "%s", m7_health);
		}
	}
	return err;
}

static int16_t did_ccp_panic(struct fba_device *fba_dev, void *user_data,
			     void *buf, size_t buf_size)
{
	bool *ccp_panic_sts = buf;

	if (buf_size < sizeof(bool)) {
		fba_dev_err(fba_dev, "%s: Invalid buf size", __func__);
		return -EFAULT;
	}
	if (fba_dev->cfg->impl_ops->did_ccp_panic != NULL)
		*ccp_panic_sts = fba_dev->cfg->impl_ops->did_ccp_panic(fba_dev);
	else
		return -EOPNOTSUPP;

	return 0;
}

static int16_t get_cxram_ccp_coredump_crc_offset(struct fba_device *fba_dev,
						 void *user_data, void *buf,
						 size_t buf_size)
{
	uint32_t *cxram_ccp_coredump_crc_offset = buf;

	if (buf_size < sizeof(uint32_t)) {
		fba_dev_err(fba_dev, "%s: Invalid buf size", __func__);
		return -EFAULT;
	}
	if (fba_dev->cfg->impl_ops->coredump_get_cxram_crc_offset != NULL)
		*cxram_ccp_coredump_crc_offset =
			fba_dev->cfg->impl_ops->coredump_get_cxram_crc_offset(
				fba_dev);
	else
		return -EOPNOTSUPP;

	return 0;
}

static int16_t get_latest_ccp_coredump_crc(struct fba_device *fba_dev,
					   void *user_data, void *buf,
					   size_t buf_size)
{
	uint32_t *latest_ccp_coredump_crc = buf;

	if (buf_size < sizeof(uint32_t)) {
		fba_dev_err(fba_dev, "%s: Invalid buf size", __func__);
		return -EFAULT;
	}
	if (fba_dev->cfg->impl_ops->coredump_get_crc != NULL)
		*latest_ccp_coredump_crc =
			fba_dev->cfg->impl_ops->coredump_get_crc(fba_dev);
	else
		return -EOPNOTSUPP;

	return 0;
}

static struct fba_mon_item_w_cb driver_mon_items_w_cb[] = {
	MON_STR("driver_version", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		get_driver_version),
	MON_STR("pci_addr", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL, get_pci_addr),
	MON_STR("m7_health", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		get_m7_health),
	MON_STR("pcie_class_id", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		get_pci_class),
	MON_STR("pcie_device_id", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		get_pci_device_id),
	MON_STR("pcie_vendor_id", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		get_pci_vendor_id),
	MON_STR("pcie_link_speed", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		get_pcie_link_speed),
	MON_STR("pcie_link_width", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		get_pcie_link_width),
	MON_U32("alert_ddr_uce", MTG_CARD_ALERTS, SAIL_MTS_DRIVER,
		(void *)FBA_MON_ALERT_DDR_UCE, get_alert_mon_value),
	MON_U32("alert_internal_mem_uce", MTG_CARD_ALERTS, SAIL_MTS_DRIVER,
		(void *)FBA_MON_ALERT_INTERNAL_MEM_UCE, get_alert_mon_value),
	MON_U32("alert_fatal_err", MTG_CARD_ALERTS, SAIL_MTS_DRIVER,
		(void *)FBA_MON_ALERT_DEVICE_FATAL_ERR, get_alert_mon_value),
	MON_U32("alert_heart_beat_failure", MTG_CARD_ALERTS, SAIL_MTS_DRIVER,
		(void *)FBA_MON_ALERT_HEART_BEAT_FAILURE, get_alert_mon_value),
	MON_U32("alert_ccp_wdt_err", MTG_CARD_ALERTS, SAIL_MTS_DRIVER,
		(void *)FBA_MON_ALERT_CCP_WDT_FAILURE, get_alert_mon_value),
	MON_U32("cxram_ccp_coredump_crc_offset", MTG_ASIC_INFO, SAIL_MTS_DRIVER,
		NULL, get_cxram_ccp_coredump_crc_offset),
	MON_BOOL("is_ready", MTG_CARD_ALERTS, SAIL_MTS_DRIVER, NULL,
		 get_ready_status),
	MON_BOOL("did_ccp_panic", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		 did_ccp_panic),
	MON_BOOL_STATIC("reset_supported", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
			get_reset_supported),
	MON_CRC_U32("latest_ccp_coredump_crc", MTG_ASIC_INFO, SAIL_MTS_DRIVER,
		    NULL, get_latest_ccp_coredump_crc),
	MON_U32("device_id", MTG_ASIC_INFO, SAIL_MTS_DRIVER, NULL,
		get_device_id),
};
#define N_DRIVER_MON_ITEMS ARRAY_SIZE(driver_mon_items_w_cb)

static void update_driver_data(struct fba_device *fba_dev)
{
	struct fba_mon *mon = &fba_dev->mon;
	struct mon_data *data = mon->src_info[SAIL_MTS_DRIVER].data;
	int i = 0;

	for (i = 0; i < N_DRIVER_MON_ITEMS; ++i, ++data) {
		struct fba_mon_item_w_cb *item_cb = &driver_mon_items_w_cb[i];

		if (item_cb->item.is_static && data->is_init)
			continue;

		data->err =
			item_cb->get_value_cb(fba_dev, item_cb->user_data,
					      data->value, item_cb->item.size);
	}
}

static int update_device_data(struct fba_device *fba_dev)
{
	struct fba_mon *mon = &fba_dev->mon;
	struct mon_data *data = mon->src_info[SAIL_MTS_CCP].data;
	int err = 0;

	// Requires caller to lock the mon->mutex
	WARN_ON(!mutex_is_locked(&mon->mutex));

	if (!mon->src_info[SAIL_MTS_CCP].item_cnt)
		return 0;

	if ((!atomic_read(&fba_dev->has_failed)) &&
	    (mon->src_info[SAIL_MTS_CCP].is_init)) {
		/* Stop polling for device data  if device has failed. */
		err = fba_admin_dma_from_device(
			fba_dev, ADMIN_MON_READ_DATA, data,
			mon->src_info[SAIL_MTS_CCP].item_cnt * sizeof(*data));
		if (err != 0)
			fba_dev_err(fba_dev, "%s failed to read data %d.",
				    __func__, err);
	}
	return err;
}

// Returns polling frequency in jiffies.
// Freq is set by the mon_poll_freq_ms module params.
// Sets disable to true iff mon_poll_freq_ms==0.
// Logs a msg when mon_poll_freq_ms has changed since last invocation.
static unsigned int get_polling_freq_jiffies(struct fba_device *fba_dev,
					     bool *disable)
{
	if (fba_dev->mon.mon_poll_freq_jiffies !=
	    msecs_to_jiffies(mon_poll_freq_ms)) {
		fba_dev->mon.mon_poll_freq_jiffies =
			msecs_to_jiffies(mon_poll_freq_ms);
		if (mon_poll_freq_ms > 0) {
			fba_dev_info(
				fba_dev,
				"sail monitoring polling frequency is now set to %u milliseconds.",
				mon_poll_freq_ms);
		} else {
			fba_dev_info(
				fba_dev,
				"sail monitoring polling is now disabled.");
		}
	}

	if (fba_dev->mon.mon_poll_freq_jiffies > 0) {
		*disable = false;
		return fba_dev->mon.mon_poll_freq_jiffies;
	}

	*disable = true;
	// When mon_poll_freq_jiffies=0 we want to still poll
	// for an update of mon_poll_freq_jiffies.
	return msecs_to_jiffies(5000);
}

static void fba_mon_work_handler(struct work_struct *work)
{
	struct delayed_work *dwork =
		container_of(work, struct delayed_work, work);
	struct fba_mon *mon = container_of(dwork, struct fba_mon, dwork);
	struct fba_device *fba_dev = container_of(mon, struct fba_device, mon);
	bool disable = false;

	queue_delayed_work(mon->workq, &mon->dwork,
			   get_polling_freq_jiffies(fba_dev, &disable));

	if (!disable) {
		mutex_lock(&mon->mutex);

		update_device_data(fba_dev);
		mutex_unlock(&mon->mutex);
	}
}

// - Copy and offset driver_mon_groups and driver_mon_items_w_cb definition
//   into mon->groups and mon->items respectively.
// - Add the offset to the ids.
// - Force name length to < MON_NAME_LEN.
static void set_driver_items_and_groups(struct fba_mon *mon)
{
	const int src = SAIL_MTS_DRIVER;
	int i = 0;
	const int group_offset = mon->src_info[src].group_offset;

	for (i = 0; i < mon->src_info[src].group_cnt; i++) {
		struct mon_group *cur = &mon->src_info[src].groups[i];

		*cur = driver_mon_groups[i];
		cur->id += mon->src_info[src].group_offset;
		// Force group name length to be < MON_NAME_LEN
		cur->name[MON_NAME_LEN - 1] = 0;
	}

	for (i = 0; i < mon->src_info[src].item_cnt; i++) {
		struct mon_item *cur = &mon->src_info[src].items[i];

		*cur = driver_mon_items_w_cb[i].item;
		cur->group += group_offset;
		// Force item name length to be < MON_NAME_LEN
		cur->name[MON_NAME_LEN - 1] = 0;
	}
}

static int read_and_offset_device_items(struct fba_device *fba_dev,
					struct fba_mon *mon)
{
	const int src = SAIL_MTS_CCP;
	int i;
	const int group_offset = mon->src_info[src].group_offset;
	const size_t size =
		mon->src_info[src].item_cnt * sizeof(struct mon_item);
	int err = 0;

	if (size == 0) // driver_test support
		return 0;

	// Read items from device into the local buffer + offset
	err = fba_admin_dma_from_device(fba_dev, ADMIN_MON_READ_ITEMS,
					&mon->src_info[src].items[0], size);
	if (err != 0) {
		fba_dev_err(
			fba_dev,
			"%s failed to read device_items of size %zu with error %d.",
			__func__, size, err);
		return err;
	}

	for (i = 0; i < mon->src_info[src].item_cnt; i++) {
		struct mon_item *cur = &mon->src_info[src].items[i];

		cur->group += group_offset;
	}

	return 0;
}

// Returns zero on success or error code otherwise.
static int read_and_offset_device_groups(struct fba_device *fba_dev,
					 struct fba_mon *mon)
{
	const int src = SAIL_MTS_CCP;
	const size_t size =
		mon->src_info[src].group_cnt * sizeof(struct mon_group);
	int i, err = 0;

	if (size == 0) // driver_test support
		return 0;

	// Read groups from device
	err = fba_admin_dma_from_device(fba_dev, ADMIN_MON_READ_GROUPS,
					&mon->src_info[src].groups[0], size);
	if (err != 0) {
		fba_dev_err(
			fba_dev,
			"%s failed to read groups of size %zu with error %d.",
			__func__, size, err);
		return err;
	}
	for (i = 0; i < mon->src_info[src].group_cnt; i++) {
		struct mon_group *cur = &mon->src_info[src].groups[i];

		cur->id += mon->src_info[src].group_offset;
	}

	return 0;
}

size_t fba_mon_get_item_cnt(struct fba_mon *mon)
{
	int src;
	size_t item_cnt = 0;

	for (src = 0; src < SAIL_MTS_MAX; ++src)
		item_cnt += mon->src_info[src].item_cnt;

	return item_cnt;
}

size_t fba_mon_get_group_cnt(struct fba_mon *mon)
{
	int src;
	size_t group_cnt = 0;

	for (src = 0; src < SAIL_MTS_MAX; ++src)
		group_cnt += mon->src_info[src].group_cnt;

	return group_cnt;
}

// Returns zero on success or error code otherwise.
static int alloc_items_groups_and_data(struct fba_mon *mon, enum mon_source src)
{
	size_t items_size = 0;
	size_t groups_size = 0;
	size_t data_size = 0;
	size_t data_set_size = 0;
	int err = 0;

	// user views the items and groups as single array which
	// includes data from all mon sources.
	// Use offsets calculated below to decide the array index
	// for items and groups as index in single array view.
	// src = 0, all offsets are 0
	// src = n, offset offset[n-1]+item_cnt[n-1]
	if (src != 0) {
		mon->src_info[src].item_offset =
			mon->src_info[src - 1].item_offset +
			mon->src_info[src - 1].item_cnt;
		mon->src_info[src].group_offset =
			mon->src_info[src - 1].group_offset +
			mon->src_info[src - 1].group_cnt;
	}

	items_size = mon->src_info[src].item_cnt * sizeof(struct mon_item);
	groups_size = mon->src_info[src].group_cnt * sizeof(struct mon_group);
	data_size = mon->src_info[src].item_cnt * sizeof(struct mon_data);
	data_set_size =
		mon->src_info[src].item_cnt * sizeof(struct mon_data_set);

	mon->src_info[src].items = kzalloc(items_size, GFP_KERNEL);
	mon->src_info[src].groups = kzalloc(groups_size, GFP_KERNEL);
	mon->src_info[src].data = kzalloc(data_size, GFP_KERNEL);
	mon->src_info[src].data_set = kzalloc(data_set_size, GFP_KERNEL);

	if (mon->src_info[src].items == NULL) {
		err = -ENOMEM;
		goto err_free_info;
	}

	if (mon->src_info[src].groups == NULL) {
		err = -ENOMEM;
		goto err_free_info;
	}

	if (mon->src_info[src].data == NULL) {
		err = -ENOMEM;
		goto err_free_info;
	}

	if (mon->src_info[src].data == NULL) {
		err = -ENOMEM;
		goto err_free_info;
	}

	return err;

err_free_info:
	kfree(mon->src_info[src].items);
	kfree(mon->src_info[src].groups);
	kfree(mon->src_info[src].data);
	kfree(mon->src_info[src].data_set);

	return err;
}

static int fba_mon_driver_init(struct fba_device *fba_dev)
{
	int rv = 0;
	struct fba_mon *mon = &fba_dev->mon;

	mon->src_info[SAIL_MTS_DRIVER].item_cnt = N_DRIVER_MON_ITEMS;
	mon->src_info[SAIL_MTS_DRIVER].group_cnt = N_DRIVER_MON_GROUPS;

	rv = alloc_items_groups_and_data(mon, SAIL_MTS_DRIVER);
	if (rv != 0) {
		fba_dev_err(fba_dev, "Failed to alloc driver items and groups");
		return rv;
	}
	set_driver_items_and_groups(mon);
	mutex_lock(&mon->mutex);
	mon->src_info[SAIL_MTS_DRIVER].is_init = true;
	update_driver_data(fba_dev);
	mutex_unlock(&mon->mutex);
	return 0;
}

static void fba_mon_driver_exit(struct fba_mon *mon)
{
	mutex_lock(&mon->mutex);
	mon->src_info[SAIL_MTS_DRIVER].is_init = false;
	mon->src_info[SAIL_MTS_DRIVER].item_cnt = 0;
	mon->src_info[SAIL_MTS_DRIVER].group_cnt = 0;
	mutex_unlock(&mon->mutex);
	kfree(mon->src_info[SAIL_MTS_DRIVER].groups);
	kfree(mon->src_info[SAIL_MTS_DRIVER].items);
	kfree(mon->src_info[SAIL_MTS_DRIVER].data);
	kfree(mon->src_info[SAIL_MTS_DRIVER].data_set);
}

void fba_mon_exit(struct fba_mon *mon)
{
	fba_mon_device_stop(mon);
	if (!mon_disable_polling)
		destroy_workqueue(mon->workq);

	// call device exit to make sure fba_mon device resources are freed.
	fba_mon_device_exit(mon);
	fba_mon_driver_exit(mon);
}

int fba_mon_init(struct fba_device *fba_dev)
{
	struct fba_mon *mon = &fba_dev->mon;
	int rv = 0;

	memset(mon, 0, sizeof(*mon));
	mutex_init(&mon->mutex);

	mon->workq = alloc_workqueue("%s_mon", 0, 0, fba_dev->name);
	if (mon->workq == NULL) {
		fba_dev_err(fba_dev, "Failed to start monitoring workqueue");
		return -ENOMEM;
	}
	// Only initialize the driver provided fba_mon items on probe.
	rv = fba_mon_driver_init(fba_dev);
	if (rv != 0) {
		fba_dev_err(fba_dev,
			    "Failed to initialize fba_mon driver data");
		goto err_destroy_wq;
	}

	if (!mon_disable_polling) {
		INIT_DELAYED_WORK(&mon->dwork, fba_mon_work_handler);
		queue_delayed_work(mon->workq, &mon->dwork, 0);
	}

	return 0;

err_destroy_wq:
	if (!mon_disable_polling)
		destroy_workqueue(mon->workq);

	return rv;
}

int fba_mon_device_init(struct fba_device *fba_dev)
{
	struct fba_mon *mon = &fba_dev->mon;
	int err;

	err = alloc_items_groups_and_data(mon, SAIL_MTS_CCP);
	if (err != 0)
		goto err_device_exit;

	err = read_and_offset_device_items(fba_dev, mon);
	if (err != 0)
		goto err_device_exit;

	err = read_and_offset_device_groups(fba_dev, mon);
	if (err != 0)
		goto err_device_exit;

	mutex_lock(&mon->mutex);
	mon->src_info[SAIL_MTS_CCP].is_init = true;
	err = update_device_data(fba_dev);
	mutex_unlock(&mon->mutex);
	if (err != 0)
		goto err_device_exit;

	return 0;

err_device_exit:
	fba_mon_device_exit(mon);

	return err;
}

void fba_mon_device_stop(struct fba_mon *mon)
{
	if (!mon_disable_polling)
		cancel_delayed_work_sync(&mon->dwork);
}

void fba_mon_device_exit(struct fba_mon *mon)
{
	mutex_lock(&mon->mutex);
	if (mon->src_info[SAIL_MTS_CCP].is_init) {
		mon->src_info[SAIL_MTS_CCP].is_init = false;
		mon->src_info[SAIL_MTS_CCP].item_cnt = 0;
		mon->src_info[SAIL_MTS_CCP].group_cnt = 0;
	}

	kfree(mon->src_info[SAIL_MTS_CCP].groups);
	mon->src_info[SAIL_MTS_CCP].groups = NULL;
	kfree(mon->src_info[SAIL_MTS_CCP].items);
	mon->src_info[SAIL_MTS_CCP].items = NULL;
	kfree(mon->src_info[SAIL_MTS_CCP].data);
	mon->src_info[SAIL_MTS_CCP].data = NULL;
	kfree(mon->src_info[SAIL_MTS_CCP].data_set);
	mon->src_info[SAIL_MTS_CCP].data_set = NULL;
	mutex_unlock(&mon->mutex);
}

static int fba_mon_get_src_from_id(struct fba_mon *mon, uint32_t id)
{
	uint32_t start;
	uint32_t end;
	int src;

	// ids from src_info[SAIL_MTS_DRIVER].offset=0 to
	// src_info[SAIL_MTS_DRIVER].item_cnt are for SAIL_MTS_DRIVER.
	// ids are from src_info[SAIL_MTS_CCP].offset to
	// src_info[SAIL_MTS_CCP].item_cnt are for SAIL_MTS_CCP.
	for (src = 0; src < SAIL_MTS_MAX; ++src) {
		start = mon->src_info[src].item_offset;
		end = start + mon->src_info[src].item_cnt;
		if ((id >= start) && (id < end))
			return src;
	}
	return -EINVAL;
}

int fba_mon_get_defs(struct fba_device *fba_dev,
		     struct mon_defs_ioctl *mon_defs_req)
{
	int src;
	int err = 0;
	size_t max_items_size;
	size_t max_groups_size;
	struct fba_mon *mon = &fba_dev->mon;

	mutex_lock(&mon->mutex);
	max_items_size = fba_mon_get_item_cnt(mon) * sizeof(struct mon_item);
	if (mon_defs_req->max_items_size < max_items_size) {
		fba_dev_err(fba_dev, "invalid item buffer size %zu",
			    (size_t)mon_defs_req->max_items_size);
		err = -EINVAL;
		goto out_unlock;
	}

	max_groups_size = fba_mon_get_group_cnt(mon) * sizeof(struct mon_group);
	if (mon_defs_req->max_groups_size < max_groups_size) {
		fba_dev_err(fba_dev, "invalid group buffer size %zu",
			    (size_t)mon_defs_req->max_groups_size);
		err = -EINVAL;
		goto out_unlock;
	}

	mon_defs_req->items_bytes_written = 0;
	mon_defs_req->groups_bytes_written = 0;
	// Get items and groups from all mon sources
	for (src = 0; src < SAIL_MTS_MAX; src++) {
		if (mon->src_info[src].is_init) {
			size_t items_size = mon->src_info[src].item_cnt *
					    sizeof(struct mon_item);
			size_t groups_size = mon->src_info[src].group_cnt *
					     sizeof(struct mon_group);

			if (items_size) {
				if (copy_to_user(
					    mon_defs_req->items +
						    mon_defs_req
							    ->items_bytes_written,
					    mon->src_info[src].items,
					    items_size)) {
					err = -EFAULT;
					goto out_unlock;
				}
				mon_defs_req->items_bytes_written += items_size;
			}

			if (groups_size) {
				if (copy_to_user(
					    mon_defs_req->groups +
						    mon_defs_req
							    ->groups_bytes_written,
					    mon->src_info[src].groups,
					    groups_size)) {
					err = -EFAULT;
					goto out_unlock;
				}

				mon_defs_req->groups_bytes_written +=
					groups_size;
			}
		}
	}

out_unlock:
	mutex_unlock(&fba_dev->mon.mutex);
	return err;
}

int fba_mon_get_data(struct fba_device *fba_dev, uint32_t *ids,
		     uint32_t id_count, void *data_arr, int16_t *err_arr)
{
	struct fba_mon *mon = &fba_dev->mon;
	char *dst_data_buf = (char *)data_arr;
	int i = 0;
	int src;
	uint32_t item_offset;
	size_t all_item_cnt;
	int err = 0;

	mutex_lock(&mon->mutex);
	// Read driver mon data every time user request is made.
	update_driver_data(fba_dev);

	// Update device data synchronously, if requested.
	if (mon_disable_polling) {
		err = update_device_data(fba_dev);
		if (err != 0)
			goto out_unlock;
	}

	all_item_cnt = fba_mon_get_item_cnt(mon);
	for (i = 0; i < id_count;
	     ++i, dst_data_buf += MON_VALUE_SIZE, ++err_arr) {
		struct mon_data *data = NULL;
		struct mon_item *item = NULL;

		if (ids[i] >= all_item_cnt) {
			fba_dev_err(fba_dev, "%s invalid id %u", __func__,
				    ids[i]);
			err = -EINVAL;
			goto out_unlock;
		}

		src = fba_mon_get_src_from_id(mon, ids[i]);
		if (src < 0) {
			fba_dev_err(fba_dev, "%s invalid src for id %u",
				    __func__, ids[i]);
			err = -EINVAL;
			goto out_unlock;
		}

		if (!fba_dev->mon.src_info[src].is_init) {
			err = -EFAULT;
			goto out_unlock;
		}
		item_offset = mon->src_info[src].item_offset;
		item = &mon->src_info[src].items[ids[i] - item_offset];
		data = &mon->src_info[src].data[ids[i] - item_offset];
		memcpy(dst_data_buf, data->value, item->size);
		*err_arr = data->err;
	}

out_unlock:
	mutex_unlock(&mon->mutex);
	return err;
}

int fba_mon_set_data(struct fba_device *fba_dev, uint32_t *ids,
		     uint32_t id_count, void *data_arr)
{
	struct fba_mon *mon = &fba_dev->mon;
	struct mon_data_set *data_set, *data_tmp;
	char *src_data_buf = (char *)data_arr;
	size_t all_item_cnt;
	uint32_t item_offset;
	int i, src, err;

	data_set = mon->src_info[SAIL_MTS_DRIVER].data_set;

	mutex_lock(&mon->mutex);
	all_item_cnt = fba_mon_get_item_cnt(mon);
	for (i = 0; i < id_count; ++i) {
		struct mon_item *item = NULL;

		if (ids[i] >= all_item_cnt) {
			fba_dev_err(fba_dev, "%s invalid id %u", __func__,
				    ids[i]);
			err = -EINVAL;
			goto out_unlock;
		}

		src = fba_mon_get_src_from_id(mon, ids[i]);
		if (src < 0) {
			fba_dev_err(fba_dev, "%s invalid src for id %u",
				    __func__, ids[i]);
			err = -EINVAL;
			goto out_unlock;
		}

		if (src == SAIL_MTS_DRIVER) { // TODO(T112986964)
			err = -EINVAL;
			goto out_unlock;
		}

		item_offset = mon->src_info[src].item_offset;
		item = &mon->src_info[src].items[i];

		data_tmp = &data_set[i];
		memcpy(data_tmp->value, src_data_buf, item->size);
		data_tmp->index = ids[i] - item_offset;

		src_data_buf += MON_VALUE_SIZE;
	}

	err = fba_admin_dma_to_device(fba_dev, ADMIN_MON_WRITE_DATA, data_set,
				      id_count * sizeof(struct mon_data_set));

out_unlock:
	mutex_unlock(&mon->mutex);
	return err;
}
