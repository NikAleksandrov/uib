/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol_uet_internal.h"

static uint8_t uet_response_next_header(uint32_t data_len,
					uint8_t request_header)
{
	if (!data_len)
		return UIB_UET_NEXT_RSP;
	return request_header == UIB_UET_NEXT_REQ_STD ?
		UIB_UET_NEXT_RSP_DATA : UIB_UET_NEXT_RSP_DATA_SMALL;
}

static size_t uet_response_header_len(uint8_t next)
{
	if (next == UIB_UET_NEXT_RSP_DATA_SMALL)
		return sizeof(struct uib_uet_ses_rsp_data_small);
	if (next == UIB_UET_NEXT_RSP_DATA)
		return sizeof(struct uib_uet_ses_rsp_data);
	return sizeof(struct uib_uet_ses_rsp);
}

struct uet_response_encoding {
	uint8_t opcode;
	uint8_t response_code;
	uint8_t guaranteed_delivery;
};

static struct uet_response_encoding
uet_response_construct(uint8_t response_code, uint32_t data_len,
			uint32_t modified_len, uint32_t requested_len,
			int guarantee_error)
{
	struct uet_response_encoding response;

	response.response_code = response_code;
	if (response_code != UIB_UET_SES_RC_OK)
		response.guaranteed_delivery = guarantee_error;
	else
		response.guaranteed_delivery = data_len ||
			modified_len != requested_len;
	if (data_len)
		response.opcode = UIB_UET_SES_RSP_WITH_DATA;
	else if (response.guaranteed_delivery)
		response.opcode = UIB_UET_SES_RSP_RESPONSE;
	else
		response.opcode = UIB_UET_SES_RSP_DEFAULT;
	return response;
}

static void uet_fill_response_cc(struct uib_qp *qp,
				 struct uib_uet_pds_ack_ext *extension,
				 uint64_t receive_time_ns)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_rccc_wire_state *credit_state;
	struct uib_uet_nscc_wire_state *cc_state;

	if (!uet->rccc_enabled) {
		extension->cc_type_flags_mpr = htons(
			(UIB_UET_CC_TYPE_NSCC << UIB_UET_CC_TYPE_SHIFT) |
			UIB_UET_MPR_ENCODING);
		cc_state = (struct uib_uet_nscc_wire_state *)
			&extension->ack_cc_state;
		uib_uet_nscc_wire_pack(
			cc_state, (uint32_t)(uib_now_ns() - receive_time_ns),
			0, 0, uet->receiver_bytes, UINT16_MAX);
		return;
	}
	extension->cc_type_flags_mpr = htons(
		(UIB_UET_CC_TYPE_CREDIT << UIB_UET_CC_TYPE_SHIFT) |
		UIB_UET_MPR_ENCODING);
	credit_state = (struct uib_uet_rccc_wire_state *)
		&extension->ack_cc_state;
	pthread_mutex_lock(&qp->lock);
	uib_uet_rccc_wire_pack(credit_state,
			       uet->rccc_destination.cumulative_credit,
			       UINT16_MAX);
	pthread_mutex_unlock(&qp->lock);
}

static int uet_rx_psn_received(const struct uib_uet_qp *uet, uint32_t psn)
{
	uint32_t offset;
	uint64_t mask;
	unsigned int word;

	if (uet_psn_before(psn, uet->rx_base_psn))
		return 1;
	offset = psn - uet->rx_base_psn;
	if (offset >= UIB_UET_PDC_MPR)
		return 0;
	word = offset / 64;
	mask = 1ULL << (offset % 64);
	return !!(uet->rx_bitmap[word] & mask);
}

static void uet_fill_response_sack(struct uib_qp *qp,
				   struct uib_uet_pds_ack_ext *extension,
				   uint32_t cack_psn)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint64_t bitmap;
	uint32_t sack_psn;
	uint32_t psn;
	uint32_t i;

	sack_psn = (cack_psn + 1) & ~7U;
	bitmap = 0;
	pthread_mutex_lock(&qp->lock);
	for (i = 0; i < 64; i++) {
		psn = sack_psn + i;
		if (!uet_psn_before(cack_psn, psn) ||
		    uet_rx_psn_received(uet, psn))
			bitmap |= 1ULL << i;
	}
	pthread_mutex_unlock(&qp->lock);
	extension->sack_psn_offset = htons((uint16_t)(int16_t)
					      (sack_psn - cack_psn));
	extension->sack_bitmap = htobe64(bitmap);
}

static void uet_fill_small_response(struct uib_qp *qp, void *wire,
				    const struct uib_uet_pds_req *request,
				    const struct uib_uet_ses_req *ses_request,
				    uint8_t response_code, const void *data,
				    uint32_t data_len)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_ses_rsp_data_small *response = wire;
	uint32_t identity;

	identity = ntohl(ses_request->index_gen_job_id);
	response->list_opcode_version_rc = uet_ses_rsp_pack(
		UIB_UET_SES_RSP_WITH_DATA, response_code);
	response->payload_len = htons(data_len & UIB_UET_SES_LENGTH_MASK);
	response->index_gen_job_id = htonl(identity);
	response->request_psn = request->psn;
	memcpy(response + 1, data, data_len);
	pthread_mutex_lock(&qp->lock);
	uet->optimized_stats.small_responses++;
	pthread_mutex_unlock(&qp->lock);
}

static void uet_fill_data_response(struct uib_qp *qp, void *wire,
				   const struct uib_uet_ses_req *request,
				   uint8_t response_code, const void *data,
				   uint32_t data_len, uint32_t modified_len,
				   uint32_t message_offset)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_ses_rsp_data *response = wire;
	uint32_t identity;

	identity = ntohl(request->index_gen_job_id);
	response->list_opcode_version_rc = uet_ses_rsp_pack(
		UIB_UET_SES_RSP_WITH_DATA, response_code);
	pthread_mutex_lock(&qp->lock);
	response->response_msg_id = htons(uet->next_msg_id++);
	pthread_mutex_unlock(&qp->lock);
	response->index_gen_job_id = htonl(identity);
	response->request_msg_id = request->msg_id;
	response->payload_len = htons(data_len & UIB_UET_SES_LENGTH_MASK);
	response->modified_len = htonl(modified_len);
	response->message_offset = htonl(message_offset);
	memcpy(response + 1, data, data_len);
}

static void uet_fill_plain_response(struct uib_qp *qp, void *wire,
				    const struct uib_uet_ses_req *request,
				    const struct uet_response_encoding *encoding,
				    uint32_t modified_len)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_ses_rsp *response = wire;
	uint32_t identity;

	response->list_opcode_version_rc = uet_ses_rsp_pack(
		encoding->opcode, encoding->response_code);
	response->msg_id = request->msg_id;
	if (encoding->response_code == UIB_UET_SES_RC_BAD_GENERATION) {
		identity = ntohl(request->index_gen_job_id);
		identity &= UIB_UET_SES_JOB_ID_MASK;
		identity |= (uint32_t)uet->session.resource_generation << 24;
		response->index_gen_job_id = htonl(identity);
	} else {
		response->index_gen_job_id = request->index_gen_job_id;
	}
	response->modified_len = htonl(modified_len);
}

