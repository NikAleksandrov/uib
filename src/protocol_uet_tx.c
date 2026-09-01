/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol_uet_internal.h"

static int uet_schedule_refill_locked(struct uib_qp *qp);

static size_t uet_out_pool_capacity(uint8_t pool_class)
{
	static const size_t capacities[UIB_UET_OUT_POOL_CLASSES] = {
		256,
		2048,
		8192,
		UIB_UET_PACKET_MAX,
	};

	return capacities[pool_class];
}

static uint8_t uet_out_pool_class(size_t packet_len)
{
	uint8_t pool_class;

	for (pool_class = 0; pool_class < UIB_UET_OUT_POOL_CLASSES;
	     pool_class++) {
		if (packet_len <= uet_out_pool_capacity(pool_class))
			return pool_class;
	}
	return UIB_UET_OUT_POOL_CLASSES;
}

static void uet_out_pool_put(struct uib_uet_out_pool *pool)
{
	if (!__atomic_sub_fetch(&pool->refs, 1, __ATOMIC_ACQ_REL)) {
		pthread_mutex_destroy(&pool->lock);
		free(pool);
	}
}

UIB_UET_INTERNAL void uet_out_pool_push(struct uib_uet_outstanding *out)
{
	struct uib_uet_out_pool *pool = out->pool;
	uint8_t pool_class = out->pool_class;

	pthread_mutex_lock(&pool->lock);
	if (!pool->closing &&
	    pool->cached[pool_class] < UIB_UET_OUT_POOL_LIMIT) {
		out->next = pool->free[pool_class];
		pool->free[pool_class] = out;
		pool->cached[pool_class]++;
		pthread_mutex_unlock(&pool->lock);
		return;
	}
	pthread_mutex_unlock(&pool->lock);
	free(out);
	uet_out_pool_put(pool);
}

UIB_UET_INTERNAL struct uib_uet_out_pool *uet_out_pool_create(void)
{
	struct uib_uet_out_pool *pool;

	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return NULL;
	if (pthread_mutex_init(&pool->lock, NULL)) {
		free(pool);
		return NULL;
	}
	pool->refs = 1;
	return pool;
}

static void uet_out_pool_prewarm(struct uib_uet_out_pool *pool,
				 uint8_t pool_class, uint32_t target)
{
	struct uib_uet_outstanding *out;
	uint32_t cached;

	if (pool_class >= UIB_UET_OUT_POOL_CLASSES)
		return;
	for (;;) {
		pthread_mutex_lock(&pool->lock);
		cached = pool->cached[pool_class];
		pthread_mutex_unlock(&pool->lock);
		if (cached >= target)
			return;
		out = malloc(sizeof(*out) +
			     uet_out_pool_capacity(pool_class));
		if (!out)
			return;
		__atomic_add_fetch(&pool->refs, 1, __ATOMIC_RELAXED);
		out->pool = pool;
		out->pool_class = pool_class;
		uet_out_pool_push(out);
	}
}

UIB_UET_INTERNAL void uet_out_pool_prewarm_mtu(struct uib_uet_out_pool *pool,
				     uint32_t path_mtu)
{
	uint8_t header_class;
	uint8_t packet_class;

	header_class = uet_out_pool_class(256);
	packet_class = uet_out_pool_class(path_mtu);
	uet_out_pool_prewarm(pool, header_class, UIB_UET_PDC_MPR);
	if (packet_class != header_class)
		uet_out_pool_prewarm(pool, packet_class, UIB_UET_PDC_MPR);
}

UIB_UET_INTERNAL void uet_out_pool_prewarm_gather(struct uib_uet_out_pool *pool)
{
	uint8_t pool_class;

	for (pool_class = 0; pool_class < UIB_UET_OUT_POOL_CLASSES;
	     pool_class++)
		uet_out_pool_prewarm(pool, pool_class,
				     UIB_UET_GATHER_POOL_WARM);
}

UIB_UET_INTERNAL void uet_out_pool_destroy(struct uib_uet_out_pool *pool)
{
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;
	uint8_t pool_class;

	if (!pool)
		return;
	pthread_mutex_lock(&pool->lock);
	pool->closing = 1;
	for (pool_class = 0; pool_class < UIB_UET_OUT_POOL_CLASSES;
	     pool_class++) {
		out = pool->free[pool_class];
		pool->free[pool_class] = NULL;
		pool->cached[pool_class] = 0;
		while (out) {
			next = out->next;
			free(out);
			uet_out_pool_put(pool);
			out = next;
		}
	}
	pthread_mutex_unlock(&pool->lock);
	uet_out_pool_put(pool);
}

UIB_UET_INTERNAL struct uib_uet_outstanding *
uet_alloc_outstanding(struct uib_uet_qp *uet, size_t packet_len)
{
	struct uib_uet_out_pool *pool = uet->out_pool;
	struct uib_uet_outstanding *out;
	uint8_t pool_class;

	pool_class = uet_out_pool_class(packet_len);
	if (pool_class == UIB_UET_OUT_POOL_CLASSES)
		return NULL;
	pthread_mutex_lock(&pool->lock);
	out = pool->free[pool_class];
	if (out) {
		pool->free[pool_class] = out->next;
		pool->cached[pool_class]--;
	}
	pthread_mutex_unlock(&pool->lock);
	if (!out) {
		out = malloc(sizeof(*out) +
			     uet_out_pool_capacity(pool_class));
		if (!out)
			return NULL;
		__atomic_add_fetch(&pool->refs, 1, __ATOMIC_RELAXED);
		out->pool = pool;
	}
	out->next = NULL;
	out->prev = NULL;
	out->ack_next = NULL;
	out->ack_prev = NULL;
	out->track = NULL;
	out->pool = pool;
	out->payload_mr = NULL;
	out->pool_class = pool_class;
	out->packet = out->storage;
	out->payload = NULL;
	out->packet_len = packet_len;
	out->header_len = packet_len;
	out->payload_len = 0;
	out->deadline_ns = 0;
	out->tx_time_ns = 0;
	out->rto_ns = 0;
	out->psn = 0;
	out->nominal_len = 0;
	out->read_offset = 0;
	out->read_len = 0;
	out->msg_id = 0;
	out->request_header = 0;
	out->path_index = 0;
	out->entropy = 0;
	out->retries = 0;
	out->rto_retries = 0;
	out->sent = 0;
	out->tx_pending = 0;
	out->sack_acked = 0;
	out->indexed = 0;
	out->refs = 1;
	return out;
}

