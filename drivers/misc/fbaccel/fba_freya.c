// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates */
//
// Freya Driver
//

#include <asm/byteorder.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/version.h>
#include <uapi/linux/limits.h>
#define FREYA_CXRAM_BASE 0x20F00000
#define FREYA_CXRAM_SIZE 0x100000
#define ARTEMIS_CXRAM_BASE 0x20C00000
#define ARTEMIS_CXRAM_SIZE 0x400000

#include "fba_freya_priv.h"
#include "fba_device.h"

#define FREYA_DRV_NAME "freya"
#define ARTEMIS_DRV_NAME "artemis"

/* Used to give each class of device it's own id space */
static DEFINE_IDA(freya_ida);

static bool gdb_on_boot;
static int llc_config = 3;
static bool pe_print_mode;
static bool ddr_bypass;
static bool pcie_console;
static bool emulation_boot_timeouts;
static unsigned int m7_hb_period_ms = M7_HEARTBEAT_PERIOD_MS;
static int pe_idcache_disable;
static int ecc_region_size_mb;
static int p2p_ecc_region_size_mb;

// Firmware boot parameters as a string.
static char *freya_boot_args = "";

module_param(gdb_on_boot, bool, 0444);
MODULE_PARM_DESC(gdb_on_boot,
		 "Enablement of gdb on Zephyr start up through boot parameter");

module_param(llc_config, int, 0644);
MODULE_PARM_DESC(llc_config,
		 "LLC firmware boot config (when firmware is loaded/reloaded)");

module_param(pe_print_mode, bool, 0444);
MODULE_PARM_DESC(pe_print_mode,
		 "PE lossless print mode enablement through boot parameter");

module_param(ddr_bypass, bool, 0444);
MODULE_PARM_DESC(ddr_bypass,
		 "Bypassing DDR initialization through boot parameter");

module_param(emulation_boot_timeouts, bool, 0444);
MODULE_PARM_DESC(emulation_boot_timeouts,
		 "Use (extra long) emulation bootflow timeouts");

module_param(m7_hb_period_ms, int, 0644);
MODULE_PARM_DESC(
	m7_hb_period_ms,
	"SAIL m7 heartbeat polling period in milliseconds (default: 60000). Set to 0 to disable.");

module_param(pe_idcache_disable, int, 0644);
MODULE_PARM_DESC(
	pe_idcache_disable,
	"Disable PE I and D cache. Enabled by default. 1 - dcache, 2 - icache, 3 - i&d cache");

module_param(ecc_region_size_mb, int, 0444);
MODULE_PARM_DESC(ecc_region_size_mb,
		 "Size of ECC region in MB (Must be multiple of 16MB)");

module_param(p2p_ecc_region_size_mb, int, 0444);
MODULE_PARM_DESC(p2p_ecc_region_size_mb,
		 "Size of P2P ECC region in MB (Must be multiple of 16MB)");

module_param(freya_boot_args, charp, 0444);
MODULE_PARM_DESC(freya_boot_args, "Firmware boot parameters.");

enum freya_boot_mode {
	FREYA_BOOT_NORMAL,
	FREYA_BOOT_MBIST,
	FREYA_BOOT_PROVISION,
};

struct fbia_priv_data {
	uint64_t cxram_sideband_ofs;
};

struct freya_priv {
	const struct fbia_priv_data *data;
	enum freya_boot_mode boot_mode;
	/* Protects against concurrent secure debug unlocks */
	struct mutex download_mtx;

	/* Used as the payload transfer location and size for Bootflow using CXRAM */
	uint32_t cxram_upgrade_ofs;
	uint32_t cxram_upgrade_sz;
};

static uint32_t freya_read_cxram_sb(struct fba_device *dev, uint32_t sb_offset)
{
	struct freya_priv *priv = dev->impl_priv;
	uint64_t offset = priv->data->cxram_sideband_ofs + sb_offset;

	return fba_read_bar(dev, BAR_CXRAM, offset);
}

static void freya_write_cxram_sb(struct fba_device *dev, uint32_t val,
				 uint32_t sb_offset)
{
	struct freya_priv *priv = dev->impl_priv;
	uint64_t offset = priv->data->cxram_sideband_ofs + sb_offset;

	fba_write_bar(dev, val, BAR_CXRAM, offset);
}

static void freya_m7_set_hb_mon_period(void *obj)
{
	struct fba_device *fba_dev = obj;

	if (!fba_dev->pcie_boot_en)
		return;
	if (m7_hb_period_ms == 0) {
		// Go back to default polling period, disable the
		// monitor from sending the heartbeat
		fba_dev->m7_health_poller->period_ms = M7_HEARTBEAT_PERIOD_MS;
		if (fba_dev->m7_health_poller->enabled)
			fba_dev_err(fba_dev,
				    "HB now disabled for M7 controller");
		fba_dev->m7_health_poller->enabled = false;
	} else {
		fba_dev->m7_health_poller->period_ms = m7_hb_period_ms;
		if (!fba_dev->m7_health_poller->enabled)
			fba_dev_info(
				fba_dev,
				"HB now enabled for M7 controller on dev with polling period of %d ms",
				m7_hb_period_ms);
		fba_dev->m7_health_poller->enabled = true;
	}
}

