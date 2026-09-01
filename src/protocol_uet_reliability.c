/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol_uet_internal.h"

static void uet_request_ack(struct uib_uet_outstanding *out);

UIB_UET_INTERNAL void uet_track_get(struct uib_uet_wr_track *track)
{
	__atomic_add_fetch(&track->refs, 1, __ATOMIC_RELAXED);
}

UIB_UET_INTERNAL void uet_free_track(struct uib_uet_wr_track *track)
{
	if (!track)
		return;
	if (__atomic_sub_fetch(&track->refs, 1, __ATOMIC_ACQ_REL))
		return;
	uib_mr_put(track->mr);
	if (track->owned_message_buffer)
		uet_free_outstanding(track->owned_message_buffer);
	else
		free(track->owned_message);
	if (track->read_sgl != track->inline_read_sgl)
		free(track->read_sgl);
	if (track->read_fragments != track->inline_read_fragments)
		free(track->read_fragments);
	uet_track_pool_push(track);
}

UIB_UET_INTERNAL void uet_free_outstanding(void *opaque)
{
	struct uib_uet_outstanding *out = opaque;
	struct uib_mr *mr;

	if (!out)
		return;
	if (__atomic_sub_fetch(&out->refs, 1, __ATOMIC_ACQ_REL))
		return;
	mr = out->payload_mr;
	out->payload_mr = NULL;
	uib_mr_put(mr);
	uet_out_pool_push(out);
}

static void uet_transport_release(void *opaque)
{
	struct uib_uet_outstanding *out;
	uint64_t deadline;
	uint64_t now_ns;
	uint64_t rto;

	out = opaque;
	if (!out)
		return;
	now_ns = uib_now_ns();
	rto = __atomic_load_n(&out->rto_ns, __ATOMIC_RELAXED);
	deadline = rto > UINT64_MAX - now_ns ? UINT64_MAX : now_ns + rto;
	if (__atomic_load_n(&out->nack_pending, __ATOMIC_ACQUIRE))
		deadline = 0;
	__atomic_store_n(&out->tx_time_ns, now_ns, __ATOMIC_RELAXED);
	__atomic_store_n(&out->deadline_ns, deadline, __ATOMIC_RELAXED);
	__atomic_store_n(&out->tx_pending, 0, __ATOMIC_RELEASE);
	uet_free_outstanding(out);
}

static void uet_outstanding_index_add(struct uib_uet_qp *uet,
				      struct uib_uet_outstanding *out)
{
	if (uib_psn_index_insert(&uet->out_index, out->psn, out)) {
		uet->unindexed_count++;
		return;
	}
	out->indexed = 1;
}

static void uet_outstanding_index_remove(struct uib_uet_qp *uet,
					 struct uib_uet_outstanding *out)
{
	if (!out->indexed) {
		if (uet->unindexed_count)
			uet->unindexed_count--;
		return;
	}
	uib_psn_index_remove(&uet->out_index, out->psn, out);
	out->indexed = 0;
}

static struct uib_uet_outstanding *
uet_first_unsent(struct uib_uet_qp *uet)
{
	struct uib_uet_outstanding *out;

	for (out = uet->out_head; out; out = out->next) {
		if (!out->sent && !out->sack_acked)
			return out;
	}
	return NULL;
}

static int uet_out_requires_response(const struct uib_uet_outstanding *out)
{
	return out->track->opcode == UIB_WR_RDMA_READ ||
		out->track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		out->track->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
		(out->track->opcode == UIB_WR_UET_ATOMIC &&
		 out->track->read_num_sge);
}

UIB_UET_INTERNAL void uet_link_outstanding(struct uib_uet_qp *uet,
				 struct uib_uet_outstanding *out)
{
	out->next = NULL;
	out->prev = uet->out_tail;
	if (uet->out_tail)
		uet->out_tail->next = out;
	else
		uet->out_head = out;
	uet->out_tail = out;
	if (!uet_out_requires_response(out)) {
		out->ack_next = NULL;
		out->ack_prev = uet->ack_tail;
		if (uet->ack_tail)
			uet->ack_tail->ack_next = out;
		else
			uet->ack_head = out;
		uet->ack_tail = out;
	}
	if (!uet->tx_cursor)
		uet->tx_cursor = out;
	uet_outstanding_index_add(uet, out);
	uet->out_count++;
}

UIB_UET_INTERNAL void uet_link_track(struct uib_uet_qp *uet,
			   struct uib_uet_wr_track *track)
{
	track->next = NULL;
	track->prev = uet->track_tail;
	if (uet->track_tail)
		uet->track_tail->next = track;
	else
		uet->track_head = track;
	uet->track_tail = track;
}

static void uet_unlink_track(struct uib_uet_qp *uet,
			     struct uib_uet_wr_track *track)
{
	if (track->prev)
		track->prev->next = track->next;
	else
		uet->track_head = track->next;
	if (track->next)
		track->next->prev = track->prev;
	else
		uet->track_tail = track->prev;
	track->next = NULL;
	track->prev = NULL;
}

static void uet_unlink_outstanding(struct uib_uet_qp *uet,
				   struct uib_uet_outstanding *out)
{
	struct uib_uet_outstanding *cursor;

	if (out->prev)
		out->prev->next = out->next;
	else
		uet->out_head = out->next;
	if (out->next)
		out->next->prev = out->prev;
	else
		uet->out_tail = out->prev;
	if (uet->tx_cursor == out) {
		cursor = out->next;
		while (cursor && (cursor->sent || cursor->sack_acked))
			cursor = cursor->next;
		uet->tx_cursor = cursor;
	}
	if (out->ack_prev)
		out->ack_prev->ack_next = out->ack_next;
	else if (uet->ack_head == out)
		uet->ack_head = out->ack_next;
	if (out->ack_next)
		out->ack_next->ack_prev = out->ack_prev;
	else if (uet->ack_tail == out)
		uet->ack_tail = out->ack_prev;
	uet_outstanding_index_remove(uet, out);
	if (out->sent)
		uet->sent_count--;
	uet->out_count--;
	uet->materialized_count--;
	out->next = NULL;
	out->prev = NULL;
	out->ack_next = NULL;
	out->ack_prev = NULL;
}

