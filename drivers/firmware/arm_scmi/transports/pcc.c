// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Message ACPI PCC
 * Transport Driver
 *
 * This transport uses ACPI PCC (PCCT Type 3/4) subspaces via the Linux
 * PCC mailbox controller to exchange SCMI messages over the standard
 * SCMI Shared Memory Transport (SMT) layout.
 *
 * PCC subspace selection is conveyed via ACPI SCMI namespace device.
 *
 * Copyright (C) 2025
 */

#include <linux/acpi.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/hashtable.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <acpi/pcc.h>

#include "../common.h"

#define SCMI_SHMEM_FLAG_INTR_ENABLED		BIT(0)
#define SCMI_TRANSPORT_PACKAGE_MAX_VERSION	(1)
#define SCMI_PROTOCOL_PACKAGE_MAX_VERSION	(1)
#define SCMI_TRANSPORT_SHARED_CHANNEL		BIT(0)
#define SCMI_TRANSPORT_P2A_CHANNEL		BIT(1)

/*
 * SCMI specification requires all parameters, message headers, return
 * arguments or any protocol data to be expressed in little endian
 * format only.
 */
struct pcc_shared_mem {
	struct acpi_pcct_ext_pcc_shared_memory header;
	u8 msg_payload[];
};

/**
 * struct scmi_pcc - Structure representing a SCMI mailbox transport
 *
 * @cl: Mailbox Client
 * @pchan: Transmit/Receive PCC/mailbox channel
 * @cinfo: SCMI channel info
 * @shmem: Transmit/Receive shared memory area
 */
struct scmi_pcc {
	struct mbox_client cl;
	struct pcc_mbox_chan *pchan;
	struct scmi_chan_info *cinfo;
};

struct pcc_transport {
	u32 pcc_ss_id;
	u32 protocol_id;
	u32 flags;
	struct hlist_node hnode;
};

/* Not all MAX_PCC_SUBSPACES will be used for SCMI, keeping it at 16 for now */
static DECLARE_HASHTABLE(pcc_id_hash, ilog2(MAX_PCC_SUBSPACES / 8));

#define client_to_scmi_pcc(c) container_of(c, struct scmi_pcc, cl)

static struct scmi_transport_core_operations *core;

static int acpi_scmi_dsd_parse_transport_package(const union acpi_object *obj)
{
	unsigned int revision = obj->package.elements[0].integer.value;
	unsigned int pkg_cnt = obj->package.elements[1].integer.value;
	int idx;

	if (revision > SCMI_TRANSPORT_PACKAGE_MAX_VERSION)
		return -EINVAL;

	for (idx = 0; idx < pkg_cnt; idx++) {
		union acpi_object *pack = &obj->package.elements[idx + 2];
		struct pcc_transport *p, *tmp;
		u32 uid;

		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p)
			return -ENOMEM;

		if (pack->type != ACPI_TYPE_PACKAGE || pack->package.count != 3)
			pr_info("Invalid transport properties pkg %d\n", idx);

		uid = pack->package.elements[1].integer.value;
		p->pcc_ss_id = pack->package.elements[0].integer.value;
		p->flags = pack->package.elements[2].integer.value;

		hash_for_each_possible(pcc_id_hash, tmp, hnode, uid)
			if (tmp) {
				pr_info("Duplicate UID %d\n", uid);
				return -EEXIST;
			}

		if (p->flags & SCMI_TRANSPORT_SHARED_CHANNEL)
			p->protocol_id = SCMI_PROTOCOL_BASE;

		hash_add(pcc_id_hash, &p->hnode, uid);
	}

	return 0;
}

static int acpi_scmi_dsd_parse_protocol_subpackage(const union acpi_object *obj,
						   int prot_id)
{
	u32 uid;
	int idx, ret = 0;
	struct pcc_transport *p;
	unsigned int pkg_cnt = obj->package.count;

	if (pkg_cnt > 2) {
		pr_warn("Only 2 channels: one Tx and one Rx needed\n");
		return -EINVAL;
	}

	for (idx = 0; idx < pkg_cnt; idx++) {
		union acpi_object *pack = &obj->package.elements[idx];

		/* Flags(pack->package.elements[1]) must be always 0 for now */
		uid = pack->package.elements[0].integer.value;
		hash_for_each_possible(pcc_id_hash, p, hnode, uid) {
			if (p->flags & SCMI_TRANSPORT_SHARED_CHANNEL) {
				pr_info("Invalid! %d channel is shared\n",
					p->pcc_ss_id);
				ret = -EINVAL;
				break;
			}
			p->protocol_id = prot_id;
		}
	}

	return ret;
}

