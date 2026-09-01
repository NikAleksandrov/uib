/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "uib/afxdp_frame.h"
#include "../src/afxdp_frame_internal.h"

static int g_fail;

static void check(const char *tag, int ok)
{
	printf("%-24s %s\n", tag, ok ? "OK" : "FAIL");
	if (!ok)
		g_fail = 1;
}

int main(void)
{
	struct uib_frame_hdr hi;
	struct iphdr ip;
	uint8_t frame[2048];
	uint8_t pay[1000];
	uint8_t mac[6];
	struct sockaddr_in src;
	const uint8_t *rpay;
	size_t rlen;
	uint8_t tos;
	int truncated;
	int flen;
	unsigned int i;

	for (i = 0; i < sizeof(pay); i++)
		pay[i] = (uint8_t)(i * 3 + 1);

	check("mac.valid",
	      !uib_afxdp_parse_mac("02:10:20:30:40:ff", mac) &&
	      !memcmp(mac, "\x02\x10\x20\x30\x40\xff", sizeof(mac)));
	check("mac.missing", uib_afxdp_parse_mac(NULL, mac) == -EINVAL);
	check("mac.short", uib_afxdp_parse_mac("02:10:20:30:40", mac) == -EINVAL);
	check("mac.trailing",
	      uib_afxdp_parse_mac("02:10:20:30:40:50x", mac) == -EINVAL);
	check("mac.range",
	      uib_afxdp_parse_mac("02:10:20:30:40:100", mac) == -EINVAL);

	memset(&hi, 0, sizeof(hi));
	memcpy(hi.src_mac, "\x02\x00\x00\x00\x00\x01", 6);
	memcpy(hi.dst_mac, "\x02\x00\x00\x00\x00\x02", 6);
	inet_pton(AF_INET, "10.0.0.1", &hi.src_ip);
	inet_pton(AF_INET, "10.0.0.2", &hi.dst_ip);
	hi.src_port = htons(4791);
	hi.dst_port = htons(4791);
	hi.tos = 0x2a;

	flen = uib_frame_build(frame, sizeof(frame), &hi, pay, sizeof(pay));
	check("build.len", flen == (int)(UIB_L2_HDRS_LEN + sizeof(pay)));

	memcpy(&ip, frame + 14, sizeof(ip));
	check("build.ip_csum_valid", uib_inet_csum(&ip, sizeof(ip)) == 0);

	check("build.overflow",
	      uib_frame_build(frame, 40, &hi, pay, sizeof(pay)) == -1);

	memset(&src, 0, sizeof(src));
	check("parse.ok", uib_frame_parse(frame, flen, htons(4791), &src,
					  &rpay, &rlen) == 0);
	check("parse.paylen", rlen == sizeof(pay));
	check("parse.payload", rpay && memcmp(rpay, pay, sizeof(pay)) == 0);
	check("parse.src_ip", src.sin_addr.s_addr == hi.src_ip);
	check("parse.src_port", src.sin_port == hi.src_port);
	check("parse.tos", ip.tos == hi.tos);
	check("parse.available",
	      !uib_frame_parse_available(frame, flen - 2, htons(4791), &src,
					 &rpay, &rlen, &tos, &truncated) &&
	      truncated && tos == hi.tos && rlen == sizeof(pay) - 2);

	check("parse.wrong_dport",
	      uib_frame_parse(frame, flen, htons(1234), &src, &rpay, &rlen) == -1);
	ip.frag_off = htons(IP_MF);
	memcpy(frame + 14, &ip, sizeof(ip));
	check("parse.fragmented",
	      uib_frame_parse(frame, flen, htons(4791), &src, &rpay,
			      &rlen) == -1);
	ip.frag_off = htons(0x4000);
	ip.tot_len = htons(sizeof(ip));
	memcpy(frame + 14, &ip, sizeof(ip));
	check("parse.short-ip",
	      uib_frame_parse(frame, flen, htons(4791), &src, &rpay,
			      &rlen) == -1);
	ip.tot_len = htons((uint16_t)(sizeof(ip) + sizeof(struct udphdr) +
				       sizeof(pay)));
	memcpy(frame + 14, &ip, sizeof(ip));

	check("parse.truncated",
	      uib_frame_parse(frame, 20, htons(4791), &src, &rpay, &rlen) == -1);

	frame[12] = 0x08;
	frame[13] = 0x06;
	check("parse.non_ip",
	      uib_frame_parse(frame, flen, htons(4791), &src, &rpay, &rlen) == -1);

	printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
