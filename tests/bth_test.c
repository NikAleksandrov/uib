/* SPDX-License-Identifier: Apache-2.0 */
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uib/bth.h"
#include "uib/protocol.h"

static int failures;

static void check(const char *name, int ok)
{
	printf("%-28s %s\n", name, ok ? "OK" : "FAIL");
	if (!ok)
		failures++;
}

int main(void)
{
	uint8_t rdma[UIB_BTH_BYTES + 64] = {0};
	uint8_t wire_packet[UIB_BTH_BYTES + 16 + UIB_ICRC_BYTES] = {0};
	struct uib_bth *bth = (struct uib_bth *)rdma;
	struct uib_bth *wire_bth = (struct uib_bth *)wire_packet;
	struct uib_wire_meta meta;
	uint32_t encoded_crc;
	uint32_t decoded_crc;
	uint32_t src;
	uint32_t dst;
	uint32_t crc;
	int rc;

	uib_bth_pack(bth, UIB_OP_SEND_ONLY, UIB_DEFAULT_PKEY, 1, 0, 1);
	memcpy(rdma + UIB_BTH_BYTES, "RXE-to-UIB", 10);
	inet_pton(AF_INET, "192.0.2.1", &src);
	inet_pton(AF_INET, "192.0.2.2", &dst);
	crc = uib_rocev2_icrc_ipv4(src, dst, htons(49169), htons(4791),
				      1, rdma, sizeof(rdma));
	/*
	 * Captured from Linux RXE for this exact IPv4/UDP/BTH/payload vector.
	 * RXE places the numeric value little-endian on the wire as a5 bb e5 33.
	 */
	check("rocev2.icrc.known-vector", crc == 0x33e5bba5u);

	memset(&meta, 0, sizeof(meta));
	meta.src_ip = src;
	meta.dst_ip = dst;
	meta.src_port = htons(49169);
	meta.dst_port = htons(4791);
	meta.ip_id = 1;
	uib_bth_pack(wire_bth, UIB_OP_SEND_ONLY, UIB_DEFAULT_PKEY, 1, 0, 1);
	memcpy(wire_packet + UIB_BTH_BYTES, "wire-validation", 15);
	rc = uib_rocev2_protocol_ops.wire_encode(&meta, wire_packet,
						    sizeof(wire_packet));
	check("rocev2.wire.encode", rc == 0);
	memcpy(&encoded_crc, wire_packet + sizeof(wire_packet) - sizeof(encoded_crc),
	       sizeof(encoded_crc));
	rc = uib_rocev2_protocol_ops.wire_decode(&meta, wire_packet,
						    sizeof(wire_packet));
	check("rocev2.wire.decode", rc == 0);
	memcpy(&decoded_crc, wire_packet + sizeof(wire_packet) - sizeof(decoded_crc),
	       sizeof(decoded_crc));
	check("rocev2.wire.preserved", decoded_crc == encoded_crc);
	wire_packet[UIB_BTH_BYTES] ^= 1;
	rc = uib_rocev2_protocol_ops.wire_decode(&meta, wire_packet,
						    sizeof(wire_packet));
	check("rocev2.wire.corrupt", rc == -EBADMSG);

	uib_bth_set_pad(bth, 3);
	check("bth.pad.set", uib_bth_pad(bth) == 3);
	uib_bth_set_pad(bth, 1);
	check("bth.pad.replace", uib_bth_pad(bth) == 1);
	check("bth.pad.preserves-opcode", bth->opcode == UIB_OP_SEND_ONLY);

	printf("RESULT %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
