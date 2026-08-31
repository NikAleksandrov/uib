/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/ip.h>

#include "uib/bth.h"
#include "uib/mr.h"
#include "uib/pd.h"
#include "uib/qp.h"
#include "uib/context.h"
#include "congestion_internal.h"
#include "host_zc_internal.h"
#include "qp_internal.h"
#include "rocev2_internal.h"
#include "transport_internal.h"
#include "utils.h"

int rocev2_scatter_recv_fragment(struct uib_qp *qp,
				 struct uib_rq_slot *slot, uint32_t offset,
				 const uint8_t *data, size_t length,
				 size_t *consumed)
{
	struct uib_sge *sge;
	size_t available;
	size_t take;
	int rc;

	if (slot->private_data) {
		rc = uib_host_loan_place(slot, offset, data, length);
		if (!rc) {
			*consumed = length;
			return 0;
		}
		if (rc != -ENOENT)
			return rc;
	}
	if (!rocev2_qp(qp)->frag_recv_mr)
		return uib_qp_scatter_at(qp->pd, slot->sg_list,
					 slot->num_sge, offset, data,
					 length, consumed);
	sge = &slot->sg_list[0];
	available = offset < sge->length ? sge->length - offset : 0;
	take = min_t(size_t, length, available);
	if (uib_mr_place(rocev2_qp(qp)->frag_recv_mr, sge->addr + offset,
			 data, take) < 0)
		return -EIO;
	*consumed = take;
	return 0;
}

static int verify_icrc(const uint8_t *pkt, size_t len)
{
	uint32_t want;
	uint32_t got;

	if (len < UIB_ICRC_BYTES)
		return -1;

	memcpy(&want, pkt + len - UIB_ICRC_BYTES, UIB_ICRC_BYTES);
	want = ntohl(want);
	got = uib_icrc(pkt, len - UIB_ICRC_BYTES);
	return got == want ? 0 : -1;
}

static void ack_outstanding(struct uib_qp *qp, uint32_t ack_psn)
{
	struct uib_congestion_ack event = {
		.ack_time_ns = uib_now_ns(),
	};
	struct uib_outstanding *next;
	struct uib_outstanding *out;
	uint64_t acknowledged = 0;

	pthread_mutex_lock(&qp->lock);
	for (out = rocev2_qp(qp)->out_head; out; out = next) {
		next = out->next;

		if (rocev2_psn_cmp(out->psn, ack_psn) > 0)
			break;
		if (out->is_final) {
			uib_qp_emit_send_wc(qp, out->wr_id, out->wr_opcode, out->signaled,
				     UIB_WC_SUCCESS, 0);
		}
		if (out->congestion_inflight)
			acknowledged += out->nominal_len;
		event.tx_time_ns = out->tx_time_ns;
		event.tx_retries = out->retries;
		event.retransmit = out->retries != 0;
		rocev2_outstanding_unlink(qp, out);
		rocev2_free_outstanding(out);
	}
	if (acknowledged) {
		event.newly_received = acknowledged > UINT32_MAX ? UINT32_MAX :
			(uint32_t)acknowledged;
		uib_congestion_on_ack(&rocev2_qp(qp)->congestion, &event);
	}
	if (rocev2_qp(qp)->sequence_retry_active &&
	    rocev2_psn_cmp(ack_psn, rocev2_qp(qp)->sequence_retry_psn) >= 0)
		rocev2_qp(qp)->sequence_retry_active = 0;
	rocev2_qp(qp)->last_acked_psn = ack_psn;
	rocev2_outstanding_retry_reschedule(qp);
	rocev2_drain_locked(qp);
	pthread_mutex_unlock(&qp->lock);
}

static void rocev2_congestion_cnp(struct uib_qp *qp)
{
	struct uib_congestion_ack event = {
		.ack_time_ns = uib_now_ns(),
		.ecn_marked = 1,
	};
	struct uib_outstanding *out;

	pthread_mutex_lock(&qp->lock);
	out = rocev2_qp(qp)->out_head ? rocev2_qp(qp)->out_head : rocev2_qp(qp)->read_head;
	if (out) {
		event.tx_time_ns = out->tx_time_ns;
		event.tx_retries = out->retries;
		event.retransmit = out->retries != 0;
		uib_congestion_on_ack(&rocev2_qp(qp)->congestion, &event);
	}
	pthread_mutex_unlock(&qp->lock);
}

void rocev2_fail_outstanding(struct uib_qp *qp, enum uib_wc_status st)
{
	struct uib_outstanding *out;

	pthread_mutex_lock(&qp->lock);
	while ((out = rocev2_qp(qp)->out_head) != NULL) {
		if (out->congestion_inflight)
			uib_congestion_on_loss(&rocev2_qp(qp)->congestion,
					       out->nominal_len);
		if (out->is_final)
			uib_qp_emit_send_wc(qp, out->wr_id, out->wr_opcode, 1, st, 0);
		rocev2_outstanding_unlink(qp, out);
		rocev2_free_outstanding(out);
	}
	while ((out = rocev2_qp(qp)->read_head) != NULL) {
		if (out->congestion_inflight)
			uib_congestion_on_loss(&rocev2_qp(qp)->congestion,
					       out->nominal_len);
		uib_qp_emit_send_wc(qp, out->wr_id, out->wr_opcode, 1, st, 0);
		rocev2_outstanding_unlink(qp, out);
		rocev2_free_outstanding(out);
	}
	rocev2_qp(qp)->frag_read_out = NULL;
	rocev2_qp(qp)->frag_read_offset = 0;
	uib_qp_retry_cancel(qp);
	pthread_mutex_unlock(&qp->lock);
}

