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
	int rc;

	/* Assume APP FW is booted if PCIe Boot is not enabled */
	if (!fba_dev->pcie_boot_en)
		return 0;

	rc = fba_wait_for_state(fba_dev, SAIL_RDY, SAIL_RDY_MAGIC,
				fba_dev->num_retries);
	if (rc < 0)
		fba_dev_err(fba_dev, "CCP boot failed");

	return rc;
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
	int rc;

	rc = tenjin_boot1(fba_dev);
	if (rc != 0)
		return rc;
	rc = tenjin_load_ccp(fba_dev);
	if (rc != 0)
		return rc;
	return tenjin_boot_ccp(fba_dev);
}

static const struct fba_impl_ops tenjin_impl_ops = {
	.is_secure_boot = tenjin_is_secure_boot,
	.get_fw_filenames = tenjin_get_fw_filenames,
	.load_firmware = tenjin_load_firmware,
};

const struct fba_cfg tenjin_cfg = {
	.name = DRV_NAME,
	.devnode_ida = &tenjin_ida,
	.host_ipc_bar = HOST_IPC_BAR,
	.host_ipc_stride = HOST_IPC_STRIDE,
	.host_ipc_offset = HOST_IPC_BAR_OFF,
	.db_mbox_base = DB_MAILBOX_BASE,
	.scratch_page_cnt = SAIL_SCRATCH_PAGE_CNT,
	.host_sq_wc_bar = -1,
	.impl_ops = &tenjin_impl_ops,
};