UIB_UET_INTERNAL void uet_qp_fini(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *next_track;
	struct uib_uet_wr_track *track;
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;
	uint32_t i;

	if (!uet)
		return;
	pthread_mutex_lock(&qp->lock);
	uet->prep_closing = 1;
	while (uet->prep_pending ||
	       __atomic_load_n(&uet->sequence_queued, __ATOMIC_ACQUIRE))
		pthread_cond_wait(&uet->prep_cond, &qp->lock);
	pthread_mutex_unlock(&qp->lock);
	for (out = uet->out_head; out; out = next) {
		next = out->next;
		uet_unlink_outstanding(uet, out);
		uet_free_outstanding(out);
	}
	for (track = uet->track_head; track; track = next_track) {
		next_track = track->next;
		uet_free_track(track);
	}
	for (i = 0; i < UIB_UET_PDC_MPR; i++)
		uet_free_outstanding(uet->deferred[i].packet);
	if (uet->recv_active)
		uib_qp_release_recv(&uet->recv_slot);
	pthread_cond_destroy(&uet->prep_cond);
	pthread_mutex_destroy(&uet->rx_lock);
	uib_psn_index_destroy(&uet->out_index);
	uet_track_pool_destroy(uet->track_pool);
	uet_out_pool_destroy(uet->out_pool);
	free(uet);
	qp->protocol_private = NULL;
}

static void uet_track_done(struct uib_qp *qp, struct uib_uet_wr_track *track,
			   enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if (!track->pending)
		return;
	if (status != UIB_WC_SUCCESS)
		track->pending = 1;
	track->pending--;
	if (track->pending)
		return;

	qp_emit_send_vendor(qp, track->wr_id, track->opcode,
			    track->signaled, status,
			    track->opcode == UIB_WR_RDMA_READ ||
			    track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
			    track->opcode == UIB_WR_ATOMIC_CMP_SWAP ?
			    track->total_len : 0, track->response_code);
	uet_unlink_track(uet, track);
	uet_free_track(track);
}

UIB_UET_INTERNAL void uet_remove_track_locked(struct uib_qp *qp,
				    struct uib_uet_wr_track *track,
				    enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;

	__atomic_store_n(&track->cancelled, 1, __ATOMIC_RELEASE);
	for (out = uet->out_head; out; out = next) {
		next = out->next;
		if (out->track != track)
			continue;
		uet_unlink_outstanding(uet, out);
		uet_free_outstanding(out);
	}
	track->pending = 1;
	uet_track_done(qp, track, status);
	if (!uet->sent_count)
		uib_qp_retry_cancel(qp);
}

static struct uib_uet_outstanding *
uet_find_outstanding(struct uib_uet_qp *uet, uint32_t psn, uint16_t msg_id)
{
	struct uib_uet_outstanding *out;

	out = uib_psn_index_lookup(&uet->out_index, psn);
	if (out && (!msg_id || out->msg_id == msg_id))
		return out;
	if (!uet->unindexed_count)
		return NULL;
	for (out = uet->out_head; out; out = out->next) {
		if (!out->indexed && out->psn == psn &&
		    (!msg_id || out->msg_id == msg_id))
			return out;
	}
	return NULL;
}

static void uet_select_path(struct uib_uet_qp *uet,
			    struct uib_uet_outstanding *out, uint64_t now_ns)
{
	uint16_t count = uet->multipath.path_count;
	uint16_t selected = uet->path_cursor;
	uint16_t candidate;
	uint16_t i;

	if (uet->multipath.mode == UIB_UET_MULTIPATH_OFF)
		return;
	for (i = 0; i < count; i++) {
		candidate = (uint16_t)((uet->path_cursor + i) % count);
		if (uet->multipath.mode != UIB_UET_MULTIPATH_PATH_AWARE ||
		    uet->paths[candidate].blocked_until_ns <= now_ns) {
			selected = candidate;
			if (i)
				uet->multipath_stats.ecn_avoids++;
			break;
		}
	}
	uet->path_cursor = (uint16_t)((selected + 1) % count);
	out->path_index = selected;
	out->entropy = uet->multipath.entropy_base + selected;
	if (uet->previous_path != UINT16_MAX &&
	    uet->previous_path != selected)
		uet->multipath_stats.path_switches++;
	uet->previous_path = selected;
	uet->multipath_stats.active_entropy = out->entropy;
	if (!uet->paths[selected].used) {
		uet->paths[selected].used = 1;
		uet->multipath_stats.paths_used++;
	}
}

static uint32_t uet_rccc_credit_pack(uint32_t credit)
{
	return htonl((credit & UIB_UET_RCCC_COUNTER_MASK) << 8);
}

static uint32_t uet_rccc_credit_unpack(uint32_t payload)
{
	return ntohl(payload) >> 8;
}

static int uet_send_control(struct uib_qp *qp,
			    const struct sockaddr_in *destination,
			    uint8_t control_type, uint32_t payload)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_pds_control control = {0};
	struct sockaddr_in dst = *destination;

	control.prologue.type_next_flags = uib_uet_prologue_pack(
		UIB_UET_PDS_TYPE_CTRL_MSG, control_type, 0);
	control.spdcid = htons(uet->spdcid);
	control.dpdcid = htons((uint16_t)qp->dest_qp_num);
	control.payload = payload;
	if (!dst.sin_port)
		dst.sin_port = htons(UIB_UET_DEFAULT_PORT);
	return uib_transport_send_entropy(qp->ctx, &dst, &control,
					  sizeof(control), 0);
}

