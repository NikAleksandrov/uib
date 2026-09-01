/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol_uet_internal.h"

/**
 * uib_uet_prologue_pack - Encode an Ultra Ethernet packet prologue.
 * @type: Ultra Ethernet packet type
 * @next: Next-header type
 * @flags: Prologue flags
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The packed prologue value in network byte order.
 */
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

/**
 * uib_uet_prologue_type - Decode the packet type from a UET prologue.
 * @prologue: Ultra Ethernet packet prologue
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded prologue type.
 */
uint8_t uib_uet_prologue_type(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)((ntohs(prologue->type_next_flags) >>
			  UIB_UET_PROLOGUE_TYPE_SHIFT) &
			 UIB_UET_PROLOGUE_TYPE_MASK);
}

/**
 * uib_uet_prologue_next - Decode the next-header type from a UET prologue.
 * @prologue: Ultra Ethernet packet prologue
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded next-header value.
 */
uint8_t uib_uet_prologue_next(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)((ntohs(prologue->type_next_flags) >>
			  UIB_UET_PROLOGUE_NEXT_SHIFT) &
			 UIB_UET_PROLOGUE_NEXT_MASK);
}

/**
 * uib_uet_prologue_flags - Decode flags from a UET prologue.
 * @prologue: Ultra Ethernet packet prologue
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded prologue flags.
 */
uint8_t uib_uet_prologue_flags(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)(ntohs(prologue->type_next_flags) &
			 UIB_UET_PROLOGUE_FLAGS_MASK);
}

/**
 * uib_uet_nscc_wire_pack - Encode NSCC receiver feedback for the wire.
 * @state: Wire or congestion-control state to encode or fill
 * @service_time_ns: Service time in nanoseconds to encode or fill
 * @restore: Restore-window flag to encode or fill
 * @penalty: Congestion-window penalty to encode or fill
 * @received_bytes: Newly received byte count to encode
 * @out_of_order: Out-of-order packet count to encode or fill
 * @uib-api: Ultra Ethernet extensions
 */
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

/**
 * uib_uet_nscc_wire_unpack - Decode NSCC receiver feedback from the wire.
 * @state: Wire or congestion-control state to encode or fill
 * @service_time_ns: Output for the decoded service time in nanoseconds
 * @restore: Output for the restore-window flag
 * @penalty: Output for the congestion-window penalty
 * @received_units: Output for the decoded receive counter units
 * @out_of_order: Output for the out-of-order packet count
 * @uib-api: Ultra Ethernet extensions
 */
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

/**
 * uib_uet_rccc_request_pack - Encode RCCC request state.
 * @ccc_id: Source congestion-control context identifier
 * @credit_target: Cumulative credit target in 256-byte units
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The packed request state in network byte order.
 */
uint32_t uib_uet_rccc_request_pack(uint8_t ccc_id, uint32_t credit_target)
{
	return htonl((uint32_t)ccc_id << 24 |
		     (credit_target & UIB_UET_RCCC_COUNTER_MASK));
}

/**
 * uib_uet_rccc_request_unpack - Decode RCCC request state.
 * @state: Packed request state in network byte order
 * @ccc_id: Output for the source congestion-control context identifier
 * @credit_target: Output for the cumulative 256-byte credit target
 * @uib-api: Ultra Ethernet extensions
 */
void uib_uet_rccc_request_unpack(uint32_t state, uint8_t *ccc_id,
				 uint32_t *credit_target)
{
	uint32_t value = ntohl(state);

	if (ccc_id)
		*ccc_id = value >> 24;
	if (credit_target)
		*credit_target = value & UIB_UET_RCCC_COUNTER_MASK;
}

/**
 * uib_uet_rccc_wire_pack - Encode an RCCC cumulative credit grant.
 * @state: Credit acknowledgement state to fill
 * @cumulative_credit: Cumulative credit in 256-byte units
 * @out_of_order: Out-of-order packet count
 * @uib-api: Ultra Ethernet extensions
 */