static void uet_track_pool_put(struct uib_uet_track_pool *pool)
{
	if (!__atomic_sub_fetch(&pool->refs, 1, __ATOMIC_ACQ_REL)) {
		pthread_mutex_destroy(&pool->lock);
		free(pool);
	}
}

static struct uib_uet_wr_track *
uet_track_pool_new(struct uib_uet_track_pool *pool)
{
	struct uib_uet_wr_track *track;
	size_t size;

	size = sizeof(*track) + pool->max_sge * sizeof(*track->inline_read_sgl);
	track = malloc(size);
	if (!track)
		return NULL;
	__atomic_add_fetch(&pool->refs, 1, __ATOMIC_RELAXED);
	track->pool = pool;
	return track;
}

UIB_UET_INTERNAL void uet_track_pool_push(struct uib_uet_wr_track *track)
{
	struct uib_uet_track_pool *pool;

	pool = track->pool;
	pthread_mutex_lock(&pool->lock);
	if (!pool->closing && pool->cached < pool->limit) {
		track->next = pool->free;
		pool->free = track;
		pool->cached++;
		pthread_mutex_unlock(&pool->lock);
		return;
	}
	pthread_mutex_unlock(&pool->lock);
	free(track);
	uet_track_pool_put(pool);
}

static struct uib_uet_wr_track *
uet_track_pool_pop(struct uib_uet_track_pool *pool)
{
	struct uib_uet_wr_track *track;

	pthread_mutex_lock(&pool->lock);
	track = pool->free;
	if (track) {
		pool->free = track->next;
		pool->cached--;
	}
	pthread_mutex_unlock(&pool->lock);
	if (!track)
		track = uet_track_pool_new(pool);
	if (!track)
		return NULL;
	memset(track, 0, sizeof(*track));
	track->pool = pool;
	track->refs = 1;
	return track;
}

UIB_UET_INTERNAL struct uib_uet_track_pool *
uet_track_pool_create(uint32_t max_send_wr, uint32_t max_send_sge)
{
	struct uib_uet_track_pool *pool;
	struct uib_uet_wr_track *track;
	uint32_t i;

	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return NULL;
	if (pthread_mutex_init(&pool->lock, NULL)) {
		free(pool);
		return NULL;
	}
	pool->limit = max_send_wr < UIB_UET_TRACK_POOL_MAX ? max_send_wr :
		UIB_UET_TRACK_POOL_MAX;
	if (!pool->limit)
		pool->limit = 64;
	pool->max_sge = max_send_sge ? max_send_sge : 1;
	pool->refs = 1;
	for (i = 0; i < pool->limit; i++) {
		track = uet_track_pool_new(pool);
		if (!track)
			break;
		track->next = pool->free;
		pool->free = track;
		pool->cached++;
	}
	return pool;
}

UIB_UET_INTERNAL void uet_track_pool_destroy(struct uib_uet_track_pool *pool)
{
	struct uib_uet_wr_track *next;
	struct uib_uet_wr_track *track;

	if (!pool)
		return;
	pthread_mutex_lock(&pool->lock);
	pool->closing = 1;
	track = pool->free;
	pool->free = NULL;
	pool->cached = 0;
	while (track) {
		next = track->next;
		free(track);
		uet_track_pool_put(pool);
		track = next;
	}
	pthread_mutex_unlock(&pool->lock);
	uet_track_pool_put(pool);
}

static struct uib_uet_wr_track *uet_alloc_track(struct uib_qp *qp,
						struct uib_send_wr *wr,
						uint32_t total_len,
						uint32_t packets)
{
	struct uib_uet_qp *uet;
	struct uib_uet_wr_track *track;

	uet = qp->protocol_private;
	track = uet_track_pool_pop(uet->track_pool);
	if (!track)
		return NULL;
	track->read_fragments = track->inline_read_fragments;
	track->read_fragment_capacity = UIB_UET_READ_FRAGMENTS_INLINE;
	track->wr_id = wr->wr_id;
	track->opcode = wr->opcode;
	track->signaled = !!(wr->send_flags & UIB_SEND_SIGNALED);
	track->total_len = total_len;
	track->pending = packets;
	if (wr->opcode == UIB_WR_RDMA_READ ||
	    wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
	    (wr->opcode == UIB_WR_UET_ATOMIC &&
	     wr->atomic.uet_fetching)) {
		track->read_sgl = track->inline_read_sgl;
		memcpy(track->read_sgl, wr->sg_list,
		       wr->num_sge * sizeof(*track->read_sgl));
		track->read_num_sge = wr->num_sge;
	}
	if (wr->opcode == UIB_WR_UET_ATOMIC) {
		track->uet_atomic = 1;
		track->atomic_datatype = wr->atomic.uet_datatype;
	}
	return track;
}

struct uet_post_context {
	struct uib_qp *qp;
	struct uib_uet_qp *uet;
	struct uib_uet_wr_track *track;
	struct sockaddr_in destination;
	const uint8_t *message;
	uint8_t *owned_message;
	struct uib_uet_outstanding *owned_message_buffer;
	struct uib_mr *mr;
	uint32_t total_len;
	uint32_t chunk_max;
	uint32_t packets;
	uint32_t first_psn;
	uint32_t clear_psn;
	uint16_t msg_id;
	uint8_t ses_opcode;
	uint8_t request_header;
	uint8_t fetching_atomic;
	uint8_t send_syn;
	size_t pds_len;
};

