// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/bpf.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/if_xdp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/udp.h>
#include <net/if.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../../../net/lib/ksft.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#define TEST_MAGIC 0x58534b54U
#define DEFAULT_COUNT 256U
#define DEFAULT_FRAME_COUNT 64U
#define DEFAULT_FRAME_SIZE 4096U
#define DEFAULT_PACKET_SIZE 256U
#define DEFAULT_RING_SIZE 64U
#define DEFAULT_TIMEOUT_MS 10000U
#define POLL_SLICE_MS 100
#define CANARY_BYTE 0xa5
#define UDP_CSUM_ZERO 0xffff
#define MAX_PACKET_SIZE 65535U
#define MAX_BATCH 64U
#define LAUNCH_DELAY_NS 100000000ULL
#define BIND_ATTEMPTS 3
#define BIND_RETRY_US 100000U

#define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | 3)

enum exit_code {
	EXIT_OK,
	EXIT_FAIL,
	EXIT_BIND,
	EXIT_LAYOUT,
	EXIT_UNSUPPORTED,
};

enum role {
	ROLE_NONE,
	ROLE_DUT_RX,
	ROLE_DUT_TX,
	ROLE_PEER_RX,
	ROLE_PEER_TX,
	ROLE_PROBE,
	ROLE_SHARED,
};

enum frame_state {
	FRAME_FREE,
	FRAME_FILL,
	FRAME_RX,
	FRAME_TX,
};

enum ring_type {
	RING_CONSUMER,
	RING_PRODUCER,
};

enum packet_result {
	PACKET_OTHER,
	PACKET_INVALID,
	PACKET_VALID,
};

enum canary_scope {
	CANARY_FRAME,
	CANARY_PREFIX,
};

enum tx_meta_scope {
	TX_META_NONE,
	TX_META_HEAD,
};

struct test_hdr {
	__be32 magic;
	__be32 seed;
	__be32 seq;
	__be32 len;
};

struct ring {
	__u32 cached_prod;
	__u32 cached_cons;
	__u32 mask;
	__u32 size;
	__u32 *producer;
	__u32 *consumer;
	__u32 *flags;
	void *desc;
	void *map;
	size_t map_sz;
};

struct frame {
	__u64 addr;
	enum frame_state state;
};

struct xsk {
	struct ring fill;
	struct ring comp;
	struct ring rx;
	struct ring tx;
	struct frame *frames;
	void *umem;
	size_t umem_len;
	__u32 tx_wakeups;
	__u32 queue;
	int bind_errno;
	int map_fd;
	int fd;
};

struct config {
	struct in6_addr src6;
	struct in6_addr dst6;
	struct in_addr src4;
	struct in_addr dst4;
	unsigned char src_mac[ETH_ALEN];
	unsigned char dst_mac[ETH_ALEN];
	const char *ifname;
	enum role role;
	__u32 timeout_ms;
	__u32 packet_size;
	__u32 frame_count;
	__u32 chunk_size;
	__u32 ring_size;
	__u32 rx_ring_size;
	__u32 fill_offset;
	__u32 tx_offset;
	__u32 interval_us;
	__u32 pause_after;
	__u32 pause_ms;
	__u32 starve_after;
	__u32 refill_batch;
	__u32 hold_ms;
	__u32 headroom;
	__u32 map_id;
	__u32 queue2;
	__u32 queue;
	__u32 count;
	__u32 seed;
	__u32 frags;
	__u32 min_frags;
	__u16 ready_port;
	__u16 port2;
	__u16 port;
	int family;
	bool unaligned;
	bool use_sg;
	bool allow_gaps;
	bool invalid_tx;
	bool cross_page;
	bool probe_layout;
	bool split_submit;
	bool sw_checksum;
	bool tx_checksum;
	bool tx_timestamp;
	bool launch_time;
	bool tx_full;
};

static int fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

	return -1;
}

static __u64 now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (__u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int parse_u32(const char *str, __u32 *value)
{
	unsigned long parsed;
	char *end;

	errno = 0;
	parsed = strtoul(str, &end, 0);
	if (errno || *end || parsed > UINT32_MAX)
		return -1;

	*value = parsed;
	return 0;
}

static int parse_u16(const char *str, __u16 *value)
{
	__u32 parsed;

	if (parse_u32(str, &parsed) || parsed > UINT16_MAX)
		return -1;

	*value = parsed;
	return 0;
}

static int parse_mac(const char *str, unsigned char *mac)
{
	unsigned int value[ETH_ALEN];
	int i;

	if (sscanf(str, "%x:%x:%x:%x:%x:%x", &value[0], &value[1],
		   &value[2], &value[3], &value[4], &value[5]) != ETH_ALEN)
		return -1;

	for (i = 0; i < ETH_ALEN; i++) {
		if (value[i] > UINT8_MAX)
			return -1;
		mac[i] = value[i];
	}

	return 0;
}

static enum role parse_role(const char *str)
{
	if (!strcmp(str, "dut-rx"))
		return ROLE_DUT_RX;
	if (!strcmp(str, "dut-tx"))
		return ROLE_DUT_TX;
	if (!strcmp(str, "peer-rx"))
		return ROLE_PEER_RX;
	if (!strcmp(str, "peer-tx"))
		return ROLE_PEER_TX;
	if (!strcmp(str, "probe"))
		return ROLE_PROBE;
	if (!strcmp(str, "shared"))
		return ROLE_SHARED;

	return ROLE_NONE;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s ROLE [OPTIONS]\n"
		"Roles: dut-rx, dut-tx, peer-rx, peer-tx, probe, shared\n"
		"  -i, --interface NAME\n"
		"  -q, --queue N\n"
		"  -m, --map-id N\n"
		"  -4, --ipv4\n"
		"  -6, --ipv6\n"
		"      --src-ip ADDR --dst-ip ADDR\n"
		"      --src-mac ADDR --dst-mac ADDR\n"
		"  -p, --port N\n"
		"  -c, --count N\n"
		"  -l, --length N\n"
		"      --chunk-size N --headroom N\n"
		"      --frames N --ring-size N\n"
		"      --fill-offset N --tx-offset N\n"
		"      --unaligned --sg --frags N\n"
		"      --min-frags N\n"
		"      --rx-ring-size N --refill-batch N\n"
		"      --allow-gaps --invalid-tx --tx-full\n"
		"      --cross-page --probe-layout --split-submit\n"
		"      --tx-checksum --sw-checksum\n"
		"      --tx-timestamp --launch-time\n"
		"      --queue2 N --port2 N\n"
		"      --ready-port N --timeout-ms N --interval-us N\n"
		"      --pause-after N --pause-ms N\n"
		"      --starve-after N --hold-ms N\n"
		"      --seed N\n",
		prog);
}