static int uet_send_nack(struct uib_qp *qp,
			 const struct sockaddr_in *destination,
			 const struct uib_uet_pds_req *request,
			 uint8_t nack_code)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_pds_nack nack;
	struct sockaddr_in dst;
	int rc;

	dst = *destination;
	rc = uib_uet_build_nack(&nack, sizeof(nack), nack_code,
				ntohl(request->psn), uet->spdcid,
				ntohs(request->spdcid));
	if (rc < 0)
		return rc;
	if (!dst.sin_port)
		dst.sin_port = htons(UIB_UET_DEFAULT_PORT);
	return uib_transport_send_entropy(qp->ctx, &dst, &nack, sizeof(nack),
					  0);
}

static int uet_send_credit_request_locked(struct uib_qp *qp,
					  uint64_t now_ns)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_rccc *rccc;
	uint32_t payload;
	uint32_t target;
	int rc;

	if (!uet->rccc_enabled)
		return 0;
	rccc = (struct uib_uet_rccc *)uet->congestion.state.bytes;
	if (rccc->credit_target_sent &&
	    now_ns < uet->credit_request_deadline_ns)
		return 0;
	target = uib_uet_rccc_credit_target(rccc);
	payload = uib_uet_rccc_request_pack((uint8_t)uet->spdcid, target);
	rc = uet_send_control(qp, &qp->remote_addr,
			      UIB_UET_CONTROL_CREDIT_REQUEST, payload);
	if (rc < 0)
		return rc;
	uib_uet_rccc_mark_target_sent(rccc);
	uet->credit_request_deadline_ns = now_ns +
					 qp->ctx->retry_timeout_ns;
	uib_qp_retry_schedule(qp, uet->credit_request_deadline_ns);
	return 0;
}

static uint64_t uet_rto_interval(const struct uib_qp *qp,
				 const struct uib_uet_outstanding *out)
{
	uint64_t interval;

	interval = qp->ctx->retry_timeout_ns;
	if (out->rto_retries >= 63 || interval > (UINT64_MAX >>
						      out->rto_retries))
		return UINT64_MAX;
	return interval << out->rto_retries;
}

static void uet_prepare_send_ref(struct uib_qp *qp,
				 struct uib_uet_outstanding *out,
				 struct uib_transport_iov_ref *ref,
				 struct iovec vectors[2], uint64_t now_ns)
{
	struct uib_uet_qp *uet;
	uint64_t rto;

	uet = qp->protocol_private;
	rto = uet_rto_interval(qp, out);
	out->rto_ns = rto;
	out->tx_time_ns = now_ns;
	out->deadline_ns = rto > UINT64_MAX - now_ns ?
		UINT64_MAX : now_ns + rto;
	__atomic_store_n(&out->tx_pending, 1, __ATOMIC_RELEASE);
	if (uib_congestion_ack_request(&uet->congestion,
				       UIB_UET_ACK_GEN_TRIGGER))
		uet_request_ack(out);
	if (!out->entropy)
		uet_select_path(uet, out, now_ns);
	vectors[0].iov_base = out->packet;
	vectors[0].iov_len = out->header_len;
	ref->dst = &out->dst;
	ref->iov = vectors;
	ref->release = uet_transport_release;
	ref->opaque = out;
	ref->len = out->packet_len;
	ref->iov_count = 1;
	ref->flow = qp->qp_num + out->msg_id;
	ref->entropy = out->entropy;
	if (out->payload_len) {
		vectors[1].iov_base = (void *)out->payload;
		vectors[1].iov_len = out->payload_len;
		ref->iov_count++;
	}
	__atomic_add_fetch(&out->refs, 1, __ATOMIC_RELAXED);
	uib_congestion_on_send(&uet->congestion, out->nominal_len);
}

static void uet_commit_send_batch(struct uib_qp *qp,
				  struct uib_uet_outstanding **outstanding,
				  uint32_t count,
				  struct uib_uet_outstanding *candidate)
{
	struct uib_uet_qp *uet;
	struct uib_uet_outstanding *out;
	uint64_t packet_deadline;
	uint64_t deadline;
	uint64_t next;
	uint32_t i;

	uet = qp->protocol_private;
	deadline = UINT64_MAX;
	for (i = 0; i < count; i++) {
		out = outstanding[i];
		out->sent = 1;
		uet->sent_count++;
		packet_deadline = __atomic_load_n(&out->deadline_ns,
						  __ATOMIC_RELAXED);
		if (packet_deadline < deadline)
			deadline = packet_deadline;
		if (out->entropy) {
			uet->paths[out->path_index].sent++;
			uet->multipath_stats.packets++;
		}
	}
	uet->tx_cursor = candidate;
	if (candidate) {
		next = uib_congestion_next_send_ns(&uet->congestion);
		if (next && next < deadline)
			deadline = next;
	}
	if (!qp->retry_scheduled || deadline < qp->retry_deadline_ns)
		uib_qp_retry_schedule(qp, deadline);
}

