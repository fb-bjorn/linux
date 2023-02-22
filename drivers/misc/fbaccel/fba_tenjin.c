// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
//
// Tenjin Driver
//
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/version.h>

#include "fba_device.h"
#include "fba_tenjin_priv.h"

static int retry_count = -1;
// FW boot args string
static char *tenjin_boot_args = "";

module_param(retry_count, int, 0664);
MODULE_PARM_DESC(retry_count, "Retry count for device init");

module_param(tenjin_boot_args, charp, 0664);
MODULE_PARM_DESC(tenjin_boot_args, "Boot arguments provided to firmware");

#define DRV_NAME "tenjin"

/* Used to give each class of device it's own id space */
static DEFINE_IDA(tenjin_ida);

#define BAR_0 0
#define BAR_1 2
#define BAR_2 4

#define COREDUMP_BAR 2
// M7 Coredump region is at 0x20013000(DTCM), Mapped on BAR2 at offset 0x13000
#define COREDUMP_M7_BAR_OFFSET 0x13000
// RISCV coredump region follows after M7 coredump region
#define COREDUMP_CCP_BAR_OFFSET (COREDUMP_M7_BAR_OFFSET + COREDUMP_M7_SIZE)

#define RISCV_BOOT_ARGS_BAR 2
#define RISCV_BOOT_ARGS_BAR_OFFSET (COREDUMP_CCP_BAR_OFFSET + COREDUMP_CCP_SIZE)

#define TENJIN_NUM_OF_CPU 2
/* Offset in BAR0 - Mapped to CRMU_IHOST_SW_PERSISTENT_REG1(0x40070C78)*/
#define BAR_BOOTSRC_SELECT 0xC78
/* BOOTSRC definitions */
#define BOOTSRC_SOFT_ENABLE BIT(14)
#define CODEPUSH_FASTBOOT BIT(0)
/* Start of ITCM */
#define CODEPUSH_BOOT1_ENTRY 0x00400000
/* 64M mapped to BAR2 */
#define CODEPUSH_BOOT2_ENTRY 0x60000000
/* mapped to CRMU_IHOST_SW_NS_PERSISTENT_REG0(0x40070400)*/
#define BAR_CODEPUSH_SBL 0x400
/* Fastboot request for Secure Boot Image (SBI) */
/* mapped to CRMU_IHOST_SW_NS_PERSISTENT_REG2(0x40070408)*/
#define BAR_CODEPUSH_SBI 0x408
// mapped offset to reset reason register (NS_PERSISTENT_REG7)
#define FBV_RESET_REASON_REG 0x41c
#define FBV_RESET_REASON_MASK GENMASK(3, 0)
// mapped to CRMU_NS_PERSISTENT_REG8 (0x40070420)
#define BAR_QSPI_STATUS 0x420

#define FB_BOOT1_STARTED 0x6
#define BOOT1_STATUS_REG 0x404
#define SRAM_OPEN BIT(16)
#define DDR_OPEN BIT(17)
#define FLASHER_BOOTED BIT(21)
#define FB_BOOT1_STATUS (DDR_OPEN | FLASHER_BOOTED)
#define FLASHER_COMPLETED (BIT(13) | BIT(14))
// Each retry attempt sleep for 100ms(BOOT_STATUS_SLEEP)
#define LOAD_IMAGE_RETRY_COUNT 20
// Each retry attempt sleep for 100ms(BOOT_STATUS_SLEEP)
#define BOOT1_STARTUP_RETRY_COUNT 50
#define BOOT_STATUS_SLEEP (HZ / 10)
#define TENJIN_M7_RESET_TIME_MS 10
#define BAR1_CODEPUSH_BASE_BOOT1 0x100000

#define TENJIN_BAR1_DMA_BUF_OFF_HI 0x61e0
#define TENJIN_BAR1_DMA_BUF_OFF_LO (TENJIN_BAR1_DMA_BUF_OFF_HI + 4)
#define TENJIN_BAR1_DMA_BUF_SZ (TENJIN_BAR1_DMA_BUF_OFF_HI + 8)

#define TENJIN_BAR1_SOTP_DAUTH_VALID_OFFSET 0x6248
#define SOTP_DAUTH_MAX 4

#define REL_MAGIC_TAG 0x68617368
#define REL_MAGIC_TAG_LEN 4

#define TENJIN_BAR1_GET_SOTP_DAUTH_MAGIC_ADDR(index)                           \
	(TENJIN_BAR1_SOTP_DAUTH_VALID_OFFSET + (index)*0x50)
#define TENJIN_BAR1_GET_SOTP_DAUTH_VALID_ADDR(index)                           \
	(TENJIN_BAR1_SOTP_DAUTH_VALID_OFFSET + REL_MAGIC_TAG_LEN + (index)*0x50)

#define SOTP_DAUTH_VALID_MASK GENMASK(7, 0)
#define SOTP_DAUTH_VALID_VALUE '1'

#define FW_LOADER_ACK_RCVD_ALL_DATA BIT(20)
#define BOOT1_STATUS_THERMAL_TRAP_MODE_MASK BIT(0)