static void uet_post_context_release(struct uet_post_context *context)
{
	uet_free_track(context->track);
	uib_mr_put(context->mr);
	if (context->owned_message_buffer)
		uet_free_outstanding(context->owned_message_buffer);
	else
		free(context->owned_message);
}

static int uet_ses_opcode(const struct uib_send_wr *wr, uint8_t *opcode)
{
	switch (wr->opcode) {
	case UIB_WR_SEND:
	case UIB_WR_SEND_WITH_IMM:
		*opcode = UIB_UET_SES_SEND;
		return 0;
	case UIB_WR_RDMA_WRITE:
	case UIB_WR_RDMA_WRITE_WITH_IMM:
		*opcode = UIB_UET_SES_WRITE;
		return 0;
	case UIB_WR_RDMA_READ:
		*opcode = UIB_UET_SES_READ;
		return 0;
	case UIB_WR_ATOMIC_FETCH_ADD:
	case UIB_WR_ATOMIC_CMP_SWAP:
		*opcode = UIB_UET_SES_FETCHING_ATOMIC;
		return 0;
	case UIB_WR_UET_ATOMIC:
		*opcode = wr->atomic.uet_fetching ?
			UIB_UET_SES_FETCHING_ATOMIC : UIB_UET_SES_ATOMIC;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int uet_post_total(const struct uib_send_wr *wr, uint32_t *total)
{
	int i;

	*total = 0;
	for (i = 0; i < wr->num_sge; i++) {
		if (UINT32_MAX - *total < wr->sg_list[i].length)
			return -EMSGSIZE;
		*total += wr->sg_list[i].length;
	}
	return 0;
}

static int uet_prepare_message(struct uib_qp *qp, struct uib_send_wr *wr,
			       struct uet_post_context *context)
{
	int rc;

	if (wr->opcode == UIB_WR_RDMA_READ || context->fetching_atomic)
		return 0;
	if (wr->num_sge == 1 && !(wr->send_flags & UIB_SEND_INLINE)) {
		context->mr = uib_mr_lookup_local(
			qp->pd, wr->sg_list[0].lkey,
			(const void *)(uintptr_t)wr->sg_list[0].addr,
			wr->sg_list[0].length, 0);
		if (!context->mr)
			return -EACCES;
		if (context->mr->kind == UIB_MR_HOST) {
			context->message = (const uint8_t *)(uintptr_t)
				wr->sg_list[0].addr;
			return 0;
		}
		uib_mr_put(context->mr);
		context->mr = NULL;
	}
	context->owned_message_buffer = uet_alloc_outstanding(
		context->uet, context->total_len ? context->total_len : 1);
	if (context->owned_message_buffer)
		context->owned_message = context->owned_message_buffer->packet;
	else
		context->owned_message = malloc(context->total_len ?
							context->total_len : 1);
	if (!context->owned_message)
		return -ENOMEM;
	rc = uib_qp_gather(qp->pd, wr->sg_list, wr->num_sge,
			   context->owned_message, context->total_len);
	if (rc < 0)
		return rc;
	context->message = context->owned_message;
	return 0;
}

static size_t uet_request_header_len(uint8_t request_header)
{
	if (request_header == UIB_UET_NEXT_REQ_SMALL)
		return sizeof(struct uib_uet_ses_req_small);
	if (request_header == UIB_UET_NEXT_REQ_MEDIUM)
		return sizeof(struct uib_uet_ses_req_medium);
	return sizeof(struct uib_uet_ses_req);
}

static size_t uet_request_data_len(const struct uib_send_wr *wr,
				   uint32_t chunk)
{
	size_t width;

	if (wr->opcode == UIB_WR_RDMA_READ)
		return 0;
	if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD)
		return sizeof(struct uib_uet_ses_atomic) + 8;
	if (wr->opcode == UIB_WR_ATOMIC_CMP_SWAP)
		return sizeof(struct uib_uet_ses_atomic) + 32;
	if (wr->opcode != UIB_WR_UET_ATOMIC)
		return chunk;
	width = uib_uet_atomic_datatype_size(wr->atomic.uet_datatype);
	if (uet_atomic_has_two_operands(wr->atomic.uet_opcode))
		width *= 2;
	else if (wr->atomic.uet_opcode == UIB_UET_AMO_READ)
		width = 0;
	return sizeof(struct uib_uet_ses_atomic) + width;
}

static int uet_prepare_post(struct uib_qp *qp, struct uib_send_wr *wr,
			    struct uet_post_context *context)
{
	uint32_t response_limit;
	int rc;

	memset(context, 0, sizeof(*context));
	context->qp = qp;
	context->uet = qp->protocol_private;
	if (wr->num_sge < 0 || (uint32_t)wr->num_sge > qp->max_send_sge ||
	    (wr->num_sge && !wr->sg_list))
		return -EINVAL;
	context->destination = qp->remote_addr;
	context->request_header = UIB_UET_NEXT_REQ_STD;
	context->fetching_atomic = wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
		(wr->opcode == UIB_WR_UET_ATOMIC && wr->atomic.uet_fetching);
	context->pds_len = context->uet->rccc_enabled ?
		sizeof(struct uib_uet_pds_cc_req) :
		sizeof(struct uib_uet_pds_req);
	rc = uet_post_total(wr, &context->total_len);
	if (rc < 0)
		return rc;
	rc = uet_ses_opcode(wr, &context->ses_opcode);
	if (rc < 0)
		return rc;
	rc = uet_prepare_message(qp, wr, context);
	if (rc < 0)
		return rc;
	if (qp->path_mtu <= context->pds_len +
	    sizeof(struct uib_uet_ses_req))
		return -EMSGSIZE;
	context->chunk_max = qp->path_mtu - context->pds_len -
		sizeof(struct uib_uet_ses_req);
	response_limit = qp->path_mtu - sizeof(struct uib_uet_pds_ack) -
		sizeof(struct uib_uet_ses_rsp);
	if (wr->opcode == UIB_WR_RDMA_READ &&
	    context->chunk_max > response_limit)
		context->chunk_max = response_limit;
	context->packets = wr->opcode == UIB_WR_RDMA_READ ||
		context->fetching_atomic ? 1 :
		(context->total_len ?
		 (context->total_len + context->chunk_max - 1) /
		 context->chunk_max : 1);
	if (wr->opcode != UIB_WR_RDMA_WRITE_WITH_IMM &&
	    context->packets == 1 &&
	    context->total_len <= UIB_UET_SES_LENGTH_MASK &&
	    context->uet->optimized_mode != UIB_UET_OPTIMIZED_OFF) {
		context->request_header =
			context->uet->optimized_mode == UIB_UET_OPTIMIZED_SMALL &&
			wr->opcode != UIB_WR_SEND &&
			wr->opcode != UIB_WR_SEND_WITH_IMM ?
			UIB_UET_NEXT_REQ_SMALL : UIB_UET_NEXT_REQ_MEDIUM;
		if (wr->opcode == UIB_WR_RDMA_READ &&
		    context->total_len + sizeof(struct uib_uet_pds_ack) +
		    sizeof(struct uib_uet_pds_ack_ext) +
		    sizeof(struct uib_uet_ses_rsp_data_small) > qp->path_mtu)
			context->request_header = UIB_UET_NEXT_REQ_STD;
	}
	context->track = uet_alloc_track(qp, wr, context->total_len,
					 context->packets);
	if (!context->track)
		return -ENOMEM;
	if (!context->destination.sin_port)
		context->destination.sin_port = htons(UIB_UET_DEFAULT_PORT);
	return 0;
}

static void uet_reserve_post(struct uib_qp *qp,
			     struct uet_post_context *context)
{
	pthread_mutex_lock(&qp->lock);
	context->msg_id = context->uet->next_msg_id++;
	if (!context->uet->tx_initialized) {
		context->uet->next_psn = qp->sq_psn;
		context->uet->publish_psn = qp->sq_psn;
		context->uet->tx_clear_psn = qp->sq_psn - 1;
		context->uet->tx_initialized = 1;
	}
	context->first_psn = context->uet->next_psn;
	context->uet->next_psn += context->packets;
	qp->sq_psn = context->uet->next_psn;
	context->clear_psn = context->uet->tx_clear_psn;
	context->send_syn = context->uet->state == UIB_UET_PDC_CLOSED;
	if (context->send_syn)
		context->uet->state = UIB_UET_PDC_SYN_SENT;
	pthread_mutex_unlock(&qp->lock);
}

static void uet_store_post(struct uib_send_wr *wr,
			   struct uet_post_context *context)
{
	struct uib_uet_wr_track *track = context->track;

	track->qp = context->qp;
	track->wr = *wr;
	track->wr.next = NULL;
	track->wr.sg_list = NULL;
	track->packets = context->packets;
	track->next_packet = 0;
	track->first_psn = context->first_psn;
	track->clear_psn = context->clear_psn;
	track->chunk_max = context->chunk_max;
	track->destination = context->destination;
	track->message = context->message;
	track->owned_message = context->owned_message;
	track->owned_message_buffer = context->owned_message_buffer;
	track->mr = context->mr;
	track->msg_id = context->msg_id;
	track->ses_opcode = context->ses_opcode;
	track->request_header = context->request_header;
	track->fetching_atomic = context->fetching_atomic;
	track->send_syn = context->send_syn;
	track->pds_len = context->pds_len;
	track->scheduled_packet = 0;
	context->owned_message = NULL;
	context->owned_message_buffer = NULL;
	context->mr = NULL;
}

static void uet_load_post(struct uib_uet_qp *uet,
			  struct uib_uet_wr_track *track,
			  struct uet_post_context *context)
{
	memset(context, 0, sizeof(*context));
	context->qp = track->qp;
	context->uet = uet;
	context->track = track;
	context->destination = track->destination;
	context->message = track->message;
	context->total_len = track->total_len;
	context->chunk_max = track->chunk_max;
	context->packets = track->packets;
	context->first_psn = track->first_psn;
	context->clear_psn = track->clear_psn;
	context->msg_id = track->msg_id;
	context->ses_opcode = track->ses_opcode;
	context->request_header = track->request_header;
	context->fetching_atomic = track->fetching_atomic;
	context->send_syn = track->send_syn;
	context->pds_len = track->pds_len;
}

static void uet_encode_atomic(struct uib_send_wr *wr,
			      struct uib_uet_ses_req *ses,
			      uint8_t *wire_data, size_t data_len)
{
	struct uib_uet_ses_atomic *atomic;
	uint8_t *operands;
	uint64_t value;
	size_t width;
	size_t i;

	atomic = (struct uib_uet_ses_atomic *)wire_data;
	operands = (uint8_t *)(atomic + 1);
	ses->buffer_offset = htobe64(wr->atomic.remote_addr);
	ses->match_bits = htobe64(wr->atomic.rkey);
	ses->request_len = htonl(wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ? 32 : 8);
	atomic->datatype = UIB_UET_AMO_UINT64;
	if (wr->opcode == UIB_WR_UET_ATOMIC) {
		width = uib_uet_atomic_datatype_size(wr->atomic.uet_datatype);
		atomic->opcode = wr->atomic.uet_opcode;
		atomic->datatype = wr->atomic.uet_datatype;
		atomic->semantic_control = wr->atomic.uet_semantic_control;
		if (atomic->opcode != UIB_UET_AMO_READ) {
			for (i = 0; i < width; i++)
				operands[i] = wr->atomic.uet_operand[width - i - 1];
		}
		if (uet_atomic_has_two_operands(atomic->opcode)) {
			for (i = 0; i < width; i++) {
				operands[i] = wr->atomic.uet_compare[width - i - 1];
				operands[width + i] =
					wr->atomic.uet_swap[width - i - 1];
			}
		}
		ses->request_len = htonl((uint32_t)(data_len - sizeof(*atomic)));
	} else if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD) {
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

static void uet_encode_ses_header(uint8_t request_header, void *wire,
				  const struct uib_uet_ses_req *ses)
{
	struct uib_uet_ses_req_medium *medium;
	struct uib_uet_ses_req_small *small;

	if (request_header == UIB_UET_NEXT_REQ_STD) {
		memcpy(wire, ses, sizeof(*ses));
		return;
	}
	if (request_header == UIB_UET_NEXT_REQ_MEDIUM) {
		medium = wire;
		medium->opcode = ses->opcode;
		medium->flags = ses->flags;
		medium->request_len = htons((uint16_t)
			(ntohl(ses->request_len) & UIB_UET_SES_LENGTH_MASK));
		medium->index_gen_job_id = ses->index_gen_job_id;
		medium->pid_on_fep = ses->pid_on_fep;
		medium->index = ses->index;
		medium->buffer_offset = ses->opcode == UIB_UET_SES_SEND ?
			ses->header_data : ses->buffer_offset;
		medium->initiator = ses->initiator;
		medium->match_bits = ses->match_bits;
		return;
	}
	small = wire;
	small->opcode = ses->opcode;
	small->flags = ses->flags;
	small->request_len = htons((uint16_t)
		(ntohl(ses->request_len) & UIB_UET_SES_LENGTH_MASK));
	small->index_gen_job_id = ses->index_gen_job_id;
	small->pid_on_fep = ses->pid_on_fep;
	small->index = ses->index;
	small->buffer_offset = ses->buffer_offset;
}

static void uet_init_ses(struct uib_qp *qp, struct uib_send_wr *wr,
			 const struct uet_post_context *context,
			 struct uib_uet_outstanding *out,
			 struct uib_uet_ses_req *ses, uint32_t packet_index,
			 uint32_t chunk, uint32_t offset, uint8_t *wire_data,
			 size_t data_len)
{
	ses->opcode = context->ses_opcode;
	if (context->uet->session.relative_addressing)
		ses->flags |= UIB_UET_SES_FLAG_REL;
	if (!packet_index)
		ses->flags |= UIB_UET_SES_FLAG_SOM;
	if (packet_index == context->packets - 1)
		ses->flags |= UIB_UET_SES_FLAG_EOM;
	ses->msg_id = htons(out->msg_id);
	ses->index_gen_job_id = htonl(
		((uint32_t)context->uet->session.resource_generation << 24) |
		context->uet->session.job_id);
	ses->pid_on_fep = htons((uint16_t)qp->dest_qp_num &
				 UIB_UET_SES_PID_MASK);
	ses->index = htons(context->uet->session.resource_index);
	ses->initiator = htonl(context->uet->session.initiator_id);
	ses->request_len = htonl(context->total_len);
	if (wr->opcode == UIB_WR_SEND || wr->opcode == UIB_WR_SEND_WITH_IMM) {
		if (!packet_index && wr->opcode == UIB_WR_SEND_WITH_IMM) {
			ses->flags |= UIB_UET_SES_FLAG_HD;
			ses->header_data = htobe64(wr->imm_data);
		}
	} else if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		   wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
		   wr->opcode == UIB_WR_UET_ATOMIC) {
		uet_encode_atomic(wr, ses, wire_data, data_len);
	} else {
		ses->buffer_offset = htobe64(wr->rdma.remote_addr);
		ses->match_bits = htobe64(wr->rdma.rkey);
		if (!packet_index &&
		    wr->opcode == UIB_WR_RDMA_WRITE_WITH_IMM) {
			ses->flags |= UIB_UET_SES_FLAG_HD;
			ses->header_data = htobe64(wr->imm_data);
		}
	}
	if (packet_index)
		ses->header_data = uib_uet_ses_fragment_info(chunk, offset);
}

static void uet_assign_request_identity(struct uet_post_context *context,
					struct uib_uet_outstanding *out,
					uint32_t packet_index,
					uint8_t *flags,
					uint16_t *clear_psn_offset)
{
	int32_t offset;

	if (context->send_syn && !packet_index)
		*flags |= UIB_UET_REQ_FLAG_SYN;
	out->psn = context->first_psn + packet_index;
	out->msg_id = context->request_header == UIB_UET_NEXT_REQ_STD ?
		context->msg_id : (uint16_t)out->psn;
	out->request_header = context->request_header;
	offset = (int32_t)(context->clear_psn - out->psn);
	if (offset < INT16_MIN)
		offset = INT16_MIN;
	if (offset > 0)
		offset = 0;
	*clear_psn_offset = htons((uint16_t)(int16_t)offset);
}

struct uet_request_layout {
	size_t header_len;
	size_t packet_len;
	size_t data_len;
	uint32_t offset;
	uint32_t chunk;
};

static int uet_request_layout(struct uib_qp *qp,
			      const struct uib_send_wr *wr,
			      const struct uet_post_context *context,
			      uint32_t packet_index,
			      struct uet_request_layout *layout)
{
	layout->offset = packet_index * context->chunk_max;
	layout->chunk = wr->opcode == UIB_WR_RDMA_READ ||
		context->fetching_atomic ? context->total_len :
		context->total_len - layout->offset;
	if (wr->opcode != UIB_WR_RDMA_READ && !context->fetching_atomic &&
	    layout->chunk > context->chunk_max)
		layout->chunk = context->chunk_max;
	layout->header_len = uet_request_header_len(context->request_header);
	layout->data_len = uet_request_data_len(wr, layout->chunk);
	layout->packet_len = context->pds_len + layout->header_len +
		layout->data_len;
	return layout->packet_len > qp->path_mtu ? -EMSGSIZE : 0;
}

static struct uib_uet_outstanding *
uet_allocate_request(struct uib_qp *qp, struct uib_send_wr *wr,
		     struct uet_post_context *context, uint32_t packet_index)
{
	struct uet_request_layout layout;
	struct uib_uet_outstanding *out;
	size_t allocation_len;
	int direct_payload;
	int rc;

	rc = uet_request_layout(qp, wr, context, packet_index, &layout);
	if (rc < 0)
		return NULL;
	direct_payload = context->mr && layout.data_len &&
		layout.data_len == layout.chunk;
	allocation_len = direct_payload ? layout.packet_len - layout.data_len :
		layout.packet_len;
	out = uet_alloc_outstanding(context->uet, allocation_len);
	if (!out)
		return NULL;
	out->track = context->track;
	out->dst = context->destination;
	out->packet_len = layout.packet_len;
	out->header_len = allocation_len;
	out->read_offset = layout.offset;
	out->read_len = layout.chunk;
	out->nominal_len = (uint32_t)layout.packet_len + 40;
	if (direct_payload) {
		out->payload = context->message + layout.offset;
		out->payload_len = layout.data_len;
		out->payload_mr = context->mr;
		uib_mr_get(out->payload_mr);
	}
	return out;
}

static void uet_encode_request(struct uib_qp *qp, struct uib_send_wr *wr,
			       struct uet_post_context *context,
			       struct uib_uet_outstanding *out,
			       uint32_t packet_index)
{
	struct uib_uet_ses_req ses = {0};
	struct uib_uet_pds_cc_req *cc_request;
	struct uib_uet_pds_req *request;
	void *wire_header;
	uint8_t *wire_data;
	size_t header_len;
	size_t data_len;
	uint32_t offset;
	uint32_t chunk;
	uint32_t target;
	uint8_t flags = 0;
	uint16_t clear_psn_offset;

	if (!((packet_index + 1) % UIB_UET_ACK_INTERVAL) ||
	    packet_index == context->packets - 1)
		flags |= UIB_UET_REQ_FLAG_AR;
	offset = out->read_offset;
	chunk = out->read_len;
	header_len = uet_request_header_len(context->request_header);
	data_len = out->payload_len ? out->payload_len :
		out->packet_len - context->pds_len - header_len;
	request = (struct uib_uet_pds_req *)out->packet;
	wire_header = out->packet + context->pds_len;
	wire_data = (uint8_t *)wire_header + header_len;
	uet_assign_request_identity(context, out, packet_index, &flags,
				    &clear_psn_offset);
	request->prologue.type_next_flags = uib_uet_prologue_pack(
		context->uet->rccc_enabled ? UIB_UET_PDS_TYPE_RUD_CC_REQ :
		UIB_UET_PDS_TYPE_RUD_REQ, context->request_header, flags);
	request->psn = htonl(out->psn);
	request->clear_psn_offset = clear_psn_offset;
	request->spdcid = htons(context->uet->spdcid);
	request->dpdcid = htons((uint16_t)qp->dest_qp_num);
	if (context->uet->rccc_enabled) {
		cc_request = (struct uib_uet_pds_cc_req *)request;
		target = uib_uet_rccc_credit_target(
			(struct uib_uet_rccc *)context->uet->congestion.state.bytes);
		cc_request->req_cc_state = uib_uet_rccc_request_pack(
			(uint8_t)context->uet->spdcid, target);
	}
	uet_init_ses(qp, wr, context, out, &ses, packet_index, chunk, offset,
		     wire_data, data_len);
	if (data_len && !out->payload_len &&
	    wr->opcode != UIB_WR_ATOMIC_FETCH_ADD &&
	    wr->opcode != UIB_WR_ATOMIC_CMP_SWAP &&
	    wr->opcode != UIB_WR_UET_ATOMIC)
		memcpy(wire_data, context->message + offset, data_len);
	uet_encode_ses_header(context->request_header, wire_header, &ses);
	uet_debug(qp, "tx request psn=%u msg=%u opcode=%u len=%zu",
		  out->psn, out->msg_id, context->ses_opcode, data_len);
}

static int uet_materialize_locked(struct uib_qp *qp,
				  struct uib_uet_wr_track **failed)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uet_post_context context;
	struct uib_uet_wr_track *track;
	struct uib_uet_outstanding *out;

	*failed = NULL;
	for (track = uet->track_head;
	     track && uet->materialized_count < UIB_UET_PDC_MPR;
	     track = track->next) {
		if (track->scheduled_packet != track->next_packet)
			break;
		uet_load_post(uet, track, &context);
		while (track->next_packet < track->packets &&
		       uet->materialized_count < UIB_UET_PDC_MPR) {
			out = uet_allocate_request(qp, &track->wr, &context,
						   track->next_packet);
			if (!out) {
				*failed = track;
				return -ENOMEM;
			}
			uet_encode_request(qp, &track->wr, &context, out,
					   track->next_packet);
			uet->materialized_count++;
			uet_link_outstanding(uet, out);
			track->next_packet++;
			track->scheduled_packet = track->next_packet;
		}
	}
	return 0;
}