static int uet_send_one_response(struct uib_qp *qp,
				 const struct sockaddr_in *destination,
				 const struct uib_uet_pds_req *request,
				 const struct uib_uet_ses_req *ses_request,
				 uint32_t cack_psn,
				 const struct uet_response_encoding *encoding,
				 const void *data, uint32_t data_len,
				 uint32_t modified_len, uint32_t message_offset,
				 uint64_t receive_time_ns, int ecn_marked,
				 uint8_t request_header)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_pds_ack_ext *extension;
	struct uib_uet_pds_ack *ack;
	struct sockaddr_in dst = *destination;
	uint8_t packet[UIB_RECV_BUF_LEN];
	size_t response_len;
	size_t packet_len;
	uint8_t next;
	void *wire;
	int rc;

	next = uet_response_next_header(data_len, request_header);
	response_len = uet_response_header_len(next);
	packet_len = sizeof(*ack) + sizeof(*extension) + response_len + data_len;
	if (packet_len > sizeof(packet))
		return -EMSGSIZE;
	memset(packet, 0, sizeof(*ack) + sizeof(*extension) + response_len);
	rc = uib_uet_build_ack(packet, packet_len, cack_psn,
			       ntohl(request->psn), uet->spdcid,
			       ntohs(request->spdcid), next);
	if (rc < 0)
		return rc;
	ack = (struct uib_uet_pds_ack *)packet;
	ack->prologue.type_next_flags = uib_uet_prologue_pack(
		UIB_UET_PDS_TYPE_ACK_CC, next,
		(ecn_marked ? UIB_UET_ACK_FLAG_M : 0) |
		(encoding->guaranteed_delivery ?
		 UIB_UET_ACK_FLAG_REQ_CLEAR : 0));
	extension = (struct uib_uet_pds_ack_ext *)(packet + rc);
	uet_fill_response_cc(qp, extension, receive_time_ns);
	uet_fill_response_sack(qp, extension, cack_psn);
	wire = extension + 1;
	if (next == UIB_UET_NEXT_RSP_DATA_SMALL)
		uet_fill_small_response(qp, wire, request, ses_request,
					encoding->response_code, data, data_len);
	else if (next == UIB_UET_NEXT_RSP_DATA)
		uet_fill_data_response(qp, wire, ses_request,
				       encoding->response_code,
				       data, data_len, modified_len,
				       message_offset);
	else
		uet_fill_plain_response(qp, wire, ses_request, encoding,
					modified_len);
	dst.sin_port = qp->remote_addr.sin_port;
	if (!dst.sin_port)
		dst.sin_port = htons(UIB_UET_DEFAULT_PORT);
	return uib_transport_send_entropy(qp->ctx, &dst, packet, packet_len,
					  ntohs(destination->sin_port));
}

static int uet_send_response(struct uib_qp *qp,
			     const struct sockaddr_in *destination,
			     const struct uib_uet_pds_req *request,
			     const struct uib_uet_ses_req *ses_request,
			     uint32_t cack_psn,
			     const struct uet_response_encoding *encoding,
			     const void *data, size_t data_len,
				     uint32_t modified_len,
				     uint64_t receive_time_ns, int ecn_marked,
				     uint8_t request_header)
{
	uint32_t response_mtu;
	uint32_t offset = 0;
	int rc;

	if (!data_len)
		return uet_send_one_response(qp, destination, request,
					     ses_request, cack_psn,
					     encoding, NULL, 0,
					     modified_len, 0,
					     receive_time_ns, ecn_marked,
					     request_header);
	if (data_len > UINT32_MAX)
		return -EMSGSIZE;
	if (request_header != UIB_UET_NEXT_REQ_STD) {
		if (data_len > UIB_UET_SES_LENGTH_MASK ||
		    data_len + sizeof(struct uib_uet_pds_ack) +
		    sizeof(struct uib_uet_pds_ack_ext) +
		    sizeof(struct uib_uet_ses_rsp_data_small) > qp->path_mtu)
			return -EMSGSIZE;
		return uet_send_one_response(qp, destination, request,
					     ses_request, cack_psn,
					     encoding, data,
					     (uint32_t)data_len, modified_len,
					     0, receive_time_ns, ecn_marked,
					     request_header);
	}
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
					   encoding,
					   (const uint8_t *)data + offset,
					   chunk, modified_len, offset,
					   receive_time_ns, ecn_marked,
					   request_header);
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

static int uet_mr_contains(const struct uib_mr *mr, uint64_t address,
			   size_t length)
{
	uint64_t base;

	base = mr->kind == UIB_MR_HOST ?
		(uint64_t)(uintptr_t)mr->addr : mr->iova;
	if (address < base || length > mr->length)
		return 0;
	return address - base <= mr->length - length;
}

static struct uib_mr *uet_lookup_remote_mr(struct uib_qp *qp,
					   uint32_t rkey,
					   uint64_t address,
					   size_t length, int access,
					   int address_only,
					   uint8_t *response_code)
{
	struct uib_mr *mr;

	if (address_only) {
		mr = uib_pd_lookup_address(qp->pd, address, length, 0);
		if (!mr) {
			*response_code = UIB_UET_SES_RC_BAD_ADDR;
			return NULL;
		}
	} else {
		mr = uib_pd_lookup_rkey(qp->pd, rkey);
		if (!mr || mr->rkey != rkey) {
			if (mr)
				uib_mr_put(mr);
			*response_code = UIB_UET_SES_RC_BAD_MKEY;
			return NULL;
		}
		if (!uet_mr_contains(mr, address, length)) {
			uib_mr_put(mr);
			*response_code = UIB_UET_SES_RC_BAD_ADDR;
			return NULL;
		}
	}
	if ((mr->access & access) != access) {
		uib_mr_put(mr);
		*response_code = UIB_UET_SES_RC_OP_VIOLATION;
		return NULL;
	}
	return mr;
}

static void uet_reset_receive(struct uib_uet_qp *uet)
{
	memset(&uet->recv_slot, 0, sizeof(uet->recv_slot));
	memset(&uet->recv_src, 0, sizeof(uet->recv_src));
	uet->recv_active = 0;
	uet->recv_eom = 0;
	uet->recv_has_imm = 0;
}

static void uet_abort_receive(struct uib_qp *qp,
			      enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if (!uet->recv_active)
		return;
	uib_qp_emit_recv_slot_src(qp, &uet->recv_slot, status, UIB_WC_RECV,
				  0, 0, 0, &uet->recv_src);
	uib_qp_release_recv(&uet->recv_slot);
	uet_reset_receive(uet);
}