static void send_read_response(struct uib_qp *qp, const struct sockaddr_in *dst,
			       uint32_t start_psn, uint64_t va, uint32_t rkey,
			       uint32_t length)
{
	uint8_t packet[UIB_ROCEV2_PKT_MAX];
	struct uib_mr *mr;
	uint32_t mtu;
	uint32_t n;
	uint32_t i;

	mr = uib_mr_check_remote(qp->pd, rkey, va, length,
				  UIB_ACCESS_REMOTE_READ);
	if (!mr) {
		rocev2_send_ack(qp, dst, start_psn, UIB_AETH_NAK_REM_ACC);
		return;
	}

	mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
	n = (length + mtu - 1) / mtu;

	if (n == 0)
		n = 1;

	for (i = 0; i < n; i++) {
		uint8_t *pkt = packet;
		uint8_t *p;
		size_t pkt_len;
		uint32_t chunk;
		uint32_t bcrc;
		uint32_t crc;
		uint32_t off;
		int has_aeth;
		uint8_t pad;
		uint8_t op;

		off = i * mtu;
		chunk = off + mtu > length ? length - off : mtu;
		pad = (uint8_t)((-(int)chunk) & 3);

		if (n == 1) {
			op = UIB_OP_RDMA_READ_RESP_ONLY;
			has_aeth = 1;
		} else if (i == 0) {
			op = UIB_OP_RDMA_READ_RESP_FIRST;
			has_aeth = 1;
		} else if (i == n - 1) {
			op = UIB_OP_RDMA_READ_RESP_LAST;
			has_aeth = 1;
		} else {
			op = UIB_OP_RDMA_READ_RESP_MIDDLE;
			has_aeth = 0;
		}

		pkt_len = UIB_BTH_BYTES + (has_aeth ? UIB_AETH_BYTES : 0) +
			chunk + pad + UIB_ICRC_BYTES;
		if (pkt_len > sizeof(packet))
			goto out;
		p = pkt;

		uib_bth_pack((struct uib_bth *)p, op, UIB_DEFAULT_PKEY,
			     qp->dest_qp_num, start_psn + i, 0);
		uib_bth_set_pad((struct uib_bth *)p, pad);
		p += UIB_BTH_BYTES;
		if (has_aeth) {
			uib_aeth_pack((struct uib_aeth *)p, UIB_AETH_ACK,
				      start_psn + i);
			p += UIB_AETH_BYTES;
		}
		if (uib_mr_read(mr, va + off, p, chunk) < 0) {
			rocev2_send_ack(qp, dst, start_psn, UIB_AETH_NAK_REM_ACC);
			goto out;
		}
		p += chunk;
		if (pad) {
			memset(p, 0, pad);
			p += pad;
		}
		crc = uib_icrc(pkt, p - pkt);
		bcrc = htonl(crc);

		memcpy(p, &bcrc, UIB_ICRC_BYTES);

		rocev2_transport_send(qp, dst, pkt, pkt_len);
	}
out:
	uib_mr_put(mr);
}

static void send_atomic_ack(struct uib_qp *qp, uint32_t psn, uint64_t value)
{
	uint8_t packet[UIB_BTH_BYTES + UIB_AETH_BYTES +
		       UIB_ROCEV2_ATOMIC_ACK_BYTES + UIB_ICRC_BYTES];
	uint64_t wire_value = htobe64(value);
	uint32_t wire_crc;
	uint32_t crc;
	uint8_t *p = packet;

	uib_bth_pack((struct uib_bth *)p, UIB_OP_ATOMIC_ACKNOWLEDGE,
		     UIB_DEFAULT_PKEY, qp->dest_qp_num, psn, 0);
	p += UIB_BTH_BYTES;
	uib_aeth_pack((struct uib_aeth *)p, UIB_AETH_ACK, psn);
	p += UIB_AETH_BYTES;
	memcpy(p, &wire_value, sizeof(wire_value));
	p += sizeof(wire_value);
	crc = uib_icrc(packet, p - packet);
	wire_crc = htonl(crc);
	memcpy(p, &wire_crc, sizeof(wire_crc));
	rocev2_transport_send(qp, &qp->remote_addr, packet, sizeof(packet));
}

static struct uib_outstanding *find_read_by_psn(struct uib_qp *qp, uint32_t psn)
{
	struct uib_outstanding *out;

	pthread_mutex_lock(&qp->lock);
	out = uib_psn_index_lookup(&rocev2_qp(qp)->out_index,
				   psn & UIB_BTH_PSN_MASK);
	if (!out) {
		for (out = rocev2_qp(qp)->read_head; out; out = out->next) {
			if (!((out->psn ^ psn) & UIB_BTH_PSN_MASK))
				break;
		}
	}
	pthread_mutex_unlock(&qp->lock);
	return out;
}

static void detach_outstanding(struct uib_qp *qp,
			       struct uib_outstanding *target)
{
	pthread_mutex_lock(&qp->lock);
	if (target == rocev2_qp(qp)->read_head || target->prev || target->next) {
		if (target->congestion_inflight) {
			struct uib_congestion_ack event = {
				.ack_time_ns = uib_now_ns(),
				.tx_time_ns = target->tx_time_ns,
				.newly_received = target->nominal_len,
				.tx_retries = target->retries,
				.retransmit = target->retries != 0,
			};

			uib_congestion_on_ack(&rocev2_qp(qp)->congestion,
					      &event);
		}
		rocev2_outstanding_unlink(qp, target);
		rocev2_outstanding_retry_reschedule(qp);
		rocev2_drain_locked(qp);
	}
	pthread_mutex_unlock(&qp->lock);
}