UIB_UET_INTERNAL int uet_refill_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *failed;
	struct uib_uet_rccc *rccc;
	int scheduled;
	int rc;

	scheduled = uet_schedule_refill_locked(qp);
	if (scheduled < 0)
		return scheduled;
	if (scheduled || uet->refill_head)
		return uet_drain_locked(qp);
	rccc = uet->rccc_enabled ?
		(struct uib_uet_rccc *)uet->congestion.state.bytes : NULL;
	do {
		rc = uet_materialize_locked(qp, &failed);
		if (rc >= 0)
			break;
		if (failed->rccc_accounted)
			uib_uet_rccc_on_cancel_data(rccc,
						failed->nominal_bytes);
		uet_remove_track_locked(qp, failed, UIB_WC_LOC_PROT_ERR);
	} while (uet->track_head);
	if (rc < 0)
		return rc;
	return uet_drain_locked(qp);
}

static void uet_commit_prepared_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *track;

	while (uet->prep_head && uet->prep_head->prep_ready &&
	       uet->prep_head->first_psn == uet->publish_psn) {
		track = uet->prep_head;
		uet->prep_head = track->prep_next;
		track->prep_next = NULL;
		uet_link_track(uet, track);
		track->next_packet = 0;
		if (track->request_header == UIB_UET_NEXT_REQ_SMALL)
			uet->optimized_stats.small_requests += track->packets;
		else if (track->request_header == UIB_UET_NEXT_REQ_MEDIUM)
			uet->optimized_stats.medium_requests += track->packets;
		else
			uet->optimized_stats.standard_requests += track->packets;
		uet->publish_psn += track->packets;
		if (uet->prep_pending)
			uet->prep_pending--;
	}
	if (!uet->prep_pending)
		pthread_cond_broadcast(&uet->prep_cond);
}