static int parse_args(int argc, char **argv, struct config *cfg)
{
	enum {
		OPT_SRC_IP = 256,
		OPT_DST_IP,
		OPT_SRC_MAC,
		OPT_DST_MAC,
		OPT_CHUNK_SIZE,
		OPT_HEADROOM,
		OPT_FRAMES,
		OPT_RING_SIZE,
		OPT_FILL_OFFSET,
		OPT_TX_OFFSET,
		OPT_UNALIGNED,
		OPT_SG,
		OPT_FRAGS,
		OPT_MIN_FRAGS,
		OPT_ALLOW_GAPS,
		OPT_INVALID_TX,
		OPT_CROSS_PAGE,
		OPT_PROBE_LAYOUT,
		OPT_SPLIT_SUBMIT,
		OPT_RX_RING_SIZE,
		OPT_REFILL_BATCH,
		OPT_READY_PORT,
		OPT_TIMEOUT_MS,
		OPT_INTERVAL_US,
		OPT_PAUSE_AFTER,
		OPT_PAUSE_MS,
		OPT_STARVE_AFTER,
		OPT_HOLD_MS,
		OPT_SEED,
		OPT_QUEUE2,
		OPT_TX_CHECKSUM,
		OPT_SW_CHECKSUM,
		OPT_TX_TIMESTAMP,
		OPT_LAUNCH_TIME,
		OPT_PORT2,
		OPT_TX_FULL,
	};
	static const struct option opts[] = {
		{ "interface", required_argument, NULL, 'i' },
		{ "queue", required_argument, NULL, 'q' },
		{ "map-id", required_argument, NULL, 'm' },
		{ "ipv4", no_argument, NULL, '4' },
		{ "ipv6", no_argument, NULL, '6' },
		{ "src-ip", required_argument, NULL, OPT_SRC_IP },
		{ "dst-ip", required_argument, NULL, OPT_DST_IP },
		{ "src-mac", required_argument, NULL, OPT_SRC_MAC },
		{ "dst-mac", required_argument, NULL, OPT_DST_MAC },
		{ "port", required_argument, NULL, 'p' },
		{ "count", required_argument, NULL, 'c' },
		{ "length", required_argument, NULL, 'l' },
		{ "chunk-size", required_argument, NULL, OPT_CHUNK_SIZE },
		{ "headroom", required_argument, NULL, OPT_HEADROOM },
		{ "frames", required_argument, NULL, OPT_FRAMES },
		{ "ring-size", required_argument, NULL, OPT_RING_SIZE },
		{ "fill-offset", required_argument, NULL, OPT_FILL_OFFSET },
		{ "tx-offset", required_argument, NULL, OPT_TX_OFFSET },
		{ "unaligned", no_argument, NULL, OPT_UNALIGNED },
		{ "sg", no_argument, NULL, OPT_SG },
		{ "frags", required_argument, NULL, OPT_FRAGS },
		{ "min-frags", required_argument, NULL, OPT_MIN_FRAGS },
		{ "allow-gaps", no_argument, NULL, OPT_ALLOW_GAPS },
		{ "invalid-tx", no_argument, NULL, OPT_INVALID_TX },
		{ "cross-page", no_argument, NULL, OPT_CROSS_PAGE },
		{ "probe-layout", no_argument, NULL, OPT_PROBE_LAYOUT },
		{ "split-submit", no_argument, NULL, OPT_SPLIT_SUBMIT },
		{ "rx-ring-size", required_argument, NULL, OPT_RX_RING_SIZE },
		{ "refill-batch", required_argument, NULL, OPT_REFILL_BATCH },
		{ "ready-port", required_argument, NULL, OPT_READY_PORT },
		{ "timeout-ms", required_argument, NULL, OPT_TIMEOUT_MS },
		{ "interval-us", required_argument, NULL, OPT_INTERVAL_US },
		{ "pause-after", required_argument, NULL, OPT_PAUSE_AFTER },
		{ "pause-ms", required_argument, NULL, OPT_PAUSE_MS },
		{ "starve-after", required_argument, NULL, OPT_STARVE_AFTER },
		{ "hold-ms", required_argument, NULL, OPT_HOLD_MS },
		{ "seed", required_argument, NULL, OPT_SEED },
		{ "queue2", required_argument, NULL, OPT_QUEUE2 },
		{ "tx-checksum", no_argument, NULL, OPT_TX_CHECKSUM },
		{ "sw-checksum", no_argument, NULL, OPT_SW_CHECKSUM },
		{ "tx-timestamp", no_argument, NULL, OPT_TX_TIMESTAMP },
		{ "launch-time", no_argument, NULL, OPT_LAUNCH_TIME },
		{ "port2", required_argument, NULL, OPT_PORT2 },
		{ "tx-full", no_argument, NULL, OPT_TX_FULL },
		{},
	};
	const char *src_ip = NULL;
	const char *dst_ip = NULL;
	int opt;

	if (argc < 2)
		return -1;

	cfg->role = parse_role(argv[1]);
	if (cfg->role == ROLE_NONE)
		return -1;

	optind = 2;
	while ((opt = getopt_long(argc, argv, "i:q:m:46p:c:l:", opts,
				  NULL)) != -1) {
		switch (opt) {
		case 'i':
			cfg->ifname = optarg;
			break;
		case 'q':
			if (parse_u32(optarg, &cfg->queue))
				return -1;
			break;
		case 'm':
			if (parse_u32(optarg, &cfg->map_id))
				return -1;
			break;
		case '4':
			cfg->family = AF_INET;
			break;
		case '6':
			cfg->family = AF_INET6;
			break;
		case 'p':
			if (parse_u16(optarg, &cfg->port))
				return -1;
			break;
		case 'c':
			if (parse_u32(optarg, &cfg->count))
				return -1;
			break;
		case 'l':
			if (parse_u32(optarg, &cfg->packet_size))
				return -1;
			break;
		case OPT_SRC_IP:
			src_ip = optarg;
			break;
		case OPT_DST_IP:
			dst_ip = optarg;
			break;
		case OPT_SRC_MAC:
			if (parse_mac(optarg, cfg->src_mac))
				return -1;
			break;
		case OPT_DST_MAC:
			if (parse_mac(optarg, cfg->dst_mac))
				return -1;
			break;
		case OPT_CHUNK_SIZE:
			if (parse_u32(optarg, &cfg->chunk_size))
				return -1;
			break;
		case OPT_HEADROOM:
			if (parse_u32(optarg, &cfg->headroom))
				return -1;
			break;
		case OPT_FRAMES:
			if (parse_u32(optarg, &cfg->frame_count))
				return -1;
			break;
		case OPT_RING_SIZE:
			if (parse_u32(optarg, &cfg->ring_size))
				return -1;
			break;
		case OPT_FILL_OFFSET:
			if (parse_u32(optarg, &cfg->fill_offset))
				return -1;
			break;
		case OPT_TX_OFFSET:
			if (parse_u32(optarg, &cfg->tx_offset))
				return -1;
			break;
		case OPT_UNALIGNED:
			cfg->unaligned = true;
			break;
		case OPT_SG:
			cfg->use_sg = true;
			break;
		case OPT_FRAGS:
			if (parse_u32(optarg, &cfg->frags))
				return -1;
			break;
		case OPT_MIN_FRAGS:
			if (parse_u32(optarg, &cfg->min_frags))
				return -1;
			break;
		case OPT_ALLOW_GAPS:
			cfg->allow_gaps = true;
			break;
		case OPT_INVALID_TX:
			cfg->invalid_tx = true;
			break;
		case OPT_CROSS_PAGE:
			cfg->cross_page = true;
			break;
		case OPT_PROBE_LAYOUT:
			cfg->probe_layout = true;
			break;
		case OPT_SPLIT_SUBMIT:
			cfg->split_submit = true;
			break;
		case OPT_RX_RING_SIZE:
			if (parse_u32(optarg, &cfg->rx_ring_size))
				return -1;
			break;
		case OPT_REFILL_BATCH:
			if (parse_u32(optarg, &cfg->refill_batch))
				return -1;
			break;
		case OPT_READY_PORT:
			if (parse_u16(optarg, &cfg->ready_port))
				return -1;
			break;
		case OPT_TIMEOUT_MS:
			if (parse_u32(optarg, &cfg->timeout_ms))
				return -1;
			break;
		case OPT_INTERVAL_US:
			if (parse_u32(optarg, &cfg->interval_us))
				return -1;
			break;
		case OPT_PAUSE_AFTER:
			if (parse_u32(optarg, &cfg->pause_after))
				return -1;
			break;
		case OPT_PAUSE_MS:
			if (parse_u32(optarg, &cfg->pause_ms))
				return -1;
			break;
		case OPT_STARVE_AFTER:
			if (parse_u32(optarg, &cfg->starve_after))
				return -1;
			break;
		case OPT_HOLD_MS:
			if (parse_u32(optarg, &cfg->hold_ms))
				return -1;
			break;
		case OPT_SEED:
			if (parse_u32(optarg, &cfg->seed))
				return -1;
			break;
		case OPT_QUEUE2:
			if (parse_u32(optarg, &cfg->queue2))
				return -1;
			break;
		case OPT_TX_CHECKSUM:
			cfg->tx_checksum = true;
			break;
		case OPT_SW_CHECKSUM:
			cfg->sw_checksum = true;
			cfg->tx_checksum = true;
			break;
		case OPT_TX_TIMESTAMP:
			cfg->tx_timestamp = true;
			break;
		case OPT_LAUNCH_TIME:
			cfg->launch_time = true;
			break;
		case OPT_PORT2:
			if (parse_u16(optarg, &cfg->port2))
				return -1;
			break;
		case OPT_TX_FULL:
			cfg->tx_full = true;
			break;
		default:
			return -1;
		}
	}

	if (!cfg->ifname)
		return -1;
	if (cfg->role == ROLE_PROBE)
		return 0;
	if (!src_ip || !dst_ip || !cfg->port)
		return -1;
	if (cfg->role == ROLE_SHARED && !cfg->port2)
		return -1;

	if (inet_pton(cfg->family, src_ip,
		      cfg->family == AF_INET ? (void *)&cfg->src4 :
		      (void *)&cfg->src6) != 1)
		return -1;
	if (inet_pton(cfg->family, dst_ip,
		      cfg->family == AF_INET ? (void *)&cfg->dst4 :
		      (void *)&cfg->dst6) != 1)
		return -1;

	return 0;
}

static __u32 csum_add(const void *data, size_t len, __u32 sum)
{
	const __u8 *byte = data;

	while (len > 1) {
		sum += ((__u16)byte[0] << 8) | byte[1];
		byte += sizeof(__u16);
		len -= sizeof(__u16);
	}
	if (len)
		sum += (__u16)byte[0] << 8;

	return sum;
}

static __u16 csum_fold(__u32 sum)
{
	while (sum >> 16)
		sum = (sum & UINT16_MAX) + (sum >> 16);

	return htons(~sum);
}

static __u16 udp4_csum(const struct iphdr *iph, const struct udphdr *udph)
{
	__u16 proto = htons(IPPROTO_UDP);
	__u16 len = udph->len;
	__u32 sum = 0;

	sum = csum_add(&iph->saddr, sizeof(iph->saddr), sum);
	sum = csum_add(&iph->daddr, sizeof(iph->daddr), sum);
	sum = csum_add(&proto, sizeof(proto), sum);
	sum = csum_add(&len, sizeof(len), sum);
	sum = csum_add(udph, ntohs(len), sum);

	return csum_fold(sum) ?: UDP_CSUM_ZERO;
}

static __u16 udp6_csum(const struct ipv6hdr *ip6h, const struct udphdr *udph)
{
	__be32 len = htonl(ntohs(udph->len));
	__be32 proto = htonl(IPPROTO_UDP);
	__u32 sum = 0;

	sum = csum_add(&ip6h->saddr, sizeof(ip6h->saddr), sum);
	sum = csum_add(&ip6h->daddr, sizeof(ip6h->daddr), sum);
	sum = csum_add(&len, sizeof(len), sum);
	sum = csum_add(&proto, sizeof(proto), sum);
	sum = csum_add(udph, ntohs(udph->len), sum);

	return csum_fold(sum) ?: UDP_CSUM_ZERO;
}

static __u16 udp_pseudo(const struct config *cfg, const void *packet)
{
	const struct ethhdr *eth = packet;
	const struct udphdr *udph;
	__u32 sum = 0;

	if (cfg->family == AF_INET) {
		const struct iphdr *iph = (const void *)(eth + 1);
		__u16 proto = htons(IPPROTO_UDP);

		udph = (const void *)(iph + 1);
		sum = csum_add(&iph->saddr, sizeof(iph->saddr), sum);
		sum = csum_add(&iph->daddr, sizeof(iph->daddr), sum);
		sum = csum_add(&proto, sizeof(proto), sum);
	} else {
		const struct ipv6hdr *ip6h = (const void *)(eth + 1);
		__be32 proto = htonl(IPPROTO_UDP);
		__be32 len;

		udph = (const void *)(ip6h + 1);
		len = htonl(ntohs(udph->len));
		sum = csum_add(&ip6h->saddr, sizeof(ip6h->saddr), sum);
		sum = csum_add(&ip6h->daddr, sizeof(ip6h->daddr), sum);
		sum = csum_add(&len, sizeof(len), sum);
		sum = csum_add(&proto, sizeof(proto), sum);
	}

	if (cfg->family == AF_INET)
		sum = csum_add(&udph->len, sizeof(udph->len), sum);

	return ~csum_fold(sum);
}

