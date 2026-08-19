/*
 * Wire formats and transport behavior follow the Ultra Ethernet
 * Specification 1.0.2 published by the Ultra Ethernet Consortium.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <arpa/inet.h>

#include "uib/uet.h"
#include "uib/protocol.h"
#include "uib/qp.h"
#include "uib/context.h"
#include "uib/transport.h"
#include "uib/mr.h"
#include "utils.h"

enum uib_uet_pdc_state {
	UIB_UET_PDC_CLOSED,
	UIB_UET_PDC_SYN_SENT,
	UIB_UET_PDC_ESTABLISHED,
};

struct uib_uet_wr_track {
	uint64_t wr_id;
	enum uib_wr_opcode opcode;
	int signaled;
	uint32_t total_len;
	uint32_t pending;
	struct uib_sge *read_sgl;
	int read_num_sge;
};

struct uib_uet_outstanding {
	struct uib_uet_outstanding *next;
	struct uib_uet_wr_track *track;
	struct sockaddr_in dst;
	uint8_t *packet;
	size_t packet_len;
	uint64_t deadline_ns;
	uint32_t psn;
	uint32_t read_offset;
	uint32_t read_len;
	uint16_t msg_id;
	uint8_t retries;
};

struct uib_uet_qp {
	enum uib_uet_pdc_state state;
	uint16_t spdcid;
	uint16_t dpdcid;
	uint16_t next_msg_id;
	uint32_t next_psn;
	uint32_t rx_base_psn;
	uint64_t rx_bitmap[2];
	uint32_t out_count;
	int rx_initialized;
	int tx_initialized;
	struct uib_uet_outstanding *out_head;
	struct uib_rq_slot recv_slot;
	uint16_t recv_msg_id;
	uint32_t recv_total;
	uint32_t recv_last_psn;
	int recv_active;
	int recv_eom;
};

static void uet_debug(struct uib_qp *qp, const char *format, ...)
{
	va_list args;

	if (!getenv("UIB_UET_DEBUG"))
		return;
	fprintf(stderr, "uib: uet qpn %u: ", qp->qp_num);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static int uet_psn_before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

uint16_t uib_uet_prologue_pack(uint8_t type, uint8_t next, uint8_t flags)
{
	uint16_t value;

	value = ((uint16_t)type & UIB_UET_PROLOGUE_TYPE_MASK) <<
		UIB_UET_PROLOGUE_TYPE_SHIFT;
	value |= ((uint16_t)next & UIB_UET_PROLOGUE_NEXT_MASK) <<
		 UIB_UET_PROLOGUE_NEXT_SHIFT;
	value |= flags & UIB_UET_PROLOGUE_FLAGS_MASK;
	return htons(value);
}

uint8_t uib_uet_prologue_type(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)((ntohs(prologue->type_next_flags) >>
			  UIB_UET_PROLOGUE_TYPE_SHIFT) &
			 UIB_UET_PROLOGUE_TYPE_MASK);
}

uint8_t uib_uet_prologue_next(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)((ntohs(prologue->type_next_flags) >>
			  UIB_UET_PROLOGUE_NEXT_SHIFT) &
			 UIB_UET_PROLOGUE_NEXT_MASK);
}

uint8_t uib_uet_prologue_flags(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)(ntohs(prologue->type_next_flags) &
			 UIB_UET_PROLOGUE_FLAGS_MASK);
}

int uib_uet_build_ack(void *packet, size_t capacity, uint32_t cack_psn,
		      uint32_t ack_psn, uint16_t spdcid, uint16_t dpdcid,
		      uint8_t next_header)
{
	struct uib_uet_pds_ack *ack = packet;
	int32_t offset = (int32_t)(ack_psn - cack_psn);

	if (!packet || capacity < sizeof(*ack) ||
	    offset < INT16_MIN || offset > INT16_MAX)
		return -EINVAL;

	memset(ack, 0, sizeof(*ack));
	ack->prologue.type_next_flags =
		uib_uet_prologue_pack(UIB_UET_PDS_TYPE_ACK, next_header, 0);
	ack->ack_psn_offset = htons((uint16_t)(int16_t)offset);
	ack->cack_psn = htonl(cack_psn);
	ack->spdcid = htons(spdcid);
	ack->dpdcid = htons(dpdcid);
	return sizeof(*ack);
}

static uint16_t uet_ses_rsp_pack(uint8_t opcode, uint8_t response_code)
{
	uint16_t value;

	value = ((uint16_t)opcode & UIB_UET_SES_RSP_OPCODE_MASK) <<
		UIB_UET_SES_RSP_OPCODE_SHIFT;
	value |= response_code & UIB_UET_SES_RSP_RC_MASK;
	return htons(value);
}

static int uet_packet_qpn(const void *packet, size_t len, uint32_t *qpn)
{
	const struct uib_uet_prologue *prologue = packet;
	uint8_t type;

	if (!packet || !qpn || len < sizeof(*prologue))
		return -EINVAL;
	type = uib_uet_prologue_type(prologue);
	switch (type) {
	case UIB_UET_PDS_TYPE_RUD_REQ: {
		const struct uib_uet_pds_req *req = packet;

		if (len < sizeof(*req) + sizeof(struct uib_uet_ses_req))
			return -EBADMSG;
		*qpn = ntohs(req->dpdcid);
		if (!*qpn) {
			const struct uib_uet_ses_req *ses =
				(const struct uib_uet_ses_req *)(req + 1);

			*qpn = ntohs(ses->pid_on_fep) & UIB_UET_SES_PID_MASK;
		}
		return *qpn ? 0 : -ENOENT;
	}
	case UIB_UET_PDS_TYPE_ACK:
	case UIB_UET_PDS_TYPE_ACK_CC: {
		const struct uib_uet_pds_ack *ack = packet;

		if (len < sizeof(*ack))
			return -EBADMSG;
		*qpn = ntohs(ack->dpdcid);
		return *qpn ? 0 : -ENOENT;
	}
	case UIB_UET_PDS_TYPE_NACK: {
		const struct uib_uet_pds_nack *nack = packet;

		if (len < sizeof(*nack))
			return -EBADMSG;
		*qpn = ntohs(nack->dpdcid);
		return *qpn ? 0 : -ENOENT;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static int uet_qp_init(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = calloc(1, sizeof(*uet));

	if (!uet)
		return -ENOMEM;
	uet->spdcid = (uint16_t)qp->qp_num;
	uet->next_msg_id = 1;
	qp->protocol_private = uet;
	return 0;
}

static void uet_free_track(struct uib_uet_wr_track *track)
{
	if (!track)
		return;
	free(track->read_sgl);
	free(track);
}

static void uet_free_outstanding(struct uib_uet_outstanding *out)
{
	if (!out)
		return;
	free(out->packet);
	free(out);
}

static void uet_qp_fini(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if (!uet)
		return;
	while (uet->out_head) {
		struct uib_uet_wr_track *track = uet->out_head->track;
		struct uib_uet_outstanding **link = &uet->out_head;

		while (*link) {
			struct uib_uet_outstanding *out = *link;

			if (out->track != track) {
				link = &out->next;
				continue;
			}
			*link = out->next;
			uet_free_outstanding(out);
		}
		uet_free_track(track);
	}
	if (uet->recv_active)
		free(uet->recv_slot.sg_list);
	free(uet);
	qp->protocol_private = NULL;
}

static void uet_track_done(struct uib_qp *qp, struct uib_uet_wr_track *track,
			   enum uib_wc_status status)
{
	if (!track->pending)
		return;
	if (status != UIB_WC_SUCCESS)
		track->pending = 1;
	track->pending--;
	if (track->pending)
		return;

	uib_qp_emit_send(qp, track->wr_id, track->opcode,
			 track->signaled, status,
			 track->opcode == UIB_WR_RDMA_READ ? track->total_len : 0);
	uet_free_track(track);
}

static void uet_remove_track_locked(struct uib_qp *qp,
				    struct uib_uet_wr_track *track,
				    enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding **link = &uet->out_head;

	while (*link) {
		struct uib_uet_outstanding *out = *link;

		if (out->track != track) {
			link = &out->next;
			continue;
		}
		*link = out->next;
		uet->out_count--;
		uet_free_outstanding(out);
	}
	track->pending = 1;
	uet_track_done(qp, track, status);
}

static struct uib_uet_outstanding *
uet_find_outstanding(struct uib_uet_qp *uet, uint32_t psn, uint16_t msg_id,
		     struct uib_uet_outstanding ***link_out)
{
	struct uib_uet_outstanding **link = &uet->out_head;

	while (*link) {
		if ((*link)->psn == psn && (!msg_id || (*link)->msg_id == msg_id)) {
			if (link_out)
				*link_out = link;
			return *link;
		}
		link = &(*link)->next;
	}
	return NULL;
}

static int uet_send_response(struct uib_qp *qp,
			     const struct sockaddr_in *destination,
			     const struct uib_uet_pds_req *request,
			     const struct uib_uet_ses_req *ses_request,
			     uint32_t cack_psn, uint8_t response_code,
			     const void *data, size_t data_len)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct sockaddr_in dst = *destination;
	struct uib_uet_ses_rsp *ses;
	uint8_t *packet;
	size_t packet_len;
	uint8_t next;
	int rc;

	next = data_len ? UIB_UET_NEXT_RSP_DATA : UIB_UET_NEXT_RSP;
	packet_len = sizeof(struct uib_uet_pds_ack) + sizeof(*ses) + data_len;
	packet = calloc(1, packet_len);
	if (!packet)
		return -ENOMEM;
	rc = uib_uet_build_ack(packet, packet_len, cack_psn,
			       ntohl(request->psn), uet->spdcid,
			       ntohs(request->spdcid), next);
	if (rc < 0) {
		free(packet);
		return rc;
	}
	ses = (struct uib_uet_ses_rsp *)(packet + rc);
	ses->list_opcode_version_rc =
		uet_ses_rsp_pack(data_len ? UIB_UET_SES_RSP_WITH_DATA :
					     UIB_UET_SES_RSP_RESPONSE,
				 response_code);
	ses->msg_id = ses_request->msg_id;
	ses->index_gen_job_id = ses_request->index_gen_job_id;
	ses->modified_len = htonl((uint32_t)data_len);
	if (data_len)
		memcpy(ses + 1, data, data_len);
	if (!dst.sin_port)
		dst.sin_port = htons(UIB_UET_DEFAULT_PORT);
	rc = uib_transport_send(qp->ctx, &dst, packet, packet_len);
	free(packet);
	return rc;
}

static void uet_rx_advance(struct uib_uet_qp *uet)
{
	while (uet->rx_bitmap[0] & 1) {
		uet->rx_bitmap[0] >>= 1;
		uet->rx_bitmap[0] |= uet->rx_bitmap[1] << 63;
		uet->rx_bitmap[1] >>= 1;
		uet->rx_base_psn++;
	}
}

static int uet_rx_mark(struct uib_uet_qp *uet, uint32_t psn, int *duplicate)
{
	uint32_t offset;
	uint64_t mask;
	unsigned int word;

	*duplicate = 0;
	if (!uet->rx_initialized)
		return -EINVAL;
	if (uet_psn_before(psn, uet->rx_base_psn)) {
		*duplicate = 1;
		return 0;
	}
	offset = psn - uet->rx_base_psn;
	if (offset >= UIB_UET_PDC_MPR)
		return -ERANGE;
	word = offset / 64;
	mask = 1ULL << (offset % 64);
	if (uet->rx_bitmap[word] & mask) {
		*duplicate = 1;
		return 0;
	}
	uet->rx_bitmap[word] |= mask;
	uet_rx_advance(uet);
	return 0;
}

static int uet_handle_send(struct uib_qp *qp,
			   const struct uib_uet_ses_req *ses,
			   const uint8_t *data, size_t data_len,
			   uint32_t psn, uint32_t source_pdc)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint8_t flags = ses->flags & 0x3f;
	uint32_t offset = (uint32_t)be64toh(ses->buffer_offset);
	size_t consumed = 0;
	int rc;

	if (!uet->recv_active) {
		rc = uib_qp_pop_recv(qp, &uet->recv_slot);
		if (rc < 0)
			return rc;
		uet->recv_active = 1;
		uet->recv_msg_id = ntohs(ses->msg_id);
		uet->recv_total = ntohl(ses->request_len);
	}
	if (!uet->recv_active || uet->recv_msg_id != ntohs(ses->msg_id))
		return -EBADMSG;

	rc = uib_qp_scatter(qp->pd, uet->recv_slot.sg_list,
			    uet->recv_slot.num_sge, offset, data, data_len,
			    &consumed);
	if (rc < 0)
		return rc;
	if (consumed != data_len)
		return -EMSGSIZE;
	if (flags & UIB_UET_SES_FLAG_EOM) {
		uet->recv_last_psn = psn;
		uet->recv_eom = 1;
	}
	if (uet->recv_eom && uet_psn_before(uet->recv_last_psn,
					     uet->rx_base_psn)) {
		uib_qp_emit_recv(qp, uet->recv_slot.wr_id, UIB_WC_SUCCESS,
				 UIB_WC_RECV, uet->recv_total, source_pdc, 0);
		free(uet->recv_slot.sg_list);
		memset(&uet->recv_slot, 0, sizeof(uet->recv_slot));
		uet->recv_active = 0;
		uet->recv_eom = 0;
	}
	return 0;
}

static int uet_handle_write(struct uib_qp *qp,
			    const struct uib_uet_ses_req *ses,
			    const uint8_t *data, size_t data_len)
{
	uint64_t address = be64toh(ses->buffer_offset);
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	struct uib_mr *mr;

	if (ntohl(ses->request_len) != data_len)
		return -EMSGSIZE;
	mr = uib_mr_check_remote(qp->pd, rkey, address, data_len,
				 UIB_ACCESS_REMOTE_WRITE);
	if (!mr)
		return -EACCES;
	return uib_mr_place(mr, address, data, data_len);
}

static int uet_handle_read(struct uib_qp *qp,
			   const struct uib_uet_ses_req *ses,
			   const void **data, size_t *data_len)
{
	uint64_t address = be64toh(ses->buffer_offset);
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	uint32_t length = ntohl(ses->request_len);
	struct uib_mr *mr;

	mr = uib_mr_check_remote(qp->pd, rkey, address, length,
				 UIB_ACCESS_REMOTE_READ);
	if (!mr)
		return -EACCES;
	*data = (const void *)(uintptr_t)address;
	*data_len = length;
	return 0;
}

static int uet_handle_request(struct uib_qp *qp,
			      const struct sockaddr_in *src,
			      const void *packet, size_t len)
{
	const struct uib_uet_pds_req *req = packet;
	const struct uib_uet_ses_req *ses;
	const uint8_t *data;
	const void *response_data = NULL;
	size_t data_len, response_len = 0;
	struct uib_uet_qp *uet = qp->protocol_private;
	uint32_t psn, cack;
	uint8_t response_code = UIB_UET_SES_RC_OK;
	uint8_t opcode;
	int duplicate;
	int rc;

	if (len < sizeof(*req) + sizeof(*ses) ||
	    uib_uet_prologue_next(&req->prologue) != UIB_UET_NEXT_REQ_STD)
		return -EBADMSG;
	ses = (const struct uib_uet_ses_req *)(req + 1);
	data = (const uint8_t *)(ses + 1);
	data_len = len - sizeof(*req) - sizeof(*ses);
	psn = ntohl(req->psn);

	pthread_mutex_lock(&qp->lock);
	uet->dpdcid = ntohs(req->spdcid);
	if (uib_uet_prologue_flags(&req->prologue) & UIB_UET_REQ_FLAG_SYN)
		uet->state = UIB_UET_PDC_ESTABLISHED;
	if (!uet->rx_initialized) {
		uet->rx_base_psn = qp->rq_psn;
		uet->rx_initialized = 1;
	}
	rc = uet_rx_mark(uet, psn, &duplicate);
	cack = uet->rx_base_psn - 1;
	pthread_mutex_unlock(&qp->lock);
	uet_debug(qp, "rx request psn=%u cack=%u msg=%u duplicate=%d rc=%d",
		  psn, cack, ntohs(ses->msg_id), duplicate, rc);
	if (rc < 0)
		return rc;

	opcode = ses->opcode & 0x3f;
	if (!duplicate || opcode == UIB_UET_SES_READ) {
		switch (opcode) {
		case UIB_UET_SES_SEND:
			rc = uet_handle_send(qp, ses, data, data_len, psn,
					     ntohs(req->spdcid));
			break;
		case UIB_UET_SES_WRITE:
			rc = uet_handle_write(qp, ses, data, data_len);
			break;
		case UIB_UET_SES_READ:
			rc = uet_handle_read(qp, ses, &response_data,
					     &response_len);
			break;
		default:
			rc = -EOPNOTSUPP;
			break;
		}
		if (rc == -EACCES)
			response_code = UIB_UET_SES_RC_BAD_MKEY;
		else if (rc < 0)
			response_code = UIB_UET_SES_RC_UNSUPP_OP;
	}

	return uet_send_response(qp, src, req, ses, cack, response_code,
				 response_data, response_len);
}

static void uet_complete_out_locked(struct uib_qp *qp,
				    struct uib_uet_outstanding **link,
				    enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out = *link;
	struct uib_uet_wr_track *track = out->track;

	*link = out->next;
	uet->out_count--;
	uet_free_outstanding(out);
	uet_track_done(qp, track, status);
}

static int uet_handle_ack(struct uib_qp *qp, const void *packet, size_t len)
{
	const struct uib_uet_pds_ack *ack = packet;
	const struct uib_uet_ses_rsp *ses;
	const uint8_t *data;
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding **exact_link = NULL;
	struct uib_uet_outstanding *exact;
	uint32_t cack, ack_psn;
	uint16_t msg_id, packed;
	size_t header_len = sizeof(*ack);
	size_t data_len;
	uint8_t response_code;
	int32_t offset;

	if (uib_uet_prologue_type(&ack->prologue) == UIB_UET_PDS_TYPE_ACK_CC)
		header_len += sizeof(struct uib_uet_pds_ack_ext);
	if (len < header_len + sizeof(*ses))
		return -EBADMSG;
	ses = (const struct uib_uet_ses_rsp *)((const uint8_t *)packet + header_len);
	data = (const uint8_t *)(ses + 1);
	data_len = len - header_len - sizeof(*ses);
	cack = ntohl(ack->cack_psn);
	offset = (int16_t)ntohs(ack->ack_psn_offset);
	ack_psn = cack + offset;
	msg_id = ntohs(ses->msg_id);
	packed = ntohs(ses->list_opcode_version_rc);
	response_code = packed & UIB_UET_SES_RSP_RC_MASK;
	uet_debug(qp, "rx ack psn=%u cack=%u msg=%u rc=%u", ack_psn, cack,
		  msg_id, response_code);

	pthread_mutex_lock(&qp->lock);
	uet->dpdcid = ntohs(ack->spdcid);
	uet->state = UIB_UET_PDC_ESTABLISHED;
	exact = uet_find_outstanding(uet, ack_psn, msg_id, &exact_link);
	if (response_code != UIB_UET_SES_RC_OK && exact) {
		struct uib_uet_wr_track *track = exact->track;
		enum uib_wc_status status = response_code == UIB_UET_SES_RC_BAD_MKEY ?
			UIB_WC_REM_ACCESS_ERR : UIB_WC_REM_OP_ERR;

		uet_remove_track_locked(qp, track, status);
		pthread_mutex_unlock(&qp->lock);
		return -EREMOTEIO;
	}
	if (response_code != UIB_UET_SES_RC_OK) {
		pthread_mutex_unlock(&qp->lock);
		return 0;
	}

	struct uib_uet_outstanding **link = &uet->out_head;

	while (*link) {
		struct uib_uet_outstanding *out = *link;

		if (out->track->opcode == UIB_WR_RDMA_READ ||
		    uet_psn_before(cack, out->psn)) {
			link = &out->next;
			continue;
		}
		uet_complete_out_locked(qp, link, UIB_WC_SUCCESS);
	}
	exact = uet_find_outstanding(uet, ack_psn, msg_id, &exact_link);
	if (!exact) {
		pthread_mutex_unlock(&qp->lock);
		return 0;
	}

	if (exact->track->opcode == UIB_WR_RDMA_READ) {
		size_t consumed = 0;
		int rc;

		if (data_len != exact->read_len) {
			struct uib_uet_wr_track *track = exact->track;

			uet_remove_track_locked(qp, track, UIB_WC_REM_OP_ERR);
			pthread_mutex_unlock(&qp->lock);
			return -EBADMSG;
		}
		rc = uib_qp_scatter(qp->pd, exact->track->read_sgl,
				    exact->track->read_num_sge,
				    exact->read_offset, data, data_len, &consumed);
		if (rc < 0 || consumed != data_len) {
			struct uib_uet_wr_track *track = exact->track;

			uet_remove_track_locked(qp, track, UIB_WC_LOC_PROT_ERR);
			pthread_mutex_unlock(&qp->lock);
			return rc < 0 ? rc : -EMSGSIZE;
		}
		uet_complete_out_locked(qp, exact_link, UIB_WC_SUCCESS);
	} else {
		uet_complete_out_locked(qp, exact_link, UIB_WC_SUCCESS);
	}
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static int uet_handle_nack(struct uib_qp *qp, const void *packet, size_t len)
{
	const struct uib_uet_pds_nack *nack = packet;
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;

	if (len < sizeof(*nack))
		return -EBADMSG;
	pthread_mutex_lock(&qp->lock);
	out = uet_find_outstanding(uet, ntohl(nack->nack_psn), 0, NULL);
	if (out)
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
	pthread_mutex_unlock(&qp->lock);
	return out ? -EREMOTEIO : 0;
}

static int uet_handle_packet(struct uib_qp *qp,
			     const struct sockaddr_in *src,
			     const void *packet, size_t len)
{
	const struct uib_uet_prologue *prologue = packet;

	if (!packet || len < sizeof(*prologue))
		return -EBADMSG;
	switch (uib_uet_prologue_type(prologue)) {
	case UIB_UET_PDS_TYPE_RUD_REQ:
		return uet_handle_request(qp, src, packet, len);
	case UIB_UET_PDS_TYPE_ACK:
	case UIB_UET_PDS_TYPE_ACK_CC:
		return uet_handle_ack(qp, packet, len);
	case UIB_UET_PDS_TYPE_NACK:
		return uet_handle_nack(qp, packet, len);
	default:
		return -EOPNOTSUPP;
	}
}

static struct uib_uet_wr_track *uet_alloc_track(struct uib_qp *qp,
						struct uib_send_wr *wr,
						uint32_t total_len,
						uint32_t packets)
{
	struct uib_uet_wr_track *track = calloc(1, sizeof(*track));

	if (!track)
		return NULL;
	track->wr_id = wr->wr_id;
	track->opcode = wr->opcode;
	track->signaled = !!(wr->send_flags & UIB_SEND_SIGNALED);
	track->total_len = total_len;
	track->pending = packets;
	if (wr->opcode == UIB_WR_RDMA_READ) {
		track->read_sgl = calloc(wr->num_sge, sizeof(*track->read_sgl));
		if (!track->read_sgl) {
			free(track);
			return NULL;
		}
		memcpy(track->read_sgl, wr->sg_list,
		       wr->num_sge * sizeof(*track->read_sgl));
		track->read_num_sge = wr->num_sge;
	}
	(void)qp;
	return track;
}

static int uet_post_one(struct uib_qp *qp, struct uib_send_wr *wr)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *track;
	struct sockaddr_in dst = qp->remote_addr;
	uint8_t *message = NULL;
	uint32_t total_len = 0;
	uint32_t chunk_max, packets;
	uint16_t wr_msg_id;
	uint8_t ses_opcode;
	int rc = 0;

	for (int i = 0; i < wr->num_sge; i++) {
		if (UINT32_MAX - total_len < wr->sg_list[i].length)
			return -EMSGSIZE;
		total_len += wr->sg_list[i].length;
	}
	if (wr->opcode != UIB_WR_RDMA_READ) {
		message = malloc(total_len ? total_len : 1);
		if (!message)
			return -ENOMEM;
		rc = uib_qp_gather(qp->pd, wr->sg_list, wr->num_sge,
				   message, total_len);
		if (rc < 0) {
			free(message);
			return rc;
		}
	}

	chunk_max = qp->path_mtu > sizeof(struct uib_uet_pds_req) +
			 sizeof(struct uib_uet_ses_req) ?
		qp->path_mtu - sizeof(struct uib_uet_pds_req) -
			sizeof(struct uib_uet_ses_req) : 0;
	if (wr->opcode == UIB_WR_RDMA_READ &&
	    chunk_max > qp->path_mtu - sizeof(struct uib_uet_pds_ack) -
			 sizeof(struct uib_uet_ses_rsp))
		chunk_max = qp->path_mtu - sizeof(struct uib_uet_pds_ack) -
			sizeof(struct uib_uet_ses_rsp);
	if (!chunk_max) {
		free(message);
		return -EMSGSIZE;
	}
	packets = total_len ? (total_len + chunk_max - 1) / chunk_max : 1;
	if (packets > UIB_UET_PDC_MPR) {
		free(message);
		return -EMSGSIZE;
	}
	track = uet_alloc_track(qp, wr, total_len, packets);
	if (!track) {
		free(message);
		return -ENOMEM;
	}

	switch (wr->opcode) {
	case UIB_WR_SEND:
	case UIB_WR_SEND_WITH_IMM:
		ses_opcode = UIB_UET_SES_SEND;
		break;
	case UIB_WR_RDMA_WRITE:
		ses_opcode = UIB_UET_SES_WRITE;
		break;
	case UIB_WR_RDMA_READ:
		ses_opcode = UIB_UET_SES_READ;
		break;
	default:
		uet_free_track(track);
		free(message);
		return -EOPNOTSUPP;
	}
	if (!dst.sin_port)
		dst.sin_port = htons(UIB_UET_DEFAULT_PORT);
	pthread_mutex_lock(&qp->lock);
	wr_msg_id = uet->next_msg_id++;
	pthread_mutex_unlock(&qp->lock);

	for (uint32_t i = 0; i < packets; i++) {
		uint32_t offset = i * chunk_max;
		uint32_t chunk = total_len - offset;
		struct uib_uet_outstanding *out;
		struct uib_uet_pds_req *req;
		struct uib_uet_ses_req *ses;
		size_t data_len, packet_len;
		uint8_t flags = UIB_UET_REQ_FLAG_AR;

		if (chunk > chunk_max)
			chunk = chunk_max;
		data_len = wr->opcode == UIB_WR_RDMA_READ ? 0 : chunk;
		packet_len = sizeof(*req) + sizeof(*ses) + data_len;
		out = calloc(1, sizeof(*out));
		if (!out) {
			rc = -ENOMEM;
			goto fail_track;
		}
		out->packet = calloc(1, packet_len);
		if (!out->packet) {
			free(out);
			rc = -ENOMEM;
			goto fail_track;
		}
		req = (struct uib_uet_pds_req *)out->packet;
		ses = (struct uib_uet_ses_req *)(req + 1);

		pthread_mutex_lock(&qp->lock);
		if (uet->out_count >= UIB_UET_PDC_MPR) {
			pthread_mutex_unlock(&qp->lock);
			uet_free_outstanding(out);
			rc = -EAGAIN;
			goto fail_track;
		}
		if (!uet->tx_initialized) {
			uet->next_psn = qp->sq_psn;
			uet->tx_initialized = 1;
		}
		if (uet->state == UIB_UET_PDC_CLOSED) {
			flags |= UIB_UET_REQ_FLAG_SYN;
			uet->state = UIB_UET_PDC_SYN_SENT;
		}
		out->psn = uet->next_psn++;
		out->msg_id = wr_msg_id;
		pthread_mutex_unlock(&qp->lock);

		req->prologue.type_next_flags =
			uib_uet_prologue_pack(UIB_UET_PDS_TYPE_RUD_REQ,
					       UIB_UET_NEXT_REQ_STD, flags);
		req->psn = htonl(out->psn);
		req->spdcid = htons(uet->spdcid);
		req->dpdcid = htons((uint16_t)qp->dest_qp_num);
		ses->opcode = ses_opcode;
		if (i == 0)
			ses->flags |= UIB_UET_SES_FLAG_SOM;
		if (i == packets - 1)
			ses->flags |= UIB_UET_SES_FLAG_EOM;
		ses->msg_id = htons(out->msg_id);
		ses->index_gen_job_id = 0;
		ses->pid_on_fep = htons((uint16_t)qp->dest_qp_num &
					 UIB_UET_SES_PID_MASK);
		if (wr->opcode == UIB_WR_SEND ||
		    wr->opcode == UIB_WR_SEND_WITH_IMM) {
			ses->buffer_offset = htobe64(offset);
			ses->request_len = htonl(total_len);
		} else {
			ses->buffer_offset = htobe64(wr->rdma.remote_addr + offset);
			ses->match_bits = htobe64(wr->rdma.rkey);
			ses->request_len = htonl(chunk);
		}
		if (data_len)
			memcpy(ses + 1, message + offset, data_len);

		out->track = track;
		out->dst = dst;
		out->packet_len = packet_len;
		out->read_offset = offset;
		out->read_len = chunk;
		out->deadline_ns = uib_now_ns() + UIB_UET_RTX_NS;
		pthread_mutex_lock(&qp->lock);
		out->next = uet->out_head;
		uet->out_head = out;
		uet->out_count++;
		uet_debug(qp, "tx request psn=%u msg=%u opcode=%u len=%zu",
			  out->psn, out->msg_id, ses_opcode, data_len);
		rc = uib_transport_send(qp->ctx, &dst, out->packet, packet_len);
		pthread_mutex_unlock(&qp->lock);
		if (rc < 0)
			goto fail_track;
	}
	free(message);
	return 0;

fail_track:
	pthread_mutex_lock(&qp->lock);
	uet_remove_track_locked(qp, track, UIB_WC_LOC_PROT_ERR);
	pthread_mutex_unlock(&qp->lock);
	free(message);
	return rc;
}

static int uet_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			 struct uib_send_wr **bad)
{
	for (struct uib_send_wr *current = wr; current; current = current->next) {
		int rc = uet_post_one(qp, current);

		if (rc < 0) {
			if (bad)
				*bad = current;
			return rc;
		}
	}
	return 0;
}

static void uet_mark_retransmit(void *packet)
{
	struct uib_uet_prologue *prologue = packet;
	uint16_t value = ntohs(prologue->type_next_flags);

	value |= UIB_UET_REQ_FLAG_RETX;
	prologue->type_next_flags = htons(value);
}

static void uet_retransmit_tick(struct uib_qp *qp, uint64_t now_ns)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;

	pthread_mutex_lock(&qp->lock);
	out = uet->out_head;
	while (out) {
		if ((int64_t)(now_ns - out->deadline_ns) < 0) {
			out = out->next;
			continue;
		}
		if (out->retries >= UIB_UET_MAX_RETRIES) {
			struct uib_uet_wr_track *track = out->track;

			uet_remove_track_locked(qp, track, UIB_WC_RETRY_EXC_ERR);
			out = uet->out_head;
			continue;
		}
		uet_mark_retransmit(out->packet);
		uet_debug(qp, "retransmit psn=%u msg=%u attempt=%u", out->psn,
			  out->msg_id, out->retries + 1);
		uib_transport_send(qp->ctx, &out->dst, out->packet,
				   out->packet_len);
		out->retries++;
		out->deadline_ns = now_ns + UIB_UET_RTX_NS;
		out = out->next;
	}
	pthread_mutex_unlock(&qp->lock);
}

const struct uib_protocol_ops uib_uet_protocol_ops = {
	.name = "uet",
	.default_port = UIB_UET_DEFAULT_PORT,
	.min_packet = sizeof(struct uib_uet_prologue),
	.qp_init = uet_qp_init,
	.qp_fini = uet_qp_fini,
	.packet_qpn = uet_packet_qpn,
	.post_send = uet_post_send,
	.handle_packet = uet_handle_packet,
	.retransmit_tick = uet_retransmit_tick,
};