static int uet_admit_post(struct uib_uet_wr_track *track)
{
	struct uib_uet_wr_track **link;
	struct uib_uet_qp *uet;
	struct uib_qp *qp;

	qp = track->qp;
	uet = qp->protocol_private;
	pthread_mutex_lock(&qp->lock);
	if (uet->prep_closing) {
		pthread_mutex_unlock(&qp->lock);
		return -ESHUTDOWN;
	}
	link = &uet->prep_head;
	while (*link && (int32_t)((*link)->first_psn - track->first_psn) < 0)
		link = &(*link)->prep_next;
	track->prep_next = *link;
	*link = track;
	uet->prep_pending++;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static uint64_t uet_post_nominal_bytes(const struct uib_send_wr *wr,
				       const struct uet_post_context *context)
{
	uint64_t total = 0;
	uint32_t packet_index;
	uint32_t offset;
	uint32_t chunk;

	for (packet_index = 0; packet_index < context->packets; packet_index++) {
		offset = packet_index * context->chunk_max;
		chunk = wr->opcode == UIB_WR_RDMA_READ ||
			context->fetching_atomic ? context->total_len :
			context->total_len - offset;
		if (wr->opcode != UIB_WR_RDMA_READ &&
		    !context->fetching_atomic && chunk > context->chunk_max)
			chunk = context->chunk_max;
		total += context->pds_len +
			uet_request_header_len(context->request_header) +
			uet_request_data_len(wr, chunk) + 40;
	}
	return total;
}

static int uet_process_post(struct uib_uet_wr_track *track)
{
	struct uib_qp *qp;

	qp = track->qp;
	pthread_mutex_lock(&qp->lock);
	track->prep_ready = 1;
	pthread_mutex_unlock(&qp->lock);
	uet_sequence_notify(qp);
	return 0;
}

static void uet_free_task_packets(struct uib_uet_prep_task *task)
{
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;

	for (out = task->result_head; out; out = next) {
		next = out->next;
		uet_free_outstanding(out);
	}
	task->result_head = NULL;
	task->result_tail = NULL;
}

static void uet_commit_refills_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet;
	struct uib_uet_prep_worker *worker;
	struct uib_uet_prep_task *task;
	struct uib_uet_wr_track *track;
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;
	struct uib_uet_rccc *rccc;
	int cancelled;
	int remove;

	uet = qp->protocol_private;
	rccc = uet->rccc_enabled ?
		(struct uib_uet_rccc *)uet->congestion.state.bytes : NULL;
	while (uet->refill_head &&
	       __atomic_load_n(&uet->refill_head->ready, __ATOMIC_ACQUIRE)) {
		task = uet->refill_head;
		uet->refill_head = task->refill_next;
		if (!uet->refill_head)
			uet->refill_tail = NULL;
		track = task->track;
		cancelled = __atomic_load_n(&track->cancelled,
					    __ATOMIC_ACQUIRE);
		if (task->rc < 0 && !track->refill_error)
			track->refill_error = task->rc;
		if (cancelled || track->refill_error) {
			uet_free_task_packets(task);
			uet->materialized_count -= task->packet_count;
		} else {
			for (out = task->result_head; out; out = next) {
				next = out->next;
				uet_link_outstanding(uet, out);
			}
			track->next_packet = task->packet_index +
				task->packet_count;
			task->result_head = NULL;
			task->result_tail = NULL;
		}
		if (track->refill_tasks)
			track->refill_tasks--;
		if (uet->prep_pending)
			uet->prep_pending--;
		remove = track->refill_error && !track->refill_tasks &&
			 !cancelled;
		worker = task->worker;
		memset(task, 0, sizeof(*task));
		task->worker = worker;
		uib_ptr_ring_push(&worker->free_tasks, task);
		if (remove) {
			if (track->rccc_accounted && rccc)
				uib_uet_rccc_on_cancel_data(rccc,
							track->nominal_bytes);
			uet_remove_track_locked(qp, track,
						UIB_WC_LOC_PROT_ERR);
		}
		uet_free_track(track);
	}
	if (!uet->prep_pending)
		pthread_cond_broadcast(&uet->prep_cond);
}