struct rocev2_rx_packet {
	struct uib_qp *qp;
	const struct sockaddr_in *src;
	const uint8_t *body;
	size_t body_len;
	uint32_t psn;
	uint8_t opcode;
	int ack_request;
	int is_rc;
};
static void rocev2_rx_advance(struct rocev2_rx_packet *rx)
{
	rocev2_qp(rx->qp)->last_rx_psn = rx->psn;
	rocev2_qp(rx->qp)->last_rx_valid = 1;
	rx->qp->rq_psn = rx->psn + 1;
}

static int rocev2_response_opcode(uint8_t opcode)
{
	return opcode == UIB_OP_ACKNOWLEDGE || opcode == UIB_OP_CNP ||
		opcode == UIB_OP_ATOMIC_ACKNOWLEDGE ||
		opcode == UIB_OP_RDMA_READ_RESP_FIRST ||
		opcode == UIB_OP_RDMA_READ_RESP_MIDDLE ||
		opcode == UIB_OP_RDMA_READ_RESP_LAST ||
		opcode == UIB_OP_RDMA_READ_RESP_ONLY;
}

static int rocev2_check_request_psn(const struct rocev2_rx_packet *rx)
{
	uint32_t expected;
	int order;

	if (!rx->is_rc || rocev2_response_opcode(rx->opcode))
		return 0;
	expected = rx->qp->rq_psn & UIB_BTH_PSN_MASK;
	order = rocev2_psn_cmp(rx->psn, expected);
	if (order < 0) {
		if ((rx->opcode == UIB_OP_FETCH_ADD ||
		     rx->opcode == UIB_OP_COMPARE_SWAP) &&
		    rocev2_qp(rx->qp)->atomic_ack_valid &&
		    !rocev2_psn_cmp(rx->psn, rocev2_qp(rx->qp)->atomic_ack_psn))
			send_atomic_ack(rx->qp, rx->psn,
					rocev2_qp(rx->qp)->atomic_ack_value);
		else
			rocev2_send_ack(rx->qp, rx->src, rocev2_qp(rx->qp)->last_rx_psn,
				 UIB_AETH_ACK);
		return 1;
	}
	if (order > 0) {
		rocev2_send_ack(rx->qp, rx->src, expected, UIB_AETH_NAK_PSN_SEQ);
		return 1;
	}
	return 0;
}

static int rocev2_handle_send_only(struct rocev2_rx_packet *rx)
{
	struct uib_rq_slot slot;
	const uint8_t *body = rx->body;
	size_t body_len = rx->body_len;
	size_t consumed = 0;
	uint32_t imm_data = 0;
	int rc;

	if (rx->opcode == UIB_OP_SEND_ONLY_WITH_IMM) {
		if (body_len < sizeof(imm_data))
			return -EBADMSG;
		memcpy(&imm_data, body, sizeof(imm_data));
		body += sizeof(imm_data);
		body_len -= sizeof(imm_data);
	}
	if (uib_qp_pop_recv_slot(rx->qp, &slot) < 0) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_RNR);
		return -ENOENT;
	}
	rc = uib_qp_scatter_recv(rx->qp, &slot, 0, body, body_len,
				 &consumed);
	uib_qp_emit_recv_slot_src(rx->qp, &slot,
				  rc < 0 ? UIB_WC_LOC_PROT_ERR :
				  UIB_WC_SUCCESS, UIB_WC_RECV,
				  (uint32_t)consumed,
				  rx->qp->dest_qp_num, imm_data, rx->src);
	uib_qp_release_recv(&slot);
	rocev2_rx_advance(rx);
	if (rx->is_rc)
		rocev2_send_ack(rx->qp, rx->src, rx->psn,
			 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
	return rc;
}

static int rocev2_handle_send_first(struct rocev2_rx_packet *rx)
{
	struct uib_rq_slot slot;
	size_t consumed = 0;
	int rc;

	if (uib_qp_pop_recv_slot(rx->qp, &slot) < 0) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_RNR);
		return -ENOENT;
	}
	uib_mr_put(rocev2_qp(rx->qp)->frag_recv_mr);
	rocev2_qp(rx->qp)->frag_recv_mr = NULL;
	if (slot.num_sge == 1)
		rocev2_qp(rx->qp)->frag_recv_mr = uib_mr_lookup_local(
			rx->qp->pd, slot.sg_list[0].lkey,
			(const void *)(uintptr_t)slot.sg_list[0].addr,
			slot.sg_list[0].length, 0);
	rc = rocev2_scatter_recv_fragment(rx->qp, &slot, 0, rx->body,
				   rx->body_len, &consumed);
	rocev2_qp(rx->qp)->frag_recv = slot;
	if (!rocev2_qp(rx->qp)->frag_recv.heap_sge)
		rocev2_qp(rx->qp)->frag_recv.sg_list = rocev2_qp(rx->qp)->frag_recv.inline_sge;
	rocev2_qp(rx->qp)->frag_recv_active = 1;
	rocev2_qp(rx->qp)->frag_recv_offset = (uint32_t)consumed;
	rocev2_rx_advance(rx);
	if (rx->is_rc && (rc < 0 || rx->ack_request))
		rocev2_send_ack(rx->qp, rx->src, rx->psn,
			 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
	return rc;
}