// Enum defining reset reason values reported by FW
enum fbv_reset_reason {
	POWER_CYCLE = 0,
	M7_MAILBOX_RESET,
	M7_WATCH_DOG_RESET,
	OVER_TEMP_RESET,
	PCIE_FLR_RESET,
	PCIE_HOT_RESET,
	PCIE_WARM_RESET,
	PCIE_COLD_RESET,
	L1_SYSTEM_RESET,
	L0_SYSTEM_RESET,
	RESERVED_RESET_0,
	RESERVED_RESET_1,
	RESERVED_RESET_2,
	RISCV_WATCH_DOG_RESET,
	THERMAL_TRAP_RESET,
	UNKNOWN_RESET
};

static const char *const fbv_reset_reason_str[] = {
	[POWER_CYCLE] = "POWER_CYCLE",
	[M7_MAILBOX_RESET] = "M7_MAILBOX_RESET",
	[M7_WATCH_DOG_RESET] = "M7_WATCH_DOG_RESET",
	[OVER_TEMP_RESET] = "OVER_TEMP_RESET",
	[PCIE_FLR_RESET] = "PCIE_FLR_RESET",
	[PCIE_HOT_RESET] = "PCIE_HOT_RESET",
	[PCIE_WARM_RESET] = "PCIE_WARM_RESET",
	[PCIE_COLD_RESET] = "PCIE_COLD_RESET",
	[L1_SYSTEM_RESET] = "L1_SYSTEM_RESET",
	[L0_SYSTEM_RESET] = "L0_SYSTEM_RESET",
	[RESERVED_RESET_0] = "RESERVED_RESET_0",
	[RESERVED_RESET_1] = "RESERVED_RESET_1",
	[RESERVED_RESET_2] = "RESERVED_RESET_2",
	[RISCV_WATCH_DOG_RESET] = "RISCV_WATCH_DOG_RESET",
	[THERMAL_TRAP_RESET] = "THERMAL_TRAP_RESET",
	[UNKNOWN_RESET] = "UNKNOWN_RESET"
};

#define RESET_REASON_LOG_MASK                                                  \
	((1 << M7_WATCH_DOG_RESET) | (1 << OVER_TEMP_RESET) |                  \
	 (1 << THERMAL_TRAP_RESET) | (1 << L1_SYSTEM_RESET) |                  \
	 (1 << L0_SYSTEM_RESET) | (1 << RISCV_WATCH_DOG_RESET) |               \
	 (1 << UNKNOWN_RESET))

// Coredump status
enum fba_coredump_status {
	FBA_COREDUMP_STATUS_NONE = 0,
	// Driver is polling coredump area
	FBA_COREDUMP_STATUS_POLLING,
	// Driver detected coredump trigger from device
	FBA_COREDUMP_STATUS_STARTED,
	// Driver detected coredump completion by device
	FBA_COREDUMP_STATUS_COMPLETED,
	// Driver timed out waiting for coredump completion
	FBA_COREDUMP_STATUS_TIMEDOUT,
	// Coredump event is notified
	FBA_COREDUMP_STATUS_NOTIFIED,
};

#define COREDUMP_POLLING_TIME_HZ (HZ)
#define COREDUMP_TIME_OUT_TIME_HZ (2 * HZ)

#define MAX_REGION_COUNT 2
#define DEVICE_REGION 0
#define HOST_REGION 1

struct coredump_region {
	int bar;
	uint32_t offset;
	uint32_t size;
};

static const struct coredump_region tenjin_coredump_regions[] = {
	{
		.bar = COREDUMP_BAR,
		.offset = COREDUMP_CCP_BAR_OFFSET,
		.size = COREDUMP_CCP_SIZE,
	},
	{
		.bar = COREDUMP_BAR,
		.offset = COREDUMP_M7_BAR_OFFSET,
		.size = COREDUMP_M7_SIZE,
	},
};
#define N_COREDUMP_CPUS ARRAY_SIZE(tenjin_coredump_regions)

#define MAX_COREDUMP_SIZE (COREDUMP_M7_SIZE + COREDUMP_CCP_SIZE)

struct fbv_coredump {
	// To check if coredump init is called
	uint32_t initialized;
	bool coredump_disabled;

	struct delayed_work coredump_work;
	struct completion coredump_ready;
	// CPU status
	enum fba_coredump_status cpu_status[N_COREDUMP_CPUS];

	struct fba_shared regions[MAX_REGION_COUNT][N_COREDUMP_CPUS];
};

struct tenjin_priv {
	struct fba_device *fba_dev;

	struct fbv_coredump coredump;

	bool ccp_panic;
};

static int get_firmware(const struct firmware **firmware_p, const char *name,
			struct device *device, size_t size)
{
	int ret;

	ret = request_firmware(firmware_p, name, device);
	if (ret)
		return ret;

	if ((*firmware_p)->size > size) {
		release_firmware(*firmware_p);
		ret = -EFBIG;
	}

	return ret;
}