UIB_UET_INTERNAL void uet_sequence_progress(struct uib_qp *qp)
{
	struct uib_uet_qp *uet;
	int pending;

	uet = qp->protocol_private;
	do {
		__atomic_store_n(&uet->sequence_pending, 0, __ATOMIC_RELEASE);
		pthread_mutex_lock(&qp->lock);
		uet_commit_prepared_locked(qp);
		uet_commit_refills_locked(qp);
		uet_refill_locked(qp);
		pthread_mutex_unlock(&qp->lock);
		pending = __atomic_exchange_n(&uet->sequence_pending, 0,
					      __ATOMIC_ACQ_REL);
	} while (pending);
	__atomic_store_n(&uet->sequence_queued, 0, __ATOMIC_RELEASE);
	if (__atomic_load_n(&uet->sequence_pending, __ATOMIC_ACQUIRE))
		uet_sequence_notify(qp);
	pthread_mutex_lock(&qp->lock);
	pthread_cond_broadcast(&uet->prep_cond);
	pthread_mutex_unlock(&qp->lock);
}

static struct uib_uet_prep_task *
uet_reserve_prep_task(struct uib_uet_context *context,
			      struct uib_uet_prep_worker **selected,
			      uint64_t *position, int *was_empty)
{
	struct uib_uet_prep_worker *worker;
	struct uib_uet_prep_task *task;
	unsigned int start;
	unsigned int index;
	unsigned int i;
	int rc;

	start = __atomic_fetch_add(&context->worker_cursor, 1,
				   __ATOMIC_RELAXED);
	for (i = 0; i < context->worker_count; i++) {
		index = (start + i) % context->worker_count;
		worker = &context->workers[index];
		task = uib_ptr_ring_pop(&worker->free_tasks);
		if (!task)
			continue;
		rc = uib_ptr_ring_reserve(&worker->ready, 1, position,
					  was_empty);
		if (!rc) {
			*selected = worker;
			return task;
		}
		uib_ptr_ring_push(&worker->free_tasks, task);
	}
	return NULL;
}