UIB_UET_INTERNAL int uet_drain_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *outstanding[UIB_UET_PDC_MPR];
	struct uib_uet_outstanding *candidate;
	struct uib_transport_iov_ref refs[UIB_UET_PDC_MPR];
	struct iovec vectors[UIB_UET_PDC_MPR][2];
	struct uib_congestion congestion_before;
	uint64_t deadline;
	uint64_t now_ns;
	uint32_t capacity;
	uint32_t count;
	uint32_t i;
	int rc;

	capacity = UIB_UET_PDC_MPR - uet->sent_count;
	candidate = uet->tx_cursor;
	count = 0;
	congestion_before = uet->congestion;
	now_ns = uib_now_ns();
	while (candidate && count < capacity) {
		if (!uib_congestion_can_send(&uet->congestion,
					     candidate->nominal_len))
			break;
		outstanding[count] = candidate;
		uet_prepare_send_ref(qp, candidate, &refs[count],
				     vectors[count], now_ns);
		count++;
		candidate = candidate->next;
		while (candidate && (candidate->sent || candidate->sack_acked))
			candidate = candidate->next;
	}
	if (!count) {
		if (candidate && uet->rccc_enabled) {
			rc = uet_send_credit_request_locked(qp, now_ns);
			if (rc < 0)
				return rc;
		}
		deadline = uib_congestion_next_send_ns(&uet->congestion);
		if (candidate && deadline) {
			if (deadline <= now_ns)
				deadline = now_ns + 1;
			uib_qp_retry_schedule(qp, deadline);
		}
		return 0;
	}
	rc = uib_transport_send_iov_ref_batch(qp->ctx, refs, count);
	if (rc < 0) {
		for (i = 0; i < count; i++)
			__atomic_store_n(&outstanding[i]->tx_pending, 0,
					 __ATOMIC_RELEASE);
		uet->congestion = congestion_before;
		return rc;
	}
	uet_commit_send_batch(qp, outstanding, count, candidate);
	return 0;
}

static void uet_complete_out_locked(struct uib_qp *qp,
				    struct uib_uet_outstanding *out,
				    enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *track = out->track;

	uet_unlink_outstanding(uet, out);
	uet_free_outstanding(out);
	uet_track_done(qp, track, status);
	if (!uet->sent_count)
		uib_qp_retry_cancel(qp);
}

static int uet_track_read_fragment(struct uib_uet_wr_track *track,
				   uint32_t offset, uint32_t length)
{
	struct uib_uet_read_fragment *fragments;
	uint32_t capacity;
	uint32_t i;

	for (i = 0; i < track->read_fragment_count; i++) {
		uint32_t old_offset = track->read_fragments[i].offset;
		uint32_t old_length = track->read_fragments[i].length;

		if (old_offset == offset && old_length == length)
			return 1;
		if (offset < old_offset + old_length && old_offset < offset + length)
			return -EBADMSG;
	}
	if (track->read_fragment_count == track->read_fragment_capacity) {
		capacity = track->read_fragment_capacity * 2;
		fragments = malloc(capacity * sizeof(*fragments));
		if (!fragments)
			return -ENOMEM;
		memcpy(fragments, track->read_fragments,
		       track->read_fragment_count * sizeof(*fragments));
		if (track->read_fragments != track->inline_read_fragments)
			free(track->read_fragments);
		track->read_fragments = fragments;
		track->read_fragment_capacity = capacity;
	}
	track->read_fragments[track->read_fragment_count].offset = offset;
	track->read_fragments[track->read_fragment_count].length = length;
	track->read_fragment_count++;
	track->read_received += length;
	return 0;
}

struct uet_ack_view {
	const struct uib_uet_pds_ack *ack;
	const struct uib_uet_pds_ack_ext *extension;
	const uint8_t *data;
	size_t data_len;
	uint32_t cack;
	uint32_t ack_psn;
	uint32_t sack_psn;
	uint32_t message_offset;
	uint32_t modified_len;
	uint32_t response_identity;
	uint16_t msg_id;
	uint64_t sack_bitmap;
	uint8_t response_code;
	uint8_t has_data_response;
};

static int uet_parse_ack(const void *packet, size_t len,
			 struct uet_ack_view *view)
{
	const struct uib_uet_ses_rsp_data_small *small;
	const struct uib_uet_ses_rsp_data *response_data;
	const struct uib_uet_ses_rsp *response;
	size_t header_len;
	uint32_t small_request_psn;
	uint16_t packed;
	uint8_t next;
	int32_t offset;

	memset(view, 0, sizeof(*view));
	small_request_psn = 0;
	view->ack = packet;
	header_len = sizeof(*view->ack);
	if (uib_uet_prologue_type(&view->ack->prologue) ==
	    UIB_UET_PDS_TYPE_ACK_CC) {
		if (len < header_len + sizeof(*view->extension))
			return -EBADMSG;
		view->extension = (const struct uib_uet_pds_ack_ext *)
			((const uint8_t *)packet + header_len);
		header_len += sizeof(*view->extension);
	}
	next = uib_uet_prologue_next(&view->ack->prologue);
	if (next == UIB_UET_NEXT_RSP_DATA) {
		if (len < header_len + sizeof(*response_data))
			return -EBADMSG;
		response_data = (const struct uib_uet_ses_rsp_data *)
			((const uint8_t *)packet + header_len);
		view->data = (const uint8_t *)(response_data + 1);
		view->data_len = len - header_len - sizeof(*response_data);
		if ((ntohs(response_data->payload_len) &
		     UIB_UET_SES_LENGTH_MASK) != view->data_len)
			return -EBADMSG;
		view->msg_id = ntohs(response_data->request_msg_id);
		view->response_identity = ntohl(response_data->index_gen_job_id);
		packed = ntohs(response_data->list_opcode_version_rc);
		view->modified_len = ntohl(response_data->modified_len);
		view->message_offset = ntohl(response_data->message_offset);
		view->has_data_response = 1;
	} else if (next == UIB_UET_NEXT_RSP_DATA_SMALL) {
		if (len < header_len + sizeof(*small))
			return -EBADMSG;
		small = (const struct uib_uet_ses_rsp_data_small *)
			((const uint8_t *)packet + header_len);
		view->data = (const uint8_t *)(small + 1);
		view->data_len = len - header_len - sizeof(*small);
		if ((ntohs(small->payload_len) & UIB_UET_SES_LENGTH_MASK) !=
		    view->data_len)
			return -EBADMSG;
		small_request_psn = ntohl(small->request_psn);
		view->msg_id = (uint16_t)small_request_psn;
		view->response_identity = ntohl(small->index_gen_job_id);
		packed = ntohs(small->list_opcode_version_rc);
		view->modified_len = (uint32_t)view->data_len;
		view->has_data_response = 1;
	} else if (next == UIB_UET_NEXT_RSP) {
		if (len < header_len + sizeof(*response))
			return -EBADMSG;
		response = (const struct uib_uet_ses_rsp *)
			((const uint8_t *)packet + header_len);
		view->data = (const uint8_t *)(response + 1);
		view->data_len = len - header_len - sizeof(*response);
		if (view->data_len)
			return -EBADMSG;
		view->msg_id = ntohs(response->msg_id);
		view->response_identity = ntohl(response->index_gen_job_id);
		packed = ntohs(response->list_opcode_version_rc);
		view->modified_len = ntohl(response->modified_len);
	} else {
		return -EBADMSG;
	}
	view->cack = ntohl(view->ack->cack_psn);
	offset = (int16_t)ntohs(view->ack->ack_psn_offset);
	view->ack_psn = view->cack + offset;
	if (view->extension) {
		offset = (int16_t)ntohs(view->extension->sack_psn_offset);
		view->sack_psn = view->cack + offset;
		view->sack_bitmap = be64toh(view->extension->sack_bitmap);
	}
	if (next == UIB_UET_NEXT_RSP_DATA_SMALL &&
	    small_request_psn != view->ack_psn)
		return -EBADMSG;
	view->response_code = packed & UIB_UET_SES_RSP_RC_MASK;
	return 0;
}

