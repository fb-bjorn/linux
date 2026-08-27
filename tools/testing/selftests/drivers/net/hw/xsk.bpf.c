// SPDX-License-Identifier: GPL-2.0

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#define XSK_MAX_QUEUES 256
#define XSK_TEST_PORTS 2

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, XSK_MAX_QUEUES);
	__type(key, __u32);
	__type(value, __u32);
} xsk_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, XSK_TEST_PORTS);
	__type(key, __u32);
	__type(value, __u32);
} xsk_port SEC(".maps");

static __always_inline void *udp_hdr(void *data, void *data_end,
				     __u16 proto)
{
	struct ipv6hdr *ip6h;
	struct ethhdr *eth;
	struct iphdr *iph;

	eth = data;
	if ((void *)(eth + 1) > data_end)
		return NULL;

	if (proto == bpf_htons(ETH_P_IP)) {
		iph = (void *)(eth + 1);
		if ((void *)(iph + 1) > data_end ||
		    iph->protocol != IPPROTO_UDP)
			return NULL;
		if (iph->ihl < 5 || (void *)iph + iph->ihl * 4 > data_end)
			return NULL;

		return (void *)iph + iph->ihl * 4;
	}

	if (proto != bpf_htons(ETH_P_IPV6))
		return NULL;

	ip6h = (void *)(eth + 1);
	if ((void *)(ip6h + 1) > data_end || ip6h->nexthdr != IPPROTO_UDP)
		return NULL;

	return ip6h + 1;
}

SEC("xdp.frags")
int xsk_redirect(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct udphdr *udph;
	struct ethhdr *eth;
	__u32 *port;
	__u32 key;

	eth = data;
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	udph = udp_hdr(data, data_end, eth->h_proto);
	if (!udph || (void *)(udph + 1) > data_end)
		return XDP_PASS;

	for (key = 0; key < XSK_TEST_PORTS; key++) {
		port = bpf_map_lookup_elem(&xsk_port, &key);
		if (port && bpf_ntohs(udph->dest) == *port &&
		    bpf_ntohs(udph->source) == *port + 1)
			return bpf_redirect_map(&xsk_map, ctx->rx_queue_index,
						XDP_PASS);
	}

	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