UIB_UET_INTERNAL void uet_process_refill_task(struct uib_uet_prep_task *task)
{
	struct uib_uet_wr_track *track;
	struct uib_uet_outstanding *out;
	struct uet_post_context post;
	struct uib_qp *qp;
	uint32_t i;
	int rc;

	track = task->track;
	qp = track->qp;
	uet_load_post(qp->protocol_private, track, &post);
	rc = 0;
	for (i = 0; i < task->packet_count; i++) {
		if (__atomic_load_n(&track->cancelled, __ATOMIC_ACQUIRE)) {
			rc = -ECANCELED;
			break;
		}
		out = uet_allocate_request(qp, &track->wr, &post,
					   task->packet_index + i);
		if (!out) {
			rc = -ENOMEM;
			break;
		}
		uet_encode_request(qp, &track->wr, &post, out,
				   task->packet_index + i);
		if (task->result_tail)
			task->result_tail->next = out;
		else
			task->result_head = out;
		task->result_tail = out;
	}
	if (rc < 0)
		uet_free_task_packets(task);
	task->rc = rc;
	__atomic_store_n(&task->ready, 1, __ATOMIC_RELEASE);
	uet_sequence_notify(qp);
}

static int uet_schedule_refill_locked(struct uib_qp *qp)
{
	struct uib_uet_context *context;
	struct uib_uet_prep_worker *worker;
	struct uib_uet_prep_task *task;
	struct uib_uet_wr_track *track;
	struct uib_uet_qp *uet;
	uint64_t position;
	uint32_t capacity;
	uint32_t count;
	uint32_t remaining;
	int scheduled;
	int was_empty;

	uet = qp->protocol_private;
	context = qp->ctx->protocol_private;
	capacity = UIB_UET_PDC_MPR - uet->materialized_count;
	scheduled = 0;
	for (track = uet->track_head; track && capacity;
	     track = track->next) {
		while (track->scheduled_packet < track->packets && capacity) {
			task = uet_reserve_prep_task(context, &worker, &position,
						     &was_empty);
			if (!task)
				return scheduled;
			remaining = track->packets - track->scheduled_packet;
			count = remaining < UIB_UET_PREP_CHUNK ? remaining :
				UIB_UET_PREP_CHUNK;
			if (count > capacity)
				count = capacity;
			memset(task, 0, sizeof(*task));
			task->worker = worker;
			task->track = track;
			task->packet_index = track->scheduled_packet;
			task->packet_count = count;
			uet_track_get(track);
			if (uet->refill_tail)
				uet->refill_tail->refill_next = task;
			else
				uet->refill_head = task;
			uet->refill_tail = task;
			track->scheduled_packet += count;
			track->refill_tasks++;
			uet->materialized_count += count;
			uet->prep_pending++;
			capacity -= count;
			scheduled++;
			uib_ptr_ring_publish(&worker->ready, position, task);
			if (was_empty) {
				pthread_mutex_lock(&worker->lock);
				pthread_cond_signal(&worker->cond);
				pthread_mutex_unlock(&worker->lock);
			}
		}
	}
	return scheduled;
}

