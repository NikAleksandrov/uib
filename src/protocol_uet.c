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
#include "uib/uet_nscc.h"
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
	struct uib_uet_read_fragment {
		uint32_t offset;
		uint32_t length;
	} *read_fragments;
	uint32_t read_fragment_count;
	uint32_t read_fragment_capacity;
	uint32_t read_received;
};

struct uib_uet_outstanding {
	struct uib_uet_outstanding *next;
	struct uib_uet_wr_track *track;
	struct sockaddr_in dst;
	uint8_t *packet;
	size_t packet_len;
	uint64_t deadline_ns;
	uint64_t tx_time_ns;
	uint32_t psn;
	uint32_t nominal_len;
	uint32_t read_offset;
	uint32_t read_len;
	uint16_t msg_id;
	uint8_t retries;
	uint8_t sent;
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
	struct uib_uet_nscc nscc;
	uint32_t receiver_bytes;
	uint32_t previous_received_units;
	int nscc_enabled;
	struct uib_uet_session_config session;
	struct uib_rq_slot recv_slot;
	uint16_t recv_msg_id;
	uint32_t recv_total;
	uint32_t recv_last_psn;
	int recv_active;
	int recv_eom;
	uint32_t recv_imm;
	int recv_has_imm;
	struct {
		uint32_t psn;
		uint16_t msg_id;
		uint8_t response_code;
		uint8_t data_len;
		uint32_t modified_len;
		uint8_t data[16];
		int valid;
	} response_cache[UIB_UET_PDC_MPR];
	uint32_t response_cache_next;
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

void uib_uet_nscc_wire_pack(struct uib_uet_nscc_wire_state *state,
			    uint32_t service_time_ns, uint8_t restore,
			    uint8_t penalty, uint32_t received_bytes,
			    uint16_t out_of_order)
{
	uint32_t received_units = (received_bytes + 255U) >> 8;
	uint32_t receiver_state;
	uint32_t service_units = (service_time_ns + 127U) >> 7;

	if (service_units > UINT16_MAX)
		service_units = UINT16_MAX;
	receiver_state = (restore ? UIB_UET_NSCC_RESTORE_MASK : 0) |
		((uint32_t)(penalty & UIB_UET_NSCC_PENALTY_MASK) <<
		 UIB_UET_NSCC_PENALTY_SHIFT) |
		(received_units & UIB_UET_NSCC_RCVD_MASK);
	state->service_time = htons((uint16_t)service_units);
	state->receiver_state = htonl(receiver_state);
	state->out_of_order_count = htons(out_of_order);
}

void uib_uet_nscc_wire_unpack(const struct uib_uet_nscc_wire_state *state,
			      uint32_t *service_time_ns, uint8_t *restore,
			      uint8_t *penalty, uint32_t *received_units,
			      uint16_t *out_of_order)
{
	uint32_t receiver_state = ntohl(state->receiver_state);

	if (service_time_ns)
		*service_time_ns = (uint32_t)ntohs(state->service_time) << 7;
	if (restore)
		*restore = !!(receiver_state & UIB_UET_NSCC_RESTORE_MASK);
	if (penalty)
		*penalty = (receiver_state >> UIB_UET_NSCC_PENALTY_SHIFT) &
			UIB_UET_NSCC_PENALTY_MASK;
	if (received_units)
		*received_units = receiver_state & UIB_UET_NSCC_RCVD_MASK;
	if (out_of_order)
		*out_of_order = ntohs(state->out_of_order_count);
}

uint64_t uib_uet_ses_fragment_info(uint32_t payload_len,
				   uint32_t message_offset)
{
	return htobe64(((uint64_t)(payload_len & 0x3fffU) << 32) |
			message_offset);
}

uint32_t uib_uet_ses_payload_len(const struct uib_uet_ses_req *request,
				 size_t packet_payload_len)
{
	uint64_t fragment;

	if (request->flags & UIB_UET_SES_FLAG_SOM)
		return (uint32_t)packet_payload_len;
	fragment = be64toh(request->header_data);
	return (uint32_t)(fragment >> 32) & 0x3fffU;
}

uint32_t uib_uet_ses_message_offset(const struct uib_uet_ses_req *request)
{
	if (request->flags & UIB_UET_SES_FLAG_SOM)
		return 0;
	return (uint32_t)be64toh(request->header_data);
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
	struct uib_uet_nscc_config config;
	const char *cc;

	if (!uet)
		return -ENOMEM;
	uet->spdcid = (uint16_t)qp->qp_num;
	uet->next_msg_id = 1;
	uet->session.pid_on_fep = (uint16_t)qp->qp_num;
	cc = getenv("UIB_UET_CC");
	uet->nscc_enabled = !cc || strcmp(cc, "none");
	uib_uet_nscc_config_default(&config, qp->path_mtu + 40);
	cc = getenv("UIB_UET_NSCC_MAX_WND");
	if (cc && strtoull(cc, NULL, 0) >= config.mtu) {
		config.max_wnd = (uint32_t)strtoull(cc, NULL, 0);
		config.bdp = config.max_wnd;
	}
	uib_uet_nscc_init(&uet->nscc, &config, uib_now_ns());
	qp->protocol_private = uet;
	return 0;
}

int uib_uet_session_configure(struct uib_qp *qp,
			      const struct uib_uet_session_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops ||
	    config->job_id > UIB_UET_SES_JOB_ID_MASK ||
	    config->pid_on_fep > UIB_UET_SES_PID_MASK ||
	    config->resource_index > UIB_UET_SES_PID_MASK)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	uet->session = *config;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

int uib_uet_session_query(struct uib_qp *qp,
			  struct uib_uet_session_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	*config = uet->session;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

int uib_uet_nscc_configure_qp(struct uib_qp *qp,
			      const struct uib_uet_nscc_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops ||
	    !config->mtu || config->max_wnd < config->mtu)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (uet->out_count) {
		pthread_mutex_unlock(&qp->lock);
		return -EBUSY;
	}
	uib_uet_nscc_init(&uet->nscc, config, uib_now_ns());
	uet->nscc_enabled = 1;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

int uib_uet_nscc_query_qp(struct uib_qp *qp, struct uib_uet_nscc *state)
{
	struct uib_uet_qp *uet;

	if (!qp || !state || qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	*state = uet->nscc;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static void uet_free_track(struct uib_uet_wr_track *track)
{
	if (!track)
		return;
	free(track->read_sgl);
	free(track->read_fragments);
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
			 track->opcode == UIB_WR_RDMA_READ ||
			 track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
			 track->opcode == UIB_WR_ATOMIC_CMP_SWAP ?
			 track->total_len : 0);
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

static int uet_transmit_locked(struct uib_qp *qp,
			       struct uib_uet_outstanding *out)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	int rc;

	if (uet->nscc_enabled &&
	    !uib_uet_nscc_can_send(&uet->nscc, out->nominal_len))
		return -EAGAIN;
	out->tx_time_ns = uib_now_ns();
	rc = uib_transport_send(qp->ctx, &out->dst, out->packet,
				out->packet_len);
	if (rc < 0)
		return rc;
	out->sent = 1;
	out->deadline_ns = out->tx_time_ns + UIB_UET_RTX_NS;
	if (uet->nscc_enabled)
		uib_uet_nscc_on_send(&uet->nscc, out->nominal_len);
	return 0;
}

static int uet_drain_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	int rc = 0;

	for (;;) {
		struct uib_uet_outstanding *candidate = NULL;
		struct uib_uet_outstanding *out;

		for (out = uet->out_head; out; out = out->next) {
			if (!out->sent)
				candidate = out;
		}
		if (!candidate)
			break;
		rc = uet_transmit_locked(qp, candidate);
		if (rc == -EAGAIN)
			return 0;
		if (rc < 0)
			return rc;
	}
	return rc;
}

static int uet_send_one_response(struct uib_qp *qp,
				 const struct sockaddr_in *destination,
				 const struct uib_uet_pds_req *request,
				 const struct uib_uet_ses_req *ses_request,
				 uint32_t cack_psn, uint8_t response_code,
				 const void *data, uint32_t data_len,
				 uint32_t modified_len, uint32_t message_offset,
				 uint64_t receive_time_ns, int ecn_marked)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct sockaddr_in dst = *destination;
	struct uib_uet_pds_ack_ext *extension;
	struct uib_uet_nscc_wire_state *cc_state;
	struct uib_uet_ses_rsp *response;
	struct uib_uet_ses_rsp_data *response_data;
	uint8_t *packet;
	size_t packet_len;
	size_t response_len;
	uint8_t next;
	int rc;

	next = data_len ? UIB_UET_NEXT_RSP_DATA : UIB_UET_NEXT_RSP;
	response_len = data_len ? sizeof(*response_data) : sizeof(*response);
	packet_len = sizeof(struct uib_uet_pds_ack) + sizeof(*extension) +
		response_len + data_len;
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
	((struct uib_uet_pds_ack *)packet)->prologue.type_next_flags =
		uib_uet_prologue_pack(UIB_UET_PDS_TYPE_ACK_CC, next,
				       ecn_marked ? UIB_UET_ACK_FLAG_M : 0);
	extension = (struct uib_uet_pds_ack_ext *)(packet + rc);
	extension->cc_type_flags_mpr = htons(0);
	cc_state = (struct uib_uet_nscc_wire_state *)&extension->ack_cc_state;
	uib_uet_nscc_wire_pack(cc_state,
			       (uint32_t)(uib_now_ns() - receive_time_ns), 0, 0,
			       uet->receiver_bytes, UINT16_MAX);
	if (data_len) {
		uint32_t identity = ntohl(ses_request->index_gen_job_id);

		response_data = (struct uib_uet_ses_rsp_data *)(extension + 1);
		response_data->list_opcode_version_rc =
			uet_ses_rsp_pack(UIB_UET_SES_RSP_WITH_DATA,
					 response_code);
		pthread_mutex_lock(&qp->lock);
		response_data->response_msg_id = htons(uet->next_msg_id++);
		pthread_mutex_unlock(&qp->lock);
		response_data->index_gen_job_id =
			htonl(identity & UIB_UET_SES_JOB_ID_MASK);
		response_data->request_msg_id = ses_request->msg_id;
		response_data->payload_len = htons(data_len & 0x3fffU);
		response_data->modified_len = htonl(modified_len);
		response_data->message_offset = htonl(message_offset);
		memcpy(response_data + 1, data, data_len);
	} else {
		response = (struct uib_uet_ses_rsp *)(extension + 1);
		response->list_opcode_version_rc =
			uet_ses_rsp_pack(UIB_UET_SES_RSP_RESPONSE,
					 response_code);
		response->msg_id = ses_request->msg_id;
		response->index_gen_job_id = ses_request->index_gen_job_id;
		response->modified_len = htonl(modified_len);
	}
	if (!dst.sin_port)
		dst.sin_port = htons(UIB_UET_DEFAULT_PORT);
	rc = uib_transport_send(qp->ctx, &dst, packet, packet_len);
	free(packet);
	return rc;
}

static int uet_send_response(struct uib_qp *qp,
			     const struct sockaddr_in *destination,
			     const struct uib_uet_pds_req *request,
			     const struct uib_uet_ses_req *ses_request,
			     uint32_t cack_psn, uint8_t response_code,
			     const void *data, size_t data_len,
			     uint32_t modified_len,
			     uint64_t receive_time_ns, int ecn_marked)
{
	uint32_t response_mtu;
	uint32_t offset = 0;
	int rc;

	if (!data_len)
		return uet_send_one_response(qp, destination, request,
					     ses_request, cack_psn,
					     response_code, NULL, 0,
					     modified_len, 0,
					     receive_time_ns, ecn_marked);
	if (data_len > UINT32_MAX)
		return -EMSGSIZE;
	if (qp->path_mtu <= sizeof(struct uib_uet_pds_ack) +
			    sizeof(struct uib_uet_pds_ack_ext) +
			    sizeof(struct uib_uet_ses_rsp_data))
		return -EMSGSIZE;
	response_mtu = qp->path_mtu - sizeof(struct uib_uet_pds_ack) -
		sizeof(struct uib_uet_pds_ack_ext) -
		sizeof(struct uib_uet_ses_rsp_data);
	while (offset < data_len) {
		uint32_t chunk = (uint32_t)data_len - offset;

		if (chunk > response_mtu)
			chunk = response_mtu;
		rc = uet_send_one_response(qp, destination, request,
					   ses_request, cack_psn,
					   response_code,
					   (const uint8_t *)data + offset,
					   chunk, modified_len, offset,
					   receive_time_ns, ecn_marked);
		if (rc < 0)
			return rc;
		offset += chunk;
	}
	return 0;
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
	uint32_t offset = uib_uet_ses_message_offset(ses);
	size_t consumed = 0;
	int rc;

	if (!uet->recv_active) {
		rc = uib_qp_pop_recv(qp, &uet->recv_slot);
		if (rc < 0)
			return rc;
		uet->recv_active = 1;
		uet->recv_msg_id = ntohs(ses->msg_id);
		uet->recv_total = ntohl(ses->request_len);
		uet->recv_has_imm = !!(flags & UIB_UET_SES_FLAG_HD);
		if (uet->recv_has_imm)
			uet->recv_imm = (uint32_t)be64toh(ses->header_data);
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
				 UIB_WC_RECV, uet->recv_total, source_pdc,
				 uet->recv_imm);
		free(uet->recv_slot.sg_list);
		memset(&uet->recv_slot, 0, sizeof(uet->recv_slot));
		uet->recv_active = 0;
		uet->recv_eom = 0;
		uet->recv_has_imm = 0;
	}
	return 0;
}

