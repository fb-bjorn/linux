// SPDX-License-Identifier: GPL-2.0-only

#include "netlink.h"
#include "common.h"

struct loopback_req_info {
	struct ethnl_req_info		base;
};

struct loopback_reply_data {
	struct ethnl_reply_data		base;
	struct ethtool_loopback_cfg	cfg;
};

#define LOOPBACK_REPDATA(__reply_base) \
	container_of(__reply_base, struct loopback_reply_data, base)

/* GET */

static const struct nla_policy
ethnl_loopback_entry_policy[ETHTOOL_A_LOOPBACK_ENTRY_MAX + 1] = {
	[ETHTOOL_A_LOOPBACK_ENTRY_LAYER]	= { .type = NLA_U32 },
	[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION]	= { .type = NLA_U32 },
	[ETHTOOL_A_LOOPBACK_ENTRY_ENABLED]	= NLA_POLICY_MAX(NLA_U8, 1),
};

const struct nla_policy ethnl_loopback_get_policy[] = {
	[ETHTOOL_A_LOOPBACK_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
};

static int loopback_prepare_data(const struct ethnl_req_info *req_base,
				 struct ethnl_reply_data *reply_base,
				 const struct genl_info *info)
{
	struct loopback_reply_data *data = LOOPBACK_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	int ret;

	if (!dev->ethtool_ops->get_loopback)
		return -EOPNOTSUPP;

	if (dev->ethtool->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(info->extack,
			       "Module firmware flashing is in progress");
		return -EBUSY;
	}

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		return ret;
	ret = dev->ethtool_ops->get_loopback(dev, &data->cfg);
	ethnl_ops_complete(dev);

	return ret;
}

static int loopback_reply_size(const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	const struct loopback_reply_data *data = LOOPBACK_REPDATA(reply_base);
	int entry_size;

	/* Per-entry: nest + layer + direction + enabled */
	entry_size = nla_total_size(0) +		/* nest */
		     nla_total_size(sizeof(u32)) +	/* layer */
		     nla_total_size(sizeof(u32)) +	/* direction */
		     nla_total_size(sizeof(u8));		/* enabled */

	return data->cfg.n_entries * entry_size;
}

static int loopback_fill_entry(struct sk_buff *skb,
			       const struct ethtool_loopback_entry *entry)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, ETHTOOL_A_LOOPBACK_ENTRY);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_u32(skb, ETHTOOL_A_LOOPBACK_ENTRY_LAYER, entry->layer) ||
	    nla_put_u32(skb, ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION, entry->direction) ||
	    nla_put_u8(skb, ETHTOOL_A_LOOPBACK_ENTRY_ENABLED, entry->enabled))
		goto err_cancel;

	nla_nest_end(skb, nest);
	return 0;

err_cancel:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int loopback_fill_reply(struct sk_buff *skb,
			       const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	const struct loopback_reply_data *data = LOOPBACK_REPDATA(reply_base);
	const struct ethtool_loopback_cfg *cfg = &data->cfg;
	u32 i;

	for (i = 0; i < cfg->n_entries; i++) {
		int ret = loopback_fill_entry(skb, &cfg->entries[i]);

		if (ret < 0)
			return ret;
	}

	return 0;
}

/* SET - replaces entire loopback configuration atomically */

const struct nla_policy ethnl_loopback_set_policy[ETHTOOL_A_LOOPBACK_ENTRY + 1] = {
	[ETHTOOL_A_LOOPBACK_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_LOOPBACK_ENTRY]  = NLA_POLICY_NESTED(ethnl_loopback_entry_policy),
};

static int ethnl_set_loopback_validate(struct ethnl_req_info *req_info, struct genl_info *info)
{
	struct net_device *dev = req_info->dev;
	const struct ethtool_ops *ops = dev->ethtool_ops;

	if (!ops->set_loopback)
		return -EOPNOTSUPP;

	if (dev->ethtool->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(info->extack,
			       "Module firmware flashing is in progress");
		return -EBUSY;
	}

	if (dev->flags & IFF_UP) {
		NL_SET_ERR_MSG(info->extack,
			       "Netdevice is up, so loopback change is not permitted");
		return -EBUSY;
	}

	return 1;
}