static void uet_reset_write(struct uib_uet_qp *uet)
{
	uet->write_active = 0;
	uet->write_eom = 0;
	uet->write_with_imm = 0;
}

static void uet_abort_write(struct uib_qp *qp, enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if (uet->write_with_imm)
		uet_abort_receive(qp, status);
	uet_reset_write(uet);
}

static int uet_handle_send(struct uib_qp *qp,
			   const struct sockaddr_in *src,
			   const struct uib_uet_ses_req *ses,
			   const uint8_t *data, size_t data_len,
			   uint32_t psn, uint32_t source_pdc,
			   uint8_t *response_code)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint8_t flags = ses->flags & UIB_UET_SES_FLAGS_MASK;
	uint32_t offset = uib_uet_ses_message_offset(ses);
	size_t consumed = 0;
	int rc;

	if (!uet->recv_active) {
		rc = uib_qp_pop_recv(qp, &uet->recv_slot);
		if (rc < 0) {
			*response_code = rc == -ENOENT ? UIB_UET_SES_RC_NO_MATCH :
				UIB_UET_SES_RC_DROPPED;
			return rc;
		}
		uet->recv_active = 1;
		uet->recv_src = *src;
		uet->recv_msg_id = ntohs(ses->msg_id);
		uet->recv_total = ntohl(ses->request_len);
		uet->recv_has_imm = !!(flags & UIB_UET_SES_FLAG_HD);
		if (uet->recv_has_imm)
			uet->recv_imm = (uint32_t)be64toh(ses->header_data);
	}
	if (!uet->recv_active || uet->recv_msg_id != ntohs(ses->msg_id)) {
		*response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
		uet_abort_receive(qp, UIB_WC_GENERAL_ERR);
		return -EBADMSG;
	}

	rc = uib_qp_scatter_recv(qp, &uet->recv_slot, offset, data, data_len,
				 &consumed);
	if (rc < 0) {
		*response_code = rc == -EMSGSIZE ? UIB_UET_SES_RC_TOO_LONG :
			UIB_UET_SES_RC_PERM_VIOLATION;
		uet_abort_receive(qp, rc == -EMSGSIZE ? UIB_WC_LOC_LEN_ERR :
				  UIB_WC_LOC_PROT_ERR);
		return rc;
	}
	if (consumed != data_len) {
		*response_code = UIB_UET_SES_RC_TOO_LONG;
		uet_abort_receive(qp, UIB_WC_LOC_LEN_ERR);
		return -EMSGSIZE;
	}
	if (flags & UIB_UET_SES_FLAG_EOM) {
		uet->recv_last_psn = psn;
		uet->recv_eom = 1;
	}
	if (uet->recv_eom && uet_psn_before(uet->recv_last_psn,
					     uet->rx_base_psn)) {
		uib_qp_emit_recv_slot_src(qp, &uet->recv_slot, UIB_WC_SUCCESS,
					  UIB_WC_RECV, uet->recv_total,
					  source_pdc, uet->recv_imm,
					  &uet->recv_src);
		uib_qp_release_recv(&uet->recv_slot);
		uet_reset_receive(uet);
	}
	return 0;
}

static int uet_handle_write(struct uib_qp *qp,
			    const struct sockaddr_in *src,
			    const struct uib_uet_ses_req *ses,
			    const uint8_t *data, size_t data_len,
			    int address_only, uint32_t psn,
			    uint32_t source_pdc, uint8_t *response_code)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint8_t flags = ses->flags & UIB_UET_SES_FLAGS_MASK;
	uint32_t offset = uib_uet_ses_message_offset(ses);
	uint64_t address = be64toh(ses->buffer_offset) + offset;
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	uint16_t msg_id = ntohs(ses->msg_id);
	struct uib_mr *mr;
	int with_imm = flags & UIB_UET_SES_FLAG_HD;
	int rc;

	if (flags & UIB_UET_SES_FLAG_SOM) {
		if (uet->write_active && uet->write_msg_id != msg_id) {
			*response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
			return -EBADMSG;
		}
		if (!uet->write_active) {
			if (with_imm) {
				if (uet->recv_active) {
					*response_code =
						UIB_UET_SES_RC_INITIATOR_ERROR;
					return -EBADMSG;
				}
				rc = uib_qp_pop_recv(qp, &uet->recv_slot);
				if (rc < 0) {
					*response_code = rc == -ENOENT ?
						UIB_UET_SES_RC_NO_MATCH :
						UIB_UET_SES_RC_DROPPED;
					return rc;
				}
				uet->recv_active = 1;
				uet->recv_src = *src;
				uet->recv_msg_id = msg_id;
				uet->recv_total = ntohl(ses->request_len);
				uet->recv_imm =
					(uint32_t)be64toh(ses->header_data);
				uet->recv_source_pdc = source_pdc;
			}
			uet->write_active = 1;
			uet->write_msg_id = msg_id;
			uet->write_eom = 0;
			uet->write_with_imm = with_imm;
		}
	}
	if (!uet->write_active || uet->write_msg_id != msg_id) {
		*response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
		return -EBADMSG;
	}
	if (uib_uet_ses_payload_len(ses, data_len) != data_len ||
	    offset > ntohl(ses->request_len) ||
	    data_len > ntohl(ses->request_len) - offset) {
		*response_code = UIB_UET_SES_RC_UNSUPP_SIZE;
		uet_abort_write(qp, UIB_WC_LOC_LEN_ERR);
		return -EMSGSIZE;
	}
	mr = uet_lookup_remote_mr(qp, rkey, address, data_len,
				  UIB_ACCESS_REMOTE_WRITE, address_only,
				  response_code);
	if (!mr) {
		uet_abort_write(qp, UIB_WC_LOC_PROT_ERR);
		return -EACCES;
	}
	rc = uib_mr_place(mr, address, data, data_len);
	uib_mr_put(mr);
	if (rc == -ERANGE)
		*response_code = UIB_UET_SES_RC_BAD_ADDR;
	else if (rc < 0)
		*response_code = UIB_UET_SES_RC_HOST_UNSUCCESS_CMPL;
	if (rc < 0) {
		uet_abort_write(qp, rc == -ERANGE ? UIB_WC_LOC_LEN_ERR :
				UIB_WC_LOC_PROT_ERR);
		return rc;
	}
	if (flags & UIB_UET_SES_FLAG_EOM) {
		uet->write_last_psn = psn;
		uet->write_eom = 1;
	}
	if (!uet->write_eom ||
	    !uet_psn_before(uet->write_last_psn, uet->rx_base_psn))
		return 0;
	if (uet->write_with_imm) {
		uib_qp_emit_recv_slot_src(qp, &uet->recv_slot, UIB_WC_SUCCESS,
					  UIB_WC_RECV_RDMA_WITH_IMM,
					  uet->recv_total,
					  uet->recv_source_pdc, uet->recv_imm,
					  &uet->recv_src);
		uib_qp_release_recv(&uet->recv_slot);
		uet_reset_receive(uet);
	}
	uet_reset_write(uet);
	return 0;
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
			   const void **data, void **owned_data,
			   struct uib_uet_outstanding **owned_buffer,
			   size_t *data_len,
			   int address_only, uint8_t *response_code)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint64_t address = be64toh(ses->buffer_offset);
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	uint32_t length = ntohl(ses->request_len);
	struct uib_mr *mr;
	int rc;

	mr = uet_lookup_remote_mr(qp, rkey, address, length,
				  UIB_ACCESS_REMOTE_READ, address_only,
				  response_code);
	if (!mr)
		return -EACCES;
	*owned_buffer = uet_alloc_outstanding(uet, length ? length : 1);
	if (*owned_buffer)
		*owned_data = (*owned_buffer)->packet;
	else
		*owned_data = malloc(length ? length : 1);
	if (!*owned_data) {
		uib_mr_put(mr);
		*response_code = UIB_UET_SES_RC_UNCOR_TRANSIENT;
		return -ENOMEM;
	}
	rc = uib_mr_read(mr, address, *owned_data, length);
	uib_mr_put(mr);
	if (rc < 0) {
		if (*owned_buffer)
			uet_free_outstanding(*owned_buffer);
		else
			free(*owned_data);
		*owned_buffer = NULL;
		*owned_data = NULL;
		*response_code = rc == -ERANGE ? UIB_UET_SES_RC_BAD_ADDR :
			UIB_UET_SES_RC_HOST_UNSUCCESS_CMPL;
		return rc;
	}
	*data = *owned_data;
	*data_len = length;
	return 0;
}