static int rocev2_handle_send_middle(struct rocev2_rx_packet *rx)
{
	size_t consumed = 0;
	int rc;

	if (!rocev2_qp(rx->qp)->frag_recv_active) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	rc = rocev2_scatter_recv_fragment(rx->qp, &rocev2_qp(rx->qp)->frag_recv,
				   rocev2_qp(rx->qp)->frag_recv_offset, rx->body,
				   rx->body_len, &consumed);
	rocev2_qp(rx->qp)->frag_recv_offset += (uint32_t)consumed;
	rocev2_rx_advance(rx);
	if (rx->is_rc && (rc < 0 || rx->ack_request))
		rocev2_send_ack(rx->qp, rx->src, rx->psn,
			 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
	return rc;
}

static int rocev2_handle_send_last(struct rocev2_rx_packet *rx)
{
	const uint8_t *body = rx->body;
	size_t body_len = rx->body_len;
	size_t consumed = 0;
	uint32_t imm_data = 0;
	uint32_t total;
	int rc;

	if (!rocev2_qp(rx->qp)->frag_recv_active) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	if (rx->opcode == UIB_OP_SEND_LAST_WITH_IMM) {
		if (body_len < sizeof(imm_data))
			return -EBADMSG;
		memcpy(&imm_data, body, sizeof(imm_data));
		body += sizeof(imm_data);
		body_len -= sizeof(imm_data);
	}
	rc = rocev2_scatter_recv_fragment(rx->qp, &rocev2_qp(rx->qp)->frag_recv,
				   rocev2_qp(rx->qp)->frag_recv_offset, body, body_len,
				   &consumed);
	total = rocev2_qp(rx->qp)->frag_recv_offset + (uint32_t)consumed;
	uib_qp_emit_recv_slot_src(rx->qp,
				  &rocev2_qp(rx->qp)->frag_recv,
				  rc < 0 ? UIB_WC_LOC_PROT_ERR :
				  UIB_WC_SUCCESS, UIB_WC_RECV, total,
				  rx->qp->dest_qp_num, imm_data, rx->src);
	uib_qp_release_recv(&rocev2_qp(rx->qp)->frag_recv);
	uib_mr_put(rocev2_qp(rx->qp)->frag_recv_mr);
	rocev2_qp(rx->qp)->frag_recv_mr = NULL;
	rocev2_qp(rx->qp)->frag_recv_active = 0;
	rocev2_qp(rx->qp)->frag_recv_offset = 0;
	rocev2_rx_advance(rx);
	if (rx->is_rc)
		rocev2_send_ack(rx->qp, rx->src, rx->psn,
			 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
	return rc;
}

static int rocev2_handle_ud_send(struct rocev2_rx_packet *rx)
{
	const struct uib_deth *deth;
	struct uib_rq_slot slot;
	const uint8_t *body = rx->body;
	size_t body_len = rx->body_len;
	size_t consumed = 0;
	uint32_t imm_data = 0;
	uint32_t qkey;
	uint32_t sqp;
	int rc;

	if (body_len < UIB_DETH_BYTES)
		return -EBADMSG;
	deth = (const struct uib_deth *)body;
	uib_deth_unpack(deth, &qkey, &sqp);
	if (qkey != rx->qp->qkey)
		return -EACCES;
	body += UIB_DETH_BYTES;
	body_len -= UIB_DETH_BYTES;
	if (rx->opcode == UIB_OP_UD_SEND_ONLY_WITH_IMM) {
		if (body_len < sizeof(imm_data))
			return -EBADMSG;
		memcpy(&imm_data, body, sizeof(imm_data));
		body += sizeof(imm_data);
		body_len -= sizeof(imm_data);
	}
	if (uib_qp_pop_recv_slot(rx->qp, &slot) < 0)
		return -ENOENT;
	rc = uib_qp_scatter_recv(rx->qp, &slot, 0, body, body_len,
				 &consumed);
	uib_qp_emit_recv_slot_src(rx->qp, &slot,
				  rc < 0 ? UIB_WC_LOC_PROT_ERR :
				  UIB_WC_SUCCESS, UIB_WC_RECV,
				  (uint32_t)consumed, sqp, imm_data, rx->src);
	uib_qp_release_recv(&slot);
	rx->qp->rq_psn++;
	return rc;
}

static int rocev2_handle_write_only(struct rocev2_rx_packet *rx)
{
	const struct uib_reth *reth;
	struct uib_rq_slot slot;
	struct uib_mr *mr;
	const uint8_t *body = rx->body;
	size_t body_len = rx->body_len;
	uint64_t va;
	uint32_t imm_data = 0;
	uint32_t rkey;
	uint32_t rlen;
	int with_imm;

	if (body_len < UIB_RETH_BYTES) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	reth = (const struct uib_reth *)body;
	uib_reth_unpack(reth, &va, &rkey, &rlen);
	body += UIB_RETH_BYTES;
	body_len -= UIB_RETH_BYTES;
	with_imm = rx->opcode == UIB_OP_RDMA_WRITE_ONLY_WITH_IMM;
	if (with_imm) {
		if (body_len < sizeof(imm_data))
			return -EBADMSG;
		memcpy(&imm_data, body, sizeof(imm_data));
		body += sizeof(imm_data);
		body_len -= sizeof(imm_data);
		if (uib_qp_pop_recv_slot(rx->qp, &slot) < 0) {
			if (rx->is_rc)
				rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_RNR);
			return -ENOENT;
		}
	}
	if (rlen > body_len)
		rlen = body_len;
	mr = uib_mr_check_remote(rx->qp->pd, rkey, va, rlen,
				 UIB_ACCESS_REMOTE_WRITE);
	if (!mr) {
		if (with_imm)
			uib_qp_release_recv(&slot);
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EACCES;
	}
	if (uib_mr_place(mr, va, body, rlen) < 0) {
		uib_mr_put(mr);
		if (with_imm)
			uib_qp_release_recv(&slot);
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EIO;
	}
	rocev2_rx_advance(rx);
	if (rx->is_rc)
		rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_ACK);
	if (with_imm) {
		uib_qp_emit_recv_slot_src(rx->qp, &slot, UIB_WC_SUCCESS,
					  UIB_WC_RECV_RDMA_WITH_IMM, rlen,
					  rx->qp->dest_qp_num, imm_data,
					  rx->src);
		uib_qp_release_recv(&slot);
	}
	uib_mr_put(mr);
	return 0;
}