// The overall design document for M7 heartbeat/watchdog:
// Driver would poll on M7 health every m7_hb_period_ms
//		1- Driver writes to M7_HEARTBEAT_CTRL_ENTRY mailbox to trigger an
//			interrupt on M7
//		2- M7 Handler of that interrupts checks the health of all the M7
//			threads and writes to M7_HEARTBEAT_STATUS_ENTRY mailbox
//		3- Driver sleeps for M7_HEARTBEAT_TIMEOUT_MS to check the
//			M7_HEARTBEAT_STATUS_ENTRY mailbox
//		4- Driver sets m7_health monitoring key based on mailbox value:
//			-  0:
//					M7 was outright hang and it couldn't update the mailbox
//			- M7_HEALTHY:
//					M7 was healthy and updated the mailbox with the M7_HEALTHY
//					value
//			- Thread bit mask:
//					Some of the M7 threads were unhealthy
static bool freya_m7_hb_poll(void *obj)
{
	struct fba_device *fba_dev = obj;
	uint32_t m7_status;

	if (!fba_dev->pcie_boot_en)
		return true;
	// Reset M7_HEARTBEAT_STATUS_ENTRY mailbox
	set_mailbox(fba_dev, M7_HEARTBEAT_STATUS_ENTRY, 0);
	// Triggering heartbeat interrupt to M7
	set_mailbox(fba_dev, M7_HEARTBEAT_CTRL_ENTRY, M7_WATCHDOG);
	// Wait for M7 to signal it's health
	msleep(M7_HEARTBEAT_TIMEOUT_MS);

	// Read M7 status
	m7_status = get_mailbox(fba_dev, M7_HEARTBEAT_STATUS_ENTRY);
	if (m7_status == M7_HEALTHY) {
		if (fba_dev->m7_health != HEALTHY) {
			if (fba_dev->m7_health != UNINITIALIZED) {
				fba_dev_info(
					fba_dev,
					"%s: M7 status is healthy and previous state was %d",
					__func__, fba_dev->m7_health);
			}
			fba_dev->m7_health = HEALTHY;
		}

	} else if (m7_status == 0) {
		if (fba_dev->m7_health != UNHEALTHY) {
			fba_dev->m7_health = UNHEALTHY;
			atomic_set(&fba_dev->reset_ok, false);
			fba_dev_err(fba_dev,
				    "%s: M7 is unresponsive to interrupts",
				    __func__);
		}
	} else {
		if (fba_dev->m7_health != UNHEALTHY) {
			fba_dev->m7_health = UNHEALTHY;
			atomic_set(&fba_dev->reset_ok, false);
			fba_dev_err(fba_dev,
				    "%s: M7 timed out, thread bitmask is 0x%x",
				    __func__, m7_status);
		}
	}
	return true;
}

static char *freya_m7_health(struct fba_device *fba_dev)
{
	switch (fba_dev->m7_health) {
	case HEALTHY:
		return "HEALTHY";
	case UNHEALTHY:
		return "UNHEALTHY";
	case UNINITIALIZED:
		return "UNINITIALIZED";
	}
	fba_dev_err(fba_dev, "%s: Invalid M7 state 0x%x", __func__,
		    fba_dev->m7_health);
	return "UNEXPECTED";
}

static void store_boot_args(struct fba_device *fba_dev)
{
	void __iomem *args_bufp;
	size_t boot_arg_len;

	boot_arg_len = strlen(freya_boot_args);
	if (boot_arg_len == 0)
		return;
	WARN_ON(boot_arg_len > CXRAM_BOOT_ARGS_MAX_SIZE);

	args_bufp = fba_dev->bars[BAR_CXRAM] + CXRAM_BOOT_ARGS_OFFSET;

	memset_io(args_bufp, 0, CXRAM_BOOT_ARGS_MAX_SIZE);
	memcpy_toio(args_bufp, freya_boot_args,
		    (boot_arg_len > CXRAM_BOOT_ARGS_MAX_SIZE) ?
				  CXRAM_BOOT_ARGS_MAX_SIZE - 1 :
				  boot_arg_len);
	fba_dev_info(fba_dev, "Configured boot args:%s", freya_boot_args);
}