static int uet_post_one(struct uib_qp *qp, struct uib_send_wr *wr)
{
	struct uet_post_context context;
	struct uib_uet_wr_track *track;
	struct uib_uet_rccc *rccc = NULL;
	uint64_t nominal_bytes = 0;
	int rc;

	rc = uet_prepare_post(qp, wr, &context);
	if (rc < 0)
		goto out;
	uet_reserve_post(qp, &context);
	uet_store_post(wr, &context);
	if (context.uet->rccc_enabled) {
		rccc = (struct uib_uet_rccc *)context.uet->congestion.state.bytes;
		nominal_bytes = uet_post_nominal_bytes(wr, &context);
		context.track->nominal_bytes = nominal_bytes;
		context.track->rccc_accounted = 1;
		pthread_mutex_lock(&qp->lock);
		uib_uet_rccc_on_new_data(rccc, nominal_bytes);
		pthread_mutex_unlock(&qp->lock);
	}
	track = context.track;
	context.track = NULL;
	rc = uet_admit_post(track);
	if (rc < 0) {
		pthread_mutex_lock(&qp->lock);
		if (track->rccc_accounted && rccc)
			uib_uet_rccc_on_cancel_data(rccc,
						track->nominal_bytes);
		pthread_mutex_unlock(&qp->lock);
		uet_free_track(track);
		goto out;
	}
	rc = uet_process_post(track);
out:
	uet_post_context_release(&context);
	return rc;
}

UIB_UET_INTERNAL int uet_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			 struct uib_send_wr **bad)
{
	struct uib_send_wr *current;
	int rc;

	for (current = wr; current; current = current->next) {
		rc = uet_post_one(qp, current);

		if (rc < 0) {
			if (bad)
				*bad = current;
			return rc;
		}
	}
	return 0;
}
