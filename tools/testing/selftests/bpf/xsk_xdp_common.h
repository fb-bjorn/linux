/* SPDX-License-Identifier: GPL-2.0 */

#ifndef XSK_XDP_COMMON_H_
#define XSK_XDP_COMMON_H_

#define MAX_SOCKETS 2
#define PKT_HDR_ALIGN (sizeof(struct ethhdr) + 2) /* Just to align the data in the packet */
#define XSK_PASS_META 0x58534b50
#define XSK_PASS_MARK 0xa5a5

struct xdp_info {
	__u64 count;
} __attribute__((aligned(32)));

#endif /* XSK_XDP_COMMON_H_ */