static void freya_store_boot_param(struct fba_device *fba_dev)
{
	uint32_t llc_conf = READ_ONCE(llc_config);

	fba_dev_info(fba_dev, "gdb_on_boot boot parameter is set to 0x%x",
		     gdb_on_boot);
	fba_write_bar(fba_dev, gdb_on_boot, BAR_CXRAM,
		      CXRAM_GDB_ON_BOOT_OFFSET);
	fba_dev_info(fba_dev, "llc_config boot parameter is set to 0x%x",
		     llc_conf);
	fba_write_bar(fba_dev, llc_conf, BAR_CXRAM, CXRAM_LLC_CONFIG_OFFSET);
	fba_dev_info(fba_dev, "pe_print_mode boot parameter is set to 0x%x",
		     pe_print_mode);
	fba_write_bar(fba_dev, pe_print_mode, BAR_CXRAM,
		      CXRAM_PE_PRINT_MODE_OFFSET);
	fba_dev_info(fba_dev, "PCIe console is %s",
		     pcie_console ? "enabled" : "disabled");
	fba_write_bar(fba_dev, pcie_console, BAR_CXRAM,
		      CXRAM_ENABLE_PCIE_CONSOLE_OFFSET);
	fba_dev_info(fba_dev,
		     "pe_idcache_disable boot parameter is set to 0x%x",
		     pe_idcache_disable);
	fba_write_bar(fba_dev, pe_idcache_disable, BAR_CXRAM,
		      CXRAM_PE_IDCACHE_CONFIG_OFFSET);

	store_boot_args(fba_dev);

	// Ensure ECC regions are a multiple of 16MB
	if (IS_ALIGNED(ecc_region_size_mb, DRAM_ECC_REGION_GRANULARITY_MB)) {
		fba_write_bar(fba_dev, ecc_region_size_mb, BAR_CXRAM,
			      CXRAM_ECC_REGION_SIZE_OFFSET);
	} else {
		fba_dev_warn(
			fba_dev,
			"ECC region size not a multiple of 16MB, setting size to 0.");
		fba_write_bar(fba_dev, 0, BAR_CXRAM,
			      CXRAM_ECC_REGION_SIZE_OFFSET);
	}

	if (IS_ALIGNED(p2p_ecc_region_size_mb,
		       DRAM_ECC_REGION_GRANULARITY_MB)) {
		fba_write_bar(fba_dev, p2p_ecc_region_size_mb, BAR_CXRAM,
			      CXRAM_P2P_ECC_SIZE_OFFSET);
	} else {
		fba_dev_warn(
			fba_dev,
			"P2P ECC region size not a multiple of 16MB, setting size to 0.");
		fba_write_bar(fba_dev, 0, BAR_CXRAM, CXRAM_P2P_ECC_SIZE_OFFSET);
	}
}

static inline void cxram_sideband_set_mask(struct fba_device *dev,
					   uint32_t mask, uint32_t newval,
					   uint64_t offset)
{
	uint32_t value;

	value = freya_read_cxram_sb(dev, offset);
	value &= ~mask;
	value |= newval;
	freya_write_cxram_sb(dev, value, offset);
}

static inline void cxram_sideband_setbit(struct fba_device *dev,
					 uint32_t setbits, uint64_t offset)
{
	uint32_t value;

	value = freya_read_cxram_sb(dev, offset);
	value |= setbits;
	freya_write_cxram_sb(dev, value, offset);
}

static inline void cxram_sideband_clearbit(struct fba_device *dev,
					   uint32_t clearbits, uint64_t offset)
{
	uint32_t value;

	value = freya_read_cxram_sb(dev, offset);
	value &= ~clearbits;
	freya_write_cxram_sb(dev, value, offset);
}

static int request_and_xfer_fw(struct fba_device *fba_dev, const char *name,
			       void __iomem *buf, size_t max_size)
{
	struct device *dev = &fba_dev->pdev->dev;
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, name, dev);
	if (ret)
		return ret;

	if (fw->size > max_size) {
		release_firmware(fw);
		return -EFBIG;
	}

	memcpy_toio(buf, fw->data, fw->size);
	fba_dev_info(fba_dev, "Image of size=0x%zx is transfered to the device",
		     fw->size);
	release_firmware(fw);

	return 0;
}

static inline int freya_wait_for_status(struct fba_device *fba_dev,
					uint64_t offset, uint32_t mask,
					uint32_t value,
					unsigned long timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	uint32_t rd_val = freya_read_cxram_sb(fba_dev, offset);

	while ((rd_val & mask) != value) {
		/* Bail early if we're unbinding. */
		if (READ_ONCE(fba_dev->exiting))
			return -ETIMEDOUT;

		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;

		cpu_relax();
		cond_resched();
		rd_val = freya_read_cxram_sb(fba_dev, offset);
	}

	return 0;
}

static void freya_boot_error_signal_print(struct fba_device *fba_dev)
{
	uint32_t error_code, asic_error, module_error, error_cnt, temp;

	error_code = freya_read_cxram_sb(fba_dev, SB_ERROR_CODE_OFS);
	asic_error = freya_read_cxram_sb(fba_dev, SB_ASIC_ERROR_TYPE_OFS);
	module_error = freya_read_cxram_sb(fba_dev, SB_MODULE_ERROR_TYPE_OFS);
	error_cnt = freya_read_cxram_sb(fba_dev, SB_INT_MEM_ERR_CNT_OFS);

	if (error_code != 0) {
		fba_dev_info(fba_dev, "ERROR_CODE: %#lx",
			     FIELD_GET(ASIC_ERROR_CODE_MASK, error_code));
		fba_dev_info(fba_dev, "CERT1_ERROR_CODE: %#lx",
			     FIELD_GET(ASIC_CERT1_ERROR_CODE_MASK, error_code));
		fba_dev_info(fba_dev, "CERT2_ERROR_CODE: %#lx",
			     FIELD_GET(ASIC_CERT2_ERROR_CODE_MASK, error_code));
		fba_dev_info(fba_dev, "CERT3_ERROR_CODE: %#lx",
			     FIELD_GET(ASIC_CERT3_ERROR_CODE_MASK, error_code));
	}

	if (FIELD_GET(ASIC_ERROR_TYPE_MASK, asic_error) != 0)
		fba_dev_info(fba_dev, "ASIC_ERROR_TYPE: %#lx",
			     FIELD_GET(ASIC_ERROR_TYPE_MASK, asic_error));

	if ((module_error & ~ASIC_TEMP_VALUE_MASK) != 0) {
		fba_dev_info(fba_dev, "MODULE_ERROR_TYPE: %#lx",
			     FIELD_GET(MODULE_ERROR_TYPE_MASK, module_error));
		fba_dev_info(fba_dev, "WARNING_FLAG: %#lx",
			     FIELD_GET(ASIC_WARNING_FLAG_MASK, module_error));
		fba_dev_info(fba_dev, "INTERRUPT_FLAG: %#lx",
			     FIELD_GET(ASIC_INTERRUPT_FLAG_MASK, module_error));
	}

	if ((error_cnt & ~ASIC_PEC_BYTE_MASK) != 0) {
		fba_dev_info(fba_dev, "INT_MEM_ERR_CNT: %#lx",
			     FIELD_GET(ASIC_INT_MEM_ERR_CNT_MASK, error_cnt));
		fba_dev_info(fba_dev, "EXT_MEM_ERR_CNT: %#lx",
			     FIELD_GET(ASIC_EXT_MEM_ERR_CNT_MASK, error_cnt));
		fba_dev_info(fba_dev, "SMBUS_ERROR: %#lx",
			     FIELD_GET(ASIC_SMBUS_ERROR_MASK, error_cnt));
	}

	temp = FIELD_GET(ASIC_TEMP_VALUE_MASK, module_error);
	if (temp > ASIC_MAX_TEMP)
		fba_dev_info(
			fba_dev,
			"MAX_ASIC_TEMP exceeding maximum ASIC temperature: %#x",
			temp);
}