static uint64_t uet_atomic_decode(const uint8_t *value, size_t width)
{
	uint64_t result = 0;
	size_t i;

	for (i = 0; i < width; i++)
		result = (result << 8) | value[i];
	return result;
}

static void uet_atomic_encode(uint8_t *value, size_t width, uint64_t input)
{
	size_t i;

	for (i = 0; i < width; i++) {
		value[width - i - 1] = input & UINT8_MAX;
		input >>= 8;
	}
}

static int64_t uet_atomic_signed(uint64_t value, size_t width)
{
	unsigned int shift = 64 - (unsigned int)width * 8;

	return (int64_t)(value << shift) >> shift;
}

static int uet_atomic_apply(uint8_t opcode, int is_signed, size_t width,
			    uint64_t old, uint64_t operand, uint64_t swap,
			    uint64_t *updated)
{
	uint64_t mask = width == sizeof(uint64_t) ? UINT64_MAX :
		(1ULL << (width * 8)) - 1;
	int64_t signed_old = uet_atomic_signed(old, width);
	int64_t signed_operand = uet_atomic_signed(operand, width);
	int condition;

	old &= mask;
	operand &= mask;
	swap &= mask;
	switch (opcode) {
	case UIB_UET_AMO_MIN:
		*updated = is_signed ?
			(signed_old < signed_operand ? old : operand) :
			(old < operand ? old : operand);
		break;
	case UIB_UET_AMO_MAX:
		*updated = is_signed ?
			(signed_old > signed_operand ? old : operand) :
			(old > operand ? old : operand);
		break;
	case UIB_UET_AMO_SUM:
		*updated = old + operand;
		break;
	case UIB_UET_AMO_DIFF:
		*updated = old - operand;
		break;
	case UIB_UET_AMO_PROD:
		*updated = old * operand;
		break;
	case UIB_UET_AMO_LOR:
		*updated = !!old || !!operand;
		break;
	case UIB_UET_AMO_LAND:
		*updated = !!old && !!operand;
		break;
	case UIB_UET_AMO_BOR:
		*updated = old | operand;
		break;
	case UIB_UET_AMO_BAND:
		*updated = old & operand;
		break;
	case UIB_UET_AMO_LXOR:
		*updated = !!old != !!operand;
		break;
	case UIB_UET_AMO_BXOR:
		*updated = old ^ operand;
		break;
	case UIB_UET_AMO_READ:
		*updated = old;
		break;
	case UIB_UET_AMO_WRITE:
		*updated = operand;
		break;
	case UIB_UET_AMO_CSWAP:
		*updated = old == operand ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_NE:
		*updated = old != operand ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_LE:
		condition = is_signed ? signed_old <= signed_operand :
			old <= operand;
		*updated = condition ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_LT:
		condition = is_signed ? signed_old < signed_operand :
			old < operand;
		*updated = condition ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_GE:
		condition = is_signed ? signed_old >= signed_operand :
			old >= operand;
		*updated = condition ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_GT:
		condition = is_signed ? signed_old > signed_operand :
			old > operand;
		*updated = condition ? swap : old;
		break;
	case UIB_UET_AMO_MSWAP:
		*updated = (old & ~operand) | (swap & operand);
		break;
	default:
		return -EOPNOTSUPP;
	}
	*updated &= mask;
	return 0;
}