static void uet_ack_path_feedback(struct uib_uet_qp *uet,
				  const struct uet_ack_view *view,
				  struct uib_uet_outstanding *out)
{
	if (!out || !out->entropy)
		return;
	uet->paths[out->path_index].acknowledged++;
	if (uet->multipath.mode != UIB_UET_MULTIPATH_PATH_AWARE ||
	    !(uib_uet_prologue_flags(&view->ack->prologue) &
	      UIB_UET_ACK_FLAG_M))
		return;
	uet->paths[out->path_index].ecn_marks++;
	uet->paths[out->path_index].blocked_until_ns =
		uib_now_ns() + uet->multipath.ecn_hold_ns;
}

static void uet_ack_congestion_feedback(struct uib_uet_qp *uet,
					 const struct uet_ack_view *view,
					 struct uib_uet_outstanding *out,
					 uint64_t rx_time_ns)
{
	const struct uib_uet_nscc_wire_state *cc_state;
	const struct uib_uet_rccc_wire_state *credit_state;
	struct uib_uet_outstanding *pending;
	struct uib_congestion_ack event = {0};
	uint32_t cumulative_credit;
	uint32_t received_units = 0;
	uint32_t service_time_ns = 0;
	uint32_t delta;
	uint16_t cc_fields;
	uint8_t cc_type;
	uint8_t restore = 0;
	uint8_t penalty = 0;

	if (!uib_congestion_enabled(&uet->congestion) || !view->extension)
		return;
	cc_fields = ntohs(view->extension->cc_type_flags_mpr);
	cc_type = (cc_fields >> UIB_UET_CC_TYPE_SHIFT) &
		UIB_UET_CC_TYPE_MASK;
	if (cc_type == UIB_UET_CC_TYPE_CREDIT) {
		credit_state = (const struct uib_uet_rccc_wire_state *)
			&view->extension->ack_cc_state;
		uib_uet_rccc_wire_unpack(credit_state, &cumulative_credit, NULL);
		for (pending = uet->ack_head; pending;
		     pending = pending->ack_next) {
			if (uet_psn_before(view->cack, pending->psn))
				break;
			if (UINT32_MAX - event.newly_received <
			    pending->nominal_len) {
				event.newly_received = UINT32_MAX;
				break;
			}
			event.newly_received += pending->nominal_len;
		}
		if (out && uet_out_requires_response(out))
			event.newly_received =
				UINT32_MAX - event.newly_received < out->nominal_len ?
				UINT32_MAX : event.newly_received + out->nominal_len;
		event.credit_valid = 1;
		event.cumulative_credit = cumulative_credit;
		goto common;
	}
	if (cc_type != UIB_UET_CC_TYPE_NSCC)
		return;
	cc_state = (const struct uib_uet_nscc_wire_state *)
		&view->extension->ack_cc_state;
	uib_uet_nscc_wire_unpack(cc_state, &service_time_ns, &restore,
				 &penalty, &received_units, NULL);
	delta = (received_units - uet->previous_received_units) &
		UIB_UET_NSCC_RCVD_MASK;
	if (delta < 0x00800000U) {
		event.newly_received = delta << 8;
		uet->previous_received_units = received_units;
	}
	event.service_time_ns = service_time_ns;
	event.receiver_cwnd_penalty = penalty;
	event.restore_cwnd = restore;
common:
	event.ack_time_ns = rx_time_ns ? rx_time_ns : uib_now_ns();
	event.ecn_marked = !!(uib_uet_prologue_flags(&view->ack->prologue) &
			       UIB_UET_ACK_FLAG_M);
	event.retransmit = !!(uib_uet_prologue_flags(&view->ack->prologue) &
			       UIB_UET_ACK_FLAG_RETX);
	if (out) {
		event.tx_time_ns = __atomic_load_n(&out->tx_time_ns,
						   __ATOMIC_RELAXED);
		event.tx_retries = out->retries;
	} else {
		event.tx_retries = 2;
	}
	uib_congestion_on_ack(&uet->congestion, &event);
}

static void uet_complete_cumulative_locked(struct uib_qp *qp, uint32_t cack)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;

	for (out = uet->ack_head; out; out = next) {
		next = out->ack_next;
		if (uet_psn_before(cack, out->psn))
			break;
		uet_complete_out_locked(qp, out, UIB_WC_SUCCESS);
	}
}

