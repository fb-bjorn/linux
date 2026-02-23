// SPDX-License-Identifier: GPL-2.0-only

/*
 * CMIS loopback helpers for drivers implementing ethtool get/set_loopback.
 *
 * Maps the generic ethtool loopback layer model to CMIS Page 13h
 * registers (CMIS 5.2, Table 8-128):
 *
 *   PMD-host,  host-loop-host → Host Side Input Loopback   (Page 13h, Byte 183)
 *   PMD-host,  line-loop-line → Host Side Output Loopback  (Page 13h, Byte 182)
 *   PMD-media, host-loop-host → Media Side Input Loopback  (Page 13h, Byte 181)
 *   PMD-media, line-loop-line → Media Side Output Loopback (Page 13h, Byte 180)
 *
 * Capabilities are read from Page 13h Byte 128, with Page 13h availability
 * checked via Page 01h Byte 142 bit 5.
 */

#include <linux/ethtool.h>
#include <linux/sfp.h>

#include "common.h"
#include "module_fw.h"
#include "cmis.h"

/* CMIS Page 00h, Byte 0: Physical module identifier */
#define CMIS_PHYS_ID_PAGE		0x00
#define CMIS_PHYS_ID_OFFSET		0x00

/* CMIS Page 01h, Byte 142: Diagnostic Pages Support */
#define CMIS_DIAG_SUPPORT_PAGE		0x01
#define CMIS_DIAG_SUPPORT_OFFSET	0x8E
#define CMIS_DIAG_PAGE13_BIT		BIT(5)

/* CMIS Page 13h, Byte 128: Loopback Capability Advertisement */
#define CMIS_LB_CAPS_PAGE		0x13
#define CMIS_LB_CAPS_OFFSET		0x80
#define CMIS_LB_CAP_MEDIA_OUTPUT	BIT(0)
#define CMIS_LB_CAP_MEDIA_INPUT		BIT(1)
#define CMIS_LB_CAP_HOST_OUTPUT		BIT(2)
#define CMIS_LB_CAP_HOST_INPUT		BIT(3)

/*
 * CMIS Page 13h, Bytes 180-183: Per-Lane Loopback Control
 *   Byte 180 (0xB4): Media Side Output  → PMD-media, line-loop-line
 *   Byte 181 (0xB5): Media Side Input   → PMD-media, host-loop-host
 *   Byte 182 (0xB6): Host Side Output   → PMD-host,  line-loop-line
 *   Byte 183 (0xB7): Host Side Input    → PMD-host,  host-loop-host
 */
#define CMIS_LB_CTRL_PAGE		0x13
#define CMIS_LB_CTRL_OFFSET		0xB4
#define CMIS_LB_CTRL_LEN		4
#define CMIS_LB_CTRL_IDX_MEDIA_OUTPUT	0
#define CMIS_LB_CTRL_IDX_MEDIA_INPUT	1
#define CMIS_LB_CTRL_IDX_HOST_OUTPUT	2
#define CMIS_LB_CTRL_IDX_HOST_INPUT	3

static bool cmis_is_module(u8 phys_id)
{
	switch (phys_id) {
	case SFF8024_ID_QSFP_DD:
	case SFF8024_ID_OSFP:
	case SFF8024_ID_DSFP:
	case SFF8024_ID_QSFP_PLUS_CMIS:
	case SFF8024_ID_SFP_DD_CMIS:
	case SFF8024_ID_SFP_PLUS_CMIS:
		return true;
	default:
		return false;
	}
}

/**
 * cmis_loopback_caps - read CMIS loopback capability mask
 * @dev: network device
 *
 * Return: >0 capability bitmask, 0 if not a CMIS module or no Page 13h,
 *         negative errno on failure.
 */
static int cmis_loopback_caps(struct net_device *dev)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_module_eeprom page = {};
	u8 val;
	int ret;

	if (!ops->get_module_eeprom_by_page)
		return 0;

	/* Read physical identifier */
	ethtool_cmis_page_init(&page, CMIS_PHYS_ID_PAGE, CMIS_PHYS_ID_OFFSET, sizeof(val));
	page.data = &val;
	ret = ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;
	if (!cmis_is_module(val))
		return 0;

	/* Check Page 13h availability */
	ethtool_cmis_page_init(&page, CMIS_DIAG_SUPPORT_PAGE,
			       CMIS_DIAG_SUPPORT_OFFSET, sizeof(val));
	page.data = &val;
	ret = ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;
	if (!(val & CMIS_DIAG_PAGE13_BIT))
		return 0;

	/* Read capability byte */
	ethtool_cmis_page_init(&page, CMIS_LB_CAPS_PAGE, CMIS_LB_CAPS_OFFSET, sizeof(val));
	page.data = &val;
	ret = ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;

	return val & (CMIS_LB_CAP_MEDIA_OUTPUT | CMIS_LB_CAP_MEDIA_INPUT |
		      CMIS_LB_CAP_HOST_OUTPUT | CMIS_LB_CAP_HOST_INPUT);
}

struct cmis_loopback_map {
	u8  cap_bit;
	u8  ctrl_idx;
	u32 layer;
	u32 direction;
};