static int rocev2_handle_write_first(struct rocev2_rx_packet *rx)
{
	const struct uib_reth *reth;
	struct uib_mr *mr;
	uint64_t va;
	size_t take;
	uint32_t rkey;
	uint32_t rlen;

	if (rx->body_len < UIB_RETH_BYTES) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	reth = (const struct uib_reth *)rx->body;
	uib_reth_unpack(reth, &va, &rkey, &rlen);
	mr = uib_mr_check_remote(rx->qp->pd, rkey, va, rlen,
				 UIB_ACCESS_REMOTE_WRITE);
	if (!mr) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EACCES;
	}
	take = rx->body_len - UIB_RETH_BYTES;
	if (take > rlen)
		take = rlen;
	if (uib_mr_place(mr, va, rx->body + UIB_RETH_BYTES, take) < 0) {
		uib_mr_put(mr);
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EIO;
	}
	uib_mr_put(rocev2_qp(rx->qp)->frag_write_mr);
	rocev2_qp(rx->qp)->frag_write_va = va;
	rocev2_qp(rx->qp)->frag_write_total = rlen;
	rocev2_qp(rx->qp)->frag_write_offset = (uint32_t)take;
	rocev2_qp(rx->qp)->frag_write_active = 1;
	rocev2_qp(rx->qp)->frag_write_mr = mr;
	rocev2_rx_advance(rx);
	if (rx->is_rc && rx->ack_request)
		rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_ACK);
	return 0;
}

static int rocev2_handle_write_middle(struct rocev2_rx_packet *rx)
{
	size_t take;
	uint32_t remain;

	if (!rocev2_qp(rx->qp)->frag_write_active) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	remain = rocev2_qp(rx->qp)->frag_write_total - rocev2_qp(rx->qp)->frag_write_offset;
	take = rx->body_len > remain ? remain : rx->body_len;
	if (uib_mr_place(rocev2_qp(rx->qp)->frag_write_mr,
			 rocev2_qp(rx->qp)->frag_write_va + rocev2_qp(rx->qp)->frag_write_offset,
			 rx->body, take) < 0) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EIO;
	}
	rocev2_qp(rx->qp)->frag_write_offset += (uint32_t)take;
	rocev2_rx_advance(rx);
	if (rx->is_rc && rx->ack_request)
		rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_ACK);
	return 0;
}

static int rocev2_handle_write_last(struct rocev2_rx_packet *rx)
{
	struct uib_rq_slot slot;
	const uint8_t *body = rx->body;
	size_t body_len = rx->body_len;
	size_t take;
	uint32_t imm_data = 0;
	uint32_t remain;
	int with_imm;

	if (!rocev2_qp(rx->qp)->frag_write_active) {
		if (rx->is_rc)
			rocev2_send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	with_imm = rx->opcode == UIB_OP_RDMA_WRITE_LAST_WITH_IMM;
	if (with_imm) {
		if (body_len < sizeof(imm_data))
			return -EBADMSG;
		memcpy(&imm_data, body, sizeof(imm_data));
		body += sizeof(imm_data);
		body_len -= sizeof(imm_data);
		if (uib_qp_pop_recv_slot(rx->qp, &slot) < 0) {
			if (rx->is_rc)
				rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_RNR);
			return -ENOENT;
		}
	}
	remain = rocev2_qp(rx->qp)->frag_write_total - rocev2_qp(rx->qp)->frag_write_offset;
	take = body_len > remain ? remain : body_len;
	if (uib_mr_place(rocev2_qp(rx->qp)->frag_write_mr,
			 rocev2_qp(rx->qp)->frag_write_va + rocev2_qp(rx->qp)->frag_write_offset,
			 body, take) < 0) {
		if (with_imm)
			uib_qp_release_recv(&slot);
		return -EIO;
	}
	rocev2_qp(rx->qp)->frag_write_offset += (uint32_t)take;
	rocev2_qp(rx->qp)->frag_write_active = 0;
	uib_mr_put(rocev2_qp(rx->qp)->frag_write_mr);
	rocev2_qp(rx->qp)->frag_write_mr = NULL;
	rocev2_rx_advance(rx);
	if (rx->is_rc)
		rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_ACK);
	if (with_imm) {
		uib_qp_emit_recv_slot_src(rx->qp, &slot, UIB_WC_SUCCESS,
					  UIB_WC_RECV_RDMA_WITH_IMM,
					  rocev2_qp(rx->qp)->frag_write_total,
					  rx->qp->dest_qp_num, imm_data,
					  rx->src);
		uib_qp_release_recv(&slot);
	}
	return 0;
}

