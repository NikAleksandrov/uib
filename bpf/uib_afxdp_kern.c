// SPDX-License-Identifier: GPL-2.0-only
/* XSKMAP redirect design follows the Linux kernel AF_XDP documentation. */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, 256);
	__type(key, __u32);
	__type(value, __u32);
} xsks_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u16);
} uib_afxdp_ports SEC(".maps");

SEC("xdp")
int uib_afxdp_redirect(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct ethhdr *eth = data;
	struct udphdr *udp;
	struct iphdr *ip;
	__u32 ip_length;
	__u32 key = 0;
	__u16 *port;

	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return XDP_PASS;

	ip = (void *)(eth + 1);
	if ((void *)(ip + 1) > data_end)
		return XDP_PASS;
	if (ip->protocol != IPPROTO_UDP)
		return XDP_PASS;

	ip_length = (__u32)ip->ihl * 4;
	if (ip_length < sizeof(*ip))
		return XDP_PASS;
	if ((void *)ip + ip_length > data_end)
		return XDP_PASS;

	udp = (void *)ip + ip_length;
	if ((void *)(udp + 1) > data_end)
		return XDP_PASS;

	port = bpf_map_lookup_elem(&uib_afxdp_ports, &key);
	if (!port || udp->dest != *port)
		return XDP_PASS;

	return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char LICENSE[] SEC("license") = "GPL";
