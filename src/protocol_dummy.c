/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#include "uib/protocol.h"
#include "uib/qp.h"
#include "uib/context.h"
#include "uib/transport.h"

#define UIB_DUMMY_MAGIC 0x55494244u
#define UIB_DUMMY_VERSION 1
#define UIB_DUMMY_PORT 18516
#define UIB_DUMMY_SEND 1

struct uib_dummy_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t opcode;
	uint16_t header_len;
	uint32_t dqpn;
	uint32_t sqpn;
	uint32_t payload_len;
} __attribute__((packed));

static int dummy_packet_qpn(const void *packet, size_t len, uint32_t *qpn)
{
	const struct uib_dummy_hdr *hdr = packet;

	if (!packet || !qpn || len < sizeof(*hdr) ||
	    ntohl(hdr->magic) != UIB_DUMMY_MAGIC ||
	    hdr->version != UIB_DUMMY_VERSION ||
	    ntohs(hdr->header_len) != sizeof(*hdr))
		return -EBADMSG;

	*qpn = ntohl(hdr->dqpn);
	return 0;
}

static int dummy_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			   struct uib_send_wr **bad)
{
	struct uib_send_wr *cur;

	for (cur = wr; cur; cur = cur->next) {
		struct sockaddr_in dst = qp->remote_addr;
		struct uib_dummy_hdr *hdr;
		uint8_t *packet;
		int payload_len;
		int rc;

		if (cur->opcode != UIB_WR_SEND &&
		    cur->opcode != UIB_WR_SEND_WITH_IMM) {
			rc = -EOPNOTSUPP;
			goto err_wc;
		}

		packet = malloc(UIB_RECV_BUF_LEN);
		if (!packet) {
			rc = -ENOMEM;
			goto err_wc;
		}
		hdr = (struct uib_dummy_hdr *)packet;
		payload_len = uib_qp_gather(qp->pd, cur->sg_list, cur->num_sge,
					    packet + sizeof(*hdr),
					    UIB_RECV_BUF_LEN - sizeof(*hdr));
		if (payload_len < 0) {
			rc = payload_len;
			free(packet);
			goto err_wc;
		}

		hdr->magic = htonl(UIB_DUMMY_MAGIC);
		hdr->version = UIB_DUMMY_VERSION;
		hdr->opcode = UIB_DUMMY_SEND;
		hdr->header_len = htons(sizeof(*hdr));
		hdr->dqpn = htonl(qp->dest_qp_num);
		hdr->sqpn = htonl(qp->qp_num);
		hdr->payload_len = htonl((uint32_t)payload_len);
		if (!dst.sin_port)
			dst.sin_port = htons(UIB_DUMMY_PORT);
		rc = uib_transport_send(qp->ctx, &dst, packet,
					 sizeof(*hdr) + (size_t)payload_len);
		free(packet);
		if (rc < 0)
			goto err_wc;

		uib_qp_emit_send(qp, cur->wr_id, cur->opcode,
				 cur->send_flags & UIB_SEND_SIGNALED,
				 UIB_WC_SUCCESS, 0);
		continue;

err_wc:
		uib_qp_emit_send(qp, cur->wr_id, cur->opcode, 1,
				 rc == -EMSGSIZE ? UIB_WC_LOC_LEN_ERR
						      : UIB_WC_LOC_PROT_ERR,
				 0);
		if (bad)
			*bad = cur;
		return rc;
	}

	return 0;
}

static int dummy_handle_packet(struct uib_qp *qp,
			       const struct sockaddr_in *src,
			       const void *packet, size_t len)
{
	const struct uib_dummy_hdr *hdr = packet;
	struct uib_rq_slot slot;
	const uint8_t *payload;
	size_t payload_len;
	size_t consumed = 0;
	uint32_t qpn;
	int rc;

	(void)src;
	if (dummy_packet_qpn(packet, len, &qpn) < 0 ||
	    hdr->opcode != UIB_DUMMY_SEND)
		return -EBADMSG;
	(void)qpn;
	payload_len = ntohl(hdr->payload_len);
	if (payload_len > len - sizeof(*hdr))
		return -EBADMSG;
	payload = (const uint8_t *)packet + sizeof(*hdr);

	rc = uib_qp_pop_recv(qp, &slot);
	if (rc < 0)
		return rc;
	rc = uib_qp_scatter(qp->pd, slot.sg_list, slot.num_sge, 0,
			    payload, payload_len, &consumed);
	uib_qp_emit_recv(qp, slot.wr_id,
			 rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			 UIB_WC_RECV, (uint32_t)consumed, ntohl(hdr->sqpn), 0);
	free(slot.sg_list);
	return rc;
}

const struct uib_protocol_ops uib_dummy_protocol_ops = {
	.name = "dummy",
	.default_port = UIB_DUMMY_PORT,
	.min_packet = sizeof(struct uib_dummy_hdr),
	.packet_qpn = dummy_packet_qpn,
	.post_send = dummy_post_send,
	.handle_packet = dummy_handle_packet,
};