static int uet_handle_write(struct uib_qp *qp,
			    const struct uib_uet_ses_req *ses,
			    const uint8_t *data, size_t data_len)
{
	uint32_t offset = uib_uet_ses_message_offset(ses);
	uint64_t address = be64toh(ses->buffer_offset) + offset;
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	struct uib_mr *mr;

	if (uib_uet_ses_payload_len(ses, data_len) != data_len ||
	    offset > ntohl(ses->request_len) ||
	    data_len > ntohl(ses->request_len) - offset)
		return -EMSGSIZE;
	mr = uib_mr_check_remote(qp->pd, rkey, address, data_len,
				 UIB_ACCESS_REMOTE_WRITE);
	if (!mr)
		return -EACCES;
	return uib_mr_place(mr, address, data, data_len);
}

static uint8_t uet_validate_session(struct uib_qp *qp,
				    const struct uib_uet_ses_req *ses)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint32_t identity = ntohl(ses->index_gen_job_id);
	uint32_t job_id = identity & UIB_UET_SES_JOB_ID_MASK;
	uint16_t pid = ntohs(ses->pid_on_fep) & UIB_UET_SES_PID_MASK;
	uint16_t index = ntohs(ses->index) & UIB_UET_SES_PID_MASK;
	uint8_t generation = identity >> 24;

	if (ses->flags >> UIB_UET_SES_VERSION_SHIFT)
		return UIB_UET_SES_RC_UNSUPP_OP;
	if (job_id != uet->session.job_id)
		return UIB_UET_SES_RC_BAD_JOB_ID;
	if (pid != uet->session.pid_on_fep)
		return UIB_UET_SES_RC_BAD_PID;
	if (index != uet->session.resource_index)
		return UIB_UET_SES_RC_BAD_INDEX;
	if (generation != uet->session.resource_generation)
		return UIB_UET_SES_RC_BAD_GENERATION;
	if (!!(ses->flags & UIB_UET_SES_FLAG_REL) !=
	    !!uet->session.relative_addressing)
		return UIB_UET_SES_RC_PERM_VIOLATION;
	return UIB_UET_SES_RC_OK;
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