static const struct cmis_loopback_map cmis_lb_map[] = {
	{ CMIS_LB_CAP_HOST_INPUT,   CMIS_LB_CTRL_IDX_HOST_INPUT,
	  ETHTOOL_LOOPBACK_LAYER_PMD_HOST,  ETHTOOL_LOOPBACK_DIRECTION_HOST_LOOP_HOST },
	{ CMIS_LB_CAP_HOST_OUTPUT,  CMIS_LB_CTRL_IDX_HOST_OUTPUT,
	  ETHTOOL_LOOPBACK_LAYER_PMD_HOST,  ETHTOOL_LOOPBACK_DIRECTION_LINE_LOOP_LINE },
	{ CMIS_LB_CAP_MEDIA_INPUT,  CMIS_LB_CTRL_IDX_MEDIA_INPUT,
	  ETHTOOL_LOOPBACK_LAYER_PMD_MEDIA, ETHTOOL_LOOPBACK_DIRECTION_HOST_LOOP_HOST },
	{ CMIS_LB_CAP_MEDIA_OUTPUT, CMIS_LB_CTRL_IDX_MEDIA_OUTPUT,
	  ETHTOOL_LOOPBACK_LAYER_PMD_MEDIA, ETHTOOL_LOOPBACK_DIRECTION_LINE_LOOP_LINE },
};

/**
 * ethtool_cmis_get_loopback - append CMIS PMD loopback entries to cfg
 * @dev: network device with get_module_eeprom_by_page support
 * @cfg: loopback configuration; PMD-host/PMD-media entries are appended
 *
 * Reads CMIS module capabilities and current loopback state from Page 13h,
 * then appends one entry for each supported loopback variant.  Returns 0
 * without adding entries if the module is not CMIS or does not advertise
 * loopback support.
 *
 * Return: 0 on success, negative errno on failure.
 */
int ethtool_cmis_get_loopback(struct net_device *dev, struct ethtool_loopback_cfg *cfg)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_module_eeprom page = {};
	u8 ctrl[CMIS_LB_CTRL_LEN];
	int caps, ret;
	u32 i;

	caps = cmis_loopback_caps(dev);
	if (caps <= 0)
		return caps;

	/* Read all four control bytes in one access */
	ethtool_cmis_page_init(&page, CMIS_LB_CTRL_PAGE, CMIS_LB_CTRL_OFFSET, sizeof(ctrl));
	page.data = ctrl;
	ret = ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(cmis_lb_map); i++) {
		const struct cmis_loopback_map *m = &cmis_lb_map[i];
		struct ethtool_loopback_entry *e;

		if (!(caps & m->cap_bit) ||
		    cfg->n_entries >= ETHTOOL_LOOPBACK_MAX_ENTRIES)
			continue;

		e = &cfg->entries[cfg->n_entries++];
		e->layer = m->layer;
		e->direction = m->direction;
		e->enabled = !!ctrl[m->ctrl_idx];
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ethtool_cmis_get_loopback);

/**
 * ethtool_cmis_set_loopback - apply PMD loopback entries to CMIS module
 * @dev: network device with get/set_module_eeprom_by_page support
 * @cfg: loopback configuration; PMD-host/PMD-media entries are applied,
 *       others ignored
 * @extack: netlink extended ack for error reporting
 *
 * Scans @cfg for PMD-host and PMD-media layer entries and writes the
 * corresponding CMIS Page 13h control bytes (0xFF for all-lanes enable,
 * 0x00 for disable).  Non-PMD entries are silently ignored.
 *
 * Return: 0 on success, negative errno on failure.
 */
int ethtool_cmis_set_loopback(struct net_device *dev, const struct ethtool_loopback_cfg *cfg,
			      struct netlink_ext_ack *extack)
{
	struct ethtool_module_eeprom page = {};
	bool en[CMIS_LB_CTRL_LEN] = {};
	bool has_pmd = false;
	u8 ctrl[CMIS_LB_CTRL_LEN];
	int caps, ret;
	u32 i, j;

	for (i = 0; i < cfg->n_entries; i++) {
		const struct ethtool_loopback_entry *e = &cfg->entries[i];

		for (j = 0; j < ARRAY_SIZE(cmis_lb_map); j++) {
			const struct cmis_loopback_map *m = &cmis_lb_map[j];

			if (e->layer == m->layer && e->direction == m->direction) {
				en[m->ctrl_idx] = e->enabled;
				has_pmd = true;
				break;
			}
		}
	}

	if (!has_pmd)
		return 0;

	if (!dev->ethtool_ops->set_module_eeprom_by_page) {
		NL_SET_ERR_MSG(extack, "Module EEPROM access not supported");
		return -EOPNOTSUPP;
	}

	caps = cmis_loopback_caps(dev);
	if (caps < 0)
		return caps;
	if (!caps) {
		NL_SET_ERR_MSG(extack, "Module does not support CMIS loopback");
		return -EOPNOTSUPP;
	}

	for (i = 0; i < ARRAY_SIZE(cmis_lb_map); i++) {
		if (en[cmis_lb_map[i].ctrl_idx] && !(caps & cmis_lb_map[i].cap_bit)) {
			NL_SET_ERR_MSG(extack, "Loopback mode not supported by module");
			return -EOPNOTSUPP;
		}
	}

	/* Read current control bytes */
	ethtool_cmis_page_init(&page, CMIS_LB_CTRL_PAGE, CMIS_LB_CTRL_OFFSET, sizeof(ctrl));
	page.data = ctrl;
	ret = dev->ethtool_ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;

	/* 0xFF = all lanes enabled, 0x00 = disabled */
	for (i = 0; i < CMIS_LB_CTRL_LEN; i++)
		ctrl[i] = en[i] ? 0xFF : 0x00;

	return dev->ethtool_ops->set_module_eeprom_by_page(dev, &page, extack);
}
EXPORT_SYMBOL_GPL(ethtool_cmis_set_loopback);