static void tenjin_buf_notify(struct fba_device *fba_dev,
			      dma_addr_t host_buf_addr, uint32_t buf_size)
{
	/* update the dma address to the card */
	fba_write_bar(fba_dev, upper_32_bits(host_buf_addr), BAR_1,
		      TENJIN_BAR1_DMA_BUF_OFF_HI);
	fba_write_bar(fba_dev, lower_32_bits(host_buf_addr), BAR_1,
		      TENJIN_BAR1_DMA_BUF_OFF_LO);
	fba_write_bar(fba_dev, buf_size, BAR_1, TENJIN_BAR1_DMA_BUF_SZ);
}

static inline int tenjin_wait_for_status(struct fba_device *fba_dev,
					 uint32_t bar, uint64_t offset,
					 uint32_t mask, uint32_t value,
					 int num_retries)
{
	uint32_t rd_val;

	if (num_retries < 1)
		num_retries = 1;

	do {
		/* Bail early if we're unbinding. */
		if (READ_ONCE(fba_dev->exiting))
			return -ETIMEDOUT;

		rd_val = fba_read_bar(fba_dev, bar, offset);

		if ((rd_val & mask) == value)
			return 0;

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(BOOT_STATUS_SLEEP);
	} while (num_retries--);

	return -ETIMEDOUT;
}

// This function reads the register over BAR and Checks if the bit is set
// from the mask provided in the bit_mask.
static inline int tenjin_wait_for_bit_set(struct fba_device *fba_dev,
					  uint32_t bar, uint64_t offset,
					  uint32_t bit_mask, uint32_t *value,
					  int num_retries)
{
	if (num_retries < 1)
		num_retries = 1;

	*value = 0;

	do {
		*value = fba_read_bar(fba_dev, bar, offset);

		if (*value & bit_mask)
			return 0;

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(BOOT_STATUS_SLEEP);
	} while (num_retries--);

	return -ETIMEDOUT;
}

static bool did_ccp_panic(struct fba_device *fba_dev)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;

	if (priv)
		return priv->ccp_panic;
	else
		return false;
}

static void tenjin_coredump_handler(struct fba_device *fba_dev, int cpu_id)
{
	int mailbox_id =
		cpu_id == 0 ? CCP_COREDUMP_MBOX_ID : M7_COREDUMP_MBOX_ID;
	uint32_t alert_type = get_mailbox(fba_dev, mailbox_id);
	struct tenjin_priv *priv = fba_dev->impl_priv;

	switch (alert_type) {
	case COREDUMP_REASON_FATAL_DDR_ECC_ERR:
		fba_dev->ddr_uce_detected = true;
		fba_dev_err(
			fba_dev,
			"Coredump triggered: Uncorrectable ECC error detected on DDR.");
		break;
	case COREDUMP_REASON_FATAL_SRAM_ECC_ERR:
		fba_dev->internal_mem_uce_detected = true;
		fba_dev_err(
			fba_dev,
			"Coredump triggered: Uncorrectable ECC error detected on SRAM.");
		break;
	case COREDUMP_REASON_FATAL_RISCV_WDT_ERR:
		fba_dev->ccp_wdt_failed = true;
		fba_dev_err(fba_dev,
			    "Coredump triggered: RISCV WDT timeout detected.");
		break;
	default:
		fba_dev->device_fatal_err_detected = true;
		if (cpu_id == 0)
			priv->ccp_panic = true;

		fba_dev_err(fba_dev, "Coredump triggered: Unkown reason.");
		break;
	}

	// Generate udev event KOBJ_CHANGE
	mutex_lock(&fba_dev->coredump_mutex);
	if (!fba_dev->coredump_disabled)
		kobject_uevent(&fba_dev->dev->kobj, KOBJ_CHANGE);
	mutex_unlock(&fba_dev->coredump_mutex);
}

static void tenjin_log_reset_reason(struct fba_device *fba_dev)
{
	uint32_t value;

	// Read and log reset reason
	value = fba_read_bar(fba_dev, BAR_0, FBV_RESET_REASON_REG);
	value = FIELD_GET(FBV_RESET_REASON_MASK, value);
	if (RESET_REASON_LOG_MASK & (1 << value)) {
		fba_dev_info(fba_dev, "Reset reason %s\n",
			     fbv_reset_reason_str[value]);
	}
}

static int coredump_get_hdr(struct fba_device *fba_dev, int region_id,
			    int cpu_id, struct coredump_hdr *hdr)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;
	struct fbv_coredump *coredump = &priv->coredump;
	struct fba_shared *region = &coredump->regions[region_id][cpu_id];

	if (!region->data)
		return -EINVAL;

	if (region->size < sizeof(*hdr))
		return -EFAULT;

	fba_shared_memcpy_from(region, 0, hdr, sizeof(*hdr));

	return 0;
}