static void uet_apply_sack_locked(struct uib_qp *qp,
				  const struct uet_ack_view *view)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;
	uint64_t bitmap;
	uint32_t psn;
	uint32_t bit;

	bitmap = view->sack_bitmap;
	for (bit = 0; bitmap; bit++, bitmap >>= 1) {
		if (!(bitmap & 1))
			continue;
		psn = view->sack_psn + bit;
		out = uet_find_outstanding(uet, psn, 0);
		if (!out || out->sack_acked)
			continue;
		out->sack_acked = 1;
		if (out->sent) {
			out->sent = 0;
			uet->sent_count--;
		}
	}
	uet->tx_cursor = uet_first_unsent(uet);
}

static int uet_atomic_response_data(const struct uib_uet_wr_track *track,
				    const struct uet_ack_view *view,
				    uint8_t *storage,
				    const uint8_t **result)
{
	uint64_t value;
	size_t width;
	size_t i;

	*result = view->data;
	if (track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    track->opcode == UIB_WR_ATOMIC_CMP_SWAP) {
		if (view->data_len != sizeof(value))
			return -EBADMSG;
		memcpy(&value, view->data, sizeof(value));
		value = be64toh(value);
		memcpy(storage, &value, sizeof(value));
		*result = storage;
		return 0;
	}
	if (track->opcode != UIB_WR_UET_ATOMIC)
		return 0;
	width = uib_uet_atomic_datatype_size(track->atomic_datatype);
	if (view->data_len != width)
		return -EBADMSG;
	for (i = 0; i < width; i++)
		storage[i] = view->data[width - i - 1];
	*result = storage;
	return 0;
}

static int uet_apply_read_response_locked(struct uib_qp *qp,
					  struct uib_uet_outstanding *out,
					  const struct uet_ack_view *view)
{
	uint8_t atomic_storage[16];
	const uint8_t *result;
	size_t consumed = 0;
	int rc;

	if (!out->track->total_len && !view->has_data_response &&
	    !view->modified_len) {
		uet_complete_out_locked(qp, out, UIB_WC_SUCCESS);
		return 0;
	}
	if (!view->has_data_response ||
	    view->modified_len != out->track->total_len ||
	    view->message_offset > view->modified_len ||
	    view->data_len > view->modified_len - view->message_offset) {
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
		return -EBADMSG;
	}
	rc = uet_track_read_fragment(out->track, view->message_offset,
				     (uint32_t)view->data_len);
	if (rc == 1)
		return 0;
	if (rc < 0) {
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
		return rc;
	}
	rc = uet_atomic_response_data(out->track, view, atomic_storage,
				      &result);
	if (rc < 0) {
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
		return rc;
	}
	rc = uib_qp_scatter(qp->pd, out->track->read_sgl,
			    out->track->read_num_sge, view->message_offset,
			    result, view->data_len, &consumed);
	if (rc < 0 || consumed != view->data_len) {
		uet_remove_track_locked(qp, out->track, UIB_WC_LOC_PROT_ERR);
		return rc < 0 ? rc : -EMSGSIZE;
	}
	if (out->track->read_received == out->track->total_len)
		uet_complete_out_locked(qp, out, UIB_WC_SUCCESS);
	return 0;
}

static enum uib_wc_status uet_response_wc_status(uint8_t response_code)
{
	switch (response_code) {
	case UIB_UET_SES_RC_AT_INVALID:
	case UIB_UET_SES_RC_AT_PERM:
	case UIB_UET_SES_RC_AT_ATS_ERROR:
	case UIB_UET_SES_RC_AT_NO_TRANS:
	case UIB_UET_SES_RC_AT_OUT_OF_RANGE:
	case UIB_UET_SES_RC_PERM_VIOLATION:
	case UIB_UET_SES_RC_OP_VIOLATION:
	case UIB_UET_SES_RC_BAD_INDEX:
	case UIB_UET_SES_RC_BAD_PID:
	case UIB_UET_SES_RC_BAD_JOB_ID:
	case UIB_UET_SES_RC_BAD_MKEY:
	case UIB_UET_SES_RC_BAD_ADDR:
		return UIB_WC_REM_ACCESS_ERR;
	default:
		return UIB_WC_REM_OP_ERR;
	}
}

static void uet_request_ack(struct uib_uet_outstanding *out)
{
	struct uib_uet_prologue *prologue;
	uint16_t value;

	prologue = (struct uib_uet_prologue *)out->packet;
	value = ntohs(prologue->type_next_flags);
	if (value & UIB_UET_REQ_FLAG_AR)
		return;
	prologue->type_next_flags = htons(value | UIB_UET_REQ_FLAG_AR);
}

static void uet_mark_retransmit(void *packet)
{
	struct uib_uet_prologue *prologue;
	uint16_t value;

	prologue = packet;
	value = ntohs(prologue->type_next_flags);
	value |= UIB_UET_REQ_FLAG_RETX | UIB_UET_REQ_FLAG_AR;
	prologue->type_next_flags = htons(value);
}