static int loopback_parse_entry(struct nlattr *attr, struct ethtool_loopback_entry *entry,
				struct netlink_ext_ack *extack)
{
	struct nlattr *tb[ETHTOOL_A_LOOPBACK_ENTRY_MAX + 1];
	int ret;

	ret = nla_parse_nested(tb, ETHTOOL_A_LOOPBACK_ENTRY_MAX, attr,
			       ethnl_loopback_entry_policy, extack);
	if (ret < 0)
		return ret;

	if (!tb[ETHTOOL_A_LOOPBACK_ENTRY_LAYER]) {
		NL_SET_ERR_MSG_ATTR(extack, attr, "loopback layer is required");
		return -EINVAL;
	}

	entry->layer = nla_get_u32(tb[ETHTOOL_A_LOOPBACK_ENTRY_LAYER]);
	if (entry->layer > ETHTOOL_LOOPBACK_LAYER_EXT) {
		NL_SET_ERR_MSG_ATTR(extack, tb[ETHTOOL_A_LOOPBACK_ENTRY_LAYER],
				    "invalid loopback layer");
		return -EINVAL;
	}

	if (tb[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION]) {
		entry->direction = nla_get_u32(tb[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION]);
		if (entry->direction > ETHTOOL_LOOPBACK_DIRECTION_LINE_LOOP_LINE) {
			NL_SET_ERR_MSG_ATTR(extack, tb[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION],
					    "invalid loopback direction");
			return -EINVAL;
		}
	}

	if (tb[ETHTOOL_A_LOOPBACK_ENTRY_ENABLED])
		entry->enabled = nla_get_u8(tb[ETHTOOL_A_LOOPBACK_ENTRY_ENABLED]);

	return 0;
}

static int ethnl_set_loopback(struct ethnl_req_info *req_info, struct genl_info *info)
{
	struct net_device *dev = req_info->dev;
	struct ethtool_loopback_cfg cfg = {};
	struct nlattr *attr;
	int rem, ret;

	nla_for_each_attr(attr, genlmsg_data(info->genlhdr),
			  genlmsg_len(info->genlhdr), rem) {
		if (nla_type(attr) != ETHTOOL_A_LOOPBACK_ENTRY)
			continue;

		if (cfg.n_entries >= ETHTOOL_LOOPBACK_MAX_ENTRIES) {
			NL_SET_ERR_MSG(info->extack, "too many loopback entries");
			return -EINVAL;
		}

		ret = loopback_parse_entry(attr, &cfg.entries[cfg.n_entries], info->extack);
		if (ret < 0)
			return ret;

		cfg.n_entries++;
	}

	if (!cfg.n_entries) {
		NL_SET_ERR_MSG(info->extack, "no loopback entries specified");
		return -EINVAL;
	}

	ret = dev->ethtool_ops->set_loopback(dev, &cfg, info->extack);

	return ret < 0 ? ret : 1;
}

const struct ethnl_request_ops ethnl_loopback_request_ops = {
	.request_cmd		= ETHTOOL_MSG_LOOPBACK_GET,
	.reply_cmd		= ETHTOOL_MSG_LOOPBACK_GET_REPLY,
	.hdr_attr		= ETHTOOL_A_LOOPBACK_HEADER,
	.req_info_size		= sizeof(struct loopback_req_info),
	.reply_data_size	= sizeof(struct loopback_reply_data),

	.prepare_data		= loopback_prepare_data,
	.reply_size		= loopback_reply_size,
	.fill_reply		= loopback_fill_reply,

	.set_validate		= ethnl_set_loopback_validate,
	.set			= ethnl_set_loopback,
};