static void coredump_check_cpu(struct fba_device *fba_dev, int region_id,
			       int cpu_id, enum fba_coredump_status *status)
{
	struct coredump_hdr hdr;
	int ret;

	ret = coredump_get_hdr(fba_dev, region_id, cpu_id, &hdr);
	if (ret != 0) {
		WARN_ON_ONCE(ret != 0);
		return;
	}

	switch (*status) {
	case FBA_COREDUMP_STATUS_NONE:
		// Check if CPU has initialized the coredump region
		if (hdr.cookie == COREDUMP_COOKIE)
			*status = FBA_COREDUMP_STATUS_POLLING;
		break;
	case FBA_COREDUMP_STATUS_POLLING:
		// Check if coredump is triggered by CPU
		if (hdr.started == 1) {
			*status = FBA_COREDUMP_STATUS_STARTED;

			/* Also signal that device has failed */
			fba_device_abort(fba_dev, "coredump triggered");
		}
		break;
	case FBA_COREDUMP_STATUS_STARTED:
		// Check if coredump is completed. If not then signal timeout
		if (hdr.completed == 1)
			*status = FBA_COREDUMP_STATUS_COMPLETED;
		else
			*status = FBA_COREDUMP_STATUS_TIMEDOUT;
		break;
	default:
		break;
	}
}

static void coredump_notify(struct fba_device *fba_dev)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;
	struct fbv_coredump *coredump = &priv->coredump;

	complete_all(&coredump->coredump_ready);
}

static void coredump_work_handler(struct work_struct *work)
{
	int cpu_id;
	enum fba_coredump_status *cpu_status;
	struct delayed_work *d_work =
		container_of(work, struct delayed_work, work);
	struct fbv_coredump *coredump =
		container_of(d_work, struct fbv_coredump, coredump_work);
	struct tenjin_priv *priv =
		container_of(coredump, struct tenjin_priv, coredump);
	struct fba_device *fba_dev = priv->fba_dev;

	// Check for coredump triggers from all CPUs.
	// If any one of them has started the coredump, set a timeout and come back again.
	// If coredump is completed or timeout reached then notify the users about the dump.
	// Check CPU core dump status
	for (cpu_id = 0; cpu_id < N_COREDUMP_CPUS; cpu_id++) {
		cpu_status = &coredump->cpu_status[cpu_id];

		coredump_check_cpu(fba_dev, 0, cpu_id, cpu_status);

		if (*cpu_status == FBA_COREDUMP_STATUS_NOTIFIED) {
			// Skip coredump polling for this CPU
			continue;
		}

		if (*cpu_status == FBA_COREDUMP_STATUS_STARTED) {
			fba_dev_err(fba_dev,
				    "Coredump trigger detected for CPU id %d",
				    cpu_id);
			// Wait for the coredump to complete
			schedule_delayed_work(&coredump->coredump_work,
					      COREDUMP_TIME_OUT_TIME_HZ);
			return;
		} else if (*cpu_status >= FBA_COREDUMP_STATUS_COMPLETED) {
			fba_dev_err(fba_dev, "Coredump completed by CPU id %d",
				    cpu_id);
			tenjin_coredump_handler(fba_dev, cpu_id);
			coredump_notify(fba_dev);
			*cpu_status = FBA_COREDUMP_STATUS_NOTIFIED;
		}
	}

	schedule_delayed_work(&coredump->coredump_work,
			      COREDUMP_POLLING_TIME_HZ);
}

// Get size of coredump region for a CPU
static int coredump_get_size(struct fba_device *fba_dev, int region_id,
			     int cpu_id)
{
	struct coredump_hdr cpu_hdr;
	int ret;

	ret = coredump_get_hdr(fba_dev, region_id, cpu_id, &cpu_hdr);
	if (ret != 0) {
		// Region is not initialized
		return 0;
	} else if (cpu_hdr.cookie != COREDUMP_COOKIE) {
		fba_dev_dbg(
			fba_dev,
			"Coredump region %d for cpu %d not initalized by device",
			region_id, cpu_id);
		return 0;
	}
	return cpu_hdr.size;
}

void fbv_coredump_start(struct fba_device *fba_dev)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;
	struct fbv_coredump *coredump = &priv->coredump;

	schedule_delayed_work(&coredump->coredump_work,
			      COREDUMP_POLLING_TIME_HZ);
}

int fbv_coredump_host_region_init(struct fba_device *fba_dev,
				  struct fba_shared *host_mem, int cpu_id)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;
	struct fbv_coredump *coredump = &priv->coredump;
	struct fba_shared *region;

	if (cpu_id >= N_COREDUMP_CPUS)
		return -EINVAL;

	region = &coredump->regions[HOST_REGION][cpu_id];
	*region = *host_mem;

	return 0;
}

int fbv_coredump_init(struct fba_device *fba_dev)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;
	struct fbv_coredump *coredump = &priv->coredump;
	struct fba_shared *region;
	int cpu_id;

	// Called from probe()
	// Only initialize the device allocated region here.
	for (cpu_id = 0; cpu_id < N_COREDUMP_CPUS; cpu_id++) {
		region = &coredump->regions[HOST_REGION][cpu_id];
		memset(region, 0, sizeof(struct fba_shared));

		region = &coredump->regions[DEVICE_REGION][cpu_id];
		fba_shared_alloc_iomem(fba_dev, region,
				       tenjin_coredump_regions[cpu_id].bar,
				       tenjin_coredump_regions[cpu_id].offset,
				       tenjin_coredump_regions[cpu_id].size);
	}

	INIT_DELAYED_WORK(&coredump->coredump_work, coredump_work_handler);
	init_completion(&coredump->coredump_ready);
	coredump->initialized = true;

	fbv_coredump_start(fba_dev);

	return 0;
}