static int
acpi_scmi_dsd_parse_protocol_package(const union acpi_object *obj, int prot_id)
{
	unsigned int revision = obj->package.elements[0].integer.value;
	union acpi_object *pack = &obj->package.elements[1];

	if (revision > SCMI_PROTOCOL_PACKAGE_MAX_VERSION)
		return -EINVAL;

	if (pack->type != ACPI_TYPE_PACKAGE) {
		pr_info("Invalid protocol transport package\n");
		return -EINVAL;
	}

	/* Empty protocol specific transport package allowed */
	if (pack->package.count != 0)
		acpi_scmi_dsd_parse_protocol_subpackage(pack, prot_id);

	pack = &obj->package.elements[2];
	if (pack->type != ACPI_TYPE_PACKAGE)
		pr_info("Invalid protocol transport association package\n");

	if (pack->package.count != 0)
		pr_info("Non-empty association package not supported\n");

	return 0;
}

static int acpi_scmi_namespace_device_parse(struct fwnode_handle *fwnode)
{
	struct acpi_device *adev = to_acpi_device_node(fwnode);
	const union acpi_object *obj;
	int ret, idx;

	hash_init(pcc_id_hash);

	for (idx = 0; idx < ARRAY_SIZE(scmi_dsd_info_list); idx++) {
		int prot_id = scmi_dsd_info_list[idx].protocol_id;
		const char *propname = scmi_dsd_info_list[idx].property_name;

		ret = acpi_dev_get_property(adev, propname, ACPI_TYPE_ANY, &obj);
		if (ret)
			continue;

		if (obj->type != ACPI_TYPE_PACKAGE) {
			dev_warn(&adev->dev, "PACKAGE property expected!\n");
			break;
		}

		if (prot_id == SCMI_PROTOCOL_BASE)
			acpi_scmi_dsd_parse_transport_package(obj);
		else
			acpi_scmi_dsd_parse_protocol_package(obj, prot_id);
	}

	return 0;
}

static int pcc_get_ss_id(u32 prot_id, bool tx)
{
	struct pcc_transport *p;
	int idx;

	hash_for_each(pcc_id_hash, idx, p, hnode)
		if (p->protocol_id == prot_id) {
			if ((!tx && (p->flags & SCMI_TRANSPORT_P2A_CHANNEL)) ||
			    (tx && !(p->flags & SCMI_TRANSPORT_P2A_CHANNEL)))
				return p->pcc_ss_id;
		}

	return -ENOENT;
}

static bool
pcc_chan_available(struct fwnode_handle *fwnode, int prot_id, int idx)
{
	/*
	 * Just parse the ACPI SCMI namespace device once for idx = 0,
	 * defer full validation until pcc_chan_setup() for simplicity.
	 */
	if (!idx)
		acpi_scmi_namespace_device_parse(fwnode);

	if (pcc_get_ss_id(prot_id, idx ? false : true) < 0)
		return false;

	return true;
}

static void tx_prepare(struct mbox_client *cl, void *m)
{
	struct scmi_pcc *smbox = client_to_scmi_pcc(cl);
	struct pcc_shared_mem __iomem *shmem = smbox->pchan->shmem;
	struct scmi_xfer *xfer = m;

	/* PCC take cares not to call tx_prepare until last transmit is done */
	/* Mark channel busy + clear error */
	iowrite32(SCMI_SHMEM_FLAG_INTR_ENABLED, &shmem->header.flags);
	iowrite32(sizeof(shmem->header.command) + xfer->tx.len,
		  &shmem->header.length);
	iowrite32(pack_scmi_header(&xfer->hdr), &shmem->header.command);
	if (xfer->tx.buf)
		memcpy_toio(shmem->msg_payload, xfer->tx.buf, xfer->tx.len);
}

static void rx_callback(struct mbox_client *cl, void *m)
{
	struct scmi_pcc *smbox = client_to_scmi_pcc(cl);
	struct pcc_shared_mem __iomem *shmem = smbox->pchan->shmem;

	core->rx_callback(smbox->cinfo, ioread32(&shmem->header.command), NULL);
}

