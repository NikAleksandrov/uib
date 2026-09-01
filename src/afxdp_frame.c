/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "uib/afxdp_frame.h"
#include "afxdp_frame_internal.h"
#include "utils.h"

/**
 * uib_afxdp_parse_mac - Parse a colon-separated Ethernet MAC address.
 * @text: Ethernet address in colon-separated notation
 * @mac: Output buffer for the six address octets
 * @uib-api: AF_XDP frame helpers
 *
 * Return: 0 on success or a negative error code on failure.
 */
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

/**
 * uib_inet_csum - Calculate an Internet checksum over a byte range.
 * @data: Input byte range
 * @len: Length of the byte range
 * @uib-api: AF_XDP frame helpers
 *
 * Return: The 16-bit Internet checksum in host byte order.
 */
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

/**
 * uib_frame_build - Build an Ethernet, IPv4, and UDP frame.
 * @out: Output frame buffer
 * @cap: Capacity of the output buffer in bytes
 * @hi: Ethernet, IPv4, and UDP header fields
 * @payload: Payload bytes appended after the UDP header
 * @paylen: Length of the payload in bytes
 * @uib-api: AF_XDP frame helpers
 *
 * Return: The total frame length on success, or -1 if @out is too small.
 */
int uib_frame_build(uint8_t *out, size_t cap, const struct uib_frame_hdr *hi,
		    const void *payload, size_t paylen)
{
	struct ethhdr eth;
	struct iphdr ip;
	struct udphdr udp;
	size_t total;

	if (!out || !hi || (paylen && !payload) ||
	    paylen > UINT16_MAX - sizeof(ip) - sizeof(udp) ||
	    paylen > SIZE_MAX - UIB_L2_HDRS_LEN)
		return -1;
	total = UIB_L2_HDRS_LEN + paylen;
	if (total > cap)
		return -1;

	memset(&eth, 0, sizeof(eth));
	memcpy(eth.h_dest, hi->dst_mac, sizeof(eth.h_dest));
	memcpy(eth.h_source, hi->src_mac, sizeof(eth.h_source));
	eth.h_proto = htons(ETH_P_IP);
	memcpy(out, &eth, sizeof(eth));

	memset(&ip, 0, sizeof(ip));
	ip.version = 4;
	ip.ihl = 5;
	ip.tos = hi->tos;
	ip.tot_len = htons((uint16_t)(sizeof(ip) + sizeof(udp) + paylen));
	ip.frag_off = htons(0x4000);
	ip.ttl = 64;
	ip.protocol = IPPROTO_UDP;
	ip.saddr = hi->src_ip;
	ip.daddr = hi->dst_ip;
	ip.check = htons(uib_inet_csum(&ip, sizeof(ip)));
	memcpy(out + ETH_HLEN, &ip, sizeof(ip));

	memset(&udp, 0, sizeof(udp));
	udp.source = hi->src_port;
	udp.dest = hi->dst_port;
	udp.len = htons((uint16_t)(sizeof(udp) + paylen));
	memcpy(out + ETH_HLEN + sizeof(ip), &udp, sizeof(udp));

	if (paylen)
		memcpy(out + UIB_L2_HDRS_LEN, payload, paylen);
	return (int)total;
}

int uib_frame_parse_meta(const uint8_t *frame, size_t len,
			 uint16_t want_dport, struct uib_frame_meta *meta,
			 const uint8_t **payload, size_t *paylen,
			 int *truncated)
{
	struct ethhdr eth;
	struct iphdr ip;
	struct udphdr udp;
	size_t available_len;
	size_t ip_hlen;
	size_t ip_len;
	size_t l4_off;
	size_t udp_len;
	uint16_t fragment;

	if (!frame || len < UIB_L2_HDRS_LEN)
		return -1;

	memcpy(&eth, frame, sizeof(eth));
	if (eth.h_proto != htons(ETH_P_IP))
		return -1;

	memcpy(&ip, frame + ETH_HLEN, sizeof(ip));
	if (ip.version != 4 || ip.protocol != IPPROTO_UDP)
		return -1;
	ip_hlen = (size_t)ip.ihl * 4;
	if (ip_hlen < sizeof(ip))
		return -1;
	ip_len = ntohs(ip.tot_len);
	if (ip_len < ip_hlen + sizeof(udp))
		return -1;
	fragment = ntohs(ip.frag_off);
	if (fragment & (IP_MF | IP_OFFMASK))
		return -1;
	l4_off = ETH_HLEN + ip_hlen;
	if (l4_off > len || len - l4_off < sizeof(udp))
		return -1;

	memcpy(&udp, frame + l4_off, sizeof(udp));
	if (udp.dest != want_dport)
		return -1;

	udp_len = ntohs(udp.len);
	available_len = len - l4_off;
	if (udp_len < sizeof(udp) || udp_len > ip_len - ip_hlen)
		return -1;

	if (meta) {
		meta->src_ip = ip.saddr;
		meta->dst_ip = ip.daddr;
		meta->src_port = udp.source;
		meta->dst_port = udp.dest;
		meta->ip_id = ntohs(ip.id);
		meta->tos = ip.tos;
	}
	if (payload)
		*payload = frame + l4_off + sizeof(udp);
	if (paylen)
		*paylen = min_t(size_t, udp_len, available_len) - sizeof(udp);
	if (truncated)
		*truncated = udp_len > available_len;
	return 0;
}

int uib_frame_parse_available(const uint8_t *frame, size_t len,
			      uint16_t want_dport, struct sockaddr_in *src,
			      const uint8_t **payload, size_t *paylen,
			      uint8_t *tos, int *truncated)
{
	struct uib_frame_meta meta;
	int rc;

	rc = uib_frame_parse_meta(frame, len, want_dport, &meta, payload,
				  paylen, truncated);
	if (rc < 0)
		return rc;
	if (src) {
		memset(src, 0, sizeof(*src));
		src->sin_family = AF_INET;
		src->sin_addr.s_addr = meta.src_ip;
		src->sin_port = meta.src_port;
	}
	if (tos)
		*tos = meta.tos;
	return 0;
}

/**
 * uib_frame_parse - Validate a received frame and locate its UDP payload.
 * @frame: Received Ethernet frame
 * @len: Length of the byte range
 * @want_dport: Required UDP destination port in network byte order
 * @src: Output for the source IPv4 address and UDP port
 * @payload: Output pointer to the UDP payload
 * @paylen: Output for the UDP payload length
 * @uib-api: AF_XDP frame helpers
 *
 * Return: 0 for a matching IPv4/UDP frame, or -1 if the frame is rejected.
 */
int uib_frame_parse(const uint8_t *frame, size_t len, uint16_t want_dport,
		    struct sockaddr_in *src, const uint8_t **payload,
		    size_t *paylen)
{
	int truncated;
	int rc;

	rc = uib_frame_parse_available(frame, len, want_dport, src, payload,
				       paylen, NULL, &truncated);
	if (rc < 0 || truncated)
		return -1;
	return 0;
}