void fbv_coredump_exit(struct fba_device *fba_dev)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;
	struct fbv_coredump *coredump = &priv->coredump;
	struct fba_shared *region;
	int cpu_id;

	// Called from remove()
	if (coredump->initialized) {
		cancel_delayed_work_sync(&coredump->coredump_work);
		coredump_notify(fba_dev);

		for (cpu_id = 0; cpu_id < N_COREDUMP_CPUS; cpu_id++) {
			region = &coredump->regions[DEVICE_REGION][cpu_id];
			fba_shared_free(fba_dev, region);
		}

		coredump->initialized = false;
	}
}

int fbv_coredump_get_status(struct fba_device *fba_dev,
			    struct coredump_status_ioctl *coredump_status)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;
	struct fbv_coredump *coredump = &priv->coredump;
	int region;
	int cpu_id;
	bool started = false;
	int ret = 0;

	coredump_status->triggered = 0;
	coredump_status->coredump_size = 0;

	for (cpu_id = 0; cpu_id < N_COREDUMP_CPUS; cpu_id++) {
		if (coredump->cpu_status[cpu_id] != FBA_COREDUMP_STATUS_NONE)
			started = true;
	}

	// Coredump polling is not started
	if (!started) {
		fba_dev_err(
			fba_dev,
			"Error in getting coredump status, Coredump polling not yet started");
		if (coredump_status->wait < 0) {
			/* if less than 0, don't wait. */
			return -EINVAL;
		}
	}

	if (coredump_status->wait >= 0) {
		ret = wait_for_completion_killable(&coredump->coredump_ready);
		if (ret)
			return ret;
		coredump_status->triggered = 1;
	} else {
		for (cpu_id = 0; cpu_id < N_COREDUMP_CPUS; cpu_id++) {
			if ((coredump->cpu_status[cpu_id] >=
			     FBA_COREDUMP_STATUS_COMPLETED)) {
				coredump_status->triggered = 1;
			}
		}
	}

	for (region = 0; region < MAX_REGION_COUNT; region++) {
		for (cpu_id = 0; cpu_id < N_COREDUMP_CPUS; cpu_id++) {
			coredump_status->coredump_size +=
				coredump_get_size(fba_dev, region, cpu_id);
		}
	}

	return ret;
}

static int coredump_get_data(struct fba_device *fba_dev, int region_id,
			     int cpu_id, void *dst, uint32_t size)
{
	struct tenjin_priv *priv = fba_dev->impl_priv;
	struct fbv_coredump *coredump = &priv->coredump;
	struct fba_shared *region;

	region = &coredump->regions[region_id][cpu_id];
	if (!region->data)
		return -ENXIO;

	if (region->size < size)
		return -EINVAL;

	fba_shared_memcpy_from(region, 0, dst, size);

	return 0;
}

int fbv_coredump_get_data(struct fba_device *fba_dev, void *coredump_mem,
			  uint32_t size)
{
	int offset = 0;
	uint32_t cpu_data_size;
	int region_id;
	int cpu_id;
	int ret;

	for (cpu_id = N_COREDUMP_CPUS - 1; cpu_id >= 0; cpu_id--) {
		for (region_id = 0; region_id < MAX_REGION_COUNT; region_id++) {
			cpu_data_size =
				coredump_get_size(fba_dev, region_id, cpu_id);
			if (cpu_data_size + offset > size) {
				// Partial reading of coredump is not supported
				fba_dev_err(
					fba_dev,
					"Error in reading coredump data, not enough memory to read full buffer");
				return -EINVAL;
			}
			if (cpu_data_size != 0) {
				ret = coredump_get_data(fba_dev, region_id,
							cpu_id,
							coredump_mem + offset,
							cpu_data_size);
				if (ret != 0) {
					fba_dev_err(
						fba_dev,
						"Error in reading data for region %d, cpu %d",
						region_id, cpu_id);
					return ret;
				}
				offset += cpu_data_size;
			}
		}
	}

	return offset;
}

static int
fba_ioctl_get_coredump_data(struct fba_device *fba_dev,
			    struct get_coredump_ioctl *coredump_data_req)
{
	void *coredump_mem;
	void __user *user = coredump_data_req->data;
	int ret;

	if (coredump_data_req->size > MAX_COREDUMP_SIZE)
		return -EINVAL;

	coredump_mem = kzalloc(coredump_data_req->size, GFP_KERNEL);
	if (!coredump_mem)
		return -ENOMEM;

	ret = fbv_coredump_get_data(fba_dev, coredump_mem,
				    coredump_data_req->size);
	if (ret < 0)
		goto out_free_coredump_mem;

	if (copy_to_user(user, coredump_mem, ret)) {
		ret = -EFAULT;
		goto out_free_coredump_mem;
	}
	coredump_data_req->size = ret;
	ret = 0;

out_free_coredump_mem:
	kfree(coredump_mem);
	return ret;
}