static int uet_handle_atomic(struct uib_qp *qp,
			     const struct uib_uet_ses_req *ses,
			     const uint8_t *data, size_t data_len,
			     const void **response, size_t *response_len,
			     uint8_t *result, int address_only,
			     uint8_t *response_code)
{
	const struct uib_uet_ses_atomic *atomic;
	uint64_t address = be64toh(ses->buffer_offset);
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	uint64_t operand = 0, swap = 0, old = 0, updated;
	int is_signed;
	struct uib_mr *mr;
	size_t width;
	int fetching;
	int rc;

	if (data_len < sizeof(*atomic)) {
		*response_code = UIB_UET_SES_RC_AMO_UNSUPP_SIZE;
		return -EMSGSIZE;
	}
	atomic = (const struct uib_uet_ses_atomic *)data;
	data += sizeof(*atomic);
	data_len -= sizeof(*atomic);
	width = uib_uet_atomic_datatype_size(atomic->datatype);
	if (!width) {
		*response_code = UIB_UET_SES_RC_AMO_UNSUPP_DT;
		return -EAFNOSUPPORT;
	}
	if (address & (width - 1)) {
		*response_code = UIB_UET_SES_RC_AMO_UNALIGNED;
		return -EINVAL;
	}
	if (uet_atomic_has_two_operands(atomic->opcode)) {
		if (data_len == 32 && width == sizeof(uint64_t)) {
			operand = uet_atomic_decode(data + 8, width);
			swap = uet_atomic_decode(data + 24, width);
		} else if (data_len == 2 * width) {
			operand = uet_atomic_decode(data, width);
			swap = uet_atomic_decode(data + width, width);
		} else {
			*response_code = UIB_UET_SES_RC_AMO_UNSUPP_SIZE;
			return -EMSGSIZE;
		}
	} else if (atomic->opcode != UIB_UET_AMO_READ) {
		if (data_len != width) {
			*response_code = UIB_UET_SES_RC_AMO_UNSUPP_SIZE;
			return -EMSGSIZE;
		}
		operand = uet_atomic_decode(data, width);
	} else if (data_len) {
		*response_code = UIB_UET_SES_RC_AMO_UNSUPP_SIZE;
		return -EMSGSIZE;
	}
	mr = uet_lookup_remote_mr(qp, rkey, address, width,
				  UIB_ACCESS_REMOTE_ATOMIC |
				  UIB_ACCESS_REMOTE_READ |
				  UIB_ACCESS_REMOTE_WRITE,
				  address_only, response_code);
	if (!mr)
		return -EACCES;
	is_signed = !(atomic->datatype & 1);
	rc = uet_atomic_apply(atomic->opcode, is_signed, width, 0,
			      operand, swap, &updated);
	if (rc < 0) {
		*response_code = UIB_UET_SES_RC_AMO_UNSUPP_OP;
		uib_mr_put(mr);
		return rc;
	}
	pthread_mutex_lock(&qp->lock);
	rc = uib_mr_read(mr, address, &old, width);
	if (rc < 0) {
		pthread_mutex_unlock(&qp->lock);
		uib_mr_put(mr);
		return rc;
	}
	rc = uet_atomic_apply(atomic->opcode, is_signed, width, old,
			      operand, swap, &updated);
	if (!rc && atomic->opcode != UIB_UET_AMO_READ)
		rc = uib_mr_place(mr, address, &updated, width);
	pthread_mutex_unlock(&qp->lock);
	uib_mr_put(mr);
	if (rc < 0) {
		*response_code = rc == -ERANGE ? UIB_UET_SES_RC_BAD_ADDR :
			UIB_UET_SES_RC_HOST_UNSUCCESS_CMPL;
		return rc;
	}
	fetching = (ses->opcode & UIB_UET_SES_OPCODE_MASK) ==
		UIB_UET_SES_FETCHING_ATOMIC;
	if (fetching) {
		uet_atomic_encode(result, width, old);
		*response = result;
		*response_len = width;
	}
	return 0;
}

static int uet_response_cache_lookup(struct uib_uet_qp *uet, uint32_t psn,
				     uint16_t msg_id, uint8_t *response_code,
				     uint32_t *modified_len, uint8_t *data,
				     size_t *data_len)
{
	uint32_t slot;

	slot = psn % UIB_UET_PDC_MPR;
	if (!uet->response_cache[slot].valid ||
	    uet->response_cache[slot].psn != psn ||
	    uet->response_cache[slot].msg_id != msg_id)
		return -ENOENT;
	*response_code = uet->response_cache[slot].response_code;
	*modified_len = uet->response_cache[slot].modified_len;
	*data_len = uet->response_cache[slot].data_len;
	memcpy(data, uet->response_cache[slot].data, *data_len);
	return 0;
}

static void uet_response_cache_store(struct uib_uet_qp *uet, uint32_t psn,
				     uint16_t msg_id, uint8_t response_code,
				     uint32_t modified_len, const void *data,
				     size_t data_len)
{
	uint32_t slot;

	if (data_len > sizeof(uet->response_cache[0].data))
		return;
	slot = psn % UIB_UET_PDC_MPR;
	uet->response_cache[slot].psn = psn;
	uet->response_cache[slot].msg_id = msg_id;
	uet->response_cache[slot].response_code = response_code;
	uet->response_cache[slot].modified_len = modified_len;
	uet->response_cache[slot].data_len = (uint8_t)data_len;
	if (data_len)
		memcpy(uet->response_cache[slot].data, data, data_len);
	uet->response_cache[slot].valid = 1;
}

static void uet_mark_guaranteed(struct uib_uet_qp *uet, uint32_t psn)
{
	uint32_t slot;

	slot = psn % UIB_UET_PDC_MPR;
	uet->guaranteed[slot].psn = psn;
	uet->guaranteed[slot].valid = 1;
}

UIB_UET_INTERNAL void uet_clear_guaranteed(struct uib_uet_qp *uet, uint32_t clear_psn)
{
	uint32_t i;

	if (clear_psn == uet->rx_clear_psn ||
	    uet_psn_before(clear_psn, uet->rx_clear_psn))
		return;
	uet->rx_clear_psn = clear_psn;
	for (i = 0; i < UIB_UET_PDC_MPR; i++) {
		if (uet->guaranteed[i].valid &&
		    !uet_psn_before(clear_psn, uet->guaranteed[i].psn))
			uet->guaranteed[i].valid = 0;
		if (uet->response_cache[i].valid &&
		    !uet_psn_before(clear_psn, uet->response_cache[i].psn))
			uet->response_cache[i].valid = 0;
	}
}

static uint32_t uet_cack_psn(const struct uib_uet_qp *uet)
{
	uint32_t cack = uet->rx_base_psn - 1;
	uint32_t i;

	for (i = 0; i < UIB_UET_PDC_MPR; i++) {
		if (!uet->guaranteed[i].valid)
			continue;
		if (!uet_psn_before(cack, uet->guaranteed[i].psn))
			cack = uet->guaranteed[i].psn - 1;
	}
	return cack;
}

static void uet_apply_request_clear(struct uib_uet_qp *uet,
				    const struct uib_uet_pds_req *request)
{
	int32_t offset;
	uint32_t clear_psn;

	offset = (int16_t)ntohs(request->clear_psn_offset);
	clear_psn = ntohl(request->psn) + offset;
	uet_clear_guaranteed(uet, clear_psn);
}

static int uet_parse_request(const struct uib_uet_pds_req *request, size_t len,
			     size_t pds_len,
			     struct uib_uet_ses_req *canonical,
			     const uint8_t **data, size_t *data_len,
			     uint8_t *request_header)
{
	const uint8_t *ses = (const uint8_t *)request + pds_len;
	size_t header_len;

	*request_header = uib_uet_prologue_next(&request->prologue);
	switch (*request_header) {
	case UIB_UET_NEXT_REQ_STD:
		header_len = sizeof(*canonical);
		if (len < pds_len + header_len)
			return -EBADMSG;
		memcpy(canonical, ses, header_len);
		break;
	case UIB_UET_NEXT_REQ_MEDIUM: {
		const struct uib_uet_ses_req_medium *medium;

		header_len = sizeof(*medium);
		if (len < pds_len + header_len)
			return -EBADMSG;
		medium = (const struct uib_uet_ses_req_medium *)ses;
		memset(canonical, 0, sizeof(*canonical));
		canonical->opcode = medium->opcode;
		canonical->flags = medium->flags;
		canonical->msg_id = htons((uint16_t)ntohl(request->psn));
		canonical->index_gen_job_id = medium->index_gen_job_id;
		canonical->pid_on_fep = medium->pid_on_fep;
		canonical->index = medium->index;
		canonical->buffer_offset = medium->buffer_offset;
		canonical->header_data = medium->buffer_offset;
		canonical->initiator = medium->initiator;
		canonical->match_bits = medium->match_bits;
		canonical->request_len =
			htonl(ntohs(medium->request_len) &
			      UIB_UET_SES_LENGTH_MASK);
		break;
	}
	case UIB_UET_NEXT_REQ_SMALL: {
		const struct uib_uet_ses_req_small *small;

		header_len = sizeof(*small);
		if (len < pds_len + header_len)
			return -EBADMSG;
		small = (const struct uib_uet_ses_req_small *)ses;
		memset(canonical, 0, sizeof(*canonical));
		canonical->opcode = small->opcode;
		canonical->flags = small->flags;
		canonical->msg_id = htons((uint16_t)ntohl(request->psn));
		canonical->index_gen_job_id = small->index_gen_job_id;
		canonical->pid_on_fep = small->pid_on_fep;
		canonical->index = small->index;
		canonical->buffer_offset = small->buffer_offset;
		canonical->request_len =
			htonl(ntohs(small->request_len) &
			      UIB_UET_SES_LENGTH_MASK);
		break;
	}
	default:
		return -EBADMSG;
	}
	*data = ses + header_len;
	*data_len = len - pds_len - header_len;
	return 0;
}

