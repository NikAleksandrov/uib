/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

#include "uib/bth.h"
#include "uib/protocol.h"
#include "uib/qp.h"

static int rocev2_packet_qpn(const void *packet, size_t len, uint32_t *qpn)
{
	if (!packet || !qpn || len < UIB_BTH_BYTES + UIB_ICRC_BYTES)
		return -EINVAL;

	uib_bth_unpack(packet, NULL, qpn, NULL, NULL);
	return 0;
}

static int rocev2_wire_encode(const struct uib_wire_meta *meta,
			      void *packet, size_t len)
{
	uint32_t crc, wire_crc;

	if (!meta || !packet || len < UIB_BTH_BYTES + UIB_ICRC_BYTES)
		return -EINVAL;

	crc = uib_rocev2_icrc_ipv4(meta->src_ip, meta->dst_ip,
				    meta->src_port, meta->dst_port,
				    meta->ip_id, packet,
				    len - UIB_ICRC_BYTES);
	wire_crc = htole32(crc);
	memcpy((uint8_t *)packet + len - UIB_ICRC_BYTES, &wire_crc,
	       sizeof(wire_crc));
	return 0;
}

static int rocev2_wire_decode(const struct uib_wire_meta *meta,
			      void *packet, size_t len)
{
	uint32_t want_le, want, got, internal;

	if (!meta || !packet || len < UIB_BTH_BYTES + UIB_ICRC_BYTES)
		return -EINVAL;

	memcpy(&want_le, (uint8_t *)packet + len - UIB_ICRC_BYTES,
	       sizeof(want_le));
	want = le32toh(want_le);
	got = uib_rocev2_icrc_ipv4(meta->src_ip, meta->dst_ip,
				   meta->src_port, meta->dst_port,
				   meta->ip_id, packet,
				   len - UIB_ICRC_BYTES);
	if (got != want)
		return -EBADMSG;

	internal = htonl(uib_icrc(packet, len - UIB_ICRC_BYTES));
	memcpy((uint8_t *)packet + len - UIB_ICRC_BYTES, &internal,
	       sizeof(internal));
	return 0;
}

const struct uib_protocol_ops uib_rocev2_protocol_ops = {
	.name = "rocev2",
	.default_port = UIB_ROCE_V2_UDP_DPORT,
	.min_packet = UIB_BTH_BYTES + UIB_ICRC_BYTES,
	.qp_fini = uib_rocev2_qp_fini,
	.packet_qpn = rocev2_packet_qpn,
	.post_send = uib_rocev2_post_send,
	.handle_packet = uib_rocev2_handle_packet,
	.retransmit_tick = uib_rocev2_retransmit_tick,
	.wire_encode = rocev2_wire_encode,
	.wire_decode = rocev2_wire_decode,
};