static long tenjin_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct fba_ctx *ctx = fp->private_data;
	struct fba_device *fba_dev = fba_ctx_to_fba_device(ctx);
	void __user *data = (void __user *)arg;
	long ret;

	switch (cmd) {
	case FBA_IOCTL_GET_COREDUMP_STATUS: {
		struct coredump_status_ioctl coredump_status_req;

		if (copy_from_user(&coredump_status_req, data,
				   sizeof(struct coredump_status_ioctl))) {
			return -EFAULT;
		}
		ret = fbv_coredump_get_status(fba_dev, &coredump_status_req);
		if (ret != 0)
			return ret;
		if (copy_to_user(data, &coredump_status_req,
				 sizeof(struct coredump_status_ioctl)))
			return -EFAULT;
		return 0;
	}
	case FBA_IOCTL_GET_COREDUMP_DATA: {
		struct get_coredump_ioctl coredump_data_req;

		if (copy_from_user(&coredump_data_req, data,
				   sizeof(struct get_coredump_ioctl))) {
			return -EFAULT;
		}
		ret = fba_ioctl_get_coredump_data(fba_dev, &coredump_data_req);
		if (ret != 0)
			return ret;

		if (copy_to_user(data, &coredump_data_req,
				 sizeof(struct get_coredump_ioctl)))
			ret = -EFAULT;
		return ret;
	}
	default:
		return -EINVAL;
	}

	return 0;
}

static int tenjin_boot1(struct fba_device *fba_dev)
{
	struct device *dev = &fba_dev->pdev->dev;
	const struct firmware *fw;
	void __iomem *bufp;
	size_t max_buf;
	int ret;
	uint64_t offset_codepush;
	uint32_t codepush;
	uint32_t value;

	/*Assume APP FW is loaded if PCIe Boot is not enabled*/
	if (!fba_dev->pcie_boot_en)
		return 0;

	tenjin_log_reset_reason(fba_dev);

	// Trigger M7 reset to start from a clean state
	fba_write_bar(fba_dev, M7_RESET_MAILBOX_VAL, M7_RESET_BAR,
		      M7_RESET_OFFSET);
	// Allow some time for M7 to reboot
	msleep(TENJIN_M7_RESET_TIME_MS);

	/*
	 * After POR, enable soft BOOTSRC so bootrom do not clear
	 * the pushed image (the TCM memories).
	 */
	value = fba_read_bar(fba_dev, BAR_0, BAR_BOOTSRC_SELECT);
	value |= BOOTSRC_SOFT_ENABLE;
	fba_write_bar(fba_dev, value, BAR_0, BAR_BOOTSRC_SELECT);

	codepush = CODEPUSH_FASTBOOT + CODEPUSH_BOOT1_ENTRY;
	offset_codepush = BAR_CODEPUSH_SBL;

	/* Write a 1 to request SRAM open bit */
	fba_write_bar(fba_dev, CODEPUSH_FASTBOOT, BAR_0, offset_codepush);

	/* Wait for device to respond */
	ret = tenjin_wait_for_status(fba_dev, BAR_0, BOOT1_STATUS_REG,
				     SRAM_OPEN, SRAM_OPEN,
				     LOAD_IMAGE_RETRY_COUNT);
	if (ret < 0) {
		fba_dev_err(fba_dev,
			    "Timeout waiting for ROM to open SRAM access");
		return ret;
	}

	bufp = fba_dev->bars[BAR_1] + BAR1_CODEPUSH_BASE_BOOT1;
	max_buf = SZ_256K;

	ret = get_firmware(&fw, fba_dev->boot1_filename, dev, max_buf);
	if (ret) {
		fba_dev_err(fba_dev,
			    "Error %d requesting BOOT1 firmware file: %s", ret,
			    fba_dev->boot1_filename);
		return ret;
	}
	memcpy_toio(bufp, fw->data, fw->size);
	fba_dev_dbg(fba_dev, "size=0x%zx", fw->size);

	fba_write_bar(fba_dev, codepush, BAR_0, offset_codepush);

	/* wait until done */
	ret = tenjin_wait_for_status(fba_dev, BAR_0, BOOT1_STATUS_REG,
				     FB_BOOT1_STARTED, FB_BOOT1_STARTED,
				     BOOT1_STARTUP_RETRY_COUNT);
	if (ret) {
		fba_dev_err(fba_dev, "Timeout waiting for boot1 to start");
		goto out_release_fw;
	}
	// At this stage boot1 is booted.
	// Check Boot1 status
	// In normal boot1 -  DDR_OPEN bit is set indicating boot1 is done and ready to receive boot2 image
	// In flasher boot1 -  FLASHER_BOOTED bt is set indicating flashing is started
	ret = tenjin_wait_for_bit_set(fba_dev, BAR_0, BOOT1_STATUS_REG,
				      FB_BOOT1_STATUS, &value,
				      BOOT1_STARTUP_RETRY_COUNT);
	if (ret) {
		fba_dev_err(
			fba_dev,
			"Error:Boot1 DDR init failed / Failed to start Flash update");
		goto out_release_fw;
	}

	if (value & FLASHER_BOOTED) {
		// Wait for flasher to complete the QSPI write
		ret = tenjin_wait_for_status(fba_dev, BAR_0, BAR_QSPI_STATUS,
					     FLASHER_COMPLETED,
					     FLASHER_COMPLETED,
					     BOOT1_STARTUP_RETRY_COUNT);
		if (ret) {
			fba_dev_err(
				fba_dev,
				"Error:Boot1 Flasher failed to complete flashing");
			goto out_release_fw;
		}
		fba_dev_info(fba_dev, "Boot1 flasher completed successfully");
	}

	atomic_set(&fba_dev->reset_ok, true);

out_release_fw:
	release_firmware(fw);
	if (ret) {
		fba_dev_err(fba_dev,
			    "Boot1 Failed to complete execution, status=0x%x",
			    value);
		value = fba_read_bar(fba_dev, BAR_0, BOOT1_STATUS_REG);
		if (FIELD_GET(BOOT1_STATUS_THERMAL_TRAP_MODE_MASK, value) !=
		    0) {
			fba_dev_err(
				fba_dev,
				"Device in thermal trap mode, requires power cycle to proceed");
		}
	}
	return ret;
}