UIB_UET_INTERNAL int uet_handle_ack(struct uib_qp *qp, const void *packet, size_t len,
			  uint64_t rx_time_ns)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;
	struct uet_ack_view view;
	enum uib_wc_status status;
	int clear_required;
	int rc;

	rc = uet_parse_ack(packet, len, &view);
	if (rc < 0)
		return rc;
	uet_debug(qp, "rx ack psn=%u cack=%u msg=%u rc=%u", view.ack_psn,
		  view.cack, view.msg_id, view.response_code);
	pthread_mutex_lock(&qp->lock);
	uet->dpdcid = ntohs(view.ack->spdcid);
	uet->state = UIB_UET_PDC_ESTABLISHED;
	out = uet_find_outstanding(uet, view.ack_psn, view.msg_id);
	uet_ack_path_feedback(uet, &view, out);
	uet_ack_congestion_feedback(uet, &view, out, rx_time_ns);
	clear_required = !!(uib_uet_prologue_flags(&view.ack->prologue) &
			    UIB_UET_ACK_FLAG_REQ_CLEAR);
	if (clear_required &&
	    uet_psn_before(uet->tx_clear_psn, view.ack_psn))
		uet->tx_clear_psn = view.ack_psn;
	if (view.response_code != UIB_UET_SES_RC_OK) {
		if (out) {
			out->track->response_code = view.response_code;
			status = uet_response_wc_status(view.response_code);
			uet_remove_track_locked(qp, out->track, status);
		}
		pthread_mutex_unlock(&qp->lock);
		uet_sequence_notify(qp);
		if (clear_required)
			uet_send_control(qp, &qp->remote_addr,
					 UIB_UET_CONTROL_CLEAR_COMMAND,
					 htonl(view.ack_psn));
		return out ? -EREMOTEIO : 0;
	}
	uet_complete_cumulative_locked(qp, view.cack);
	if (view.extension)
		uet_apply_sack_locked(qp, &view);
	out = uet_find_outstanding(uet, view.ack_psn, view.msg_id);
	if (!out) {
		rc = 0;
	} else if (uet_out_requires_response(out)) {
		rc = uet_apply_read_response_locked(qp, out, &view);
	} else {
		uet_complete_out_locked(qp, out, UIB_WC_SUCCESS);
		rc = 0;
	}
	pthread_mutex_unlock(&qp->lock);
	uet_sequence_notify(qp);
	if (clear_required)
		uet_send_control(qp, &qp->remote_addr,
				 UIB_UET_CONTROL_CLEAR_COMMAND,
				 htonl(view.ack_psn));
	return rc;
}

UIB_UET_INTERNAL int uet_handle_nack(struct uib_qp *qp, const void *packet, size_t len)
{
	const struct uib_uet_pds_nack *nack = packet;
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;
	struct uib_congestion_nack event;
	uint64_t now_ns;
	int found;
	int pending;
	int trimmed;
	int requeued;

	if (len < sizeof(*nack))
		return -EBADMSG;
	trimmed = nack->nack_code == UIB_UET_NACK_TRIMMED ||
		nack->nack_code == UIB_UET_NACK_TRIMMED_LASTHOP;
	now_ns = uib_now_ns();
	requeued = 0;
	pthread_mutex_lock(&qp->lock);
	out = uet_find_outstanding(uet, ntohl(nack->nack_psn), 0);
	found = !!out;
	if (out) {
		pending = __atomic_load_n(&out->tx_pending, __ATOMIC_ACQUIRE);
		if (trimmed && ((!out->sent && !pending) ||
		    __atomic_load_n(&out->nack_pending, __ATOMIC_ACQUIRE))) {
			requeued = 1;
			goto unlock;
		}
		memset(&event, 0, sizeof(event));
		event.bytes = out->nominal_len;
		event.now_ns = now_ns;
		event.trimmed = trimmed;
		event.last_hop = nack->nack_code ==
			UIB_UET_NACK_TRIMMED_LASTHOP;
		uib_congestion_on_nack(&uet->congestion, &event);
		if (trimmed) {
			uet_mark_retransmit(out->packet);
			out->retries++;
			if (pending) {
				__atomic_store_n(&out->nack_pending, 1,
						 __ATOMIC_RELEASE);
			} else if (out->sent) {
				out->sent = 0;
				uet->sent_count--;
			}
			uet->tx_cursor = uet_first_unsent(uet);
			uib_qp_retry_schedule(qp, now_ns);
			requeued = 1;
		} else {
			uet_remove_track_locked(qp, out->track,
						UIB_WC_REM_OP_ERR);
		}
	}
unlock:
	pthread_mutex_unlock(&qp->lock);
	if (found)
		uet_sequence_notify(qp);
	if (requeued)
		return 0;
	return found ? -EREMOTEIO : 0;
}

UIB_UET_INTERNAL int uet_trim_code(const struct uib_uet_qp *uet,
			 const struct uib_rx_meta *meta)
{
	if (!uet->trimming_enabled || !meta)
		return 0;
	if (meta->dscp == uet->trimmed_dscp)
		return UIB_UET_NACK_TRIMMED;
	if (meta->dscp == uet->trimmed_last_hop_dscp)
		return UIB_UET_NACK_TRIMMED_LASTHOP;
	return 0;
}

UIB_UET_INTERNAL int uet_handle_trimmed(struct uib_qp *qp,
			      const struct sockaddr_in *source,
			      const void *packet, size_t len, uint8_t type,
			      uint8_t nack_code)
{
	const struct uib_uet_pds_req *request;
	const struct uib_uet_pds_ack *ack;
	struct uib_uet_pds_nack nack;
	uint32_t ack_psn;
	int32_t offset;

	switch (type) {
	case UIB_UET_PDS_TYPE_RUD_REQ:
	case UIB_UET_PDS_TYPE_RUD_CC_REQ:
		if (len < sizeof(*request))
			return -EBADMSG;
		request = packet;
		return uet_send_nack(qp, source, request, nack_code);
	case UIB_UET_PDS_TYPE_ACK:
	case UIB_UET_PDS_TYPE_ACK_CC:
		if (len < sizeof(*ack))
			return -EBADMSG;
		ack = packet;
		offset = (int16_t)ntohs(ack->ack_psn_offset);
		ack_psn = ntohl(ack->cack_psn) + offset;
		uib_uet_build_nack(&nack, sizeof(nack), nack_code, ack_psn,
				   ntohs(ack->dpdcid), ntohs(ack->spdcid));
		return uet_handle_nack(qp, &nack, sizeof(nack));
	default:
		return 0;
	}
}