static int rocev2_handle_read_request(struct rocev2_rx_packet *rx)
{
	const struct uib_reth *reth;
	uint64_t va;
	uint32_t rkey;
	uint32_t rlen;
	uint32_t mtu;
	uint32_t span;

	if (rx->body_len < UIB_RETH_BYTES)
		return -EBADMSG;
	reth = (const struct uib_reth *)rx->body;
	uib_reth_unpack(reth, &va, &rkey, &rlen);
	send_read_response(rx->qp, &rx->qp->remote_addr, rx->psn, va, rkey,
			   rlen);
	mtu = rx->qp->path_mtu ? rx->qp->path_mtu : UIB_PMTU;
	span = (rlen + mtu - 1) / mtu;
	if (!span)
		span = 1;
	rocev2_qp(rx->qp)->last_rx_psn = rx->psn + span - 1;
	rocev2_qp(rx->qp)->last_rx_valid = 1;
	rx->qp->rq_psn = rx->psn + span;
	return 0;
}

static int rocev2_handle_atomic_request(struct rocev2_rx_packet *rx)
{
	const struct uib_atomic_eth *atomic;
	struct uib_mr *mr;
	uint64_t compare;
	uint64_t operand;
	uint64_t expected;
	uint64_t old;
	uint64_t va;
	uint32_t rkey;

	if (rx->body_len < sizeof(*atomic))
		return -EBADMSG;
	atomic = (const struct uib_atomic_eth *)rx->body;
	va = be64toh(atomic->va);
	rkey = ntohl(atomic->rkey);
	operand = be64toh(atomic->swap_data);
	compare = be64toh(atomic->compare_data);
	if ((va & (UIB_ROCEV2_ATOMIC_ACK_BYTES - 1)) ||
	    !(rx->qp->access_flags & UIB_ACCESS_REMOTE_ATOMIC)) {
		rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_NAK_REM_ACC);
		return -EACCES;
	}
	mr = uib_mr_check_remote(rx->qp->pd, rkey, va, UIB_ROCEV2_ATOMIC_ACK_BYTES,
				 UIB_ACCESS_REMOTE_ATOMIC);
	if (!mr) {
		rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_NAK_REM_ACC);
		return -EACCES;
	}
	if (mr->kind != UIB_MR_HOST) {
		uib_mr_put(mr);
		rocev2_send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_NAK_REM_OP);
		return -EOPNOTSUPP;
	}
	if (rx->opcode == UIB_OP_FETCH_ADD) {
		old = __atomic_fetch_add((uint64_t *)(uintptr_t)va, operand,
					 __ATOMIC_SEQ_CST);
	} else {
		expected = compare;
		__atomic_compare_exchange_n((uint64_t *)(uintptr_t)va,
					    &expected, operand, 0,
					    __ATOMIC_SEQ_CST,
					    __ATOMIC_SEQ_CST);
		old = expected;
	}
	uib_mr_put(mr);
	rocev2_qp(rx->qp)->atomic_ack_psn = rx->psn;
	rocev2_qp(rx->qp)->atomic_ack_value = old;
	rocev2_qp(rx->qp)->atomic_ack_valid = 1;
	rocev2_rx_advance(rx);
	send_atomic_ack(rx->qp, rx->psn, old);
	return 0;
}

static int rocev2_handle_atomic_response(struct rocev2_rx_packet *rx)
{
	struct uib_outstanding *out;
	uint64_t wire_value;
	uint64_t value;
	size_t consumed = 0;
	int rc;

	if (rx->body_len < UIB_AETH_BYTES + UIB_ROCEV2_ATOMIC_ACK_BYTES)
		return -EBADMSG;
	memcpy(&wire_value, rx->body + UIB_AETH_BYTES, sizeof(wire_value));
	value = be64toh(wire_value);
	out = find_read_by_psn(rx->qp, rx->psn);
	if (!out)
		return 0;
	rc = uib_qp_scatter_at(rx->qp->pd, out->read_sgl, out->read_num_sge, 0,
			(const uint8_t *)&value, sizeof(value), &consumed);
	detach_outstanding(rx->qp, out);
	uib_qp_emit_send_wc(rx->qp, out->wr_id, out->wr_opcode, out->signaled,
		     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
		     (uint32_t)consumed);
	rocev2_free_outstanding(out);
	return rc;
}

static int rocev2_handle_read_response(struct rocev2_rx_packet *rx)
{
	struct uib_outstanding *out;
	const uint8_t *body = rx->body;
	size_t body_len = rx->body_len;
	size_t consumed = 0;
	uint32_t total;
	int rc;

	if (rx->opcode != UIB_OP_RDMA_READ_RESP_MIDDLE) {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		body += UIB_AETH_BYTES;
		body_len -= UIB_AETH_BYTES;
	}
	out = rx->opcode == UIB_OP_RDMA_READ_RESP_ONLY ||
		rx->opcode == UIB_OP_RDMA_READ_RESP_FIRST ?
		find_read_by_psn(rx->qp, rx->psn) : rocev2_qp(rx->qp)->frag_read_out;
	if (!out)
		return 0;
	if (rx->opcode == UIB_OP_RDMA_READ_RESP_FIRST)
		rocev2_qp(rx->qp)->frag_read_out = out;
	rc = uib_qp_scatter_at(rx->qp->pd, out->read_sgl, out->read_num_sge,
			rx->opcode == UIB_OP_RDMA_READ_RESP_ONLY ||
			rx->opcode == UIB_OP_RDMA_READ_RESP_FIRST ? 0 :
			rocev2_qp(rx->qp)->frag_read_offset,
			body, body_len, &consumed);
	if (rx->opcode == UIB_OP_RDMA_READ_RESP_FIRST) {
		rocev2_qp(rx->qp)->frag_read_offset = (uint32_t)consumed;
		return rc;
	}
	if (rx->opcode == UIB_OP_RDMA_READ_RESP_MIDDLE) {
		rocev2_qp(rx->qp)->frag_read_offset += (uint32_t)consumed;
		return rc;
	}
	total = rx->opcode == UIB_OP_RDMA_READ_RESP_ONLY ?
		(uint32_t)consumed :
		rocev2_qp(rx->qp)->frag_read_offset + (uint32_t)consumed;
	detach_outstanding(rx->qp, out);
	uib_qp_emit_send_wc(rx->qp, out->wr_id, out->wr_opcode, out->signaled,
		     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS, total);
	rocev2_free_outstanding(out);
	if (rx->opcode == UIB_OP_RDMA_READ_RESP_LAST) {
		rocev2_qp(rx->qp)->frag_read_out = NULL;
		rocev2_qp(rx->qp)->frag_read_offset = 0;
	}
	return rc;
}