static int tenjin_load_ccp(struct fba_device *fba_dev)
{
	struct device *dev = &fba_dev->pdev->dev;
	const struct firmware *fw;
	void *bufp;
	size_t max_buf;
	int ret, n_alloc_tries, alloc_retry_period_ms;
	uint64_t offset_codepush;
	uint32_t codepush;
	dma_addr_t boot2_dma_addr;
	void __iomem *args_bufp;
	size_t boot_arg_len;

	/* Assume APP FW is loaded if PCIe Boot is not enabled */
	if (!fba_dev->pcie_boot_en)
		return 0;

	// Copy boot args
	args_bufp =
		fba_dev->bars[RISCV_BOOT_ARGS_BAR] + RISCV_BOOT_ARGS_BAR_OFFSET;
	// Reset boot arg string in device
	memset_io(args_bufp, 0, RISCV_BOOT_ARGS_REGION_SIZE);
	boot_arg_len = strlen(tenjin_boot_args);
	WARN_ON(boot_arg_len >= RISCV_BOOT_ARGS_REGION_SIZE);
	memcpy_toio(args_bufp, tenjin_boot_args, boot_arg_len);
	if (boot_arg_len > 0) {
		fba_dev_info(fba_dev, "Configured RISCV boot args:%s",
			     tenjin_boot_args);
	}

	codepush = CODEPUSH_BOOT2_ENTRY;
	offset_codepush = BAR_CODEPUSH_SBI;

	/* Wait for device to respond */
	ret = tenjin_wait_for_status(fba_dev, BAR_0, BOOT1_STATUS_REG, DDR_OPEN,
				     DDR_OPEN, LOAD_IMAGE_RETRY_COUNT);
	if (ret < 0) {
		fba_dev_err(fba_dev, "boot2 timeout");
		return ret;
	}

	n_alloc_tries = 8;
	alloc_retry_period_ms = 4;
	max_buf = SZ_4M;
	while (n_alloc_tries--) {
		bufp = dma_alloc_coherent(dev, max_buf, &boot2_dma_addr,
					  GFP_KERNEL);
		if (bufp || (n_alloc_tries == 0))
			break;

		fba_dev_warn(
			fba_dev,
			"failed to allocate fw boot buffer. tries remaining = %d, retrying in %d ms",
			n_alloc_tries, alloc_retry_period_ms);
		msleep(alloc_retry_period_ms);
		/* Exponential backoff */
		alloc_retry_period_ms *= 2;
	}
	if (!bufp) {
		fba_dev_err(fba_dev,
			    "failed to allocate fw boot buffer of size 0x%zx",
			    max_buf);
		ret = -ENOMEM;
		goto out_free_buf;
	}

	tenjin_buf_notify(fba_dev, boot2_dma_addr, max_buf);
	/*TODO:Add support for loading FW > 4M */
	ret = get_firmware(&fw, fba_dev->boot2_filename, dev, max_buf);
	if (ret) {
		fba_dev_err(fba_dev,
			    "Error %d requesting CCP firmware file: %s", ret,
			    fba_dev->boot2_filename);
		goto out_free_buf;
	}
	memcpy(bufp, fw->data, fw->size);
	fba_dev_dbg(fba_dev, "size=0x%zx", fw->size);

	// Clear the mailbox before we start loading/booting ccp
	// WARNING: Be careful in moving below code. The mailbox registers
	// mapped over PCIe IMAP need to be opened for Non Secure access
	// before Host can set it to zero.
	// Boot1 does this before indicating the DDR_OPEN. So this place
	// is safe to set mailbox to zero.
	set_mailbox(fba_dev, SAIL_RDY, 0);
	fba_write_bar(fba_dev, codepush, BAR_0, offset_codepush);

	/*
	 * Check for ack from card. when Ack is received,
	 * it means all the data is received by card.
	 * Exit the loop after ack is received.
	 */
	ret = tenjin_wait_for_status(fba_dev, BAR_0, BOOT1_STATUS_REG,
				     FW_LOADER_ACK_RCVD_ALL_DATA,
				     FW_LOADER_ACK_RCVD_ALL_DATA,
				     LOAD_IMAGE_RETRY_COUNT);
	if (ret == 0) {
		fba_dev_info(fba_dev, "App load done");
	} else {
		fba_dev_err(fba_dev, "Error. No reply from card");
		ret = -ETIMEDOUT;
	}

	release_firmware(fw);

out_free_buf:
	dma_free_coherent(dev, max_buf, bufp, boot2_dma_addr);

	return ret;
}