static void freya_boot_error_signal_reset(struct fba_device *fba_dev)
{
	// This will reset all ERROR_CODE, CERT1_ERROR_CODE, CERT2_ERROR_CODE,
	// CERT3_ERROR_CODE
	freya_write_cxram_sb(fba_dev, 0, SB_ERROR_CODE_OFS);

	// This will reset all ASIC_ERROR_TYPE, MODULE_ERROR_TYPE, WARNING_FLAG,
	// INTERRUPT_FLAG
	freya_write_cxram_sb(fba_dev, 0, SB_ASIC_ERROR_TYPE_OFS);

	// This will reset all MAX_ASIC_TEMP, INT_MEM_ERR_CNT, EXT_MEM_ERR_CNT,
	// SMBUS_ERROR
	freya_write_cxram_sb(fba_dev, 0, SB_MAX_ASIC_TEMP_OFS);
}

static int freya_download_image(struct fba_device *fba_dev,
				enum image_location image_destination,
				const char *file_name, uint32_t load_timeout)
{
	struct freya_priv *priv = fba_dev->impl_priv;
	void __iomem *bufp;
	unsigned long timeout;
	size_t max_buf;
	uint32_t val;
	int ret;

	// Print and reset the persistent error code as M7 would not reset these on
	// any type of reset
	freya_boot_error_signal_print(fba_dev);
	freya_boot_error_signal_reset(fba_dev);

	// Clearing handshaking bits
	cxram_sideband_clearbit(fba_dev, CONTROL_CLEAR_MASK, SB_FW_UPDATE_OFS);

	// Verify bits were cleared
	val = freya_read_cxram_sb(fba_dev, SB_FW_UPDATE_OFS);
	if ((val & CONTROL_CLEAR_MASK) != 0) {
		fba_dev_err(fba_dev,
			    "Failed to clear handshaking bits, device failed?");
		return -EIO;
	}

	// Setting up the location of the image source (it can only be SRAM)
	// and image final destination
	cxram_sideband_set_mask(fba_dev, FW_IMAGE_SRC_MASK | FW_IMAGE_DST_MASK,
				FIELD_PREP(FW_IMAGE_SRC_MASK, LOCATION_SRAM) |
					FIELD_PREP(FW_IMAGE_DST_MASK,
						   image_destination),
				SB_FW_IMAGE_TARGET_CMD_OFS);

	// Setting the initiate download bit from host
	cxram_sideband_setbit(fba_dev, INIT_DOWNLOAD, SB_FW_UPDATE_OFS);
	fba_dev_info(fba_dev, "Initiate download");
	timeout = emulation_boot_timeouts ? EMULATION_DEFAULT_TIMEOUT :
						  RDY_FOR_IMG_XFER_TIMEOUT_MS;

	// Waiting for the device to respond that it is ready for image transfer
	ret = freya_wait_for_status(fba_dev, SB_FW_UPDATE_OFS,
				    READY_FOR_FW_IMAGE, READY_FOR_FW_IMAGE,
				    timeout);
	if (ret < 0) {
		// Timeout while waiting for the device to respond
		fba_dev_err(fba_dev, "Ready for FW image timeout");
		goto err_out;
	}
	fba_dev_info(fba_dev, "Device ready for FW image");

	// Transfer the image to context SRAM using BAR 2(a portion right at the
	// beginning of this BAR is mapped to CxRAM during boot0 initialization,
	// so the offset from CxRAM start address is the same offset of this BAR)
	bufp = (void __iomem *)(fba_dev->bars[BAR_CXRAM] +
				priv->cxram_upgrade_ofs);
	max_buf = priv->cxram_upgrade_sz;

	ret = request_and_xfer_fw(fba_dev, file_name, bufp, max_buf);
	if (ret) {
		fba_dev_err(fba_dev, "Error %d requesting firmware file: %s",
			    ret, file_name);
		goto err_out;
	}

	// Signal to the device that the image transfer is done
	cxram_sideband_setbit(fba_dev, TRANSFER_DONE, SB_FW_UPDATE_OFS);
	fba_dev_info(fba_dev, "Device is notified that transfer is done");
	timeout = emulation_boot_timeouts ? EMULATION_DEFAULT_TIMEOUT :
						  IMAGE_RECEIVED_TIMEOUT_MS;

	// Waiting for the device to acknowledge the transfer
	ret = freya_wait_for_status(fba_dev, SB_FW_UPDATE_OFS,
				    UPDATE_IN_PROGRESS, UPDATE_IN_PROGRESS,
				    timeout);

	// Timeout while waiting for the device to respond
	if (ret < 0) {
		fba_dev_err(fba_dev, "Image received timeout");
		goto err_out;
	}
	fba_dev_info(fba_dev,
		     "Device is in progress of authenticating the image");

	// Clearing the transfer done signal
	cxram_sideband_clearbit(fba_dev, TRANSFER_DONE, SB_FW_UPDATE_OFS);

	// Verify the transfer done signal was cleared
	val = freya_read_cxram_sb(fba_dev, SB_FW_UPDATE_OFS);
	if ((val & TRANSFER_DONE) != 0) {
		fba_dev_err(fba_dev,
			    "Failed to clear transfer done, device failed?");
		return -EIO;
	}

	fba_dev_info(
		fba_dev,
		"Transfer done flag is cleared, waiting for update complete");

	// Waiting for the device to complete image verification and transfer to
	// the destination location
	ret = freya_wait_for_status(fba_dev, SB_FW_UPDATE_OFS, UPDATE_COMPLETE,
				    UPDATE_COMPLETE, load_timeout);

	if (ret < 0) {
		fba_dev_err(
			fba_dev,
			"Device failed to either verify the image or transfer it to the destination location.");
		goto err_out;
	}

	fba_dev_info(fba_dev,
		     "Device image authentication and transfer is complete");

	return ret;

err_out:
	// This is needed when we want to abort the boot flow from host side if host
	// didn't receive the required response from the device in a timely manner.
	cxram_sideband_clearbit(fba_dev, INIT_DOWNLOAD, SB_FW_UPDATE_OFS);
	// Print the debug signals for further investigation
	freya_boot_error_signal_print(fba_dev);
	return ret;
}