void uib_uet_rccc_wire_pack(struct uib_uet_rccc_wire_state *state,
			    uint32_t cumulative_credit,
			    uint16_t out_of_order)
{
	cumulative_credit &= UIB_UET_RCCC_COUNTER_MASK;
	state->credit[0] = cumulative_credit >> 16;
	state->credit[1] = cumulative_credit >> 8;
	state->credit[2] = cumulative_credit;
	memset(state->reserved, 0, sizeof(state->reserved));
	state->out_of_order_count = htons(out_of_order);
}

/**
 * uib_uet_rccc_wire_unpack - Decode an RCCC cumulative credit grant.
 * @state: Credit acknowledgement state to decode
 * @cumulative_credit: Output for cumulative credit in 256-byte units
 * @out_of_order: Output for the out-of-order packet count
 * @uib-api: Ultra Ethernet extensions
 */
void uib_uet_rccc_wire_unpack(const struct uib_uet_rccc_wire_state *state,
			      uint32_t *cumulative_credit,
			      uint16_t *out_of_order)
{
	if (cumulative_credit)
		*cumulative_credit = (uint32_t)state->credit[0] << 16 |
			(uint32_t)state->credit[1] << 8 | state->credit[2];
	if (out_of_order)
		*out_of_order = ntohs(state->out_of_order_count);
}

/**
 * uib_uet_ses_fragment_info - Encode SES fragment length and message offset.
 * @payload_len: Fragment payload length in bytes
 * @message_offset: Offset of the fragment in the complete message
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The packed SES fragment-information field in network byte order.
 */
uint64_t uib_uet_ses_fragment_info(uint32_t payload_len,
				   uint32_t message_offset)
{
	return htobe64(((uint64_t)(payload_len & UIB_UET_SES_LENGTH_MASK) << 32) |
			message_offset);
}

/**
 * uib_uet_ses_payload_len - Decode an SES request's fragment payload length.
 * @request: Ultra Ethernet session request
 * @packet_payload_len: Payload bytes available in the packet
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded fragment payload length in bytes.
 */
uint32_t uib_uet_ses_payload_len(const struct uib_uet_ses_req *request,
				 size_t packet_payload_len)
{
	uint64_t fragment;

	if (request->flags & UIB_UET_SES_FLAG_SOM)
		return (uint32_t)packet_payload_len;
	fragment = be64toh(request->header_data);
	return (uint32_t)(fragment >> 32) & UIB_UET_SES_LENGTH_MASK;
}

/**
 * uib_uet_ses_message_offset - Decode an SES request's message offset.
 * @request: Ultra Ethernet session request
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded message offset in bytes.
 */
uint32_t uib_uet_ses_message_offset(const struct uib_uet_ses_req *request)
{
	if (request->flags & UIB_UET_SES_FLAG_SOM)
		return 0;
	return (uint32_t)be64toh(request->header_data);
}

/**
 * uib_uet_build_ack - Build a UET cumulative or exact ACK packet.
 * @packet: Packet buffer
 * @capacity: Capacity of the output buffer in bytes
 * @cack_psn: Highest cumulatively acknowledged packet sequence number
 * @ack_psn: Packet sequence number acknowledged exactly
 * @spdcid: Source packet delivery context identifier
 * @dpdcid: Destination packet delivery context identifier
 * @next_header: Next-header type to encode
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The encoded ACK length on success, or -EINVAL for invalid input.
 */
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

/**
 * uib_uet_build_nack - Build a UET negative acknowledgement packet.
 * @packet: Packet buffer
 * @capacity: Capacity of the output buffer in bytes
 * @nack_code: UET negative acknowledgement reason
 * @nack_psn: Packet sequence number being rejected
 * @spdcid: Source packet delivery context identifier
 * @dpdcid: Destination packet delivery context identifier
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The encoded NACK length on success, or -EINVAL for invalid input.
 */
int uib_uet_build_nack(void *packet, size_t capacity, uint8_t nack_code,
		       uint32_t nack_psn, uint16_t spdcid, uint16_t dpdcid)
{
	struct uib_uet_pds_nack *nack = packet;

	if (!packet || capacity < sizeof(*nack))
		return -EINVAL;
	memset(nack, 0, sizeof(*nack));
	nack->prologue.type_next_flags = uib_uet_prologue_pack(
		UIB_UET_PDS_TYPE_NACK, UIB_UET_NEXT_NONE, 0);
	nack->nack_code = nack_code;
	nack->nack_psn = htonl(nack_psn);
	nack->spdcid = htons(spdcid);
	nack->dpdcid = htons(dpdcid);
	return sizeof(*nack);
}