static int tenjin_boot_ccp(struct fba_device *fba_dev)
{
	int ret;

	/* Assume APP FW is booted if PCIe Boot is not enabled */
	if (!fba_dev->pcie_boot_en)
		return 0;

	ret = fba_wait_for_state(fba_dev, SAIL_RDY, SAIL_RDY_MAGIC,
				 fba_dev->num_retries);
	if (ret < 0)
		fba_dev_err(fba_dev, "CCP boot failed");

	return ret;
}

static bool tenjin_is_secure_boot(struct fba_device *fba_dev)
{
	uint32_t magic, valid;
	int i;

	/* If any one entry is valid, then we need to load signed fw */
	for (i = 0; i < SOTP_DAUTH_MAX; i++) {
		magic = fba_read_bar(fba_dev, BAR_1,
				     TENJIN_BAR1_GET_SOTP_DAUTH_MAGIC_ADDR(i));
		if (magic != REL_MAGIC_TAG)
			continue;

		valid = fba_read_bar(fba_dev, BAR_1,
				     TENJIN_BAR1_GET_SOTP_DAUTH_VALID_ADDR(i));
		if ((valid & SOTP_DAUTH_VALID_MASK) == SOTP_DAUTH_VALID_VALUE)
			return true;
	}

	return false;
}

static void tenjin_get_fw_filenames(struct fba_device *fba_dev,
				    const char **boot1, const char **boot2)
{
	if (tenjin_is_secure_boot(fba_dev)) {
		*boot1 = TENJIN_DEF_BOOT1_FILENAME_SIGNED;
		*boot2 = TENJIN_DEF_BOOT2_FILENAME_SIGNED;
	} else {
		*boot1 = TENJIN_DEF_BOOT1_FILENAME_UNSIGNED;
		*boot2 = TENJIN_DEF_BOOT2_FILENAME_UNSIGNED;
	}
}

static int tenjin_load_firmware(struct fba_device *fba_dev)
{
	int ret;

	ret = tenjin_boot1(fba_dev);
	if (ret != 0)
		return ret;
	ret = tenjin_load_ccp(fba_dev);
	if (ret != 0)
		return ret;
	return tenjin_boot_ccp(fba_dev);
}

static int tenjin_init(struct fba_device *fba_dev)
{
	struct tenjin_priv *priv;

	priv = kzalloc(sizeof(struct tenjin_priv), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	priv->fba_dev = fba_dev;

	fba_dev->impl_priv = priv;

	/* We don't need to wait for DMA, so initialize as completed. */
	fba_device_signal_coredump_done(fba_dev);

	return 0;
}

static void tenjin_exit(struct fba_device *fba_dev)
{
	kfree(fba_dev->impl_priv);
}

static const struct fba_impl_ops tenjin_impl_ops = {
	.impl_init = tenjin_init,
	.impl_exit = tenjin_exit,
	.priv_ioctl = tenjin_ioctl,

	.is_secure_boot = tenjin_is_secure_boot,
	.get_fw_filenames = tenjin_get_fw_filenames,
	.load_firmware = tenjin_load_firmware,

	.coredump_init = fbv_coredump_init,
	.coredump_exit = fbv_coredump_exit,

	.did_ccp_panic = did_ccp_panic,
};

const struct fba_cfg tenjin_cfg = {
	.name = DRV_NAME,
	.devnode_ida = &tenjin_ida,
	.host_ipc_bar = HOST_IPC_BAR,
	.host_ipc_stride = HOST_IPC_STRIDE,
	.host_ipc_offset = HOST_IPC_BAR_OFF,
	.db_mbox_base = DB_MAILBOX_BASE,
	.scratch_page_cnt = SAIL_SCRATCH_PAGE_CNT,
	.usoc_bar = TENJIN_USOC_BAR,
	.usoc_pfn_ofs = TENJIN_USOC_PFN_OFS,
	.usoc_msi_irq_ds = USOC_DS_MSIX_VECTOR,
	.usoc_msi_irq_us = USOC_US_MSIX_VECTOR,
	.host_sq_wc_bar = -1,
	.uart_mbox_base = TENJIN_UART_MBOX_OFFSET,
	.uart_msi_irq_rx = UART_RX_MSIX_VECTOR,
	.uart_msi_irq_tx = UART_TX_MSIX_VECTOR,
	.p2p_bar = -1,
	.impl_ops = &tenjin_impl_ops,
	.ctx_limits = {
		.sub_cnt = CONTEXT_SUB_CAP,
	},
};
