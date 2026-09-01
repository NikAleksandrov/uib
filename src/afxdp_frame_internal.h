/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_AFXDP_FRAME_INTERNAL_H
#define UIB_AFXDP_FRAME_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

struct uib_frame_meta {
	uint32_t src_ip;
	uint32_t dst_ip;
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t ip_id;
	uint8_t tos;
};

int uib_frame_parse_meta(const uint8_t *frame, size_t len,
			 uint16_t want_dport, struct uib_frame_meta *meta,
			 const uint8_t **payload, size_t *paylen,
			 int *truncated);

int uib_frame_parse_available(const uint8_t *frame, size_t len,
			      uint16_t want_dport, struct sockaddr_in *src,
			      const uint8_t **payload, size_t *paylen,
			      uint8_t *tos, int *truncated);

#endif