struct uet_request_result {
	const void *data;
	void *owned_data;
	struct uib_uet_outstanding *owned_buffer;
	size_t data_len;
	uint32_t modified_len;
	uint32_t response_identity;
	uint8_t response_code;
	uint8_t guarantee_error;
	uint8_t storage[16];
};

static void uet_request_result_release(struct uet_request_result *result)
{
	if (result->owned_buffer)
		uet_free_outstanding(result->owned_buffer);
	else
		free(result->owned_data);
	result->owned_buffer = NULL;
	result->owned_data = NULL;
	result->data = NULL;
	result->data_len = 0;
}

static int uet_defer_request(struct uib_qp *qp,
			     const struct sockaddr_in *source,
			     const void *packet, size_t packet_len,
			     uint32_t psn, uint16_t msg_id, uint8_t opcode,
			     int ecn_marked)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *old;
	struct uib_uet_outstanding *out;
	struct uib_uet_deferred *deferred;
	uint32_t slot;

	out = uet_alloc_outstanding(uet, packet_len);
	if (!out)
		return -ENOMEM;
	memcpy(out->packet, packet, packet_len);
	out->psn = psn;
	slot = psn % UIB_UET_PDC_MPR;
	pthread_mutex_lock(&qp->lock);
	deferred = &uet->deferred[slot];
	if (deferred->packet && deferred->psn == psn) {
		pthread_mutex_unlock(&qp->lock);
		uet_free_outstanding(out);
		return 0;
	}
	old = deferred->packet;
	if (!old)
		__atomic_add_fetch(&uet->deferred_count, 1, __ATOMIC_RELAXED);
	deferred->packet = out;
	deferred->source = *source;
	deferred->psn = psn;
	deferred->msg_id = msg_id;
	deferred->opcode = opcode;
	deferred->ecn_marked = !!ecn_marked;
	pthread_mutex_unlock(&qp->lock);
	uet_free_outstanding(old);
	return 0;
}

static int uet_deferred_ready(const struct uib_uet_qp *uet,
			      const struct uib_uet_deferred *deferred)
{
	if (deferred->psn == uet->rx_base_psn &&
	    ((deferred->opcode == UIB_UET_SES_SEND && !uet->recv_active) ||
	     (deferred->opcode == UIB_UET_SES_WRITE && !uet->write_active)))
		return 1;
	if (uet->recv_active && deferred->opcode == UIB_UET_SES_SEND &&
	    deferred->msg_id == uet->recv_msg_id)
		return 1;
	if (uet->write_active && deferred->opcode == UIB_UET_SES_WRITE &&
	    deferred->msg_id == uet->write_msg_id)
		return 1;
	return 0;
}

static void uet_drop_deferred_copy(struct uib_qp *qp, uint32_t psn)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *packet;
	struct uib_uet_deferred *deferred;
	uint32_t slot;

	packet = NULL;
	if (!__atomic_load_n(&uet->deferred_count, __ATOMIC_RELAXED))
		return;
	slot = psn % UIB_UET_PDC_MPR;
	pthread_mutex_lock(&qp->lock);
	deferred = &uet->deferred[slot];
	if (deferred->packet && deferred->psn == psn) {
		packet = deferred->packet;
		memset(deferred, 0, sizeof(*deferred));
		__atomic_sub_fetch(&uet->deferred_count, 1,
				   __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&qp->lock);
	uet_free_outstanding(packet);
}

static struct uib_uet_outstanding *
uet_take_deferred(struct uib_qp *qp, struct sockaddr_in *source,
		  int *ecn_marked)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *packet;
	struct uib_uet_deferred *deferred;
	uint32_t best_offset;
	uint32_t offset;
	uint32_t best;
	uint32_t i;

	packet = NULL;
	if (!__atomic_load_n(&uet->deferred_count, __ATOMIC_RELAXED))
		return NULL;
	best_offset = UINT32_MAX;
	best = 0;
	pthread_mutex_lock(&qp->lock);
	for (i = 0; i < UIB_UET_PDC_MPR; i++) {
		deferred = &uet->deferred[i];
		if (!deferred->packet || !uet_deferred_ready(uet, deferred))
			continue;
		offset = deferred->psn - uet->rx_base_psn;
		if (offset >= best_offset)
			continue;
		best = i;
		best_offset = offset;
	}
	if (best_offset != UINT32_MAX) {
		deferred = &uet->deferred[best];
		packet = deferred->packet;
		*source = deferred->source;
		*ecn_marked = deferred->ecn_marked;
		memset(deferred, 0, sizeof(*deferred));
		__atomic_sub_fetch(&uet->deferred_count, 1,
				   __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&qp->lock);
	return packet;
}