static void freya_check_ddr_init_status(struct fba_device *fba_dev)
{
	uint32_t val = freya_read_cxram_sb(fba_dev, SB_BOOT_STATUS2_OFS);

	val = FIELD_GET(DDR_INITIALIZED_MASK, val);
	fba_dev_err(fba_dev, "Boot1 Ready timeout. DDR Init complete: %d", val);
}

static int freya_boot(struct fba_device *fba_dev,
		      enum image_location image_destination,
		      const char *file_name, uint32_t load_timeout)
{
	int rc;

	rc = freya_download_image(fba_dev, image_destination, file_name,
				  load_timeout);
	if (rc == 0) {
		// Signal the device to boot the new image
		cxram_sideband_setbit(fba_dev, BOOT_CMD,
				      SB_FW_IMAGE_TARGET_CMD_OFS);
		fba_dev_info(fba_dev, "Boot request sent to the device");
		// Clear the request initiating the firmware download
		cxram_sideband_clearbit(fba_dev, INIT_DOWNLOAD,
					SB_FW_UPDATE_OFS);
		cxram_sideband_clearbit(fba_dev, READY_FOR_FW_IMAGE,
					SB_FW_UPDATE_OFS);
	}

	return rc;
}

static int freya_boot1(struct fba_device *fba_dev)
{
	int ret;
	unsigned long timeout;

	// Assume CCP FW is loaded if PCIe Boot is not enabled
	if (!fba_dev->pcie_boot_en)
		return 0;

	if ((ecc_region_size_mb != 0) || (p2p_ecc_region_size_mb != 0)) {
		/* Only enable ECC if the user requested some amount of ECC protected
		 * DRAM since turning it on automatically reduces capacity by 1/8.
		 * Boot1 currently is not aware of the CCP boot parameters in CXRAM,
		 * and relies on these config bits in the sideband struct to enable
		 * ECC. This redundancy could be eliminated in the future.
		 */
		cxram_sideband_setbit(fba_dev, LPDDR_INLINE_ECC_ENABLED_MASK,
				      SB_LPDDR_ECC_CONFIG_OFS);
	} else {
		freya_write_cxram_sb(fba_dev, 0, SB_LPDDR_ECC_CONFIG_OFS);
	}

	timeout = emulation_boot_timeouts ? EMULATION_DEFAULT_TIMEOUT :
						  BOOT1_LOAD_TIMEOUT_MS;

	/*
	 * The DDR Initialized bit may not be cleared by the device until after boot1 has
	 * started booting, which may cause the driver to falsely believe DDR init
	 * is done before it has even started. So clear the bit here to guarantee
	 * that can't happen.
	 */
	cxram_sideband_clearbit(fba_dev, DDR_INITIALIZED_MASK,
				SB_BOOT_STATUS2_OFS);

	ret = freya_boot(fba_dev, LOCATION_ITCM, fba_dev->boot1_filename,
			 timeout);

	if (ret < 0) {
		fba_dev_err(fba_dev, "Boot1 load has failed");
		return ret;
	}
	fba_dev_info(fba_dev, "Loaded boot1 image, booting image: %s",
		     fba_dev->boot1_filename);

	if (!ddr_bypass) {
		timeout = emulation_boot_timeouts ? EMULATION_DEFAULT_TIMEOUT :
							  BOOT1_READY_TIMEOUT_MS;

		ret = freya_wait_for_status(fba_dev, SB_BOOT_STATUS2_OFS,
					    BOOT1_COMPLETE_MASK,
					    BOOT1_COMPLETE_MASK, timeout);
		if (ret) {
			freya_check_ddr_init_status(fba_dev);
			return ret;
		}
	}

	cxram_sideband_clearbit(fba_dev, BOOT1_COMPLETE_MASK,
				SB_BOOT_STATUS2_OFS);
	fba_dev_info(fba_dev, "Boot1 ready for CCP transfer");
	freya_store_boot_param(fba_dev);

	if ((fba_dev->pdev->class >> 8) != 0) {
		/* Reset requires PCI quirk to be in effect. */
		atomic_set(&fba_dev->reset_ok, true);
	}
	fba_poller_start(fba_dev->m7_health_poller);

	return ret;
}