static size_t hdr_len(const struct config *cfg)
{
	size_t ip_len = cfg->family == AF_INET ? sizeof(struct iphdr) :
						       sizeof(struct ipv6hdr);

	return sizeof(struct ethhdr) + ip_len + sizeof(struct udphdr) +
	       sizeof(struct test_hdr);
}

static int build_pkt(const struct config *cfg, void *buf, __u32 seq)
{
	struct ipv6hdr *ip6h = NULL;
	struct iphdr *iph = NULL;
	struct test_hdr *test;
	struct udphdr *udph;
	struct ethhdr *eth;
	__u32 payload_len;
	__u8 *payload;
	__u32 i;

	if (cfg->packet_size < hdr_len(cfg) ||
	    cfg->packet_size > MAX_PACKET_SIZE)
		return -1;

	memset(buf, 0, cfg->packet_size);
	eth = buf;
	memcpy(eth->h_source, cfg->src_mac, ETH_ALEN);
	memcpy(eth->h_dest, cfg->dst_mac, ETH_ALEN);

	if (cfg->family == AF_INET) {
		eth->h_proto = htons(ETH_P_IP);
		iph = (void *)(eth + 1);
		iph->version = 4;
		iph->ihl = sizeof(*iph) / 4;
		iph->ttl = 64;
		iph->protocol = IPPROTO_UDP;
		iph->tot_len = htons(cfg->packet_size - sizeof(*eth));
		iph->saddr = cfg->src4.s_addr;
		iph->daddr = cfg->dst4.s_addr;
		iph->check = csum_fold(csum_add(iph, sizeof(*iph), 0));
		udph = (void *)(iph + 1);
	} else {
		__u16 ipv6_len;

		eth->h_proto = htons(ETH_P_IPV6);
		ip6h = (void *)(eth + 1);
		ip6h->version = 6;
		ip6h->hop_limit = 64;
		ip6h->nexthdr = IPPROTO_UDP;
		ipv6_len = cfg->packet_size - sizeof(*eth) - sizeof(*ip6h);
		ip6h->payload_len = htons(ipv6_len);
		ip6h->saddr = cfg->src6;
		ip6h->daddr = cfg->dst6;
		udph = (void *)(ip6h + 1);
	}

	udph->source = htons(cfg->port + 1);
	udph->dest = htons(cfg->port);
	udph->len = htons(cfg->packet_size - ((char *)udph - (char *)buf));

	test = (void *)(udph + 1);
	test->magic = htonl(TEST_MAGIC);
	test->seed = htonl(cfg->seed);
	test->seq = htonl(seq);
	test->len = htonl(cfg->packet_size);

	payload = (void *)(test + 1);
	payload_len = cfg->packet_size - (payload - (__u8 *)buf);
	for (i = 0; i < payload_len; i++)
		payload[i] = (__u8)(cfg->seed + seq + i);

	if (cfg->family == AF_INET)
		udph->check = udp4_csum(iph, udph);
	else
		udph->check = udp6_csum(ip6h, udph);

	return 0;
}

static struct test_hdr *get_test_hdr(const struct config *cfg, void *buf,
				     size_t len)
{
	size_t off = hdr_len(cfg) - sizeof(struct test_hdr);
	struct test_hdr *test;

	if (len < hdr_len(cfg))
		return NULL;

	test = buf + off;
	if (ntohl(test->magic) != TEST_MAGIC || ntohl(test->seed) != cfg->seed)
		return NULL;

	return test;
}

static enum packet_result verify_pkt(const struct config *cfg, const void *buf,
				     size_t len, __u32 *seq, void *expected)
{
	struct test_hdr *test;

	test = get_test_hdr(cfg, (void *)buf, len);
	if (!test)
		return PACKET_OTHER;
	if (ntohl(test->len) != len)
		return PACKET_INVALID;

	*seq = ntohl(test->seq);
	if (build_pkt(cfg, expected, *seq) || memcmp(expected, buf, len))
		return PACKET_INVALID;

	return PACKET_VALID;
}

static int raw_open(const struct config *cfg)
{
	struct sockaddr_ll addr = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
	};
	int ignore = 1;
	int fd;

	addr.sll_ifindex = if_nametoindex(cfg->ifname);
	if (!addr.sll_ifindex)
		return -errno;

	fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (fd < 0)
		return -errno;

	if (bind(fd, (void *)&addr, sizeof(addr))) {
		close(fd);
		return -errno;
	}

	setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &ignore,
		   sizeof(ignore));

	return fd;
}

static int ready_open(const struct config *cfg)
{
	struct sockaddr_storage addr = {};
	int one = 1;
	int fd;

	if (!cfg->ready_port)
		return -1;

	fd = socket(cfg->family, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (cfg->family == AF_INET) {
		struct sockaddr_in *addr4 = (void *)&addr;

		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(cfg->ready_port);
		addr4->sin_addr.s_addr = INADDR_ANY;
	} else {
		struct sockaddr_in6 *addr6 = (void *)&addr;

		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(cfg->ready_port);
		addr6->sin6_addr = in6addr_any;
	}

	if (bind(fd, (void *)&addr,
		 cfg->family == AF_INET ? sizeof(struct sockaddr_in) :
					  sizeof(struct sockaddr_in6)) ||
	    listen(fd, 1)) {
		close(fd);
		return -errno;
	}

	return fd;
}

static int ready_accept(int listen_fd, __u32 timeout_ms)
{
	struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
	int fd;
	int ret;

	ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0)
		return -errno;
	if (!ret)
		return -ETIMEDOUT;

	fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return -errno;

	close(fd);
	return 0;
}

static int ready_wait(const struct config *cfg)
{
	int listen_fd;
	int err;

	listen_fd = ready_open(cfg);
	if (listen_fd < 0)
		return listen_fd;

	ksft_ready();
	err = ready_accept(listen_fd, cfg->timeout_ms);
	close(listen_fd);
	return err;
}

static int peer_tx(const struct config *cfg)
{
	struct sockaddr_ll addr = {
		.sll_family = AF_PACKET,
		.sll_halen = ETH_ALEN,
	};
	void *packet;
	__u32 seq;
	int fd;

	fd = raw_open(cfg);
	if (fd < 0)
		return fail("packet socket: %s", strerror(-fd));

	addr.sll_ifindex = if_nametoindex(cfg->ifname);
	addr.sll_protocol = cfg->family == AF_INET ? htons(ETH_P_IP) :
						       htons(ETH_P_IPV6);
	memcpy(addr.sll_addr, cfg->dst_mac, ETH_ALEN);

	packet = malloc(cfg->packet_size);
	if (!packet) {
		close(fd);
		return fail("packet allocation failed");
	}
	if (cfg->ready_port) {
		int err = ready_wait(cfg);

		if (err) {
			free(packet);
			close(fd);
			return fail("ready wait: %s", strerror(-err));
		}
	}

	for (seq = 0; seq < cfg->count; seq++) {
		if (cfg->pause_ms && seq == cfg->pause_after)
			usleep(cfg->pause_ms * 1000);
		if (build_pkt(cfg, packet, seq)) {
			free(packet);
			close(fd);
			return fail("invalid packet length");
		}
		if (sendto(fd, packet, cfg->packet_size, 0, (void *)&addr,
			   sizeof(addr)) != cfg->packet_size) {
			free(packet);
			close(fd);
			return fail("sendto: %s", strerror(errno));
		}
		if (cfg->interval_us)
			usleep(cfg->interval_us);
	}

	free(packet);
	close(fd);
	return 0;
}

static int peer_rx(const struct config *cfg)
{
	__u64 deadline = now_ms() + cfg->timeout_ms;
	unsigned char *expected_packet;
	unsigned char *packet;
	bool stopping = false;
	__u32 expected = 0;
	int ready_fd = -1;
	int err = 0;
	int fd;

	packet = malloc(MAX_PACKET_SIZE);
	expected_packet = malloc(MAX_PACKET_SIZE);
	if (!packet || !expected_packet) {
		free(expected_packet);
		free(packet);
		return fail("packet allocation failed");
	}

	fd = raw_open(cfg);
	if (fd < 0) {
		err = fail("packet socket: %s", strerror(-fd));
		goto out;
	}

	if (cfg->ready_port) {
		ready_fd = ready_open(cfg);
		if (ready_fd < 0) {
			err = fail("ready socket: %s", strerror(-ready_fd));
			goto out;
		}
	}

	ksft_ready();
	while (expected < cfg->count || !cfg->count) {
		struct pollfd pfd[] = {
			{ .fd = fd, .events = POLLIN },
			{ .fd = ready_fd,
			  .events = stopping ? 0 : POLLIN },
		};
		ssize_t len;
		__u32 seq;
		int ret;

		if ((cfg->count || stopping) && now_ms() >= deadline)
			break;

		ret = poll(pfd, cfg->count ? 1 : 2, POLL_SLICE_MS);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0) {
			err = fail("poll: %s", strerror(errno));
			goto out;
		}
		if (!ret)
			continue;
		if (!cfg->count && !stopping &&
		    (pfd[1].revents & POLLIN)) {
			err = ready_accept(ready_fd, cfg->timeout_ms);
			if (err) {
				err = fail("stop signal: %s", strerror(-err));
				goto out;
			}
			stopping = true;
			/* Catch packets delayed past DUT completion. */
			deadline = now_ms() + cfg->timeout_ms;
		}
		if (!(pfd[0].revents & POLLIN))
			continue;

		len = recv(fd, packet, MAX_PACKET_SIZE, 0);
		if (len < 0)
			continue;
		ret = verify_pkt(cfg, packet, len, &seq, expected_packet);
		if (ret == PACKET_OTHER)
			continue;
		if (ret == PACKET_INVALID) {
			err = fail("packet content mismatch");
			goto out;
		}
		if (!cfg->count) {
			err = fail("unexpected packet %u", seq);
			goto out;
		}
		if (!cfg->allow_gaps && seq != expected) {
			err = fail("sequence mismatch: expected=%u actual=%u",
				   expected, seq);
			goto out;
		}
		if (cfg->allow_gaps && seq < expected)
			continue;

		expected = seq + 1;
		deadline = now_ms() + cfg->timeout_ms;
	}

	if (cfg->count && expected < cfg->count)
		err = fail("received %u of %u packets", expected, cfg->count);