static int rocev2_prepare_retry_list_locked(struct uib_qp *qp,
					    struct uib_outstanding *head,
					    uint32_t expected_psn)
{
	struct uib_outstanding *out;

	for (out = head; out; out = out->next) {
		if (!out->sent || rocev2_psn_cmp(out->psn, expected_psn) < 0)
			continue;
		if (!rocev2_psn_cmp(out->psn, expected_psn) &&
		    out->retries >= UIB_MAX_RETRIES)
			return -ETIMEDOUT;
		if (out->congestion_inflight) {
			uib_congestion_on_loss(&rocev2_qp(qp)->congestion,
					       out->nominal_len);
			out->congestion_inflight = 0;
		}
		rocev2_request_ack(out);
		out->sent = 0;
		if (!rocev2_psn_cmp(out->psn, expected_psn))
			out->retries++;
		out->deadline_ns = 0;
	}
	return 0;
}

static void rocev2_handle_sequence_nak(struct uib_qp *qp,
				       uint32_t expected_psn)
{
	int rc;

	pthread_mutex_lock(&qp->lock);
	if (rocev2_qp(qp)->sequence_retry_active &&
	    rocev2_psn_cmp(expected_psn, rocev2_qp(qp)->sequence_retry_psn) <= 0) {
		pthread_mutex_unlock(&qp->lock);
		return;
	}
	rocev2_qp(qp)->sequence_retry_psn = expected_psn;
	rocev2_qp(qp)->sequence_retry_active = 1;
	rc = rocev2_prepare_retry_list_locked(qp, rocev2_qp(qp)->out_head, expected_psn);
	if (!rc)
		rc = rocev2_prepare_retry_list_locked(qp, rocev2_qp(qp)->read_head,
						       expected_psn);
	if (rc < 0)
		qp->state = UIB_QPS_ERR;
	else {
		uib_qp_retry_cancel(qp);
		rocev2_drain_locked(qp);
	}
	pthread_mutex_unlock(&qp->lock);
	if (rc < 0)
		rocev2_fail_outstanding(qp, UIB_WC_RETRY_EXC_ERR);
}

static int rocev2_handle_acknowledgement(struct rocev2_rx_packet *rx)
{
	const struct uib_aeth *aeth;
	uint32_t msn;
	uint8_t syndrome;

	if (rx->body_len < UIB_AETH_BYTES)
		return -EBADMSG;
	aeth = (const struct uib_aeth *)rx->body;
	uib_aeth_unpack(aeth, &syndrome, &msn);
	if ((syndrome & UIB_AETH_CODE_MASK) == UIB_AETH_ACK_CODE) {
		ack_outstanding(rx->qp, rx->psn);
	} else if ((syndrome & UIB_AETH_CODE_MASK) == UIB_AETH_RNR_CODE) {
		return 0;
	} else if (syndrome == UIB_AETH_NAK_PSN_SEQ) {
		rocev2_handle_sequence_nak(rx->qp, rx->psn);
	} else {
		pthread_mutex_lock(&rx->qp->lock);
		rx->qp->state = UIB_QPS_ERR;
		pthread_mutex_unlock(&rx->qp->lock);
		rocev2_fail_outstanding(rx->qp,
				 syndrome == UIB_AETH_NAK_REM_ACC ?
				 UIB_WC_REM_ACCESS_ERR : UIB_WC_REM_OP_ERR);
	}
	return 0;
}