UIB_UET_INTERNAL uint16_t uet_ses_rsp_pack(uint8_t opcode, uint8_t response_code)
{
	uint16_t value;

	value = ((uint16_t)opcode & UIB_UET_SES_RSP_OPCODE_MASK) <<
		UIB_UET_SES_RSP_OPCODE_SHIFT;
	value |= response_code & UIB_UET_SES_RSP_RC_MASK;
	return htons(value);
}

UIB_UET_INTERNAL int uet_packet_qpn(const void *packet, size_t len, uint32_t *qpn)
{
	const struct uib_uet_prologue *prologue = packet;
	uint8_t type;

	if (!packet || !qpn || len < sizeof(*prologue))
		return -EINVAL;
	type = uib_uet_prologue_type(prologue);
	switch (type) {
	case UIB_UET_PDS_TYPE_RUD_REQ:
	case UIB_UET_PDS_TYPE_RUD_CC_REQ: {
		const struct uib_uet_pds_req *req = packet;
		size_t pds_len;
		uint8_t next;

		if (len < sizeof(*req))
			return -EBADMSG;
		pds_len = type == UIB_UET_PDS_TYPE_RUD_CC_REQ ?
			sizeof(struct uib_uet_pds_cc_req) : sizeof(*req);
		if (len < pds_len)
			return -EBADMSG;
		*qpn = ntohs(req->dpdcid);
		if (!*qpn) {
			const struct uib_uet_ses_req_small *ses;
			size_t header_len;

			next = uib_uet_prologue_next(&req->prologue);
			header_len = next == UIB_UET_NEXT_REQ_SMALL ?
				sizeof(struct uib_uet_ses_req_small) :
				next == UIB_UET_NEXT_REQ_MEDIUM ?
				sizeof(struct uib_uet_ses_req_medium) :
				next == UIB_UET_NEXT_REQ_STD ?
				sizeof(struct uib_uet_ses_req) : 0;
			if (!header_len || len < pds_len + header_len)
				return -EBADMSG;
			ses = (const struct uib_uet_ses_req_small *)
				((const uint8_t *)req + pds_len);
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
	case UIB_UET_PDS_TYPE_CTRL_MSG: {
		const struct uib_uet_pds_control *control = packet;

		if (len < sizeof(*control))
			return -EBADMSG;
		*qpn = ntohs(control->dpdcid);
		return *qpn ? 0 : -ENOENT;
	}
	default:
		return -EOPNOTSUPP;
	}
}

UIB_UET_INTERNAL uint8_t uet_ipv4_tos(struct uib_context *ctx, const void *packet,
			    size_t len)
{
	const struct uib_uet_context *context = ctx->protocol_private;
	const struct uib_uet_prologue *prologue = packet;
	uint8_t next;
	uint8_t type;

	if (!context || !context->trimming_enabled ||
	    len < sizeof(*prologue))
		return IPTOS_ECN_ECT0;
	type = uib_uet_prologue_type(prologue);
	if (type == UIB_UET_PDS_TYPE_RUD_REQ ||
	    type == UIB_UET_PDS_TYPE_RUD_CC_REQ)
		return (uint8_t)(context->trimmable_dscp << 2) |
			IPTOS_ECN_ECT0;
	if (type != UIB_UET_PDS_TYPE_ACK &&
	    type != UIB_UET_PDS_TYPE_ACK_CC)
		return IPTOS_ECN_ECT0;
	next = uib_uet_prologue_next(prologue);
	if (next != UIB_UET_NEXT_RSP_DATA &&
	    next != UIB_UET_NEXT_RSP_DATA_SMALL)
		return IPTOS_ECN_ECT0;
	return (uint8_t)(context->trimmable_dscp << 2) | IPTOS_ECN_ECT0;
}