out:
	if (ready_fd >= 0)
		close(ready_fd);
	if (fd >= 0)
		close(fd);
	free(expected_packet);
	free(packet);

	return err;
}

static __u32 prod_free(struct ring *ring, __u32 count)
{
	__u32 entries = ring->cached_cons - ring->cached_prod;

	if (entries >= count)
		return entries;

	ring->cached_cons = __atomic_load_n(ring->consumer, __ATOMIC_ACQUIRE);
	ring->cached_cons += ring->size;

	return ring->cached_cons - ring->cached_prod;
}

static __u32 prod_reserve(struct ring *ring, __u32 count, __u32 *idx)
{
	if (prod_free(ring, count) < count)
		return 0;

	*idx = ring->cached_prod;
	ring->cached_prod += count;

	return count;
}

static void prod_submit(struct ring *ring, __u32 count)
{
	__atomic_store_n(ring->producer, *ring->producer + count,
			 __ATOMIC_RELEASE);
}

static __u32 cons_peek(struct ring *ring, __u32 count, __u32 *idx)
{
	__u32 entries = ring->cached_prod - ring->cached_cons;
	__u32 producer;

	if (!entries) {
		producer = __atomic_load_n(ring->producer, __ATOMIC_ACQUIRE);
		ring->cached_prod = producer;
		entries = ring->cached_prod - ring->cached_cons;
	}
	if (!entries)
		return 0;

	*idx = ring->cached_cons;
	entries = entries > count ? count : entries;
	ring->cached_cons += entries;

	return entries;
}

static void cons_release(struct ring *ring, __u32 count)
{
	__atomic_store_n(ring->consumer, *ring->consumer + count,
			 __ATOMIC_RELEASE);
}

static bool ring_needs_wakeup(struct ring *ring)
{
	return __atomic_load_n(ring->flags, __ATOMIC_ACQUIRE) &
	       XDP_RING_NEED_WAKEUP;
}

static void wake_rx(struct xsk *xsk)
{
	struct pollfd pfd = { .fd = xsk->fd, .events = POLLIN };

	if (!ring_needs_wakeup(&xsk->fill))
		return;

	poll(&pfd, 1, 0);
}

static int map_ring(struct ring *ring, int fd, __u64 offset,
		    const struct xdp_ring_offset *off, __u32 count,
		    size_t elem_size, enum ring_type type)
{
	size_t flags_end;
	size_t desc_end;
	size_t map_sz;
	void *map;

	desc_end = off->desc + count * elem_size;
	flags_end = off->flags + sizeof(*ring->flags);
	map_sz = desc_end > flags_end ? desc_end : flags_end;
	map = mmap(NULL, map_sz, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_POPULATE, fd, offset);
	if (map == MAP_FAILED)
		return -errno;

	ring->producer = map + off->producer;
	ring->consumer = map + off->consumer;
	ring->flags = map + off->flags;
	ring->desc = map + off->desc;
	ring->cached_prod = 0;
	ring->cached_cons = 0;
	if (type == RING_PRODUCER)
		ring->cached_cons = count;
	ring->mask = count - 1;
	ring->size = count;
	ring->map = map;
	ring->map_sz = map_sz;

	return 0;
}