static int uet_handle_atomic(struct uib_qp *qp,
			     const struct uib_uet_ses_req *ses,
			     const uint8_t *data, size_t data_len,
			     const void **response, size_t *response_len,
			     uint64_t *result)
{
	const struct uib_uet_ses_atomic *atomic;
	uint64_t address = be64toh(ses->buffer_offset);
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	uint64_t operand, compare, swap, old;
	struct uib_mr *mr;
	uint64_t *target;

	if (data_len < sizeof(*atomic))
		return -EMSGSIZE;
	atomic = (const struct uib_uet_ses_atomic *)data;
	data += sizeof(*atomic);
	data_len -= sizeof(*atomic);
	if (atomic->datatype != UIB_UET_AMO_UINT64 || (address & 7))
		return -EINVAL;
	mr = uib_mr_check_remote(qp->pd, rkey, address, sizeof(*target),
				 UIB_ACCESS_REMOTE_ATOMIC |
				 UIB_ACCESS_REMOTE_READ |
				 UIB_ACCESS_REMOTE_WRITE);
	if (!mr)
		return -EACCES;
	target = (uint64_t *)(uintptr_t)address;
	if (atomic->opcode == UIB_UET_AMO_SUM && data_len == sizeof(operand)) {
		memcpy(&operand, data, sizeof(operand));
		operand = be64toh(operand);
		old = __atomic_fetch_add(target, operand, __ATOMIC_SEQ_CST);
	} else if (atomic->opcode == UIB_UET_AMO_CSWAP && data_len == 32) {
		memcpy(&compare, data + 8, sizeof(compare));
		memcpy(&swap, data + 24, sizeof(swap));
		compare = be64toh(compare);
		swap = be64toh(swap);
		old = compare;
		__atomic_compare_exchange_n(target, &old, swap, 0,
					    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	} else {
		return -EOPNOTSUPP;
	}
	*result = htobe64(old);
	*response = result;
	*response_len = sizeof(*result);
	return 0;
}

static int uet_response_cache_lookup(struct uib_uet_qp *uet, uint32_t psn,
				     uint16_t msg_id, uint8_t *response_code,
				     uint32_t *modified_len, uint8_t *data,
				     size_t *data_len)
{
	for (uint32_t i = 0; i < UIB_UET_PDC_MPR; i++) {
		if (!uet->response_cache[i].valid ||
		    uet->response_cache[i].psn != psn ||
		    uet->response_cache[i].msg_id != msg_id)
			continue;
		*response_code = uet->response_cache[i].response_code;
		*modified_len = uet->response_cache[i].modified_len;
		*data_len = uet->response_cache[i].data_len;
		memcpy(data, uet->response_cache[i].data, *data_len);
		return 0;
	}
	return -ENOENT;
}

static void uet_response_cache_store(struct uib_uet_qp *uet, uint32_t psn,
				     uint16_t msg_id, uint8_t response_code,
				     uint32_t modified_len, const void *data,
				     size_t data_len)
{
	uint32_t slot;

	if (data_len > sizeof(uet->response_cache[0].data))
		return;
	slot = uet->response_cache_next++ % UIB_UET_PDC_MPR;
	uet->response_cache[slot].psn = psn;
	uet->response_cache[slot].msg_id = msg_id;
	uet->response_cache[slot].response_code = response_code;
	uet->response_cache[slot].modified_len = modified_len;
	uet->response_cache[slot].data_len = (uint8_t)data_len;
	if (data_len)
		memcpy(uet->response_cache[slot].data, data, data_len);
	uet->response_cache[slot].valid = 1;
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
	uint64_t atomic_result;
	uint32_t modified_len = 0;
	uint8_t cached_data[16];
	uint16_t msg_id;
	int duplicate;
	int rc;
	uint64_t receive_time_ns = uib_now_ns();

	if (len < sizeof(*req) + sizeof(*ses) ||
	    uib_uet_prologue_next(&req->prologue) != UIB_UET_NEXT_REQ_STD)
		return -EBADMSG;
	ses = (const struct uib_uet_ses_req *)(req + 1);
	data = (const uint8_t *)(ses + 1);
	data_len = len - sizeof(*req) - sizeof(*ses);
	psn = ntohl(req->psn);
	msg_id = ntohs(ses->msg_id);

	pthread_mutex_lock(&qp->lock);
	uet->dpdcid = ntohs(req->spdcid);
	if (uib_uet_prologue_flags(&req->prologue) & UIB_UET_REQ_FLAG_SYN)
		uet->state = UIB_UET_PDC_ESTABLISHED;
	if (!uet->rx_initialized) {
		uet->rx_base_psn = qp->rq_psn;
		uet->rx_initialized = 1;
	}
	rc = uet_rx_mark(uet, psn, &duplicate);
	if (!rc && !duplicate && uet->nscc_enabled)
		uet->receiver_bytes += (uint32_t)len + 40;
	cack = uet->rx_base_psn - 1;
	pthread_mutex_unlock(&qp->lock);
	uet_debug(qp, "rx request psn=%u cack=%u msg=%u duplicate=%d rc=%d",
		  psn, cack, ntohs(ses->msg_id), duplicate, rc);
	if (rc < 0)
		return rc;

	opcode = ses->opcode & 0x3f;
	response_code = uet_validate_session(qp, ses);
	if (duplicate && opcode != UIB_UET_SES_READ) {
		pthread_mutex_lock(&qp->lock);
		rc = uet_response_cache_lookup(uet, psn, msg_id,
					       &response_code, &modified_len,
					       cached_data, &response_len);
		pthread_mutex_unlock(&qp->lock);
		if (!rc && response_len) {
			response_data = cached_data;
		} else if (rc < 0) {
			response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
			modified_len = 0;
			response_len = 0;
		}
	} else if (response_code == UIB_UET_SES_RC_OK) {
		switch (opcode) {
		case UIB_UET_SES_SEND:
			rc = uet_handle_send(qp, ses, data, data_len, psn,
					     ntohs(req->spdcid));
			modified_len = ntohl(ses->request_len);
			break;
		case UIB_UET_SES_WRITE:
			rc = uet_handle_write(qp, ses, data, data_len);
			modified_len = ntohl(ses->request_len);
			break;
		case UIB_UET_SES_READ:
			rc = uet_handle_read(qp, ses, &response_data,
					     &response_len);
			modified_len = response_len;
			break;
		case UIB_UET_SES_FETCHING_ATOMIC:
			rc = uet_handle_atomic(qp, ses, data, data_len,
					       &response_data, &response_len,
					       &atomic_result);
			modified_len = response_len;
			break;
		default:
			rc = -EOPNOTSUPP;
			break;
		}
		if (rc < 0)
			modified_len = 0;
		if (rc == -EACCES)
			response_code = UIB_UET_SES_RC_BAD_MKEY;
		else if (rc < 0)
			response_code = UIB_UET_SES_RC_UNSUPP_OP;
	}
	if (!duplicate && opcode != UIB_UET_SES_READ) {
		pthread_mutex_lock(&qp->lock);
		uet_response_cache_store(uet, psn, msg_id, response_code,
					 modified_len, response_data, response_len);
		pthread_mutex_unlock(&qp->lock);
	}

	return uet_send_response(qp, src, req, ses, cack, response_code,
				 response_data, response_len, modified_len,
				 receive_time_ns, 0);
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

static int uet_track_read_fragment(struct uib_uet_wr_track *track,
				   uint32_t offset, uint32_t length)
{
	struct uib_uet_read_fragment *fragments;
	uint32_t capacity;

	for (uint32_t i = 0; i < track->read_fragment_count; i++) {
		uint32_t old_offset = track->read_fragments[i].offset;
		uint32_t old_length = track->read_fragments[i].length;

		if (old_offset == offset && old_length == length)
			return 1;
		if (offset < old_offset + old_length && old_offset < offset + length)
			return -EBADMSG;
	}
	if (track->read_fragment_count == track->read_fragment_capacity) {
		capacity = track->read_fragment_capacity ?
			track->read_fragment_capacity * 2 : 8;
		fragments = realloc(track->read_fragments,
				    capacity * sizeof(*fragments));
		if (!fragments)
			return -ENOMEM;
		track->read_fragments = fragments;
		track->read_fragment_capacity = capacity;
	}
	track->read_fragments[track->read_fragment_count].offset = offset;
	track->read_fragments[track->read_fragment_count].length = length;
	track->read_fragment_count++;
	track->read_received += length;
	return 0;
}

static int uet_handle_ack(struct uib_qp *qp, const void *packet, size_t len)
{
	const struct uib_uet_pds_ack *ack = packet;
	const struct uib_uet_pds_ack_ext *extension = NULL;
	const struct uib_uet_ses_rsp *response = NULL;
	const struct uib_uet_ses_rsp_data *response_data = NULL;
	const uint8_t *data;
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding **exact_link = NULL;
	struct uib_uet_outstanding *exact;
	uint32_t cack, ack_psn;
	uint16_t msg_id, packed;
	size_t header_len = sizeof(*ack);
	size_t data_len;
	uint32_t message_offset = 0;
	uint32_t modified_len;
	uint8_t response_code;
	uint8_t next;
	int32_t offset;
	uint32_t received_units = 0;
	uint32_t service_time_ns = 0;
	uint8_t restore = 0, penalty = 0;

	if (uib_uet_prologue_type(&ack->prologue) == UIB_UET_PDS_TYPE_ACK_CC) {
		extension = (const struct uib_uet_pds_ack_ext *)(ack + 1);
		header_len += sizeof(struct uib_uet_pds_ack_ext);
	}
	next = uib_uet_prologue_next(&ack->prologue);
	if (next == UIB_UET_NEXT_RSP_DATA) {
		if (len < header_len + sizeof(*response_data))
			return -EBADMSG;
		response_data = (const struct uib_uet_ses_rsp_data *)
			((const uint8_t *)packet + header_len);
		data = (const uint8_t *)(response_data + 1);
		data_len = len - header_len - sizeof(*response_data);
		if ((ntohs(response_data->payload_len) & 0x3fffU) != data_len)
			return -EBADMSG;
		msg_id = ntohs(response_data->request_msg_id);
		packed = ntohs(response_data->list_opcode_version_rc);
		modified_len = ntohl(response_data->modified_len);
		message_offset = ntohl(response_data->message_offset);
	} else if (next == UIB_UET_NEXT_RSP) {
		if (len < header_len + sizeof(*response))
			return -EBADMSG;
		response = (const struct uib_uet_ses_rsp *)
			((const uint8_t *)packet + header_len);
		data = (const uint8_t *)(response + 1);
		data_len = len - header_len - sizeof(*response);
		if (data_len)
			return -EBADMSG;
		msg_id = ntohs(response->msg_id);
		packed = ntohs(response->list_opcode_version_rc);
		modified_len = ntohl(response->modified_len);
	} else {
		return -EBADMSG;
	}
	cack = ntohl(ack->cack_psn);
	offset = (int16_t)ntohs(ack->ack_psn_offset);
	ack_psn = cack + offset;
	response_code = packed & UIB_UET_SES_RSP_RC_MASK;
	uet_debug(qp, "rx ack psn=%u cack=%u msg=%u rc=%u", ack_psn, cack,
		  msg_id, response_code);

	pthread_mutex_lock(&qp->lock);
	uet->dpdcid = ntohs(ack->spdcid);
	uet->state = UIB_UET_PDC_ESTABLISHED;
	exact = uet_find_outstanding(uet, ack_psn, msg_id, &exact_link);
	if (uet->nscc_enabled && extension) {
		const struct uib_uet_nscc_wire_state *cc_state =
			(const struct uib_uet_nscc_wire_state *)
			&extension->ack_cc_state;
		struct uib_uet_nscc_ack nscc_ack = {0};
		uint32_t delta;

		uib_uet_nscc_wire_unpack(cc_state, &service_time_ns, &restore,
					 &penalty, &received_units, NULL);
		delta = (received_units - uet->previous_received_units) &
			UIB_UET_NSCC_RCVD_MASK;
		if (delta < 0x00800000U) {
			nscc_ack.newly_received = delta << 8;
			uet->previous_received_units = received_units;
		}
		nscc_ack.ack_time_ns = uib_now_ns();
		nscc_ack.receiver_cwnd_penalty = penalty;
		nscc_ack.restore_cwnd = restore;
		nscc_ack.ecn_marked = !!(uib_uet_prologue_flags(&ack->prologue) &
					 UIB_UET_ACK_FLAG_M);
		nscc_ack.retransmit = !!(uib_uet_prologue_flags(&ack->prologue) &
					 UIB_UET_ACK_FLAG_RETX);
		if (exact) {
			nscc_ack.tx_time_ns = exact->tx_time_ns;
			nscc_ack.tx_retries = exact->retries;
		} else {
			nscc_ack.tx_retries = 2;
		}
		uib_uet_nscc_on_ack(&uet->nscc, &nscc_ack);
	}
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
		    out->track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		    out->track->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
		    uet_psn_before(cack, out->psn)) {
			link = &out->next;
			continue;
		}
		uet_complete_out_locked(qp, link, UIB_WC_SUCCESS);
	}
	exact = uet_find_outstanding(uet, ack_psn, msg_id, &exact_link);
	if (!exact) {
		uet_drain_locked(qp);
		pthread_mutex_unlock(&qp->lock);
		return 0;
	}

	if (exact->track->opcode == UIB_WR_RDMA_READ ||
	    exact->track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    exact->track->opcode == UIB_WR_ATOMIC_CMP_SWAP) {
		size_t consumed = 0;
		uint64_t atomic_value;
		const uint8_t *result_data = data;
		int rc;

		if (!exact->track->total_len && !response_data && !modified_len) {
			uet_complete_out_locked(qp, exact_link, UIB_WC_SUCCESS);
			uet_drain_locked(qp);
			pthread_mutex_unlock(&qp->lock);
			return 0;
		}
		if (!response_data || modified_len != exact->track->total_len ||
		    message_offset > modified_len ||
		    data_len > modified_len - message_offset) {
			struct uib_uet_wr_track *track = exact->track;

			uet_remove_track_locked(qp, track, UIB_WC_REM_OP_ERR);
			pthread_mutex_unlock(&qp->lock);
			return -EBADMSG;
		}
		rc = uet_track_read_fragment(exact->track, message_offset,
					     (uint32_t)data_len);
		if (rc == 1) {
			uet_drain_locked(qp);
			pthread_mutex_unlock(&qp->lock);
			return 0;
		}
		if (rc < 0) {
			struct uib_uet_wr_track *track = exact->track;

			uet_remove_track_locked(qp, track, UIB_WC_REM_OP_ERR);
			pthread_mutex_unlock(&qp->lock);
			return rc;
		}
		if (exact->track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		    exact->track->opcode == UIB_WR_ATOMIC_CMP_SWAP) {
			if (data_len != sizeof(atomic_value)) {
				struct uib_uet_wr_track *track = exact->track;

				uet_remove_track_locked(qp, track,
							UIB_WC_REM_OP_ERR);
				pthread_mutex_unlock(&qp->lock);
				return -EBADMSG;
			}
			memcpy(&atomic_value, data, sizeof(atomic_value));
			atomic_value = be64toh(atomic_value);
			result_data = (const uint8_t *)&atomic_value;
		}
		rc = uib_qp_scatter(qp->pd, exact->track->read_sgl,
				    exact->track->read_num_sge,
				    message_offset, result_data, data_len,
				    &consumed);
		if (rc < 0 || consumed != data_len) {
			struct uib_uet_wr_track *track = exact->track;

			uet_remove_track_locked(qp, track, UIB_WC_LOC_PROT_ERR);
			pthread_mutex_unlock(&qp->lock);
			return rc < 0 ? rc : -EMSGSIZE;
		}
		if (exact->track->read_received == exact->track->total_len)
			uet_complete_out_locked(qp, exact_link, UIB_WC_SUCCESS);
	} else {
		uet_complete_out_locked(qp, exact_link, UIB_WC_SUCCESS);
	}
	uet_drain_locked(qp);
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
	if (out) {
		if (uet->nscc_enabled)
			uib_uet_nscc_on_nack(&uet->nscc, out->nominal_len,
				nack->nack_code == UIB_UET_NACK_TRIMMED ||
				nack->nack_code == UIB_UET_NACK_TRIMMED_LASTHOP,
				nack->nack_code == UIB_UET_NACK_TRIMMED_LASTHOP,
				0, uib_now_ns());
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
	}
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
	if (wr->opcode == UIB_WR_RDMA_READ ||
	    wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    wr->opcode == UIB_WR_ATOMIC_CMP_SWAP) {
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
	int fetching_atomic;
	int rc = 0;

	fetching_atomic = wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		wr->opcode == UIB_WR_ATOMIC_CMP_SWAP;

	for (int i = 0; i < wr->num_sge; i++) {
		if (UINT32_MAX - total_len < wr->sg_list[i].length)
			return -EMSGSIZE;
		total_len += wr->sg_list[i].length;
	}
	if (wr->opcode != UIB_WR_RDMA_READ && !fetching_atomic) {
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
	packets = wr->opcode == UIB_WR_RDMA_READ || fetching_atomic ? 1 :
		(total_len ? (total_len + chunk_max - 1) / chunk_max : 1);
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
	case UIB_WR_ATOMIC_FETCH_ADD:
	case UIB_WR_ATOMIC_CMP_SWAP:
		ses_opcode = UIB_UET_SES_FETCHING_ATOMIC;
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
		uint32_t chunk = wr->opcode == UIB_WR_RDMA_READ || fetching_atomic ?
			total_len :
			total_len - offset;
		struct uib_uet_outstanding *out;
		struct uib_uet_pds_req *req;
		struct uib_uet_ses_req *ses;
		size_t data_len, packet_len;
		uint8_t flags = UIB_UET_REQ_FLAG_AR;

		if (wr->opcode != UIB_WR_RDMA_READ && !fetching_atomic &&
		    chunk > chunk_max)
			chunk = chunk_max;
		if (wr->opcode == UIB_WR_RDMA_READ)
			data_len = 0;
		else if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD)
			data_len = sizeof(struct uib_uet_ses_atomic) + 8;
		else if (wr->opcode == UIB_WR_ATOMIC_CMP_SWAP)
			data_len = sizeof(struct uib_uet_ses_atomic) + 32;
		else
			data_len = chunk;
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
		if (uet->session.relative_addressing)
			ses->flags |= UIB_UET_SES_FLAG_REL;
		if (i == 0)
			ses->flags |= UIB_UET_SES_FLAG_SOM;
		if (i == packets - 1)
			ses->flags |= UIB_UET_SES_FLAG_EOM;
		ses->msg_id = htons(out->msg_id);
		ses->index_gen_job_id = htonl(
			((uint32_t)uet->session.resource_generation << 24) |
			uet->session.job_id);
		ses->pid_on_fep = htons((uint16_t)qp->dest_qp_num &
					 UIB_UET_SES_PID_MASK);
		ses->index = htons(uet->session.resource_index);
		ses->initiator = htonl(uet->session.initiator_id);
		ses->request_len = htonl(total_len);
		if (wr->opcode == UIB_WR_SEND ||
		    wr->opcode == UIB_WR_SEND_WITH_IMM) {
			ses->buffer_offset = 0;
			if (i == 0 && wr->opcode == UIB_WR_SEND_WITH_IMM) {
				ses->flags |= UIB_UET_SES_FLAG_HD;
				ses->header_data = htobe64(wr->imm_data);
			}
		} else if (!fetching_atomic) {
			ses->buffer_offset = htobe64(wr->rdma.remote_addr);
			ses->match_bits = htobe64(wr->rdma.rkey);
		} else {
			struct uib_uet_ses_atomic *atomic =
				(struct uib_uet_ses_atomic *)(ses + 1);
			uint8_t *operands = (uint8_t *)(atomic + 1);
			uint64_t value;

			ses->buffer_offset = htobe64(wr->atomic.remote_addr);
			ses->match_bits = htobe64(wr->atomic.rkey);
			ses->request_len = htonl(wr->opcode ==
				UIB_WR_ATOMIC_CMP_SWAP ? 32 : 8);
			atomic->datatype = UIB_UET_AMO_UINT64;
			if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD) {
				atomic->opcode = UIB_UET_AMO_SUM;
				value = htobe64(wr->atomic.compare_add);
				memcpy(operands, &value, sizeof(value));
			} else {
				atomic->opcode = UIB_UET_AMO_CSWAP;
				value = htobe64(wr->atomic.compare_add);
				memcpy(operands + 8, &value, sizeof(value));
				value = htobe64(wr->atomic.swap);
				memcpy(operands + 24, &value, sizeof(value));
			}
		}
		if (i)
			ses->header_data = uib_uet_ses_fragment_info(chunk, offset);
		if (data_len && !fetching_atomic)
			memcpy(ses + 1, message + offset, data_len);

		out->track = track;
		out->dst = dst;
		out->packet_len = packet_len;
		out->read_offset = offset;
		out->read_len = chunk;
		out->deadline_ns = uib_now_ns() + UIB_UET_RTX_NS;
		out->nominal_len = (uint32_t)packet_len + 40;
		pthread_mutex_lock(&qp->lock);
		out->next = uet->out_head;
		uet->out_head = out;
		uet->out_count++;
		uet_debug(qp, "tx request psn=%u msg=%u opcode=%u len=%zu",
			  out->psn, out->msg_id, ses_opcode, data_len);
		rc = uet_drain_locked(qp);
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
		if (!out->sent) {
			out = out->next;
			continue;
		}
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
		if (uet->nscc_enabled)
			uib_uet_nscc_on_inferred_loss(&uet->nscc,
						      out->nominal_len);
		out->sent = 0;
		out->retries++;
		out = out->next;
	}
	uet_drain_locked(qp);
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
