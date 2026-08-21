/* SPDX-License-Identifier: Apache-2.0 AND Linux-OpenIB */
/*
 * RoCEv2 ICRC pseudo-header handling is adapted from Linux RXE.
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#ifdef UIB_HAVE_ZLIB
#include <zlib.h>
#endif

#include "uib/bth.h"

#ifndef UIB_HAVE_ZLIB
static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_table_init(void)
{
	uint32_t i;
	int k;

	for (i = 0; i < 256; i++) {
		uint32_t c = i;

		for (k = 0; k < 8; k++)
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
}
#endif

uint32_t uib_crc32(uint32_t crc, const void *buf, size_t len)
{
#ifdef UIB_HAVE_ZLIB
	return (uint32_t)crc32_z(crc, buf, len);
#else
	pthread_once(&crc_once, crc_table_init);
	const uint8_t *p = buf;

	crc = ~crc;
	while (len--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
#endif
}

uint32_t uib_icrc(const void *packet, size_t len)
{
	return uib_crc32(0, packet, len);
}

uint32_t uib_rocev2_icrc_ipv4(uint32_t src_ip, uint32_t dst_ip,
			      uint16_t src_port, uint16_t dst_port,
			      uint16_t ip_id, const void *rdma,
			      size_t rdma_len)
{
	struct iphdr ip;
	struct udphdr udp;
	struct uib_bth bth;
	uint32_t crc;

	if (!rdma || rdma_len < UIB_BTH_BYTES)
		return 0;

	memset(&ip, 0, sizeof(ip));
	ip.version  = 4;
	ip.ihl      = 5;
	ip.tos      = 0xff;
	ip.tot_len  = htons((uint16_t)(sizeof(ip) + sizeof(udp) +
					      rdma_len + UIB_ICRC_BYTES));
	ip.id       = htons(ip_id);
	ip.frag_off = htons(0x4000);
	ip.ttl      = 0xff;
	ip.protocol = IPPROTO_UDP;
	ip.check    = 0xffff;
	ip.saddr    = src_ip;
	ip.daddr    = dst_ip;

	memset(&udp, 0, sizeof(udp));
	udp.source = src_port;
	udp.dest   = dst_port;
	udp.len    = htons((uint16_t)(sizeof(udp) + rdma_len + UIB_ICRC_BYTES));
	udp.check  = 0xffff;

	memcpy(&bth, rdma, sizeof(bth));
	/* The BTH reserved byte is masked for invariant CRC calculation. */
	bth.dqpn |= htonl(0xff000000u);

	/*
	 * 0x2144df1c is the complemented form of RXE's 0xdebb20e3 seed,
	 * suitable for the finalized/cumulative uib_crc32() interface.
	 */
	crc = uib_crc32(0x2144df1cu, &ip, sizeof(ip));
	crc = uib_crc32(crc, &udp, sizeof(udp));
	crc = uib_crc32(crc, &bth, sizeof(bth));
	crc = uib_crc32(crc, (const uint8_t *)rdma + sizeof(bth),
			rdma_len - sizeof(bth));
	return crc;
}

void uib_bth_pack(struct uib_bth *bth, uint8_t opcode, uint16_t pkey,
		  uint32_t dqpn, uint32_t psn, int ack_request)
{
	uint32_t apsn = psn & UIB_BTH_PSN_MASK;

	bth->opcode = opcode;
	bth->flags  = 0;
	bth->pkey   = htons(pkey);
	bth->dqpn   = htonl(dqpn & UIB_BTH_PSN_MASK);
	if (ack_request)
		apsn |= UIB_BTH_ACK_REQ;
	bth->apsn = htonl(apsn);
}

void uib_bth_unpack(const struct uib_bth *bth, uint8_t *opcode,
		    uint32_t *dqpn, uint32_t *psn, int *ack_request)
{
	uint32_t apsn = ntohl(bth->apsn);

	if (opcode)
		*opcode = bth->opcode;
	if (dqpn)
		*dqpn = ntohl(bth->dqpn) & UIB_BTH_PSN_MASK;
	if (psn)
		*psn = apsn & UIB_BTH_PSN_MASK;
	if (ack_request)
		*ack_request = (apsn & UIB_BTH_ACK_REQ) ? 1 : 0;
}

void uib_bth_set_pad(struct uib_bth *bth, uint8_t pad)
{
	bth->flags = (uint8_t)((bth->flags & ~UIB_BTH_PAD_MASK) |
			       ((pad & 3u) << UIB_BTH_PAD_SHIFT));
}

uint8_t uib_bth_pad(const struct uib_bth *bth)
{
	return (uint8_t)((bth->flags & UIB_BTH_PAD_MASK) >> UIB_BTH_PAD_SHIFT);
}

void uib_reth_pack(struct uib_reth *r, uint64_t va, uint32_t rkey, uint32_t len)
{
	uint32_t hi = (uint32_t)(va >> 32);
	uint32_t lo = (uint32_t)(va & 0xffffffffu);
	uint32_t bhi = htonl(hi);
	uint32_t blo = htonl(lo);

	memcpy(&r->va, &bhi, 4);
	memcpy(((uint8_t *)&r->va) + 4, &blo, 4);
	r->rkey = htonl(rkey);
	r->length = htonl(len);
}

void uib_reth_unpack(const struct uib_reth *r, uint64_t *va,
		     uint32_t *rkey, uint32_t *len)
{
	uint32_t bhi, blo;

	memcpy(&bhi, &r->va, 4);
	memcpy(&blo, ((const uint8_t *)&r->va) + 4, 4);
	uint64_t v = ((uint64_t)ntohl(bhi) << 32) | ntohl(blo);

	if (va)
		*va = v;
	if (rkey)
		*rkey = ntohl(r->rkey);
	if (len)
		*len = ntohl(r->length);
}

void uib_deth_pack(struct uib_deth *d, uint32_t qkey, uint32_t sqp)
{
	d->qkey = htonl(qkey);
	d->sqp  = htonl(sqp & 0x00ffffff);
}

void uib_deth_unpack(const struct uib_deth *d, uint32_t *qkey, uint32_t *sqp)
{
	if (qkey)
		*qkey = ntohl(d->qkey);
	if (sqp)
		*sqp = ntohl(d->sqp) & 0x00ffffff;
}

void uib_aeth_pack(struct uib_aeth *a, uint8_t syndrome, uint32_t msn)
{
	uint32_t v = ((uint32_t)syndrome << 24) | (msn & 0x00ffffff);

	a->syndrome_msn = htonl(v);
}

void uib_aeth_unpack(const struct uib_aeth *a, uint8_t *syndrome, uint32_t *msn)
{
	uint32_t v = ntohl(a->syndrome_msn);

	if (syndrome)
		*syndrome = (uint8_t)(v >> 24);
	if (msn)
		*msn = v & 0x00ffffff;
}
