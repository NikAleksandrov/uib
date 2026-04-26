#ifndef UIB_AFXDP_FRAME_H
#define UIB_AFXDP_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

#define UIB_L2_HDRS_LEN 42

struct uib_frame_hdr {
	uint8_t  src_mac[6];
	uint8_t  dst_mac[6];
	uint32_t src_ip;
	uint32_t dst_ip;
	uint16_t src_port;
	uint16_t dst_port;
};

uint16_t uib_inet_csum(const void *data, size_t len);

int uib_frame_build(uint8_t *out, size_t cap, const struct uib_frame_hdr *hi,
		    const void *payload, size_t paylen);

int uib_frame_parse(const uint8_t *frame, size_t len, uint16_t want_dport,
		    struct sockaddr_in *src, const uint8_t **payload,
		    size_t *paylen);

#endif