static int uet_handle_credit_request(struct uib_qp *qp,
				     const struct sockaddr_in *source,
				     const struct uib_uet_pds_control *control)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint32_t cumulative_credit;
	uint32_t credit_target;
	uint32_t payload;

	if (!uet->rccc_enabled)
		return -EOPNOTSUPP;
	uib_uet_rccc_request_unpack(control->payload, NULL, &credit_target);
	pthread_mutex_lock(&qp->lock);
	uet->dpdcid = ntohs(control->spdcid);
	uet->state = UIB_UET_PDC_ESTABLISHED;
	cumulative_credit = uib_uet_rccc_destination_update(
		&uet->rccc_destination, credit_target, uib_now_ns());
	pthread_mutex_unlock(&qp->lock);
	payload = uet_rccc_credit_pack(cumulative_credit);
	return uet_send_control(qp, source, UIB_UET_CONTROL_CREDIT, payload);
}

static int uet_handle_credit(struct uib_qp *qp,
			     const struct uib_uet_pds_control *control)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_rccc *rccc;
	uint32_t cumulative_credit;

	if (!uet->rccc_enabled)
		return -EOPNOTSUPP;
	cumulative_credit = uet_rccc_credit_unpack(control->payload);
	pthread_mutex_lock(&qp->lock);
	rccc = (struct uib_uet_rccc *)uet->congestion.state.bytes;
	uib_uet_rccc_on_credit(rccc, cumulative_credit);
	uet->credit_request_deadline_ns = 0;
	pthread_mutex_unlock(&qp->lock);
	uet_sequence_notify(qp);
	return 0;
}

UIB_UET_INTERNAL int uet_handle_control(struct uib_qp *qp,
			      const struct sockaddr_in *source,
			      const void *packet, size_t len)
{
	const struct uib_uet_pds_control *control = packet;
	uint8_t control_type;

	if (len < sizeof(*control))
		return -EBADMSG;
	if (control->psn)
		return -EBADMSG;
	control_type = uib_uet_prologue_next(&control->prologue);
	switch (control_type) {
	case UIB_UET_CONTROL_CREDIT_REQUEST:
		return uet_handle_credit_request(qp, source, control);
	case UIB_UET_CONTROL_CREDIT:
		return uet_handle_credit(qp, control);
	case UIB_UET_CONTROL_CLEAR_COMMAND:
		pthread_mutex_lock(&qp->lock);
		uet_clear_guaranteed(qp->protocol_private,
				     ntohl(control->payload));
		pthread_mutex_unlock(&qp->lock);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

UIB_UET_INTERNAL void uet_retransmit_tick(struct uib_qp *qp, uint64_t now_ns)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;
	uint64_t next_deadline = UINT64_MAX;
	uint64_t packet_deadline;
	uint32_t sent_remaining;

	pthread_mutex_lock(&qp->lock);
	out = uet->out_head;
	sent_remaining = uet->sent_count;
	while (out && sent_remaining) {
		if (!out->sent) {
			out = out->next;
			continue;
		}
		sent_remaining--;
		if (__atomic_load_n(&out->tx_pending, __ATOMIC_ACQUIRE)) {
			out = out->next;
			continue;
		}
		if (__atomic_exchange_n(&out->nack_pending, 0,
					__ATOMIC_ACQ_REL)) {
			uet->sent_count--;
			out->sent = 0;
			out = out->next;
			continue;
		}
		packet_deadline = __atomic_load_n(&out->deadline_ns,
						  __ATOMIC_RELAXED);
		if ((int64_t)(now_ns - packet_deadline) < 0) {
			out = out->next;
			continue;
		}
		if (out->rto_retries >= UIB_UET_MAX_RETRIES) {
			struct uib_uet_wr_track *track = out->track;

			uet_remove_track_locked(qp, track, UIB_WC_RETRY_EXC_ERR);
			out = uet->out_head;
			sent_remaining = uet->sent_count;
			continue;
		}
		uet_mark_retransmit(out->packet);
		uet_debug(qp, "retransmit psn=%u msg=%u rto-attempt=%u",
			  out->psn, out->msg_id, out->rto_retries + 1);
		uib_congestion_on_loss(&uet->congestion, out->nominal_len);
		if (out->entropy) {
			uet->paths[out->path_index].timeouts++;
			if (uet->multipath.mode ==
			    UIB_UET_MULTIPATH_PATH_AWARE)
				uet->paths[out->path_index].blocked_until_ns =
					now_ns + uet->multipath.failure_hold_ns;
			uet->multipath_stats.timeout_failovers++;
			out->entropy = 0;
		}
		uet->sent_count--;
		out->sent = 0;
		out->retries++;
		out->rto_retries++;
		out = out->next;
	}
	uet->tx_cursor = uet_first_unsent(uet);
	if (uet->rccc_enabled && uet->tx_cursor &&
	    uet->credit_request_deadline_ns &&
	    uet->credit_request_deadline_ns < next_deadline)
		next_deadline = uet->credit_request_deadline_ns;
	sent_remaining = uet->sent_count;
	for (out = uet->out_head; out && sent_remaining;
	     out = out->next) {
		if (!out->sent)
			continue;
		sent_remaining--;
		if (__atomic_load_n(&out->tx_pending, __ATOMIC_ACQUIRE)) {
			packet_deadline = qp->ctx->retry_timeout_ns >
				UINT64_MAX - now_ns ? UINT64_MAX :
				now_ns + qp->ctx->retry_timeout_ns;
		} else {
			packet_deadline = __atomic_load_n(&out->deadline_ns,
							  __ATOMIC_RELAXED);
		}
		if (packet_deadline < next_deadline)
			next_deadline = packet_deadline;
	}
	if (next_deadline != UINT64_MAX)
		uib_qp_retry_schedule(qp, next_deadline);
	else
		uib_qp_retry_cancel(qp);
	pthread_mutex_unlock(&qp->lock);
	uet_sequence_notify(qp);
}