static int pcc_chan_setup(struct scmi_chan_info *cinfo, struct device *dev,
			  bool tx)
{
	const char *desc = tx ? "Tx" : "Rx";
	struct device *cdev = cinfo->dev;
	struct scmi_pcc *smbox;
	int ret, ss_id;
	struct mbox_client *cl;

	smbox = devm_kzalloc(dev, sizeof(*smbox), GFP_KERNEL);
	if (!smbox)
		return -ENOMEM;

	cl = &smbox->cl;
	cl->dev = cdev;
	cl->tx_prepare = tx ? tx_prepare : NULL;
	cl->rx_callback = rx_callback;
	cl->tx_block = false;

	ss_id = pcc_get_ss_id(cinfo->id, tx);
	if (ss_id < 0)
		return ss_id;

	smbox->pchan = pcc_mbox_request_channel(cl, ss_id);
	if (IS_ERR(smbox->pchan)) {
		ret = PTR_ERR(smbox->pchan);
		if (ret != -EPROBE_DEFER)
			dev_err(cdev,
				"failed to request SCMI %s mailbox\n", desc);
		return ret;
	}

	cinfo->transport_info = smbox;
	smbox->cinfo = cinfo;

	return 0;
}

static int pcc_chan_free(int id, void *p, void *data)
{
	struct scmi_chan_info *cinfo = p;
	struct scmi_pcc *smbox = cinfo->transport_info;

	if (smbox && !IS_ERR(smbox->pchan)) {
		pcc_mbox_free_channel(smbox->pchan);
		cinfo->transport_info = NULL;
		smbox->pchan = NULL;
		smbox->cinfo = NULL;
	}

	return 0;
}

static int
pcc_send_message(struct scmi_chan_info *cinfo, struct scmi_xfer *xfer)
{
	struct scmi_pcc *smbox = cinfo->transport_info;
	int ret;

	/*
	 * The mailbox layer has its own queue. However the mailbox queue
	 * confuses the per message SCMI timeouts since the clock starts when
	 * the message is submitted into the mailbox queue. So when multiple
	 * messages are queued up the clock starts on all messages instead of
	 * only the one inflight.
	 */
	ret = mbox_send_message(smbox->pchan->mchan, xfer);
	/* mbox_send_message returns non-negative value on success */
	if (ret < 0)
		return ret;

	return 0;
}

static void pcc_fetch_response(struct scmi_chan_info *cinfo,
				   struct scmi_xfer *xfer)
{
	struct scmi_pcc *smbox = cinfo->transport_info;
	struct pcc_shared_mem __iomem *shmem = smbox->pchan->shmem;
	size_t len = ioread32(&shmem->header.length);

	xfer->hdr.status = ioread32(shmem->msg_payload);
	/* Skip the length of header and status in shmem area i.e 8 bytes */
	xfer->rx.len = min_t(size_t, xfer->rx.len, len > 8 ? len - 8 : 0);

	/* Take a copy to the rx buffer.. */
	memcpy_fromio(xfer->rx.buf, shmem->msg_payload + 4, xfer->rx.len);
}

static void pcc_fetch_notification(struct scmi_chan_info *cinfo,
				       size_t max_len, struct scmi_xfer *xfer)
{
	struct scmi_pcc *smbox = cinfo->transport_info;
	struct pcc_shared_mem __iomem *shmem = smbox->pchan->shmem;
	size_t len = ioread32(&shmem->header.length);

	/* Skip only the length of header in shmem area i.e 4 bytes */
	xfer->rx.len = min_t(size_t, max_len, len > 4 ? len - 4 : 0);

	/* Take a copy to the rx buffer.. */
	memcpy_fromio(xfer->rx.buf, shmem->msg_payload, xfer->rx.len);
}

static const struct scmi_transport_ops scmi_pcc_ops = {
	.chan_available = pcc_chan_available,
	.chan_setup = pcc_chan_setup,
	.chan_free = pcc_chan_free,
	.send_message = pcc_send_message,
	.fetch_response = pcc_fetch_response,
	.fetch_notification = pcc_fetch_notification,
};

static struct scmi_desc scmi_pcc_desc = {
	.ops = &scmi_pcc_ops,
	.max_rx_timeout_ms = 30, /* We may increase this if required */
	.max_msg = 20, /* Limited by MBOX_TX_QUEUE_LEN */
	.max_msg_size = SCMI_SHMEM_MAX_PAYLOAD_SIZE,
};

#ifdef CONFIG_ACPI
static const struct acpi_device_id scmi_acpi_ids[] = {
	{"ARML0001", 0},
	{}
};
MODULE_DEVICE_TABLE(acpi, scmi_acpi_ids);
#endif

DEFINE_SCMI_ACPI_TRANSPORT_DRIVER(scmi_pcc, scmi_pcc_driver,
				  scmi_pcc_desc, scmi_acpi_ids, core);
module_platform_driver(scmi_pcc_driver);

MODULE_AUTHOR("Sudeep Holla <sudeep.holla@arm.com>");
MODULE_DESCRIPTION("SCMI ACPI PCC Transport driver");
MODULE_LICENSE("GPL");
