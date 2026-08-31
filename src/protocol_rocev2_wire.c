/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <endian.h>

#include "uib/bth.h"
#include "uib/protocol.h"
#include "rocev2_internal.h"

int rocev2_packet_qpn(const void *packet, size_t len, uint32_t *qpn)
{
	if (!packet || !qpn || len < UIB_BTH_BYTES + UIB_ICRC_BYTES)
		return -EINVAL;

	uib_bth_unpack(packet, NULL, qpn, NULL, NULL);
	return 0;
}

int rocev2_path_mtu_valid(uint32_t path_mtu)
{
	return path_mtu == 256 || path_mtu == 512 || path_mtu == 1024 ||
	       path_mtu == 2048 || path_mtu == UIB_PMTU;
}

int rocev2_wire_encode(const struct uib_wire_meta *meta,
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

int rocev2_wire_decode(const struct uib_wire_meta *meta,
			void *packet, size_t len)
{
	uint32_t want_le;
	uint32_t want;
	uint32_t got;

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
	return 0;
}