static int uet_accept_request(struct uib_qp *qp,
			      const struct uib_uet_pds_req *request,
			      size_t packet_len, size_t payload_len,
			      uint8_t packet_type,
			      uint8_t opcode, uint8_t ses_flags,
			      uint16_t msg_id, uint32_t psn,
			      uint64_t receive_time_ns, int *duplicate,
			      uint32_t *cack, int *ack_due, int *new_pdc)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	const struct uib_uet_pds_cc_req *cc_request;
	uint32_t credit_target;
	uint32_t ack_bytes;
	uint32_t offset;
	int stale;
	int rc;

	pthread_mutex_lock(&qp->lock);
	*ack_due = 0;
	*new_pdc = !uet->rx_initialized;
	if (packet_type == UIB_UET_PDS_TYPE_RUD_CC_REQ && uet->rccc_enabled) {
		cc_request = (const struct uib_uet_pds_cc_req *)request;
		uib_uet_rccc_request_unpack(cc_request->req_cc_state, NULL,
					    &credit_target);
		uib_uet_rccc_destination_update(&uet->rccc_destination,
						credit_target,
						receive_time_ns);
	}
	uet->dpdcid = ntohs(request->spdcid);
	if (uib_uet_prologue_flags(&request->prologue) & UIB_UET_REQ_FLAG_SYN)
		uet->state = UIB_UET_PDC_ESTABLISHED;
	if (!uet->rx_initialized) {
		uet->rx_base_psn = qp->rq_psn;
		uet->rx_clear_psn = qp->rq_psn - 1;
		uet->rx_initialized = 1;
	}
	uet_apply_request_clear(uet, request);
	stale = uet_psn_before(psn, uet->rx_base_psn);
	/* The first SEND packet claims the receive WQE. */
	if (opcode == UIB_UET_SES_SEND && !uet->recv_active &&
	    uet_psn_before(uet->rx_base_psn, psn)) {
		offset = psn - uet->rx_base_psn;
		if (offset >= UIB_UET_PDC_MPR) {
			pthread_mutex_unlock(&qp->lock);
			return -ERANGE;
		}
		uet_debug(qp, "defer request psn=%u msg=%u expected_psn=%u",
			  psn, msg_id, uet->rx_base_psn);
		pthread_mutex_unlock(&qp->lock);
		return 1;
	}
	/* Retain later SEND messages until the active receive WQE completes. */
	if (!stale && uet->recv_active &&
	    uet->recv_msg_id != msg_id) {
		uet_debug(qp, "defer request psn=%u msg=%u active_msg=%u",
			  psn, msg_id, uet->recv_msg_id);
		pthread_mutex_unlock(&qp->lock);
		return 1;
	}
	if (!stale && opcode == UIB_UET_SES_WRITE &&
	    ((!uet->write_active &&
	      (!(ses_flags & UIB_UET_SES_FLAG_SOM) ||
	       psn != uet->rx_base_psn)) ||
	     (uet->write_active && uet->write_msg_id != msg_id))) {
		uet_debug(qp, "defer write psn=%u msg=%u active_msg=%u", psn,
			  msg_id, uet->write_msg_id);
		pthread_mutex_unlock(&qp->lock);
		return 1;
	}
	rc = uet_rx_mark(uet, psn, duplicate);
	if (!rc && !*duplicate) {
		if (uib_congestion_enabled(&uet->congestion))
			uet->receiver_bytes += (uint32_t)packet_len + 40;
		ack_bytes = payload_len < UIB_UET_ACK_GEN_MIN_PACKET ?
			UIB_UET_ACK_GEN_MIN_PACKET : (uint32_t)payload_len;
		if (ack_bytes >= UIB_UET_ACK_GEN_TRIGGER -
				 uet->ack_gen_count) {
			uet->ack_gen_count = UIB_UET_ACK_GEN_TRIGGER;
			*ack_due = 1;
		} else {
			uet->ack_gen_count += ack_bytes;
		}
	}
	*cack = uet_cack_psn(uet);
	pthread_mutex_unlock(&qp->lock);
	uet_debug(qp, "rx request psn=%u cack=%u msg=%u duplicate=%d rc=%d",
		  psn, *cack, msg_id, *duplicate, rc);
	return rc;
}

static int uet_execute_request(struct uib_qp *qp,
			       const struct sockaddr_in *src,
			       const struct uib_uet_pds_req *request,
			       const struct uib_uet_ses_req *ses,
			       const uint8_t *data, size_t data_len,
			       uint8_t request_header, uint32_t psn,
			       struct uet_request_result *result)
{
	uint8_t opcode;
	int rc;

	opcode = ses->opcode & UIB_UET_SES_OPCODE_MASK;
	result->response_code = UIB_UET_SES_RC_OK;
	if ((ses->flags & UIB_UET_SES_FLAG_IE) ||
	    opcode == UIB_UET_SES_MSG_ERROR) {
		result->response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
		uet_abort_receive(qp, UIB_WC_GENERAL_ERR);
		return -ECANCELED;
	}
	switch (opcode) {
	case UIB_UET_SES_NOOP:
		if (data_len || ntohl(ses->request_len) ||
		    (ses->flags & (UIB_UET_SES_FLAG_SOM |
				   UIB_UET_SES_FLAG_EOM)) !=
		    (UIB_UET_SES_FLAG_SOM | UIB_UET_SES_FLAG_EOM)) {
			result->response_code = UIB_UET_SES_RC_UNSUPP_SIZE;
			rc = -EMSGSIZE;
		} else {
			result->modified_len = 0;
			rc = 0;
		}
		break;
	case UIB_UET_SES_SEND:
		rc = uet_handle_send(qp, src, ses, data, data_len, psn,
				     ntohs(request->spdcid),
				     &result->response_code);
		result->modified_len = ntohl(ses->request_len);
		break;
	case UIB_UET_SES_WRITE:
		rc = uet_handle_write(qp, src, ses, data, data_len,
				      request_header == UIB_UET_NEXT_REQ_SMALL,
				      psn, ntohs(request->spdcid),
				      &result->response_code);
		result->modified_len = ntohl(ses->request_len);
		break;
	case UIB_UET_SES_READ:
		rc = uet_handle_read(qp, ses, &result->data, &result->owned_data,
				     &result->owned_buffer, &result->data_len,
				     request_header == UIB_UET_NEXT_REQ_SMALL,
				     &result->response_code);
		result->modified_len = result->data_len;
		break;
	case UIB_UET_SES_FETCHING_ATOMIC:
	case UIB_UET_SES_ATOMIC:
		rc = uet_handle_atomic(qp, ses, data, data_len, &result->data,
				       &result->data_len, result->storage,
				       request_header == UIB_UET_NEXT_REQ_SMALL,
				       &result->response_code);
		result->modified_len = result->data_len;
		break;
	default:
		result->response_code = UIB_UET_SES_RC_UNSUPP_OP;
		rc = -EOPNOTSUPP;
		break;
	}
	if (rc < 0)
		result->modified_len = 0;
	return rc;
}

static void uet_duplicate_response(struct uib_qp *qp, uint32_t psn,
				   uint16_t msg_id,
				   struct uet_request_result *result)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	int rc;

	pthread_mutex_lock(&qp->lock);
	rc = uet_response_cache_lookup(uet, psn, msg_id,
				       &result->response_code,
				       &result->modified_len, result->storage,
				       &result->data_len);
	pthread_mutex_unlock(&qp->lock);
	if (!rc && result->data_len) {
		result->data = result->storage;
	} else if (rc < 0) {
		result->response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
		result->modified_len = 0;
		result->data_len = 0;
	}
}