int uib_rocev2_handle_packet_meta(struct uib_qp *qp,
				  const struct sockaddr_in *src,
				  const void *packet, size_t len,
				  const struct uib_rx_meta *meta)
{
	const struct uib_bth *bth;
	struct rocev2_rx_packet rx;
	uint32_t dqpn;
	uint8_t pad;

	if (!qp || !src || !packet)
		return -EINVAL;
	if (len < UIB_BTH_BYTES + UIB_ICRC_BYTES)
		return -EBADMSG;
	if ((!meta || !meta->integrity_validated) &&
	    verify_icrc(packet, len) < 0)
		return -EBADMSG;
	bth = packet;
	memset(&rx, 0, sizeof(rx));
	rx.qp = qp;
	rx.src = src;
	uib_bth_unpack(bth, &rx.opcode, &dqpn, &rx.psn, &rx.ack_request);
	if (dqpn != qp->qp_num)
		return -ENOENT;
	rx.body = (const uint8_t *)packet + UIB_BTH_BYTES;
	rx.body_len = len - UIB_BTH_BYTES - UIB_ICRC_BYTES;
	rx.is_rc = qp->qp_type == UIB_QPT_RC;
	pad = uib_bth_pad(bth);
	if (pad > rx.body_len)
		return -EBADMSG;
	rx.body_len -= pad;
	if (meta && meta->ecn == IPTOS_ECN_CE && rx.opcode != UIB_OP_CNP)
		rocev2_send_cnp(qp, src);
	if (rocev2_check_request_psn(&rx))
		return 0;

	switch (rx.opcode) {
	case UIB_OP_CNP:
		rocev2_congestion_cnp(qp);
		return 0;
	case UIB_OP_SEND_ONLY:
	case UIB_OP_SEND_ONLY_WITH_IMM:
		return rocev2_handle_send_only(&rx);
	case UIB_OP_SEND_FIRST:
		return rocev2_handle_send_first(&rx);
	case UIB_OP_SEND_MIDDLE:
		return rocev2_handle_send_middle(&rx);
	case UIB_OP_SEND_LAST:
	case UIB_OP_SEND_LAST_WITH_IMM:
		return rocev2_handle_send_last(&rx);
	case UIB_OP_UD_SEND_ONLY:
	case UIB_OP_UD_SEND_ONLY_WITH_IMM:
		return rocev2_handle_ud_send(&rx);
	case UIB_OP_RDMA_WRITE_ONLY:
	case UIB_OP_RDMA_WRITE_ONLY_WITH_IMM:
		return rocev2_handle_write_only(&rx);
	case UIB_OP_RDMA_WRITE_FIRST:
		return rocev2_handle_write_first(&rx);
	case UIB_OP_RDMA_WRITE_MIDDLE:
		return rocev2_handle_write_middle(&rx);
	case UIB_OP_RDMA_WRITE_LAST:
	case UIB_OP_RDMA_WRITE_LAST_WITH_IMM:
		return rocev2_handle_write_last(&rx);
	case UIB_OP_RDMA_READ_REQ:
		return rocev2_handle_read_request(&rx);
	case UIB_OP_FETCH_ADD:
	case UIB_OP_COMPARE_SWAP:
		return rocev2_handle_atomic_request(&rx);
	case UIB_OP_ATOMIC_ACKNOWLEDGE:
		return rocev2_handle_atomic_response(&rx);
	case UIB_OP_RDMA_READ_RESP_ONLY:
	case UIB_OP_RDMA_READ_RESP_FIRST:
	case UIB_OP_RDMA_READ_RESP_MIDDLE:
	case UIB_OP_RDMA_READ_RESP_LAST:
		return rocev2_handle_read_response(&rx);
	case UIB_OP_ACKNOWLEDGE:
		return rocev2_handle_acknowledgement(&rx);
	default:
		return -ENOTSUP;
	}
}

static void rocev2_retransmit_list(struct uib_qp *qp,
				  struct uib_outstanding *head,
				  uint64_t now, uint64_t *next_deadline)
{
	struct uib_outstanding *next;
	struct uib_outstanding *out;

	for (out = head; out; out = next) {
		next = out->next;
		if (!out->sent)
			continue;
		if ((int64_t)(now - out->deadline_ns) >= 0) {
			if (out->retries >= UIB_MAX_RETRIES) {
				if (out->congestion_inflight)
					uib_congestion_on_loss(
						&rocev2_qp(qp)->congestion,
							       out->nominal_len);
				if (out->is_final || out->is_read)
					uib_qp_emit_send_wc(qp, out->wr_id,
						     out->wr_opcode, 1,
						     UIB_WC_RETRY_EXC_ERR, 0);
				rocev2_outstanding_unlink(qp, out);
				rocev2_free_outstanding(out);
				qp->state = UIB_QPS_ERR;
				continue;
			}
			if (out->congestion_inflight) {
				uib_congestion_on_loss(
					&rocev2_qp(qp)->congestion,
						       out->nominal_len);
				out->congestion_inflight = 0;
			}
			if (!uib_congestion_can_send(&rocev2_qp(qp)->congestion,
						     out->nominal_len)) {
				out->deadline_ns = now + qp->ctx->retry_timeout_ns;
				if (out->deadline_ns < *next_deadline)
					*next_deadline = out->deadline_ns;
				continue;
			}
			out->tx_time_ns = now;
			uib_congestion_on_send(&rocev2_qp(qp)->congestion,
					       out->nominal_len);
			out->congestion_inflight = 1;
			if (rocev2_send_outstanding(qp, out) < 0) {
				uib_congestion_on_loss(
					&rocev2_qp(qp)->congestion,
						       out->nominal_len);
				out->congestion_inflight = 0;
				out->deadline_ns = now + qp->ctx->retry_timeout_ns;
				if (out->deadline_ns < *next_deadline)
					*next_deadline = out->deadline_ns;
				continue;
			}
			out->retries++;
			out->deadline_ns = now + qp->ctx->retry_timeout_ns;
		}
		if (out->deadline_ns < *next_deadline)
			*next_deadline = out->deadline_ns;
	}
}

void uib_rocev2_retransmit_tick(struct uib_qp *qp, uint64_t now_ns)
{
	uint64_t next_deadline = UINT64_MAX;

	pthread_mutex_lock(&qp->lock);
	rocev2_retransmit_list(qp, rocev2_qp(qp)->out_head, now_ns, &next_deadline);
	rocev2_retransmit_list(qp, rocev2_qp(qp)->read_head, now_ns, &next_deadline);
	if (next_deadline != UINT64_MAX)
		uib_qp_retry_schedule(qp, next_deadline);
	else
		uib_qp_retry_cancel(qp);
	rocev2_drain_locked(qp);
	pthread_mutex_unlock(&qp->lock);
}