static int freya_load_mbist(struct fba_device *fba_dev)
{
	int ret;
	uint32_t result;

	// Assume CCP FW is loaded if PCIe Boot is not enabled
	if (!fba_dev->pcie_boot_en)
		return 0;

	ret = freya_boot(fba_dev, LOCATION_MBIST, FREYA_DEF_MBIST_FILENAME,
			 BOOT1_LOAD_TIMEOUT_MS);
	if (ret < 0) {
		fba_dev_err(
			fba_dev,
			"MBIST load has failed, staying in DRAM UP to retry");
		return ret;
	}
	fba_dev_info(fba_dev, "MBIST load done and MBIST is running");

	ret = freya_wait_for_status(
		fba_dev, SB_EXEC_STATUS_OFS, EXEC_STATUS_MASK,
		FIELD_PREP(EXEC_STATUS_MASK, EXEC_STATUS_COMPLETE),
		MBIST_EXEC_TIMEOUT_MS);
	if (ret) {
		fba_dev_err(fba_dev, "MBIST Test timed out");
		return ret;
	}

	result = FIELD_GET(EXEC_RESULT_MASK,
			   freya_read_cxram_sb(fba_dev, SB_EXEC_RESULT_OFS));
	fba_dev_info(fba_dev, "MBIST Test finished, result : %s",
		     ((result == EXEC_RESULT_PASS) ?
				    "PASS" :
				    ((result == EXEC_RESULT_ABORTED) ? "ABORTED" :
								       "FAIL")));
	return ret;
}

static int freya_load_ccp(struct fba_device *fba_dev)
{
	int ret;
	unsigned long timeout;

	// Assume CCP FW is loaded if PCIe Boot is not enabled
	if (!fba_dev->pcie_boot_en)
		return 0;

	//Clear the mailbox before we start loading/booting ccp
	set_mailbox(fba_dev, SAIL_RDY, 0);

	timeout = emulation_boot_timeouts ? EMULATION_DEFAULT_TIMEOUT :
						  CCP_LOAD_TIMEOUT_MS;

	ret = freya_boot(fba_dev, LOCATION_RISCV, fba_dev->boot2_filename,
			 timeout);
	if (ret == 0)
		fba_dev_info(fba_dev, "Loaded CCP image: %s",
			     fba_dev->boot2_filename);

	return ret;
}

static int freya_boot_ccp(struct fba_device *fba_dev)
{
	int rc;
	int retry_limit;

	/*Assume APP FW is booted if PCIe Boot is not enabled*/
	if (!fba_dev->pcie_boot_en)
		return 0;

	retry_limit = emulation_boot_timeouts ? EMULATION_CCP_BOOT_RETRIES :
						      WAIT_FOR_CCP_BOOT_RETRIES;

	rc = fba_wait_for_state(fba_dev, SAIL_RDY, SAIL_RDY_MAGIC, retry_limit);
	if (rc < 0)
		fba_dev_err(fba_dev, "CCP boot failed");

	return rc;
}

static int freya_secure_dbg_unlock(struct fba_device *fba_dev)
{
	struct freya_priv *priv = fba_dev->impl_priv;
	int ret;

	// Assume CCP FW is loaded if PCIe Boot is not enabled
	if (!fba_dev->pcie_boot_en)
		return 0;

	mutex_lock(&priv->download_mtx);

	ret = freya_download_image(fba_dev, LOCATION_MBIST,
				   FREYA_DEF_SECURE_DBG_FILENAME,
				   BOOT1_LOAD_TIMEOUT_MS);
	if (ret < 0) {
		fba_dev_err(fba_dev, "Secure Debug HBIN download failed.");
	} else {
		cxram_sideband_clearbit(fba_dev, INIT_DOWNLOAD,
					SB_FW_UPDATE_OFS);
		fba_dev_info(fba_dev, "Secure Debug HBIN loaded successfully.");
	}

	mutex_unlock(&priv->download_mtx);
	return ret;
}