static int bpf_cmd(enum bpf_cmd cmd, union bpf_attr *attr)
{
	return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

static int map_get_fd(__u32 id)
{
	union bpf_attr attr = { .map_id = id };

	return bpf_cmd(BPF_MAP_GET_FD_BY_ID, &attr);
}

static int map_update(int fd, __u32 key, int value)
{
	union bpf_attr attr = {
		.map_fd = fd,
		.key = (__u64)(uintptr_t)&key,
		.value = (__u64)(uintptr_t)&value,
		.flags = BPF_ANY,
	};

	return bpf_cmd(BPF_MAP_UPDATE_ELEM, &attr);
}

static void map_delete(int fd, __u32 key)
{
	union bpf_attr attr = {
		.map_fd = fd,
		.key = (__u64)(uintptr_t)&key,
	};

	bpf_cmd(BPF_MAP_DELETE_ELEM, &attr);
}

static __u64 page_size(void)
{
	return sysconf(_SC_PAGESIZE);
}

static __u64 frame_base(const struct config *cfg, __u32 frame)
{
	__u64 stride = cfg->chunk_size;

	if (cfg->unaligned)
		stride = page_size() * (cfg->cross_page ? 2 : 1);

	return (__u64)frame * stride;
}

static __u64 fill_addr(const struct config *cfg, __u32 frame)
{
	__u64 offset = cfg->fill_offset;

	if (cfg->cross_page)
		offset = page_size() - XDP_PACKET_HEADROOM - cfg->headroom -
			 cfg->packet_size / 2;

	return frame_base(cfg, frame) + offset;
}

static int xsk_open(const struct config *cfg, struct xsk *xsk)
{
	__u32 rx_ring_size = cfg->rx_ring_size ?: cfg->ring_size;
	__u32 bind_flags = XDP_USE_NEED_WAKEUP;
	struct xdp_mmap_offsets off = {};
	__u32 ring_size = cfg->ring_size;
	struct xdp_options options = {};
	socklen_t optlen = sizeof(off);
	struct sockaddr_xdp addr = {};
	struct xdp_umem_reg reg = {};
	int err;

	bind_flags |= cfg->sw_checksum ? XDP_COPY : XDP_ZEROCOPY;
	if (cfg->use_sg)
		bind_flags |= XDP_USE_SG;

	xsk->umem_len = frame_base(cfg, cfg->frame_count - 1) +
			cfg->chunk_size;
	if (cfg->unaligned)
		xsk->umem_len += fill_addr(cfg, 0);
	xsk->umem = mmap(NULL, xsk->umem_len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (xsk->umem == MAP_FAILED)
		return -errno;

	xsk->frames = calloc(cfg->frame_count, sizeof(*xsk->frames));
	if (!xsk->frames)
		return -ENOMEM;

	xsk->fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (xsk->fd < 0)
		return -errno;

	reg.addr = (__u64)(uintptr_t)xsk->umem;
	reg.len = xsk->umem_len;
	reg.chunk_size = cfg->chunk_size;
	reg.headroom = cfg->headroom;
	if (cfg->unaligned)
		reg.flags |= XDP_UMEM_UNALIGNED_CHUNK_FLAG;
	if (cfg->tx_checksum || cfg->tx_timestamp || cfg->launch_time) {
		reg.flags |= XDP_UMEM_TX_METADATA_LEN;
		reg.tx_metadata_len = sizeof(struct xsk_tx_metadata);
	}
	if (cfg->sw_checksum)
		reg.flags |= XDP_UMEM_TX_SW_CSUM;

	if (setsockopt(xsk->fd, SOL_XDP, XDP_UMEM_REG, &reg, sizeof(reg)))
		return -errno;
	if (setsockopt(xsk->fd, SOL_XDP, XDP_UMEM_FILL_RING, &ring_size,
		       sizeof(ring_size)))
		return -errno;
	if (setsockopt(xsk->fd, SOL_XDP, XDP_UMEM_COMPLETION_RING,
		       &ring_size, sizeof(ring_size)))
		return -errno;
	if ((cfg->role == ROLE_DUT_RX || cfg->role == ROLE_PROBE) &&
	    setsockopt(xsk->fd, SOL_XDP, XDP_RX_RING, &rx_ring_size,
		       sizeof(rx_ring_size)))
		return -errno;
	if (cfg->role == ROLE_DUT_TX &&
	    setsockopt(xsk->fd, SOL_XDP, XDP_TX_RING, &ring_size,
		       sizeof(ring_size)))
		return -errno;

	if (getsockopt(xsk->fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen))
		return -errno;
	err = map_ring(&xsk->fill, xsk->fd, XDP_UMEM_PGOFF_FILL_RING,
		       &off.fr, ring_size, sizeof(__u64), RING_PRODUCER);
	if (err)
		return err;
	err = map_ring(&xsk->comp, xsk->fd, XDP_UMEM_PGOFF_COMPLETION_RING,
		       &off.cr, ring_size, sizeof(__u64), RING_CONSUMER);
	if (err)
		return err;
	if (cfg->role == ROLE_DUT_RX || cfg->role == ROLE_PROBE) {
		err = map_ring(&xsk->rx, xsk->fd, XDP_PGOFF_RX_RING, &off.rx,
			       rx_ring_size, sizeof(struct xdp_desc),
			       RING_CONSUMER);
		if (err)
			return err;
	}
	if (cfg->role == ROLE_DUT_TX) {
		err = map_ring(&xsk->tx, xsk->fd, XDP_PGOFF_TX_RING, &off.tx,
			       ring_size, sizeof(struct xdp_desc),
			       RING_PRODUCER);
		if (err)
			return err;
	}

	addr.sxdp_family = AF_XDP;
	addr.sxdp_ifindex = if_nametoindex(cfg->ifname);
	addr.sxdp_queue_id = cfg->queue;
	addr.sxdp_flags = bind_flags;
	if (!addr.sxdp_ifindex)
		return -errno;

	for (err = 0; err < BIND_ATTEMPTS; err++) {
		if (!bind(xsk->fd, (void *)&addr, sizeof(addr)))
			break;
		if (errno != EBUSY) {
			xsk->bind_errno = errno;
			return -errno;
		}
		usleep(BIND_RETRY_US);
	}
	if (err == BIND_ATTEMPTS) {
		xsk->bind_errno = EBUSY;
		return -EBUSY;
	}

	optlen = sizeof(options);
	if (getsockopt(xsk->fd, SOL_XDP, XDP_OPTIONS, &options, &optlen))
		return -errno;
	if (!cfg->sw_checksum && !(options.flags & XDP_OPTIONS_ZEROCOPY))
		return -EOPNOTSUPP;
	xsk->queue = cfg->queue;

	if (!cfg->map_id)
		return 0;

	xsk->map_fd = map_get_fd(cfg->map_id);
	if (xsk->map_fd < 0)
		return -errno;
	if (map_update(xsk->map_fd, cfg->queue, xsk->fd))
		return -errno;

	return 0;
}

static void xsk_close(struct xsk *xsk)
{
	if (xsk->map_fd >= 0) {
		map_delete(xsk->map_fd, xsk->queue);
		close(xsk->map_fd);
	}
	if (xsk->fill.map)
		munmap(xsk->fill.map, xsk->fill.map_sz);
	if (xsk->comp.map)
		munmap(xsk->comp.map, xsk->comp.map_sz);
	if (xsk->rx.map)
		munmap(xsk->rx.map, xsk->rx.map_sz);
	if (xsk->tx.map)
		munmap(xsk->tx.map, xsk->tx.map_sz);
	if (xsk->fd >= 0)
		close(xsk->fd);
	free(xsk->frames);
	if (xsk->umem && xsk->umem != MAP_FAILED)
		munmap(xsk->umem, xsk->umem_len);
}

static void poison_frame(const struct config *cfg, struct xsk *xsk,
			 __u32 frame)
{
	__u64 addr = cfg->unaligned ? fill_addr(cfg, frame) :
					frame_base(cfg, frame);

	memset(xsk->umem + addr, CANARY_BYTE, cfg->chunk_size);
}

static int fill_frames(const struct config *cfg, struct xsk *xsk,
		       __u32 first, __u32 count)
{
	__u64 *ring = xsk->fill.desc;
	__u32 idx;
	__u32 i;

	if (prod_reserve(&xsk->fill, count, &idx) != count)
		return fail("fill ring has fewer than %u entries", count);

	for (i = 0; i < count; i++) {
		__u32 frame;
		__u64 addr;

		frame = first + i;
		addr = fill_addr(cfg, frame);
		poison_frame(cfg, xsk, frame);
		xsk->frames[frame].addr = cfg->unaligned ? addr :
						 frame_base(cfg, frame);
		xsk->frames[frame].state = FRAME_FILL;
		ring[(idx + i) & xsk->fill.mask] = addr;
	}
	prod_submit(&xsk->fill, count);
	wake_rx(xsk);

	return 0;
}

static int frame_find(const struct config *cfg, struct xsk *xsk, __u64 addr)
{
	__u64 canonical;
	__u32 i;

	if (cfg->unaligned)
		canonical = addr & XSK_UNALIGNED_BUF_ADDR_MASK;
	else
		canonical = addr & ~((__u64)cfg->chunk_size - 1);

	for (i = 0; i < cfg->frame_count; i++) {
		if (xsk->frames[i].addr == canonical)
			return i;
	}

	return -1;
}

static __u64 data_addr(const struct config *cfg, __u64 addr)
{
	if (!cfg->unaligned)
		return addr;

	return (addr & XSK_UNALIGNED_BUF_ADDR_MASK) +
	       (addr >> XSK_UNALIGNED_BUF_OFFSET_SHIFT);
}

static int canary_ok(const struct config *cfg, struct xsk *xsk,
		     int frame, __u64 data, __u32 len,
		     enum canary_scope scope)
{
	__u64 start = xsk->frames[frame].addr;
	__u64 end = start + cfg->chunk_size;
	__u64 i;

	if (data < start || data + len > end)
		return -1;

	for (i = start; i < data; i++) {
		if (*((__u8 *)xsk->umem + i) != CANARY_BYTE)
			return -1;
	}
	if (scope == CANARY_PREFIX)
		return 0;
	for (i = data + len; i < end; i++) {
		if (*((__u8 *)xsk->umem + i) != CANARY_BYTE)
			return -1;
	}

	return 0;
}

static int tx_canary_ok(const struct config *cfg, struct xsk *xsk,
			int frame, __u64 data, __u32 len,
			enum tx_meta_scope meta_scope)
{
	__u32 used_len = len;
	__u64 used = data;

	if (meta_scope == TX_META_HEAD &&
	    (cfg->tx_checksum || cfg->tx_timestamp || cfg->launch_time)) {
		used -= sizeof(struct xsk_tx_metadata);
		used_len += sizeof(struct xsk_tx_metadata);
	}

	return canary_ok(cfg, xsk, frame, used, used_len, CANARY_FRAME);
}

static int recycle(const struct config *cfg, struct xsk *xsk,
		   const __u32 *frames, __u32 count)
{
	__u64 *ring = xsk->fill.desc;
	__u32 idx;
	__u32 i;

	if (prod_reserve(&xsk->fill, count, &idx) != count)
		return -1;

	for (i = 0; i < count; i++) {
		__u32 frame = frames[i];

		poison_frame(cfg, xsk, frame);
		xsk->frames[frame].state = FRAME_FILL;
		ring[(idx + i) & xsk->fill.mask] = fill_addr(cfg, frame);
	}
	prod_submit(&xsk->fill, count);
	wake_rx(xsk);

	return 0;
}

static int wait_rx(struct xsk *xsk, __u64 deadline)
{
	struct pollfd pfd = { .fd = xsk->fd, .events = POLLIN };
	int ret;

	while (now_ms() < deadline) {
		ret = poll(&pfd, 1, POLL_SLICE_MS);
		if (ret > 0)
			return 0;
		if (ret < 0 && errno != EINTR)
			return -errno;
	}

	return -ETIMEDOUT;
}

static int dut_rx(const struct config *cfg)
{
	struct xsk xsk = { .fd = -1, .map_fd = -1 };
	__u64 deadline = now_ms() + cfg->timeout_ms;
	struct xdp_statistics before = {};
	struct xdp_statistics after = {};
	unsigned char *expected_packet;
	unsigned char *packet;
	bool have_seq = false;
	__u32 held_count = 0;
	__u32 packet_len = 0;
	__u32 frag_count = 0;
	bool starved = false;
	bool layout = false;
	__u32 received = 0;
	__u32 last_seq = 0;
	socklen_t stat_len;
	__u32 *held;
	int err;

	err = xsk_open(cfg, &xsk);
	if (err) {
		if (xsk.bind_errno)
			fprintf(stderr, "bind_errno=%d\n", xsk.bind_errno);
		else
			fprintf(stderr, "setup_errno=%d\n", -err);
		xsk_close(&xsk);
		return EXIT_BIND;
	}
	if (fill_frames(cfg, &xsk, 0, cfg->frame_count)) {
		xsk_close(&xsk);
		return EXIT_FAIL;
	}
	stat_len = sizeof(before);
	if (getsockopt(xsk.fd, SOL_XDP, XDP_STATISTICS, &before, &stat_len)) {
		xsk_close(&xsk);
		return EXIT_FAIL;
	}

	packet = malloc(MAX_PACKET_SIZE);
	expected_packet = malloc(MAX_PACKET_SIZE);
	held = calloc(cfg->frame_count, sizeof(*held));
	if (!packet || !expected_packet || !held) {
		free(packet);
		free(expected_packet);
		free(held);
		xsk_close(&xsk);
		return EXIT_FAIL;
	}

	ksft_ready();
	if (cfg->hold_ms && !cfg->starve_after)
		usleep(cfg->hold_ms * 1000);
	while (received < cfg->count) {
		struct xdp_desc *descs = xsk.rx.desc;
		__u32 avail;
		__u32 idx;
		__u32 i;

		avail = cons_peek(&xsk.rx, MAX_BATCH, &idx);
		if (!avail) {
			if (wait_rx(&xsk, deadline)) {
				if (cfg->probe_layout && !received) {
					layout = true;
					goto out;
				}
				err = fail("packet timeout: got=%u want=%u",
					   received, cfg->count);
				goto out;
			}
			continue;
		}

		for (i = 0; i < avail; i++) {
			struct xdp_desc *desc = &descs[(idx + i) & xsk.rx.mask];
			__u64 addr = data_addr(cfg, desc->addr);
			enum canary_scope scope;
			__u32 seq;
			int frame;

			frame = frame_find(cfg, &xsk, desc->addr);
			if (frame < 0 ||
			    xsk.frames[frame].state != FRAME_FILL) {
				err = fail("invalid frame ownership: addr=%llu",
					   (unsigned long long)desc->addr);
				goto release;
			}
			xsk.frames[frame].state = FRAME_RX;
			if (held_count == cfg->frame_count) {
				err = fail("too many held RX frames");
				goto release;
			}
			held[held_count++] = frame;
			if (addr != xsk.frames[frame].addr + cfg->headroom +
			    XDP_PACKET_HEADROOM) {
				err = fail("unexpected data address: addr=%llu",
					   (unsigned long long)desc->addr);
				goto release;
			}

			scope = cfg->use_sg && !frag_count ? CANARY_PREFIX :
							     CANARY_FRAME;
			if (canary_ok(cfg, &xsk, frame, addr, desc->len,
				      scope)) {
				err = fail("frame %d canary changed", frame);
				goto release;
			}
			if (packet_len + desc->len > MAX_PACKET_SIZE) {
				err = fail("packet exceeds receive buffer");
				goto release;
			}

			memcpy(packet + packet_len, xsk.umem + addr, desc->len);
			packet_len += desc->len;
			frag_count++;
			if (desc->options & XDP_PKT_CONTD)
				continue;

			if (!cfg->use_sg && frag_count != 1) {
				err = fail("unexpected fragment count: %u",
					   frag_count);
				goto release;
			}
			if (cfg->use_sg && (frag_count < cfg->min_frags ||
					    frag_count > cfg->frags)) {
				err = fail("fragment count: %u..%u, got %u",
					   cfg->min_frags, cfg->frags,
					   frag_count);
				goto release;
			}
			if (verify_pkt(cfg, packet, packet_len, &seq,
				       expected_packet) != PACKET_VALID) {
				err = fail("packet content mismatch: index=%u",
					   received);
				goto release;
			}
			if (!cfg->allow_gaps && seq != received) {
				err = fail("sequence mismatch: want=%u got=%u",
					   received, seq);
				goto release;
			}
			if (cfg->allow_gaps && have_seq && seq <= last_seq) {
				err = fail("sequence order: old=%u new=%u",
					   last_seq, seq);
				goto release;
			}
			last_seq = seq;
			have_seq = true;
			received++;
			deadline = now_ms() + cfg->timeout_ms;
			packet_len = 0;
			frag_count = 0;
		}

release:
		cons_release(&xsk.rx, avail);
		if (err)
			goto out;

		if (!starved && cfg->starve_after &&
		    received < cfg->starve_after)
			continue;
		if (!starved && cfg->starve_after) {
			usleep(cfg->hold_ms * 1000);
			if (!ring_needs_wakeup(&xsk.fill)) {
				err = fail("fill ring did not request wakeup");
				goto out;
			}
			starved = true;
		}
		while (held_count) {
			__u32 count = held_count;

			if (starved && cfg->refill_batch &&
			    count > cfg->refill_batch)
				count = cfg->refill_batch;
			if (recycle(cfg, &xsk, held, count)) {
				err = fail("fill ring recycle failed");
				goto out;
			}
			held_count -= count;
			memmove(held, held + count,
				held_count * sizeof(*held));
		}
	}

	stat_len = sizeof(after);
	if (getsockopt(xsk.fd, SOL_XDP, XDP_STATISTICS, &after, &stat_len)) {
		err = fail("XDP_STATISTICS: %s", strerror(errno));
		goto out;
	}
	if (cfg->hold_ms && !cfg->starve_after &&
	    after.rx_ring_full <= before.rx_ring_full) {
		err = fail("rx_ring_full did not increase");
		goto out;
	}
	if (cfg->starve_after && after.rx_fill_ring_empty_descs <=
	    before.rx_fill_ring_empty_descs) {
		err = fail("rx_fill_ring_empty_descs did not increase");
		goto out;
	}

	err = 0;
out:
	free(held);
	free(expected_packet);
	free(packet);
	xsk_close(&xsk);
	if (layout)
		return EXIT_LAYOUT;

	return err ? EXIT_FAIL : EXIT_OK;
}

static bool has_tx_meta(const struct config *cfg)
{
	return cfg->tx_checksum || cfg->tx_timestamp || cfg->launch_time;
}

static __u32 tx_data_off(const struct config *cfg)
{
	__u32 offset = cfg->tx_offset;

	if (has_tx_meta(cfg) && !offset)
		offset = sizeof(struct xsk_tx_metadata);

	return offset;
}

static __u64 tx_addr(const struct config *cfg, __u32 frame)
{
	__u64 base = frame_base(cfg, frame);
	__u32 offset = tx_data_off(cfg);

	if (!cfg->unaligned)
		return base + offset;

	return base | ((__u64)offset <<
		       XSK_UNALIGNED_BUF_OFFSET_SHIFT);
}

static int hwtstamp_set(const char *ifname, struct hwtstamp_config *saved)
{
	struct hwtstamp_config config = {
		.tx_type = HWTSTAMP_TX_ON,
		.rx_filter = HWTSTAMP_FILTER_NONE,
	};
	struct ifreq ifr = {};
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_data = (void *)saved;
	if (ioctl(fd, SIOCGHWTSTAMP, &ifr)) {
		close(fd);
		return -errno;
	}

	ifr.ifr_data = (void *)&config;
	if (ioctl(fd, SIOCSHWTSTAMP, &ifr)) {
		close(fd);
		return -errno;
	}

	close(fd);
	return 0;
}

static void hwtstamp_restore(const char *ifname,
			     struct hwtstamp_config *config)
{
	struct ifreq ifr = {};
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;

	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_data = (void *)config;
	ioctl(fd, SIOCSHWTSTAMP, &ifr);
	close(fd);
}

static int phc_open(const char *ifname)
{
	struct ethtool_ts_info info = { .cmd = ETHTOOL_GET_TS_INFO };
	struct ifreq ifr = { .ifr_data = (void *)&info };
	char path[32];
	int sock;
	int fd;

	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock < 0)
		return -errno;
	if (ioctl(sock, SIOCETHTOOL, &ifr)) {
		close(sock);
		return -errno;
	}
	close(sock);
	if (info.phc_index < 0)
		return -EOPNOTSUPP;

	snprintf(path, sizeof(path), "/dev/ptp%d", info.phc_index);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	return fd;
}

static __u64 phc_now(int fd)
{
	struct timespec ts;

	if (clock_gettime(FD_TO_CLOCKID(fd), &ts))
		return 0;

	return (__u64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static __u64 tx_meta_set(const struct config *cfg, void *data, int phc_fd)
{
	struct xsk_tx_metadata *meta = data - sizeof(*meta);
	struct udphdr *udph;
	__u64 launch = 0;

	memset(meta, 0, sizeof(*meta));
	if (cfg->tx_timestamp)
		meta->flags |= XDP_TXMD_FLAGS_TIMESTAMP;
	if (cfg->launch_time) {
		launch = phc_now(phc_fd) + LAUNCH_DELAY_NS;
		meta->flags |= XDP_TXMD_FLAGS_LAUNCH_TIME;
		meta->request.launch_time = launch;
	}
	if (!cfg->tx_checksum)
		return launch;

	if (cfg->family == AF_INET)
		udph = data + sizeof(struct ethhdr) + sizeof(struct iphdr);
	else
		udph = data + sizeof(struct ethhdr) + sizeof(struct ipv6hdr);

	udph->check = udp_pseudo(cfg, data);
	meta->flags |= XDP_TXMD_FLAGS_CHECKSUM;
	meta->request.csum_start = (void *)udph - data;
	meta->request.csum_offset = offsetof(struct udphdr, check);

	return launch;
}

static int kick_tx(struct xsk *xsk)
{
	int ret;

	if (!ring_needs_wakeup(&xsk->tx))
		return 0;

	xsk->tx_wakeups++;
	ret = sendto(xsk->fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (ret >= 0 || errno == EAGAIN || errno == EBUSY || errno == ENOBUFS)
		return 0;

	return -errno;
}

static int wait_comp(const struct config *cfg, struct xsk *xsk,
		     const __u64 *expected, __u32 count, __u64 deadline,
		     __u64 *last_timestamp, int phc_fd, __u64 launch_time)
{
	__u64 *ring = xsk->comp.desc;
	__u32 done = 0;

	while (done < count) {
		__u32 avail;
		__u32 idx;
		__u32 i;

		avail = cons_peek(&xsk->comp, count - done, &idx);
		if (!avail) {
			if (now_ms() >= deadline)
				return -ETIMEDOUT;
			kick_tx(xsk);
			usleep(1000);
			continue;
		}

		for (i = 0; i < avail; i++) {
			__u64 addr = ring[(idx + i) & xsk->comp.mask];
			struct xsk_tx_metadata *meta;
			__u64 timestamp;

			if (addr != expected[done + i])
				return -EINVAL;
			if (!cfg->tx_timestamp)
				continue;

			meta = xsk->umem + data_addr(cfg, addr) - sizeof(*meta);
			timestamp = meta->completion.tx_timestamp;
			if (!timestamp || timestamp < *last_timestamp)
				return -EINVAL;
			*last_timestamp = timestamp;
		}
		cons_release(&xsk->comp, avail);
		done += avail;
	}
	if (cfg->launch_time && phc_now(phc_fd) < launch_time)
		return -ETIME;

	return 0;
}

static int wait_invalid(struct xsk *xsk, __u64 expected,
			__u64 invalid_before, __u64 deadline)
{
	__u64 *ring = xsk->comp.desc;
	struct xdp_statistics stats;
	socklen_t stat_len;
	__u32 idx;
	int err;

	while (now_ms() < deadline) {
		if (cons_peek(&xsk->comp, 1, &idx)) {
			if (ring[idx & xsk->comp.mask] != expected)
				return -EINVAL;
			cons_release(&xsk->comp, 1);
		}

		stat_len = sizeof(stats);
		if (getsockopt(xsk->fd, SOL_XDP, XDP_STATISTICS, &stats,
			       &stat_len))
			return -errno;
		if (stats.tx_invalid_descs > invalid_before)
			return 0;
		err = kick_tx(xsk);
		if (err)
			return err;
		usleep(1000);
	}

	return -ETIMEDOUT;
}

static int tx_full(const struct config *cfg, struct xsk *xsk,
		   unsigned char *packet)
{
	__u64 deadline = now_ms() + cfg->timeout_ms;
	struct xdp_desc *ring = xsk->tx.desc;
	__u64 expected[MAX_BATCH];
	__u64 last_timestamp = 0;
	__u64 actual;
	__u64 addr;
	__u32 idx;
	__u32 i;

	if (prod_reserve(&xsk->tx, cfg->ring_size, &idx) != cfg->ring_size)
		return fail("cannot reserve the full TX ring");

	for (i = 0; i < cfg->ring_size; i++) {
		struct xdp_desc *desc = &ring[(idx + i) & xsk->tx.mask];

		addr = tx_addr(cfg, i);
		actual = data_addr(cfg, addr);
		if (build_pkt(cfg, packet, i))
			return fail("invalid packet length");
		memset(xsk->umem + frame_base(cfg, i), CANARY_BYTE,
		       cfg->chunk_size);
		memcpy(xsk->umem + actual, packet, cfg->packet_size);
		desc->addr = addr;
		desc->len = cfg->packet_size;
		desc->options = 0;
		expected[i] = addr;
	}
	prod_submit(&xsk->tx, cfg->ring_size);
	if (kick_tx(xsk))
		return fail("TX kick failed");
	if (wait_comp(cfg, xsk, expected, cfg->ring_size, deadline,
		      &last_timestamp, -1, 0))
		return fail("TX-ring completion failed");

	if (prod_reserve(&xsk->tx, 1, &idx) != 1)
		return fail("TX ring did not recover");
	if (build_pkt(cfg, packet, cfg->ring_size))
		return fail("invalid packet length");
	addr = tx_addr(cfg, 0);
	actual = data_addr(cfg, addr);
	memcpy(xsk->umem + actual, packet, cfg->packet_size);
	ring[idx & xsk->tx.mask].addr = addr;
	ring[idx & xsk->tx.mask].len = cfg->packet_size;
	ring[idx & xsk->tx.mask].options = 0;
	expected[0] = tx_addr(cfg, 0);
	prod_submit(&xsk->tx, 1);
	if (kick_tx(xsk))
		return fail("TX recovery kick failed");
	if (wait_comp(cfg, xsk, expected, 1, deadline, &last_timestamp,
		      -1, 0))
		return fail("TX recovery completion failed");

	return 0;
}

static int dut_tx(const struct config *cfg)
{
	struct xsk xsk = { .fd = -1, .map_fd = -1 };
	struct hwtstamp_config hwtstamp = {};
	struct xdp_statistics before = {};
	struct xdp_statistics after = {};
	bool restore_hwtstamp = false;
	__u64 last_timestamp = 0;
	unsigned char *packet;
	socklen_t stat_len;
	int phc_fd = -1;
	__u32 seq;
	int err;

	if (cfg->tx_timestamp) {
		err = hwtstamp_set(cfg->ifname, &hwtstamp);
		if (err) {
			fail("timestamp setup: %s", strerror(-err));
			if (err == -EOPNOTSUPP || err == -ENOTTY)
				return EXIT_UNSUPPORTED;
			return EXIT_FAIL;
		}
		restore_hwtstamp = true;
	}
	err = xsk_open(cfg, &xsk);
	if (err) {
		if (xsk.bind_errno)
			fprintf(stderr, "bind_errno=%d\n", xsk.bind_errno);
		else
			fprintf(stderr, "setup_errno=%d\n", -err);
		xsk_close(&xsk);
		if (restore_hwtstamp)
			hwtstamp_restore(cfg->ifname, &hwtstamp);
		return EXIT_BIND;
	}
	if (cfg->launch_time) {
		phc_fd = phc_open(cfg->ifname);
		if (phc_fd < 0) {
			err = fail("PHC open: %s", strerror(-phc_fd));
			goto out_close;
		}
	}
	stat_len = sizeof(before);
	if (getsockopt(xsk.fd, SOL_XDP, XDP_STATISTICS, &before, &stat_len)) {
		err = fail("XDP_STATISTICS: %s", strerror(errno));
		goto out_close;
	}

	packet = malloc(cfg->packet_size);
	if (!packet) {
		err = fail("packet allocation failed");
		goto out_close;
	}
	ksft_ready();
	if (cfg->hold_ms)
		usleep(cfg->hold_ms * 1000);
	if (cfg->tx_full) {
		err = tx_full(cfg, &xsk, packet);
		goto out;
	}

	for (seq = 0; seq < cfg->count; seq++) {
		__u64 deadline = now_ms() + cfg->timeout_ms;
		struct xdp_desc *ring = xsk.tx.desc;
		__u32 expected_len[MAX_BATCH];
		__u32 left = cfg->packet_size;
		__u64 expected[MAX_BATCH];
		__u64 launch_time = 0;
		__u32 copied = 0;
		__u32 idx = 0;
		__u32 i;

		if (build_pkt(cfg, packet, seq)) {
			err = fail("invalid packet length");
			goto out;
		}
		while (prod_reserve(&xsk.tx, cfg->frags, &idx) != cfg->frags) {
			if (now_ms() >= deadline) {
				err = fail("TX ring reserve timed out");
				goto out;
			}
			kick_tx(&xsk);
			usleep(1000);
		}

		for (i = 0; i < cfg->frags; i++) {
			struct xdp_desc *desc = &ring[(idx + i) & xsk.tx.mask];
			__u32 frames_left = cfg->frags - i;
			__u64 actual;
			__u32 frame;
			__u64 addr;
			__u32 len;

			frame = (seq * cfg->frags + i) % cfg->frame_count;
			addr = tx_addr(cfg, frame);
			actual = data_addr(cfg, addr);
			if (!i && cfg->frags > 1)
				len = hdr_len(cfg);
			else
				len = (left + frames_left - 1) / frames_left;

			if (xsk.frames[frame].state != FRAME_FREE) {
				err = fail("TX frame %u is still owned", frame);
				goto out;
			}
			if (len > cfg->chunk_size - tx_data_off(cfg)) {
				err = fail("fragment does not fit in chunk");
				goto out;
			}
			memset(xsk.umem + frame_base(cfg, frame), CANARY_BYTE,
			       cfg->chunk_size);
			xsk.frames[frame].addr = frame_base(cfg, frame);
			xsk.frames[frame].state = FRAME_TX;
			memcpy(xsk.umem + actual, packet + copied, len);
			if (!i && (cfg->tx_checksum || cfg->tx_timestamp ||
				   cfg->launch_time)) {
				void *data = xsk.umem + actual;

				launch_time = tx_meta_set(cfg, data, phc_fd);
			}
			desc->addr = addr;
			desc->len = len;
			desc->options = i + 1 < cfg->frags ? XDP_PKT_CONTD : 0;
			if (!i && (cfg->tx_checksum || cfg->tx_timestamp ||
				   cfg->launch_time))
				desc->options |= XDP_TX_METADATA;
			expected[i] = addr;
			expected_len[i] = len;
			left -= len;
			copied += len;
		}

		if (cfg->invalid_tx)
			ring[idx & xsk.tx.mask].len = cfg->chunk_size -
						      tx_data_off(cfg) + 1;

		if (cfg->split_submit) {
			__u32 first = cfg->frags / 2;

			prod_submit(&xsk.tx, first);
			if (kick_tx(&xsk)) {
				err = fail("split TX kick failed");
				goto out;
			}
			prod_submit(&xsk.tx, cfg->frags - first);
		} else {
			prod_submit(&xsk.tx, cfg->frags);
		}
		if (kick_tx(&xsk)) {
			err = fail("TX kick failed");
			goto out;
		}
		if (cfg->invalid_tx) {
			__u64 actual = data_addr(cfg, expected[0]);
			__u32 len = expected_len[0];

			/* Completion is optional for an invalid descriptor. */
			if (wait_invalid(&xsk, expected[0],
					 before.tx_invalid_descs, deadline)) {
				err = fail("invalid TX not consumed");
				goto out;
			}
			if (tx_canary_ok(cfg, &xsk, 0, actual, len,
					 TX_META_HEAD)) {
				err = fail("invalid TX frame canary changed");
				goto out;
			}
			xsk.frames[0].state = FRAME_FREE;
			continue;
		}
		if (wait_comp(cfg, &xsk, expected, cfg->frags, deadline,
			      &last_timestamp, phc_fd, launch_time)) {
			err = fail("TX completion mismatch or timeout");
			goto out;
		}
		for (i = 0; i < cfg->frags; i++) {
			struct xdp_desc *desc = &ring[(idx + i) & xsk.tx.mask];
			__u32 frame;
			__u64 addr;

			frame = (seq * cfg->frags + i) % cfg->frame_count;
			addr = data_addr(cfg, desc->addr);

			if (tx_canary_ok(cfg, &xsk, frame, addr,
					 expected_len[i],
					 i ? TX_META_NONE : TX_META_HEAD)) {
				err = fail("TX frame %u canary changed", frame);
				goto out;
			}
			xsk.frames[frame].state = FRAME_FREE;
		}
	}

	stat_len = sizeof(after);
	if (getsockopt(xsk.fd, SOL_XDP, XDP_STATISTICS, &after, &stat_len)) {
		err = fail("XDP_STATISTICS: %s", strerror(errno));
		goto out;
	}
	if (cfg->invalid_tx &&
	    after.tx_invalid_descs <= before.tx_invalid_descs) {
		err = fail("tx_invalid_descs did not increase");
		goto out;
	}
	if (!xsk.tx_wakeups) {
		err = fail("TX ring never requested wakeup");
		goto out;
	}

	err = 0;
out:
	free(packet);
out_close:
	if (phc_fd >= 0)
		close(phc_fd);
	xsk_close(&xsk);
	if (restore_hwtstamp)
		hwtstamp_restore(cfg->ifname, &hwtstamp);
	return err ? EXIT_FAIL : EXIT_OK;
}

static int probe(const struct config *cfg)
{
	struct xsk xsk = { .fd = -1, .map_fd = -1 };
	int err;

	err = xsk_open(cfg, &xsk);
	if (err) {
		if (xsk.bind_errno)
			fprintf(stderr, "bind_errno=%d\n", xsk.bind_errno);
		else
			fprintf(stderr, "setup_errno=%d\n", -err);
	}
	xsk_close(&xsk);

	return err ? EXIT_BIND : EXIT_OK;
}

static void shared_close(struct xsk *xsk)
{
	if (xsk->map_fd >= 0) {
		map_delete(xsk->map_fd, xsk->queue);
		close(xsk->map_fd);
	}
	if (xsk->fill.map)
		munmap(xsk->fill.map, xsk->fill.map_sz);
	if (xsk->comp.map)
		munmap(xsk->comp.map, xsk->comp.map_sz);
	if (xsk->rx.map)
		munmap(xsk->rx.map, xsk->rx.map_sz);
	if (xsk->fd >= 0)
		close(xsk->fd);
}

static int shared_open(const struct config *cfg, const struct xsk *owner,
		       struct xsk *shared)
{
	struct xdp_mmap_offsets off = {};
	__u32 ring_size = cfg->ring_size;
	struct xdp_options options = {};
	socklen_t optlen = sizeof(off);
	struct sockaddr_xdp addr = {};
	int err;

	shared->fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (shared->fd < 0)
		return -errno;
	if (setsockopt(shared->fd, SOL_XDP, XDP_UMEM_FILL_RING,
		       &ring_size, sizeof(ring_size)) ||
	    setsockopt(shared->fd, SOL_XDP, XDP_UMEM_COMPLETION_RING,
		       &ring_size, sizeof(ring_size)) ||
	    setsockopt(shared->fd, SOL_XDP, XDP_RX_RING, &ring_size,
		       sizeof(ring_size)))
		return -errno;

	if (getsockopt(shared->fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen))
		return -errno;
	err = map_ring(&shared->fill, shared->fd, XDP_UMEM_PGOFF_FILL_RING,
		       &off.fr, ring_size, sizeof(__u64), RING_PRODUCER);
	if (err)
		return err;
	err = map_ring(&shared->comp, shared->fd,
		       XDP_UMEM_PGOFF_COMPLETION_RING, &off.cr, ring_size,
		       sizeof(__u64), RING_CONSUMER);
	if (err)
		return err;
	err = map_ring(&shared->rx, shared->fd, XDP_PGOFF_RX_RING, &off.rx,
		       ring_size, sizeof(struct xdp_desc), RING_CONSUMER);
	if (err)
		return err;

	addr.sxdp_family = AF_XDP;
	addr.sxdp_flags = XDP_SHARED_UMEM;
	addr.sxdp_ifindex = if_nametoindex(cfg->ifname);
	addr.sxdp_queue_id = cfg->queue2;
	addr.sxdp_shared_umem_fd = owner->fd;
	if (bind(shared->fd, (void *)&addr, sizeof(addr))) {
		shared->bind_errno = errno;
		return -errno;
	}

	optlen = sizeof(options);
	if (getsockopt(shared->fd, SOL_XDP, XDP_OPTIONS, &options, &optlen))
		return -errno;
	if (!(options.flags & XDP_OPTIONS_ZEROCOPY))
		return -EOPNOTSUPP;
	shared->queue = cfg->queue2;
	if (cfg->map_id) {
		shared->map_fd = map_get_fd(cfg->map_id);
		if (shared->map_fd < 0)
			return -errno;
		if (map_update(shared->map_fd, cfg->queue2, shared->fd))
			return -errno;
	}

	return 0;
}

static int shared_recv(const struct config *cfg, struct xsk *xsk,
		       __u64 deadline, void *packet, void *expected)
{
	struct xdp_desc *ring = xsk->rx.desc;
	__u32 held[MAX_BATCH];
	__u32 held_count = 0;
	__u32 packet_len = 0;
	__u32 frag_count = 0;
	bool starved = false;
	__u32 received = 0;
	int err = 0;

	while (received < cfg->count) {
		__u32 avail;
		__u32 idx;
		__u32 i;

		avail = cons_peek(&xsk->rx, MAX_BATCH, &idx);
		if (!avail) {
			if (wait_rx(xsk, deadline))
				return fail("shared UMEM receive timeout");
			continue;
		}

		for (i = 0; i < avail; i++) {
			struct xdp_desc *desc = &ring[(idx + i) & xsk->rx.mask];
			__u64 addr = data_addr(cfg, desc->addr);
			enum packet_result result;
			enum canary_scope scope;
			__u32 seq;
			int frame;

			frame = frame_find(cfg, xsk, desc->addr);
			if (frame < 0 ||
			    xsk->frames[frame].state != FRAME_FILL) {
				err = fail("shared UMEM ownership failure");
				goto release;
			}
			if (addr != xsk->frames[frame].addr + cfg->headroom +
			    XDP_PACKET_HEADROOM) {
				err = fail("shared UMEM address mismatch");
				goto release;
			}
			scope = cfg->use_sg && !frag_count ? CANARY_PREFIX :
							     CANARY_FRAME;
			if (packet_len + desc->len > MAX_PACKET_SIZE ||
			    canary_ok(cfg, xsk, frame, addr, desc->len,
				      scope)) {
				err = fail("shared UMEM descriptor failure");
				goto release;
			}

			xsk->frames[frame].state = FRAME_RX;
			if (held_count == MAX_BATCH) {
				err = fail("too many shared RX frames");
				goto release;
			}
			held[held_count++] = frame;
			memcpy(packet + packet_len, xsk->umem + addr,
			       desc->len);
			packet_len += desc->len;
			frag_count++;
			if (desc->options & XDP_PKT_CONTD)
				continue;
			if (frag_count < cfg->min_frags ||
			    frag_count > cfg->frags) {
				err = fail("shared UMEM fragment mismatch");
				goto release;
			}
			result = verify_pkt(cfg, packet, packet_len, &seq,
					    expected);
			if (result != PACKET_VALID || seq != received) {
				err = fail("shared UMEM packet mismatch");
				goto release;
			}

			received++;
			deadline = now_ms() + cfg->timeout_ms;
			packet_len = 0;
			frag_count = 0;
		}

release:
		cons_release(&xsk->rx, avail);
		if (err)
			return err;

		if (!starved && cfg->starve_after &&
		    received < cfg->starve_after)
			continue;
		if (!starved && cfg->starve_after) {
			usleep(cfg->hold_ms * 1000);
			if (!ring_needs_wakeup(&xsk->fill))
				return fail("shared FILL wakeup missing");
			starved = true;
		}
		if (recycle(cfg, xsk, held, held_count))
			return fail("shared UMEM recycle failed");
		held_count = 0;
	}

	return 0;
}

static int shared(const struct config *cfg)
{
	struct xsk second = { .fd = -1, .map_fd = -1 };
	struct xsk owner = { .fd = -1, .map_fd = -1 };
	struct config second_cfg = *cfg;
	enum exit_code code = EXIT_BIND;
	struct config owner_cfg = *cfg;
	unsigned char *expected = NULL;
	unsigned char *packet = NULL;
	__u64 deadline;
	__u32 split;
	int err;

	owner_cfg.role = ROLE_DUT_RX;
	err = xsk_open(&owner_cfg, &owner);
	if (!err)
		err = shared_open(cfg, &owner, &second);
	if (err)
		goto out;
	code = EXIT_FAIL;
	packet = malloc(MAX_PACKET_SIZE);
	expected = malloc(MAX_PACKET_SIZE);
	if (!packet || !expected) {
		err = -ENOMEM;
		goto out;
	}

	second.umem = owner.umem;
	second.frames = owner.frames;
	second_cfg.port = cfg->port2;
	split = cfg->frame_count / 2;
	err = fill_frames(cfg, &owner, 0, split);
	if (!err)
		err = fill_frames(cfg, &second, split,
				  cfg->frame_count - split);
	if (err)
		goto out;

	ksft_ready();
	deadline = now_ms() + cfg->timeout_ms;
	err = shared_recv(&owner_cfg, &owner, deadline, packet, expected);
	if (!err) {
		deadline = now_ms() + cfg->timeout_ms;
		err = shared_recv(&second_cfg, &second, deadline, packet,
				  expected);
	}

out:
	if (err && code == EXIT_BIND && owner.bind_errno)
		fprintf(stderr, "owner_bind_errno=%d\n", owner.bind_errno);
	else if (err && code == EXIT_BIND && second.bind_errno)
		fprintf(stderr, "shared_bind_errno=%d\n", second.bind_errno);
	else if (err)
		fprintf(stderr, "shared_errno=%d\n", -err);
	free(expected);
	free(packet);
	shared_close(&second);
	xsk_close(&owner);

	return err ? code : EXIT_OK;
}

int main(int argc, char **argv)
{
	struct config cfg = {
		.timeout_ms = DEFAULT_TIMEOUT_MS,
		.packet_size = DEFAULT_PACKET_SIZE,
		.frame_count = DEFAULT_FRAME_COUNT,
		.chunk_size = DEFAULT_FRAME_SIZE,
		.ring_size = DEFAULT_RING_SIZE,
		.family = AF_INET6,
		.count = DEFAULT_COUNT,
		.seed = 1,
		.frags = 1,
		.min_frags = 1,
	};

	if (parse_args(argc, argv, &cfg)) {
		usage(argv[0]);
		return EXIT_FAIL;
	}
	if (!cfg.rx_ring_size)
		cfg.rx_ring_size = cfg.ring_size;
	if (!cfg.frame_count || !cfg.ring_size ||
	    cfg.frame_count > cfg.ring_size || cfg.frags > MAX_BATCH ||
	    cfg.frags > cfg.frame_count || !cfg.frags || !cfg.min_frags ||
	    cfg.min_frags > cfg.frags ||
	    (cfg.ring_size & (cfg.ring_size - 1)) ||
	    (cfg.rx_ring_size & (cfg.rx_ring_size - 1)) ||
	    (cfg.split_submit && cfg.frags < 2) ||
	    (cfg.invalid_tx && (cfg.count != 1 || cfg.frags != 1)) ||
	    tx_data_off(&cfg) > cfg.chunk_size ||
	    (has_tx_meta(&cfg) && cfg.tx_offset &&
	     cfg.tx_offset < sizeof(struct xsk_tx_metadata)) ||
	    (cfg.tx_full && (cfg.ring_size > MAX_BATCH ||
	     cfg.frame_count != cfg.ring_size ||
	     cfg.count != cfg.ring_size + 1 ||
	     cfg.packet_size > cfg.chunk_size - tx_data_off(&cfg))) ||
	    (cfg.role == ROLE_PEER_RX && !cfg.count && !cfg.ready_port) ||
	    (cfg.cross_page && (!cfg.unaligned ||
	     cfg.packet_size / 2 + cfg.headroom + XDP_PACKET_HEADROOM >=
	     page_size()))) {
		fail("invalid ring or frame count");
		return EXIT_FAIL;
	}

	switch (cfg.role) {
	case ROLE_DUT_RX:
		return dut_rx(&cfg);
	case ROLE_DUT_TX:
		return dut_tx(&cfg);
	case ROLE_PEER_RX:
		return peer_rx(&cfg) ? EXIT_FAIL : EXIT_OK;
	case ROLE_PEER_TX:
		return peer_tx(&cfg) ? EXIT_FAIL : EXIT_OK;
	case ROLE_PROBE:
		return probe(&cfg);
	case ROLE_SHARED:
		return shared(&cfg);
	case ROLE_NONE:
		break;
	}

	return EXIT_FAIL;
}