static int uet_record_message_error(struct uib_uet_qp *uet, uint16_t msg_id,
				    uint32_t psn, uint8_t *response_code)
{
	uint32_t slot;

	slot = msg_id % UIB_UET_PDC_MPR;
	if (uet->message_error[slot].valid &&
	    uet->message_error[slot].msg_id == msg_id) {
		*response_code = uet->message_error[slot].response_code;
		return uet->message_error[slot].first_psn == psn;
	}
	uet->message_error[slot].first_psn = psn;
	uet->message_error[slot].msg_id = msg_id;
	uet->message_error[slot].response_code = *response_code;
	uet->message_error[slot].valid = 1;
	return 1;
}

static struct uet_response_encoding
uet_finish_request(struct uib_qp *qp, struct uet_request_result *result,
		   uint32_t psn, uint16_t msg_id, uint8_t opcode,
		   int duplicate, uint32_t requested_len, uint32_t *cack)
{
	struct uib_uet_qp *uet;
	struct uet_response_encoding encoding;

	uet = qp->protocol_private;
	pthread_mutex_lock(&qp->lock);
	if (result->response_code != UIB_UET_SES_RC_OK)
		result->guarantee_error = uet_record_message_error(
			uet, msg_id, psn, &result->response_code);
	if (!duplicate && opcode != UIB_UET_SES_READ)
		uet_response_cache_store(uet, psn, msg_id,
					 result->response_code,
					 result->modified_len, result->data,
					 result->data_len);
	pthread_mutex_unlock(&qp->lock);

	if (result->response_code != UIB_UET_SES_RC_OK) {
		uet_request_result_release(result);
		result->modified_len = 0;
	}
	encoding = uet_response_construct(result->response_code,
					  (uint32_t)result->data_len,
					  result->modified_len,
					  requested_len,
					  result->guarantee_error);
	pthread_mutex_lock(&qp->lock);
	if (encoding.guaranteed_delivery)
		uet_mark_guaranteed(uet, psn);
	*cack = uet_cack_psn(uet);
	pthread_mutex_unlock(&qp->lock);
	return encoding;
}

static int uet_handle_request_one(struct uib_qp *qp,
				  const struct sockaddr_in *src,
				  const void *packet, size_t len,
				  int ecn_marked)
{
	const struct uib_uet_pds_req *request = packet;
	struct uib_uet_ses_req ses;
	struct uet_request_result result = {
		.response_code = UIB_UET_SES_RC_OK,
	};
	struct uet_response_encoding encoding;
	const uint8_t *data;
	struct uib_uet_qp *uet = qp->protocol_private;
	uint64_t receive_time_ns = uib_now_ns();
	size_t data_len;
	size_t pds_len;
	uint32_t psn;
	uint32_t cack;
	uint16_t msg_id;
	uint8_t request_header;
	uint8_t request_flags;
	uint8_t packet_type;
	uint8_t opcode;
	int duplicate;
	int ack_requested;
	int ack_due;
	int new_pdc;
	int pdc_idle;
	int send_ack;
	int rc;

	if (len < sizeof(*request))
		return -EBADMSG;
	packet_type = uib_uet_prologue_type(&request->prologue);
	pds_len = packet_type == UIB_UET_PDS_TYPE_RUD_CC_REQ ?
		sizeof(struct uib_uet_pds_cc_req) : sizeof(*request);
	if (len < pds_len)
		return -EBADMSG;
	rc = uet_parse_request(request, len, pds_len, &ses, &data, &data_len,
			       &request_header);
	if (rc < 0)
		return rc;
	psn = ntohl(request->psn);
	msg_id = ntohs(ses.msg_id);
	opcode = ses.opcode & UIB_UET_SES_OPCODE_MASK;
	request_flags = uib_uet_prologue_flags(&request->prologue);
	ack_requested = !!(request_flags & UIB_UET_REQ_FLAG_AR);
	rc = uet_accept_request(qp, request, len, data_len, packet_type, opcode,
				ses.flags & UIB_UET_SES_FLAGS_MASK, msg_id, psn,
				receive_time_ns, &duplicate, &cack, &ack_due,
				&new_pdc);
	if (rc < 0)
		return rc;
	if (rc > 0)
		return uet_defer_request(qp, src, packet, len, psn, msg_id,
					 opcode,
					 ecn_marked);
	uet_drop_deferred_copy(qp, psn);
	result.response_code = uet_validate_session(qp, &ses);
	if (duplicate && opcode != UIB_UET_SES_READ) {
		uet_duplicate_response(qp, psn, msg_id, &result);
	} else if (result.response_code == UIB_UET_SES_RC_OK) {
		uet_execute_request(qp, src, request, &ses, data, data_len,
				    request_header, psn, &result);
	}
	encoding = uet_finish_request(qp, &result, psn, msg_id, opcode,
				      duplicate, ntohl(ses.request_len), &cack);
	pdc_idle = result.response_code == UIB_UET_SES_RC_OK &&
		((opcode == UIB_UET_SES_SEND && !uet->recv_active) ||
		 (opcode == UIB_UET_SES_WRITE && !uet->write_active));
	send_ack = ack_requested || ack_due || new_pdc || duplicate ||
		ecn_marked || pdc_idle ||
		(request_flags & UIB_UET_REQ_FLAG_SYN) ||
		encoding.guaranteed_delivery ||
		result.response_code != UIB_UET_SES_RC_OK ||
		(opcode != UIB_UET_SES_SEND && opcode != UIB_UET_SES_WRITE);
	if (!send_ack) {
		uet_request_result_release(&result);
		return 0;
	}
	rc = uet_send_response(qp, src, request, &ses, cack, &encoding,
			       result.data, result.data_len,
			       result.modified_len,
			       receive_time_ns, ecn_marked, request_header);
	if (rc >= 0) {
		pthread_mutex_lock(&qp->lock);
		uet->ack_gen_count = 0;
		pthread_mutex_unlock(&qp->lock);
	}
	uet_request_result_release(&result);
	return rc;
}

UIB_UET_INTERNAL int uet_handle_request(struct uib_qp *qp,
			      const struct sockaddr_in *source,
			      const void *packet, size_t len, int ecn_marked)
{
	struct uib_uet_qp *uet;
	struct uib_uet_outstanding *deferred;
	struct sockaddr_in deferred_source;
	int deferred_ecn;
	int rc;

	uet = qp->protocol_private;
	pthread_mutex_lock(&uet->rx_lock);
	rc = uet_handle_request_one(qp, source, packet, len, ecn_marked);
	if (rc < 0)
		goto out;
	for (;;) {
		deferred = uet_take_deferred(qp, &deferred_source,
					       &deferred_ecn);
		if (!deferred) {
			rc = 0;
			break;
		}
		rc = uet_handle_request_one(qp, &deferred_source,
					    deferred->packet,
					    deferred->packet_len,
					    deferred_ecn);
		uet_free_outstanding(deferred);
		if (rc < 0)
			break;
	}

out:
	pthread_mutex_unlock(&uet->rx_lock);
	return rc;
}