static int freya_provision(struct fba_device *fba_dev)
{
	int ret;
	int result;

	// Assume CCP FW is loaded if PCIe Boot is not enabled
	if (!fba_dev->pcie_boot_en)
		return 0;

	ret = freya_boot(fba_dev, LOCATION_MBIST,
			 FREYA_DEF_PROVISIONING_FILENAME,
			 BOOT1_LOAD_TIMEOUT_MS);
	if (ret < 0) {
		fba_dev_err(fba_dev, "Provisioning HBIN load failed.");
		return ret;
	}
	fba_dev_info(fba_dev, "Provisioning HBIN loaded, waiting for result.");

	ret = freya_wait_for_status(
		fba_dev, SB_EXEC_STATUS_OFS, EXEC_STATUS_MASK,
		FIELD_PREP(EXEC_STATUS_MASK, EXEC_STATUS_COMPLETE),
		MBIST_EXEC_TIMEOUT_MS);
	if (ret != 0) {
		fba_dev_err(fba_dev, "Provisioning timed out");
		return ret;
	}

	result = freya_read_cxram_sb(fba_dev, SB_EXEC_RESULT_OFS);
	if (result == EXEC_RESULT_PASS)
		fba_dev_info(fba_dev, "Provisioning completed successfully");
	else if (result == EXEC_RESULT_ABORTED)
		fba_dev_err(fba_dev, "Provisioning aborted");
	else
		fba_dev_err(fba_dev, "Provisioning failed");

	return ret;
}

static bool freya_is_secure_boot(struct fba_device *fba_dev)
{
	uint32_t val = freya_read_cxram_sb(fba_dev, SB_BOOT_STATUS2_OFS);

	val = FIELD_GET(LCS_STATE_MASK, val);
	fba_dev_info(fba_dev, "Freya lcs_state=%d", val);

	return (val == LCS_STATE_SECURE);
}

static void freya_get_fw_filenames(struct fba_device *fba_dev,
				   const char **boot1, const char **boot2)
{
	*boot1 = FREYA_DEF_BOOT1_FILENAME;
	*boot2 = FREYA_DEF_BOOT2_FILENAME;
}

static void artemis_get_fw_filenames(struct fba_device *fba_dev,
				     const char **boot1, const char **boot2)
{
	*boot1 = ARTEMIS_DEF_BOOT1_FILENAME;
	*boot2 = ARTEMIS_DEF_BOOT2_FILENAME;
}

static int freya_load_firmware(struct fba_device *fba_dev)
{
	struct freya_priv *priv = fba_dev->impl_priv;
	enum freya_boot_mode boot_mode;
	int rc;

	boot_mode = READ_ONCE(priv->boot_mode);
	rc = freya_boot1(fba_dev);
	if (rc != 0)
		return rc;

	if (boot_mode == FREYA_BOOT_MBIST) {
		(void)freya_load_mbist(fba_dev);
		return -ENODEV;
	} else if (boot_mode == FREYA_BOOT_PROVISION) {
		(void)freya_provision(fba_dev);
		return -ENODEV;
	}

	rc = freya_load_ccp(fba_dev);
	if (rc != 0)
		return rc;
	return freya_boot_ccp(fba_dev);
}

// Last host IPC mailbox group (31) is assigned to Freya M7, the first entry's
// callback is set to reset CCP and CIP. The second entry serves as a status
// to let the driver know that the CCP CIP reset is done
static int reset_ccp(struct fba_device *fba_dev)
{
	int rc;

	if (!fba_dev->pcie_boot_en)
		return 0;

	fba_dev_info(fba_dev, "Interrupting M7 to reset CCP and CIP.");
	set_mailbox(fba_dev, M7_MBOX_CTRL, CCP_CIP_RESET);
	fba_dev_info(fba_dev, "Waiting for M7 to reset CCP and CIP.");
	rc = fba_wait_for_state_nobail(fba_dev, M7_MBOX_STATUS,
				       CCP_CIP_RESET_DONE,
				       fba_dev->num_retries);
	set_mailbox(fba_dev, M7_MBOX_CTRL, STATUS_RESET);
	rc = fba_wait_for_state_nobail(fba_dev, M7_MBOX_STATUS,
				       STATUS_RESET_DONE, fba_dev->num_retries);
	return rc;
}

static long fbia_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct fba_ctx *ctx = fp->private_data;
	struct fba_device *dev = fba_ctx_to_fba_device(ctx);

	switch (cmd) {
	case FBA_IOCTL_SECURE_DBG_UNLOCK:
		return freya_secure_dbg_unlock(dev);
	default:
		return -EINVAL;
	}

	return 0;
}

static int freya_reset_controllers(struct fba_device *fba_dev)
{
	return reset_ccp(fba_dev);
}

static const char *freya_get_boot_mode(struct fba_device *dev)
{
	struct freya_priv *priv = dev->impl_priv;

	switch (priv->boot_mode) {
	case FREYA_BOOT_MBIST:
		return "mbist";
	case FREYA_BOOT_PROVISION:
		return "provision";
	default:
		return "normal";
	}
}

static int freya_set_boot_mode(struct fba_device *dev, const char *mode)
{
	struct freya_priv *priv = dev->impl_priv;

	if (strncmp(mode, "normal", 6) == 0) {
		priv->boot_mode = FREYA_BOOT_NORMAL;
		return 0;
	}
	if (strncmp(mode, "mbist", 5) == 0) {
		priv->boot_mode = FREYA_BOOT_MBIST;
		return 0;
	}
	if (strncmp(mode, "prov", 4) == 0) {
		priv->boot_mode = FREYA_BOOT_PROVISION;
		return 0;
	}

	return -EINVAL;
}

