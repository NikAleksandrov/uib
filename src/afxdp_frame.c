/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "uib/afxdp_frame.h"

int uib_afxdp_parse_mac(const char *text, uint8_t mac[6])
{
	unsigned int byte[6];
	char trailing;
	int i;

	if (!text || !mac ||
	    sscanf(text, "%x:%x:%x:%x:%x:%x%c",
		   &byte[0], &byte[1], &byte[2], &byte[3], &byte[4],
		   &byte[5], &trailing) != 6)
		return -EINVAL;
	for (i = 0; i < 6; i++) {
		if (byte[i] > UINT8_MAX)
			return -EINVAL;
		mac[i] = (uint8_t)byte[i];
	}
	return 0;
}

uint16_t uib_inet_csum(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i + 1 < len; i += 2)
		sum += (uint32_t)((p[i] << 8) | p[i + 1]);
	if (i < len)
		sum += (uint32_t)(p[i] << 8);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

int uib_frame_build(uint8_t *out, size_t cap, const struct uib_frame_hdr *hi,
		    const void *payload, size_t paylen)
{
	struct ethhdr *eth;
	struct iphdr  *ip;
	struct udphdr *udp;
	size_t total = UIB_L2_HDRS_LEN + paylen;

	if (total > cap)
		return -1;

	eth = (struct ethhdr *)out;
	memcpy(eth->h_dest,   hi->dst_mac, 6);
	memcpy(eth->h_source, hi->src_mac, 6);
	eth->h_proto = htons(ETH_P_IP);

	ip = (struct iphdr *)(out + ETH_HLEN);
	memset(ip, 0, sizeof(*ip));
	ip->version  = 4;
	ip->ihl      = 5;
	ip->tos      = 0;
	ip->tot_len  = htons((uint16_t)(sizeof(*ip) + sizeof(*udp) + paylen));
	ip->id       = 0;
	ip->frag_off = htons(0x4000);
	ip->ttl      = 64;
	ip->protocol = IPPROTO_UDP;
	ip->saddr    = hi->src_ip;
	ip->daddr    = hi->dst_ip;
	ip->check    = 0;
	ip->check    = htons(uib_inet_csum(ip, sizeof(*ip)));

	udp = (struct udphdr *)(out + ETH_HLEN + sizeof(*ip));
	udp->source = hi->src_port;
	udp->dest   = hi->dst_port;
	udp->len    = htons((uint16_t)(sizeof(*udp) + paylen));
	udp->check  = 0;

	if (paylen)
		memcpy(out + UIB_L2_HDRS_LEN, payload, paylen);
	return (int)total;
}

int uib_frame_parse(const uint8_t *frame, size_t len, uint16_t want_dport,
		    struct sockaddr_in *src, const uint8_t **payload,
		    size_t *paylen)
{
	const struct ethhdr *eth;
	const struct iphdr  *ip;
	const struct udphdr *udp;
	size_t ip_hlen, l4_off, udp_len;

	if (len < UIB_L2_HDRS_LEN)
		return -1;

	eth = (const struct ethhdr *)frame;
	if (eth->h_proto != htons(ETH_P_IP))
		return -1;

	ip = (const struct iphdr *)(frame + ETH_HLEN);
	if (ip->version != 4 || ip->protocol != IPPROTO_UDP)
		return -1;
	ip_hlen = (size_t)ip->ihl * 4;
	if (ip_hlen < sizeof(*ip))
		return -1;
	l4_off = ETH_HLEN + ip_hlen;
	if (l4_off + sizeof(*udp) > len)
		return -1;

	udp = (const struct udphdr *)(frame + l4_off);
	if (udp->dest != want_dport)
		return -1;

	udp_len = ntohs(udp->len);
	if (udp_len < sizeof(*udp) || l4_off + udp_len > len)
		return -1;

	if (src) {
		memset(src, 0, sizeof(*src));
		src->sin_family = AF_INET;
		src->sin_addr.s_addr = ip->saddr;
		src->sin_port = udp->source;
	}
	if (payload)
		*payload = frame + l4_off + sizeof(*udp);
	if (paylen)
		*paylen = udp_len - sizeof(*udp);
	return 0;
}