static int freya_poller_init(struct fba_device *fba_dev)
{
	fba_dev->m7_health_poller =
		kzalloc(sizeof(struct fba_poller), GFP_KERNEL);
	if (fba_dev->m7_health_poller == NULL)
		return -ENOMEM;

	fba_poller_init(fba_dev->m7_health_poller, fba_dev, freya_m7_hb_poll,
			freya_m7_set_hb_mon_period);
	return 0;
}

static int freya_init_priv(struct fba_device *fba_dev,
			   const struct fbia_priv_data *data)
{
	struct freya_priv *priv;
	int rc;

	priv = kzalloc(sizeof(struct freya_priv), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	priv->data = data;
	fba_dev->impl_priv = priv;

	rc = freya_poller_init(fba_dev);
	if (rc != 0) {
		fba_dev_err(fba_dev, "freya_poller_init() failed with %d", rc);
		kfree(fba_dev->impl_priv);
		return rc;
	}
	mutex_init(&priv->download_mtx);

	return 0;
}

static const struct fbia_priv_data freya_data = {
	.cxram_sideband_ofs = FREYA_CXRAM_SIZE - SIDEBAND_SIZE,
};

static const struct fbia_priv_data artemis_data = {
	.cxram_sideband_ofs = ARTEMIS_CXRAM_SIZE - SIDEBAND_SIZE,
};

static int freya_init(struct fba_device *fba_dev)
{
	struct freya_priv *priv;
	int rc;

	rc = freya_init_priv(fba_dev, &freya_data);
	if (rc != 0)
		return rc;

	priv = fba_dev->impl_priv;
	priv->cxram_upgrade_ofs =
		SRAM_UPGRADE_OFS(FREYA_CXRAM_BASE, FREYA_CXRAM_SIZE);
	priv->cxram_upgrade_sz = SRAM_UPGRADE_MEM_SIZE;

	return 0;
}

static int artemis_init(struct fba_device *fba_dev)
{
	struct freya_priv *priv;
	int rc;

	rc = freya_init_priv(fba_dev, &artemis_data);
	if (rc != 0)
		return rc;

	priv = fba_dev->impl_priv;
	priv->cxram_upgrade_ofs =
		freya_read_cxram_sb(fba_dev, SB_TRANSFER_MEM_START_OFS) -
		ARTEMIS_CXRAM_BASE;
	priv->cxram_upgrade_sz =
		freya_read_cxram_sb(fba_dev, SB_TRANSFER_MEM_MAX_SIZE_OFS);

	return 0;
}

static void freya_exit(struct fba_device *fba_dev)
{
	struct freya_priv *priv = fba_dev->impl_priv;

	mutex_destroy(&priv->download_mtx);
	kfree(fba_dev->impl_priv);
}

static const struct fba_impl_ops freya_impl_ops = {
	.impl_init = freya_init,
	.impl_exit = freya_exit,
	.priv_ioctl = fbia_ioctl,

	.is_secure_boot = freya_is_secure_boot,
	.get_fw_filenames = freya_get_fw_filenames,
	.get_boot_mode = freya_get_boot_mode,
	.set_boot_mode = freya_set_boot_mode,
	.load_firmware = freya_load_firmware,
	.reset_controller = freya_reset_controllers,
	.boot_health_check = freya_m7_health,
};

static const struct fba_impl_ops artemis_impl_ops = {
	.impl_init = artemis_init,
	.impl_exit = freya_exit,
	.priv_ioctl = fbia_ioctl,

	.is_secure_boot = freya_is_secure_boot,
	.get_fw_filenames = artemis_get_fw_filenames,
	.get_boot_mode = freya_get_boot_mode,
	.set_boot_mode = freya_set_boot_mode,
	.load_firmware = freya_load_firmware,
	.reset_controller = freya_reset_controllers,
	.boot_health_check = freya_m7_health,
};

const struct fba_cfg freya_cfg = {
	.name = FREYA_DRV_NAME,
	.devnode_ida = &freya_ida,
	.host_ipc_bar = HOST_IPC_BAR,
	.host_ipc_stride = HOST_IPC_STRIDE,
	.host_ipc_offset = HOST_IPC_BAR_OFF,
	.db_mbox_base = DB_MAILBOX_BASE,
	.scratch_page_cnt = SAIL_SCRATCH_PAGE_CNT,
	.host_sq_wc_bar = HOST_SQ_BAR,
	.p2p_bar = P2P_BAR,
	.impl_ops = &freya_impl_ops,
	.ctx_limits = {
		.sub_cnt = CONTEXT_SUB_CAP,
	},
};

const struct fba_cfg artemis_cfg = {
	.name = ARTEMIS_DRV_NAME,
	.devnode_ida = &freya_ida,
	.host_ipc_bar = HOST_IPC_BAR,
	.host_ipc_stride = HOST_IPC_STRIDE,
	.host_ipc_offset = HOST_IPC_BAR_OFF,
	.db_mbox_base = DB_MAILBOX_BASE,
	.scratch_page_cnt = SAIL_SCRATCH_PAGE_CNT,
	.host_sq_wc_bar = HOST_SQ_BAR,
	.p2p_bar = P2P_BAR,
	.impl_ops = &artemis_impl_ops,
	.ctx_limits = {
		.sub_cnt = CONTEXT_SUB_CAP,
	},
};
