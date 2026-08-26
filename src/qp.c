/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#include <urcu-bp.h>

#include "uib/qp.h"
#include "uib/pd.h"
#include "uib/mr.h"
#include "uib/cq.h"
#include "uib/context.h"
#include "uib/env.h"
#include "uib/transport.h"
#include "uib/bth.h"
#include "uib/uet_nscc.h"
#include "host_zc_internal.h"
#include "ptr_ring.h"
#include "utils.h"

#define UIB_PKT_MAX (UIB_PMTU + 64)
#define UIB_OUT_POOL_MAX 1024
#define UIB_OUT_POOL_CLASSES 3
#define UIB_TX_BATCH_STACK 64

struct uib_outstanding_pool {
	struct uib_ptr_ring free[UIB_OUT_POOL_CLASSES];
	uint32_t            refs;
	int                 closing;
};

static size_t outstanding_pool_capacity(uint8_t pool_class)
{
	static const size_t capacities[UIB_OUT_POOL_CLASSES] = {
		256,
		2048,
		UIB_PKT_MAX,
	};

	return capacities[pool_class];
}

static uint8_t outstanding_pool_class(size_t packet_len)
{
	uint8_t pool_class;

	for (pool_class = 0; pool_class < UIB_OUT_POOL_CLASSES; pool_class++) {
		if (packet_len <= outstanding_pool_capacity(pool_class))
			return pool_class;
	}
	return UIB_OUT_POOL_CLASSES;
}

static void outstanding_pool_put(struct uib_outstanding_pool *pool)
{
	if (!__atomic_sub_fetch(&pool->refs, 1, __ATOMIC_ACQ_REL))
		free(pool);
}

static void outstanding_pool_push(struct uib_outstanding *out)
{
	struct uib_outstanding_pool *pool = out->pool;
	uint8_t pool_class = out->pool_class;
	int cached;

	rcu_read_lock();
	cached = !__atomic_load_n(&pool->closing, __ATOMIC_ACQUIRE) &&
		 !uib_ptr_ring_push(&pool->free[pool_class], out);
	rcu_read_unlock();
	if (cached)
		return;
	free(out);
	outstanding_pool_put(pool);
}

static void outstanding_pool_destroy(struct uib_outstanding_pool *pool)
{
	struct uib_outstanding *out;
	uint8_t pool_class;

	if (!pool)
		return;
	__atomic_store_n(&pool->closing, 1, __ATOMIC_RELEASE);
	synchronize_rcu();
	for (pool_class = 0; pool_class < UIB_OUT_POOL_CLASSES; pool_class++) {
		while ((out = uib_ptr_ring_pop(&pool->free[pool_class])) != NULL) {
			free(out);
			outstanding_pool_put(pool);
		}
		uib_ptr_ring_destroy(&pool->free[pool_class]);
	}
	outstanding_pool_put(pool);
}

static struct uib_outstanding_pool *
outstanding_pool_create(uint32_t max_send_wr)
{
	struct uib_outstanding_pool *pool;
	uint32_t ring_capacity;
	uint8_t pool_class;
	uint32_t limit;
	int rc;

	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return NULL;
	limit = max_send_wr > UIB_OUT_POOL_MAX / 16 ? UIB_OUT_POOL_MAX :
		max_send_wr * 16;
	if (!max_send_wr)
		limit = UIB_OUT_POOL_MAX;
	if (limit < 64)
		limit = 64;
	if (limit > UIB_OUT_POOL_MAX)
		limit = UIB_OUT_POOL_MAX;
	ring_capacity = 64;
	while (ring_capacity < limit)
		ring_capacity <<= 1;
	limit = ring_capacity;
	for (pool_class = 0; pool_class < UIB_OUT_POOL_CLASSES; pool_class++) {
		rc = uib_ptr_ring_init(&pool->free[pool_class], limit);
		if (rc < 0)
			goto error;
	}
	pool->refs = 1;
	return pool;

error:
	while (pool_class)
		uib_ptr_ring_destroy(&pool->free[--pool_class]);
	free(pool);
	return NULL;
}

/**
 * uib_create_qp - Create a queue pair from PD, CQ, and depth attributes.
 * @pd: Protection domain
 * @attr: Queue pair attributes
 * @uib-api: Queue pairs and data transfer
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_qp *uib_create_qp(struct uib_pd *pd,
			     const struct uib_qp_init_attr *attr)
{
	struct uib_qp *qp;
	int rc;

	if (!pd || !attr) {
		errno = EINVAL;
		return NULL;
	}
	qp = calloc(1, sizeof(*qp));
	if (!qp)
		return NULL;

	qp->ctx          = pd->ctx;
	qp->pd           = pd;
	qp->send_cq      = attr->send_cq;
	qp->recv_cq      = attr->recv_cq;
	qp->qp_type      = attr->qp_type;
	qp->state        = UIB_QPS_RESET;
	qp->max_send_sge = attr->max_send_sge ? attr->max_send_sge : 1;
	qp->max_recv_sge = attr->max_recv_sge ? attr->max_recv_sge : 1;
	qp->path_mtu     = UIB_PMTU;
	qp->out_pool     = outstanding_pool_create(attr->max_send_wr);
	if (!qp->out_pool) {
		free(qp);
		return NULL;
	}

	qp->rq_size = attr->max_recv_wr ? (int)attr->max_recv_wr + 1 : 65;
	qp->rq      = calloc(qp->rq_size, sizeof(*qp->rq));
	if (!qp->rq) {
		outstanding_pool_destroy(qp->out_pool);
		free(qp);
		return NULL;
	}

	pthread_mutex_init(&qp->lock, NULL);

	qp->qp_num = uib_register_qp(pd->ctx, qp);
	if (qp->qp_num == 0) {
		outstanding_pool_destroy(qp->out_pool);
		free(qp->rq);
		free(qp);
		errno = ENOSPC;
		return NULL;
	}
	rc = qp->ctx->pops->qp_init ? qp->ctx->pops->qp_init(qp) : 0;
	if (rc < 0) {
		uib_unregister_qp(pd->ctx, qp);
		pthread_mutex_destroy(&qp->lock);
		free(qp->rq);
		outstanding_pool_destroy(qp->out_pool);
		free(qp);
		errno = -rc;
		return NULL;
	}
	pd->refcnt++;
	return qp;
}

static void free_outstanding(void *opaque)
{
	struct uib_outstanding *o = opaque;

	if (!o)
		return;
	if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL))
		return;
	free(o->read_sgl);
	o->read_sgl = NULL;
	outstanding_pool_push(o);
}

static struct uib_outstanding *alloc_outstanding(struct uib_qp *qp,
						 size_t packet_len)
{
	struct uib_outstanding_pool *pool = qp->out_pool;
	struct uib_outstanding *out;
	uint8_t pool_class;

	pool_class = outstanding_pool_class(packet_len);
	if (pool_class == UIB_OUT_POOL_CLASSES)
		return NULL;
	if (__atomic_load_n(&pool->closing, __ATOMIC_ACQUIRE))
		return NULL;
	out = uib_ptr_ring_pop(&pool->free[pool_class]);
	if (!out) {
		out = malloc(sizeof(*out) +
			     outstanding_pool_capacity(pool_class));
		if (!out)
			return NULL;
		__atomic_add_fetch(&pool->refs, 1, __ATOMIC_RELAXED);
		out->pool = pool;
	}
	out->pool_class = pool_class;
	out->next = NULL;
	out->prev = NULL;
	out->retries = 0;
	out->is_read = 0;
	out->is_final = 0;
	out->indexed = 0;
	out->congestion_inflight = 0;
	out->nominal_len = 0;
	out->tx_time_ns = 0;
	out->read_sgl = NULL;
	out->read_num_sge = 0;
	out->read_total_len = 0;
	out->pkt = out->packet;
	out->pkt_len = packet_len;
	out->refs = 1;
	return out;
}

static int send_outstanding(struct uib_qp *qp,
			    struct uib_outstanding *out)
{
	__atomic_add_fetch(&out->refs, 1, __ATOMIC_RELAXED);
	return uib_transport_send_ref(qp->ctx, &out->dst, out->pkt,
				      out->pkt_len, free_outstanding, out);
}

static uint32_t rocev2_congestion_mtu(const struct uib_qp *qp)
{
	return (qp->path_mtu ? qp->path_mtu : UIB_PMTU) + 40;
}

static uint32_t rocev2_nominal_len(const struct uib_qp *qp,
				   size_t packet_len)
{
	uint32_t mtu = rocev2_congestion_mtu(qp);
	uint64_t length = packet_len + 40;

	if (length < mtu)
		length = mtu;
	return length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
}

static int rocev2_congestion_init(struct uib_qp *qp)
{
	struct uib_uet_nscc_config config;
	const void *configuration = NULL;
	const char *cc;
	uint32_t mtu;

	cc = getenv(UIB_ENV_ROCEV2_CC);
	if (!cc)
		cc = "none";
	mtu = rocev2_congestion_mtu(qp);
	if (!strcmp(cc, "nscc")) {
		const char *max_wnd;
		char *end;
		unsigned long long value;

		uib_uet_nscc_config_default(&config, mtu);
		max_wnd = getenv(UIB_ENV_ROCEV2_NSCC_MAX_WND);
		if (max_wnd) {
			errno = 0;
			value = strtoull(max_wnd, &end, 0);
			if (errno || end == max_wnd || *end || value < mtu ||
			    value > UINT32_MAX)
				return -EINVAL;
			config.max_wnd = (uint32_t)value;
			config.bdp = config.max_wnd;
		}
		configuration = &config;
	}
	return uib_congestion_init(&qp->congestion, cc, configuration, mtu,
				   now_ns());
}

int uib_rocev2_qp_init(struct uib_qp *qp)
{
	return rocev2_congestion_init(qp);
}

void uib_rocev2_qp_modified(struct uib_qp *qp, int attr_mask)
{
	if (attr_mask & UIB_QP_PATH_MTU) {
		struct uib_congestion previous = qp->congestion;

		if (rocev2_congestion_init(qp))
			qp->congestion = previous;
	}
}

void uib_rocev2_qp_fini(struct uib_qp *qp)
{
	uib_psn_index_destroy(&qp->out_index);
}

/**
 * uib_destroy_qp - Destroy a QP and protocol-specific outstanding state.
 * @qp: Queue pair
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_destroy_qp(struct uib_qp *qp)
{
	struct uib_context *ctx;
	struct uib_outstanding *o;
	uint32_t i;

	if (!qp)
		return -EINVAL;
	ctx = qp->ctx;
	uib_unregister_qp(ctx, qp);
	if (qp->ctx->pops->qp_fini)
		qp->ctx->pops->qp_fini(qp);

	pthread_mutex_lock(&qp->lock);
	o = qp->out_head;
	while (o) {
		struct uib_outstanding *n = o->next;

		free_outstanding(o);
		o = n;
	}
	o = qp->read_head;
	while (o) {
		struct uib_outstanding *n = o->next;

		free_outstanding(o);
		o = n;
	}
	qp->out_head = qp->out_tail = NULL;
	qp->read_head = qp->read_tail = NULL;
	if (qp->frag_recv_active)
		uib_qp_release_recv(&qp->frag_recv);
	uib_mr_put(qp->frag_recv_mr);
	qp->frag_recv_mr = NULL;
	uib_mr_put(qp->frag_write_mr);
	qp->frag_write_mr = NULL;
	for (i = 0; i < qp->rq_size; i++) {
		if (qp->rq[i].valid)
			uib_qp_release_recv(&qp->rq[i]);
	}
	pthread_mutex_unlock(&qp->lock);

	pthread_mutex_destroy(&qp->lock);
	if (qp->pd)
		qp->pd->refcnt--;
	free(qp->rq);
	outstanding_pool_destroy(qp->out_pool);
	free(qp);
	return 0;
}

/**
 * uib_modify_qp - Change QP state, peer, PSNs, access, or path MTU.
 * @qp: Queue pair
 * @attr: Queue pair attributes
 * @attr_mask: Bitmask selecting attributes to modify
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_modify_qp(struct uib_qp *qp, const struct uib_qp_attr *attr,
		  int attr_mask)
{
	if (!qp || !attr)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	if (attr_mask & UIB_QP_STATE)
		qp->state = attr->qp_state;
	if (attr_mask & UIB_QP_DEST_QPN)
		qp->dest_qp_num = attr->dest_qp_num;
	if (attr_mask & UIB_QP_RQ_PSN) {
		qp->rq_psn       = attr->rq_psn;
		qp->last_acked_psn = attr->rq_psn ? attr->rq_psn - 1 :
			UIB_BTH_PSN_MASK;
	}
	if (attr_mask & UIB_QP_SQ_PSN) {
		qp->sq_psn       = attr->sq_psn;
		qp->last_acked_psn = attr->sq_psn ? attr->sq_psn - 1 :
			UIB_BTH_PSN_MASK;
	}
	if ((attr_mask & UIB_QP_PATH_MTU) && attr->path_mtu)
		qp->path_mtu = attr->path_mtu;
	if (attr_mask & UIB_QP_QKEY)
		qp->qkey = attr->qkey;
	if (attr_mask & UIB_QP_AV)
		qp->remote_addr = attr->remote_addr;
	if (attr_mask & UIB_QP_ACCESS_FLAGS)
		qp->access_flags = attr->qp_access_flags;
	if (qp->ctx->pops->qp_modified)
		qp->ctx->pops->qp_modified(qp, attr_mask);
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static int gather_total(struct uib_pd *pd, const struct uib_sge *sgl, int n,
			uint8_t *out, size_t cap)
{
	size_t off = 0;
	int i;

	for (i = 0; i < n; i++) {
		const struct uib_sge *s = &sgl[i];
		struct uib_mr *mr;
		int rc;

		if (off + s->length > cap)
			return -EMSGSIZE;
		mr = uib_mr_lookup_local(pd, s->lkey,
					 (const void *)(uintptr_t)s->addr,
					 s->length, 0);
		if (!mr)
			return -EACCES;
		rc = uib_mr_read(mr, s->addr, out + off, s->length);
		uib_mr_put(mr);
		if (rc < 0)
			return rc;
		off += s->length;
	}
	return (int)off;
}

/**
 * uib_qp_gather - Gather an SGE list into a contiguous packet buffer.
 * @pd: Protection domain
 * @sgl: Scatter-gather list
 * @n: Number of scatter-gather entries
 * @out: Contiguous output buffer
 * @cap: Capacity of the output buffer in bytes
 * @uib-api: Queue pairs and data transfer
 *
 * Return: The total number of gathered bytes on success, or a negative error
 * code.
 */
int uib_qp_gather(struct uib_pd *pd, const struct uib_sge *sgl, int n,
		  uint8_t *out, size_t cap)
{
	return gather_total(pd, sgl, n, out, cap);
}

static int scatter_at(struct uib_pd *pd, struct uib_sge *sgl, int n,
		      uint32_t msg_offset,
		      const uint8_t *in, size_t in_len, size_t *consumed_out)
{
	size_t skip = msg_offset;
	int i;

	for (i = 0; i < n && skip > 0; i++) {
		if (skip < sgl[i].length)
			break;
		skip -= sgl[i].length;
	}
	if (i == n) {
		*consumed_out = 0;
		return 0;
	}
	size_t off_in_sge = skip;
	size_t consumed = 0;

	for (; i < n && consumed < in_len; i++) {
		struct uib_mr *mr;
		size_t avail = sgl[i].length - off_in_sge;
		size_t take = (avail < in_len - consumed) ? avail : (in_len - consumed);
		int rc;

		mr = uib_mr_lookup_local(pd, sgl[i].lkey,
					 (void *)(uintptr_t)(sgl[i].addr +
							     off_in_sge),
					 take, UIB_ACCESS_LOCAL_WRITE);
		if (!mr)
			return -EACCES;
		rc = uib_mr_place(mr, sgl[i].addr + off_in_sge,
				  in + consumed, take);
		uib_mr_put(mr);
		if (rc < 0)
			return rc;
		consumed += take;
		off_in_sge = 0;
	}
	*consumed_out = consumed;
	return 0;
}

static int scatter_recv_fragment(struct uib_qp *qp,
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
	if (!qp->frag_recv_mr)
		return scatter_at(qp->pd, slot->sg_list, slot->num_sge, offset,
				  data, length, consumed);
	sge = &slot->sg_list[0];
	available = offset < sge->length ? sge->length - offset : 0;
	take = length < available ? length : available;
	if (uib_mr_place(qp->frag_recv_mr, sge->addr + offset, data, take) < 0)
		return -EIO;
	*consumed = take;
	return 0;
}

/**
 * uib_qp_scatter - Scatter received bytes into an SGE list.
 * @pd: Protection domain
 * @sgl: Scatter-gather list
 * @n: Number of scatter-gather entries
 * @msg_offset: Offset in the scatter list at which to begin
 * @in: Contiguous input buffer
 * @in_len: Number of bytes available in the input buffer
 * @consumed_out: Output for the number of input bytes consumed
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_qp_scatter(struct uib_pd *pd, struct uib_sge *sgl, int n,
		   uint32_t msg_offset, const uint8_t *in, size_t in_len,
		   size_t *consumed_out)
{
	return scatter_at(pd, sgl, n, msg_offset, in, in_len, consumed_out);
}

int uib_qp_scatter_recv(struct uib_qp *qp, struct uib_rq_slot *slot,
			uint32_t msg_offset, const uint8_t *in,
			size_t in_len, size_t *consumed_out)
{
	if (!qp || !slot || !consumed_out)
		return -EINVAL;
	if (slot->private_data &&
	    !uib_host_loan_place(slot, msg_offset, in, in_len)) {
		*consumed_out = in_len;
		return 0;
	}
	return scatter_at(qp->pd, slot->sg_list, slot->num_sge, msg_offset,
			  in, in_len, consumed_out);
}

static void emit_send_wc(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status st, uint32_t byte_len)
{
	if (!qp->send_cq)
		return;
	if (st == UIB_WC_SUCCESS && !signaled)
		return;
	struct uib_wc wc = {0};

	wc.wr_id    = wr_id;
	wc.status   = st;
	wc.qp_num   = qp->qp_num;
	wc.byte_len = byte_len;
	switch (wr_op) {
	case UIB_WR_SEND:
	case UIB_WR_SEND_WITH_IMM:
		wc.opcode = UIB_WC_SEND;
		break;
	case UIB_WR_RDMA_WRITE:
	case UIB_WR_RDMA_WRITE_WITH_IMM:
		wc.opcode = UIB_WC_RDMA_WRITE;
		break;
	case UIB_WR_RDMA_READ:
		wc.opcode = UIB_WC_RDMA_READ;
		break;
	case UIB_WR_ATOMIC_FETCH_ADD:
		wc.opcode = UIB_WC_FETCH_ADD;
		break;
	case UIB_WR_ATOMIC_CMP_SWAP:
		wc.opcode = UIB_WC_COMP_SWAP;
		break;
	case UIB_WR_UET_ATOMIC:
		wc.opcode = UIB_WC_UET_ATOMIC;
		break;
	}
	uib_cq_push(qp->send_cq, &wc);
}

/**
 * uib_qp_emit_send - Publish a send work completion.
 * @qp: Queue pair
 * @wr_id: Caller-defined work request identifier
 * @wr_op: Work request operation code
 * @signaled: Nonzero to request a completion
 * @status: Completion status
 * @byte_len: Number of bytes completed
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_emit_send(struct uib_qp *qp, uint64_t wr_id,
		      enum uib_wr_opcode wr_op, int signaled,
		      enum uib_wc_status status, uint32_t byte_len)
{
	emit_send_wc(qp, wr_id, wr_op, signaled, status, byte_len);
}

static int emit_recv_wc(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wc_status st, enum uib_wc_opcode op,
			 uint32_t byte_len, uint32_t src_qp,
			 uint32_t imm_data, void *private_data)
{
	if (!qp->recv_cq)
		return -EINVAL;
	struct uib_wc wc = {0};

	wc.wr_id    = wr_id;
	wc.status   = st;
	wc.opcode   = op;
	wc.byte_len = byte_len;
	wc.qp_num   = qp->qp_num;
	wc.src_qp   = src_qp;
	wc.imm_data = imm_data;
	wc.private_data = private_data;
	return uib_cq_push(qp->recv_cq, &wc);
}

/**
 * uib_qp_emit_recv - Publish a receive work completion.
 * @qp: Queue pair
 * @wr_id: Caller-defined work request identifier
 * @status: Completion status
 * @opcode: Completion operation code
 * @byte_len: Number of bytes completed
 * @src_qp: Source queue pair number
 * @imm_data: Immediate data in network byte order
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_emit_recv(struct uib_qp *qp, uint64_t wr_id,
		      enum uib_wc_status status, enum uib_wc_opcode opcode,
		      uint32_t byte_len, uint32_t src_qp, uint32_t imm_data)
{
	emit_recv_wc(qp, wr_id, status, opcode, byte_len, src_qp, imm_data,
		     NULL);
}

/**
 * uib_qp_emit_recv_slot - Publish a receive completion with private ownership.
 * @qp: Queue pair
 * @slot: Receive queue slot
 * @status: Completion status
 * @opcode: Completion operation code
 * @byte_len: Number of bytes completed
 * @src_qp: Source queue pair number
 * @imm_data: Immediate data in network byte order
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_emit_recv_slot(struct uib_qp *qp, struct uib_rq_slot *slot,
			   enum uib_wc_status status,
			   enum uib_wc_opcode opcode, uint32_t byte_len,
			   uint32_t src_qp, uint32_t imm_data)
{
	if (!qp || !slot)
		return;
	if (!emit_recv_wc(qp, slot->wr_id, status, opcode, byte_len, src_qp,
			  imm_data, slot->private_data)) {
		slot->private_data = NULL;
		slot->private_release = NULL;
	}
}

static void outstanding_index_add(struct uib_qp *qp,
				  struct uib_outstanding *out)
{
	if (!out->is_read ||
	    uib_psn_index_insert(&qp->out_index,
				 out->psn & UIB_BTH_PSN_MASK, out))
		return;
	out->indexed = 1;
}

static void outstanding_index_remove(struct uib_qp *qp,
				     struct uib_outstanding *out)
{
	if (!out->indexed)
		return;
	uib_psn_index_remove(&qp->out_index,
			     out->psn & UIB_BTH_PSN_MASK, out);
	out->indexed = 0;
}

static void outstanding_unlink(struct uib_qp *qp,
			       struct uib_outstanding *out)
{
	struct uib_outstanding **tail;
	struct uib_outstanding **head;

	if (out->is_read) {
		head = &qp->read_head;
		tail = &qp->read_tail;
	} else {
		head = &qp->out_head;
		tail = &qp->out_tail;
	}
	if (out->prev)
		out->prev->next = out->next;
	else
		*head = out->next;
	if (out->next)
		out->next->prev = out->prev;
	else
		*tail = out->prev;
	outstanding_index_remove(qp, out);
	out->next = NULL;
	out->prev = NULL;
	qp->out_count--;
}

static void enqueue_outstanding(struct uib_qp *qp, struct uib_outstanding *out)
{
	struct uib_outstanding **tail;
	struct uib_outstanding **head;

	if (out->is_read) {
		head = &qp->read_head;
		tail = &qp->read_tail;
	} else {
		head = &qp->out_head;
		tail = &qp->out_tail;
	}
	out->next = NULL;
	out->prev = *tail;
	out->deadline_ns = uib_now_ns() + UIB_RTO_NS;
	if (*tail)
		(*tail)->next = out;
	else
		*head = out;
	*tail = out;
	outstanding_index_add(qp, out);
	qp->out_count++;
	if (!qp->retry_scheduled ||
	    out->deadline_ns < qp->retry_deadline_ns)
		uib_qp_retry_schedule(qp, out->deadline_ns);
}

static void outstanding_retry_reschedule(struct uib_qp *qp)
{
	uint64_t deadline = UINT64_MAX;

	if (qp->out_head)
		deadline = qp->out_head->deadline_ns;
	if (qp->read_head && qp->read_head->deadline_ns < deadline)
		deadline = qp->read_head->deadline_ns;
	if (deadline != UINT64_MAX)
		uib_qp_retry_schedule(qp, deadline);
	else
		uib_qp_retry_cancel(qp);
}

static void send_ack(struct uib_qp *qp, const struct sockaddr_in *dst,
		     uint32_t ack_psn, uint8_t syndrome)
{
	struct sockaddr_in target = *dst;
	if (qp->remote_addr.sin_family == AF_INET) {
		target.sin_addr = qp->remote_addr.sin_addr;
		target.sin_port = qp->remote_addr.sin_port;
	}
	uint8_t pkt[UIB_BTH_BYTES + UIB_AETH_BYTES + UIB_ICRC_BYTES];
	struct uib_bth *bth = (struct uib_bth *)pkt;

	uib_bth_pack(bth, UIB_OP_ACKNOWLEDGE, UIB_DEFAULT_PKEY,
		     qp->dest_qp_num, ack_psn, 0);
	struct uib_aeth *aeth = (struct uib_aeth *)(pkt + UIB_BTH_BYTES);

	uib_aeth_pack(aeth, syndrome, ack_psn);
	uint32_t crc = uib_icrc(pkt, UIB_BTH_BYTES + UIB_AETH_BYTES);
	uint32_t bcrc = htonl(crc);

	memcpy(pkt + UIB_BTH_BYTES + UIB_AETH_BYTES, &bcrc, UIB_ICRC_BYTES);
	uib_transport_send(qp->ctx, &target, pkt, sizeof(pkt));
}

static size_t packet_length(const struct uib_qp *qp, uint8_t op,
			    int is_first, size_t chunk)
{
	int is_ud = (qp->qp_type == UIB_QPT_UD);
	size_t length = UIB_BTH_BYTES + chunk + UIB_ICRC_BYTES;

	if (is_first && (op == UIB_OP_RDMA_WRITE_FIRST ||
			 op == UIB_OP_RDMA_WRITE_ONLY ||
			 op == UIB_OP_RDMA_WRITE_ONLY_WITH_IMM ||
			 op == UIB_OP_RDMA_READ_REQ))
		length += UIB_RETH_BYTES;
	if (is_ud)
		length += UIB_DETH_BYTES;
	if (op == UIB_OP_SEND_LAST_WITH_IMM ||
	    op == UIB_OP_SEND_ONLY_WITH_IMM ||
	    op == UIB_OP_RDMA_WRITE_LAST_WITH_IMM ||
	    op == UIB_OP_RDMA_WRITE_ONLY_WITH_IMM ||
	    op == UIB_OP_UD_SEND_ONLY_WITH_IMM)
		length += sizeof(uint32_t);
	length += (size_t)((-(int)chunk) & 3);
	return length;
}

static int send_one_packet(struct uib_qp *qp, struct uib_send_wr *wr,
			   uint8_t op, int is_first, int is_last,
			   const uint8_t *payload, size_t chunk,
			   uint64_t reth_va, uint32_t reth_rkey,
			   uint32_t reth_total, struct uib_outstanding *out,
			   void *packet,
			   size_t capacity, uint32_t *out_psn,
			   size_t *out_pkt_len, struct sockaddr_in *out_dst,
			   int defer_send)
{
	struct sockaddr_in dst;
	uint8_t *pkt = packet;
	uint8_t *p = pkt;
	uint32_t bcrc;
	uint32_t crc;
	uint32_t dqpn;
	uint32_t psn;
	uint8_t pad;
	int is_rc;
	int is_ud;
	int rc;

	if (packet_length(qp, op, is_first, chunk) > capacity)
		return -EMSGSIZE;
	is_ud = qp->qp_type == UIB_QPT_UD;
	is_rc = qp->qp_type == UIB_QPT_RC;
	dqpn = is_ud ? wr->ud.remote_qpn : qp->dest_qp_num;
	dst = is_ud ? wr->ud.remote_addr : qp->remote_addr;
	psn = qp->sq_psn++;

	uib_bth_pack((struct uib_bth *)p, op, UIB_DEFAULT_PKEY, dqpn, psn,
		     is_rc && is_last);
	pad = (uint8_t)((-(int)chunk) & 3);
	uib_bth_set_pad((struct uib_bth *)p, pad);
	p += UIB_BTH_BYTES;

	if (is_first && (op == UIB_OP_RDMA_WRITE_FIRST ||
			 op == UIB_OP_RDMA_WRITE_ONLY ||
			 op == UIB_OP_RDMA_WRITE_ONLY_WITH_IMM ||
			 op == UIB_OP_RDMA_READ_REQ)) {
		uib_reth_pack((struct uib_reth *)p, reth_va, reth_rkey, reth_total);
		p += UIB_RETH_BYTES;
	}

	if (is_ud) {
		uib_deth_pack((struct uib_deth *)p, wr->ud.qkey, qp->qp_num);
		p += UIB_DETH_BYTES;
	}
	if (op == UIB_OP_SEND_LAST_WITH_IMM ||
	    op == UIB_OP_SEND_ONLY_WITH_IMM ||
	    op == UIB_OP_RDMA_WRITE_LAST_WITH_IMM ||
	    op == UIB_OP_RDMA_WRITE_ONLY_WITH_IMM ||
	    op == UIB_OP_UD_SEND_ONLY_WITH_IMM) {
		memcpy(p, &wr->imm_data, sizeof(wr->imm_data));
		p += sizeof(wr->imm_data);
	}

	if (chunk > 0) {
		memcpy(p, payload, chunk);
		p += chunk;
	}
	if (pad) {
		memset(p, 0, pad);
		p += pad;
	}

	crc = uib_icrc(pkt, p - pkt);
	bcrc = htonl(crc);
	memcpy(p, &bcrc, UIB_ICRC_BYTES);
	p += UIB_ICRC_BYTES;
	*out_pkt_len = (size_t)(p - pkt);
	if (out) {
		out->dst = dst;
		out->pkt_len = *out_pkt_len;
		if (defer_send)
			rc = 0;
		else
			rc = send_outstanding(qp, out);
	} else {
		rc = uib_transport_send(qp->ctx, &dst, pkt, *out_pkt_len);
	}
	if (rc < 0)
		return rc;

	if (out_psn)
		*out_psn = psn;
	if (out_dst)
		*out_dst = dst;
	return 0;
}

static int post_rdma_read(struct uib_qp *qp, struct uib_send_wr *wr,
			  size_t total)
{
	struct uib_congestion congestion_before;
	struct uib_outstanding *out;
	struct sockaddr_in dst;
	size_t packet_len;
	uint32_t n_resp;
	uint32_t mtu;
	uint32_t psn;
	uint32_t start_psn;
	int rc;

	mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
	n_resp = (total + mtu - 1) / mtu;
	if (n_resp == 0)
		n_resp = 1;
	packet_len = packet_length(qp, UIB_OP_RDMA_READ_REQ, 1, 0);
	out = alloc_outstanding(qp, packet_len);
	if (!out)
		return -ENOMEM;
	if (wr->num_sge > 0) {
		out->read_sgl = calloc(wr->num_sge, sizeof(*out->read_sgl));
		if (!out->read_sgl) {
			free_outstanding(out);
			return -ENOMEM;
		}
		memcpy(out->read_sgl, wr->sg_list,
		       wr->num_sge * sizeof(*out->read_sgl));
		out->read_num_sge = wr->num_sge;
		out->read_total_len = (uint32_t)total;
	}
	/*
	 * Serialize transmission with insertion into the outstanding list so a
	 * fast peer cannot ACK before the request becomes visible locally.
	 */
	pthread_mutex_lock(&qp->lock);
	congestion_before = qp->congestion;
	start_psn = qp->sq_psn;
	out->nominal_len = rocev2_nominal_len(qp, packet_len);
	if (!uib_congestion_can_send(&qp->congestion, out->nominal_len)) {
		pthread_mutex_unlock(&qp->lock);
		free_outstanding(out);
		return -EAGAIN;
	}
	out->tx_time_ns = now_ns();
	uib_congestion_on_send(&qp->congestion, out->nominal_len);
	out->congestion_inflight = 1;
	rc = send_one_packet(qp, wr, UIB_OP_RDMA_READ_REQ, 1, 1, NULL, 0,
			     wr->rdma.remote_addr, wr->rdma.rkey,
			     (uint32_t)total, out, out->pkt, out->pkt_len, &psn,
			     &out->pkt_len, &dst, 0);
	if (rc < 0) {
		qp->congestion = congestion_before;
		qp->sq_psn = start_psn;
		pthread_mutex_unlock(&qp->lock);
		free_outstanding(out);
		return rc;
	}
	qp->sq_psn += (n_resp - 1);
	out->psn = psn;
	out->wire_opcode = UIB_OP_RDMA_READ_REQ;
	out->signaled = (wr->send_flags & UIB_SEND_SIGNALED) ? 1 : 0;
	out->is_read = 1;
	out->is_final = 1;
	out->wr_id = wr->wr_id;
	out->wr_opcode = wr->opcode;
	out->dst = dst;
	enqueue_outstanding(qp, out);
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static uint8_t pick_opcode(enum uib_qp_type qpt, enum uib_wr_opcode op,
			   int with_imm, uint32_t n, uint32_t i)
{
	int is_ud = (qpt == UIB_QPT_UD);

	if (is_ud)
		return with_imm ? UIB_OP_UD_SEND_ONLY_WITH_IMM :
			UIB_OP_UD_SEND_ONLY;
	int is_only  = (n == 1);
	int is_first = (i == 0);
	int is_last  = (i == n - 1);

	switch (op) {
	case UIB_WR_SEND:
	case UIB_WR_SEND_WITH_IMM:
		if (is_only)
			return with_imm ? UIB_OP_SEND_ONLY_WITH_IMM :
				UIB_OP_SEND_ONLY;
		if (is_first)
			return UIB_OP_SEND_FIRST;
		if (is_last)
			return with_imm ? UIB_OP_SEND_LAST_WITH_IMM :
				UIB_OP_SEND_LAST;
		return UIB_OP_SEND_MIDDLE;
	case UIB_WR_RDMA_WRITE:
	case UIB_WR_RDMA_WRITE_WITH_IMM:
		if (is_only)
			return with_imm ? UIB_OP_RDMA_WRITE_ONLY_WITH_IMM :
				UIB_OP_RDMA_WRITE_ONLY;
		if (is_first)
			return UIB_OP_RDMA_WRITE_FIRST;
		if (is_last)
			return with_imm ? UIB_OP_RDMA_WRITE_LAST_WITH_IMM :
				UIB_OP_RDMA_WRITE_LAST;
		return UIB_OP_RDMA_WRITE_MIDDLE;
	case UIB_WR_RDMA_READ:
		return UIB_OP_RDMA_READ_REQ;
	case UIB_WR_ATOMIC_FETCH_ADD:
	case UIB_WR_ATOMIC_CMP_SWAP:
	case UIB_WR_UET_ATOMIC:
		return UIB_BTH_INVALID_OPCODE;
	}
	return UIB_BTH_INVALID_OPCODE;
}

static int post_one_send(struct uib_qp *qp, struct uib_send_wr *wr)
{
	struct uib_congestion congestion_before;
	struct uib_transport_ref stack_refs[UIB_TX_BATCH_STACK];
	struct uib_transport_ref *refs;
	struct uib_outstanding *out;
	struct sockaddr_in dst;
	const uint8_t *message;
	struct uib_mr *mr;
	uint8_t stack_packet[UIB_PKT_MAX];
	uint8_t *owned_message;
	uint8_t *packet;
	size_t packet_len;
	size_t chunk;
	size_t total;
	size_t off;
	uint32_t mtu;
	uint32_t psn;
	uint32_t n;
	uint32_t i;
	uint32_t built;
	uint32_t start_psn;
	uint8_t op;
	int is_first;
	int with_imm;
	int is_last;
	int is_rc;
	int is_ud;
	int err;
	int rc;

	with_imm = wr->opcode == UIB_WR_SEND_WITH_IMM ||
		wr->opcode == UIB_WR_RDMA_WRITE_WITH_IMM;
	is_ud = qp->qp_type == UIB_QPT_UD;
	is_rc = qp->qp_type == UIB_QPT_RC;
	total = 0;
	for (i = 0; i < (uint32_t)wr->num_sge; i++)
		total += wr->sg_list[i].length;
	if (wr->opcode == UIB_WR_RDMA_READ)
		return post_rdma_read(qp, wr, total);
	if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    wr->opcode == UIB_WR_ATOMIC_CMP_SWAP)
		return -EOPNOTSUPP;
	mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
	if (is_ud && total > mtu)
		return -EMSGSIZE;
	message = NULL;
	owned_message = NULL;
	mr = NULL;
	if (total > 0) {
		if (wr->send_flags & UIB_SEND_INLINE) {
			if (wr->num_sge == 1) {
				message = (const uint8_t *)(uintptr_t)
					wr->sg_list[0].addr;
			} else {
				size_t copied = 0;

				owned_message = malloc(total);
				if (!owned_message)
					return -ENOMEM;
				for (i = 0; i < (uint32_t)wr->num_sge; i++) {
					memcpy(owned_message + copied,
					       (const void *)(uintptr_t)
					       wr->sg_list[i].addr,
					       wr->sg_list[i].length);
					copied += wr->sg_list[i].length;
				}
				message = owned_message;
			}
		} else if (wr->num_sge == 1) {
			mr = uib_mr_lookup_local(
				qp->pd, wr->sg_list[0].lkey,
				(const void *)(uintptr_t)wr->sg_list[0].addr,
				wr->sg_list[0].length, 0);
			if (!mr)
				return -EACCES;
			if (mr->kind == UIB_MR_HOST)
				message = (const uint8_t *)(uintptr_t)
					wr->sg_list[0].addr;
			else {
				uib_mr_put(mr);
				mr = NULL;
			}
		}
		if (!message) {
			owned_message = malloc(total);
			if (!owned_message)
				return -ENOMEM;
			rc = gather_total(qp->pd, wr->sg_list, wr->num_sge,
					  owned_message, total);
			if (rc < 0) {
				free(owned_message);
				return rc;
			}
			message = owned_message;
		}
	}
	n = total <= mtu ? 1 : (uint32_t)((total + mtu - 1) / mtu);
	if (n == 0)
		n = 1;
	refs = stack_refs;
	if (is_rc && n > UIB_TX_BATCH_STACK) {
		refs = malloc((size_t)n * sizeof(*refs));
		if (!refs) {
			uib_mr_put(mr);
			free(owned_message);
			return -ENOMEM;
		}
	}
	err = 0;
	built = 0;
	if (is_rc)
		pthread_mutex_lock(&qp->lock);
	congestion_before = qp->congestion;
	start_psn = qp->sq_psn;
	for (i = 0; i < n; i++) {
		off = (size_t)i * mtu;
		chunk = off + mtu > total ? total - off : mtu;
		is_first = i == 0;
		is_last = i == n - 1;
		op = pick_opcode(qp->qp_type, wr->opcode, with_imm, n, i);
		if (op == UIB_BTH_INVALID_OPCODE) {
			err = -EINVAL;
			break;
		}
		packet_len = packet_length(qp, op, is_first, chunk);
		out = NULL;
		packet = stack_packet;
		if (is_rc) {
			out = alloc_outstanding(qp, packet_len);
			if (!out) {
				err = -ENOMEM;
				break;
			}
			packet = out->pkt;
		}
		rc = send_one_packet(qp, wr, op, is_first, is_last,
				     message ? message + off : NULL, chunk,
				     wr->rdma.remote_addr, wr->rdma.rkey,
				     (uint32_t)total, out, packet, packet_len,
				     &psn, &packet_len, &dst, is_rc);
		if (rc < 0) {
			free_outstanding(out);
			err = rc;
			break;
		}
		if (is_rc) {
			out->nominal_len = rocev2_nominal_len(qp, packet_len);
			if (!uib_congestion_can_send(&qp->congestion,
						     out->nominal_len)) {
				free_outstanding(out);
				err = -EAGAIN;
				break;
			}
			out->tx_time_ns = now_ns();
			uib_congestion_on_send(&qp->congestion,
					       out->nominal_len);
			out->congestion_inflight = 1;
			out->psn = psn;
			out->wire_opcode = op;
			out->signaled =
				(wr->send_flags & UIB_SEND_SIGNALED) ? 1 : 0;
			out->is_final = is_last;
			out->wr_id = wr->wr_id;
			out->wr_opcode = wr->opcode;
			out->dst = dst;
			out->pkt_len = packet_len;
			refs[built].dst = &out->dst;
			refs[built].buf = out->pkt;
			refs[built].release = free_outstanding;
			refs[built].opaque = out;
			refs[built].len = out->pkt_len;
			refs[built].entropy = 0;
			built++;
		}
	}
	if (is_rc && !err) {
		for (i = 0; i < built; i++) {
			out = refs[i].opaque;
			__atomic_add_fetch(&out->refs, 1, __ATOMIC_RELAXED);
		}
		rc = uib_transport_send_ref_batch(qp->ctx, refs, built);
		if (rc < 0) {
			err = rc;
		} else {
			for (i = 0; i < built; i++) {
				out = refs[i].opaque;
				enqueue_outstanding(qp, out);
			}
			built = 0;
		}
	}
	if (is_rc && err) {
		qp->congestion = congestion_before;
		qp->sq_psn = start_psn;
	}
	for (i = 0; i < built; i++) {
		out = refs[i].opaque;
		free_outstanding(out);
	}
	if (is_rc)
		pthread_mutex_unlock(&qp->lock);
	if (refs != stack_refs)
		free(refs);
	uib_mr_put(mr);
	free(owned_message);
	if (!is_rc && !err)
		emit_send_wc(qp, wr->wr_id, wr->opcode,
			     wr->send_flags & UIB_SEND_SIGNALED,
			     UIB_WC_SUCCESS, (uint32_t)total);
	return err;
}

int uib_rocev2_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			 struct uib_send_wr **bad)
{
	struct uib_send_wr *cur;

	if (!qp || !wr)
		return -EINVAL;

	pthread_mutex_lock(&qp->lock);
	if (qp->state != UIB_QPS_RTS && qp->state != UIB_QPS_RTR) {
		pthread_mutex_unlock(&qp->lock);
		if (bad)
			*bad = wr;
		return -EINVAL;
	}
	pthread_mutex_unlock(&qp->lock);

	for (cur = wr; cur; cur = cur->next) {
		int rc = post_one_send(qp, cur);

		if (rc < 0) {
			emit_send_wc(qp, cur->wr_id, cur->opcode,
				     cur->send_flags & UIB_SEND_SIGNALED,
				     rc == -ERANGE ? UIB_WC_LOC_LEN_ERR
						   : UIB_WC_LOC_PROT_ERR, 0);
			if (bad)
				*bad = cur;
			return rc;
		}
	}
	return 0;
}

/**
 * uib_post_send - Post SEND, WRITE, READ, or atomic work requests.
 * @qp: Queue pair
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
		  struct uib_send_wr **bad)
{
	if (!qp || !wr)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	if (qp->state != UIB_QPS_RTS && qp->state != UIB_QPS_RTR) {
		pthread_mutex_unlock(&qp->lock);
		if (bad)
			*bad = wr;
		return -EINVAL;
	}
	pthread_mutex_unlock(&qp->lock);
	if (!qp->ctx->pops || !qp->ctx->pops->post_send)
		return -EOPNOTSUPP;

	return qp->ctx->pops->post_send(qp, wr, bad);
}

static int post_recv_internal(struct uib_qp *qp, struct uib_recv_wr *wr,
			      struct uib_recv_wr **bad, void *private_data,
			      void (*private_release)(void *private_data))
{
	struct uib_rq_slot *slot;
	struct uib_recv_wr *cur;
	uint32_t next;

	if (!qp || !wr)
		return -EINVAL;

	pthread_mutex_lock(&qp->lock);
	for (cur = wr; cur; cur = cur->next) {
		next = (qp->rq_head + 1) % qp->rq_size;

		if (next == qp->rq_tail) {
			pthread_mutex_unlock(&qp->lock);
			if (bad)
				*bad = cur;
			return -ENOSPC;
		}
		slot = &qp->rq[qp->rq_head];

		slot->wr_id = cur->wr_id;
		slot->num_sge = cur->num_sge;
		slot->heap_sge = cur->num_sge > UIB_RQ_INLINE_SGE;
		if (private_data) {
			slot->private_data = private_data;
			slot->private_release = private_release;
			private_data = NULL;
			private_release = NULL;
		}
		if (slot->heap_sge) {
			slot->sg_list = malloc((size_t)cur->num_sge *
					       sizeof(*slot->sg_list));
			if (!slot->sg_list) {
				pthread_mutex_unlock(&qp->lock);
				if (bad)
					*bad = cur;
				return -ENOMEM;
			}
		} else {
			slot->sg_list = slot->inline_sge;
		}
		memcpy(slot->sg_list, cur->sg_list,
		       cur->num_sge * sizeof(struct uib_sge));
		slot->valid = 1;
		qp->rq_head = next;
	}
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_post_recv - Post one or more receive work requests.
 * @qp: Queue pair
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_post_recv(struct uib_qp *qp, struct uib_recv_wr *wr,
		  struct uib_recv_wr **bad)
{
	return post_recv_internal(qp, wr, bad, NULL, NULL);
}

int uib_post_recv_owned(struct uib_qp *qp, struct uib_recv_wr *request,
			void *private_data,
			void (*private_release)(void *private_data))
{
	return post_recv_internal(qp, request, NULL, private_data,
				  private_release);
}

static int pop_recv_slot(struct uib_qp *qp, struct uib_rq_slot *out)
{
	pthread_mutex_lock(&qp->lock);
	if (qp->rq_tail == qp->rq_head) {
		pthread_mutex_unlock(&qp->lock);
		return -ENOENT;
	}
	*out = qp->rq[qp->rq_tail];
	if (!out->heap_sge)
		out->sg_list = out->inline_sge;
	qp->rq[qp->rq_tail].valid = 0;
	qp->rq[qp->rq_tail].sg_list = NULL;
	qp->rq[qp->rq_tail].private_data = NULL;
	qp->rq[qp->rq_tail].private_release = NULL;
	qp->rq_tail = (qp->rq_tail + 1) % qp->rq_size;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_qp_pop_recv - Consume the oldest posted receive request.
 * @qp: Queue pair
 * @out: Receive slot structure to fill
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_qp_pop_recv(struct uib_qp *qp, struct uib_rq_slot *out)
{
	return pop_recv_slot(qp, out);
}

/**
 * uib_qp_release_recv - Release storage retained by a popped receive slot.
 * @slot: Receive queue slot
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_release_recv(struct uib_rq_slot *slot)
{
	if (!slot)
		return;
	if (slot->heap_sge)
		free(slot->sg_list);
	if (slot->private_release)
		slot->private_release(slot->private_data);
	slot->sg_list = NULL;
	slot->heap_sge = 0;
	slot->private_data = NULL;
	slot->private_release = NULL;
}

static int verify_icrc(const uint8_t *pkt, size_t len)
{
	if (len < UIB_ICRC_BYTES)
		return -1;
	uint32_t want;

	memcpy(&want, pkt + len - UIB_ICRC_BYTES, UIB_ICRC_BYTES);
	want = ntohl(want);
	uint32_t got = uib_icrc(pkt, len - UIB_ICRC_BYTES);
	return got == want ? 0 : -1;
}

static int psn_cmp(uint32_t a, uint32_t b)
{
	int32_t d = (int32_t)((a - b) << 8);

	if (d == 0)
		return 0;
	return d < 0 ? -1 : 1;
}

static void ack_outstanding(struct uib_qp *qp, uint32_t ack_psn)
{
	struct uib_congestion_ack event = {
		.ack_time_ns = now_ns(),
	};
	struct uib_outstanding *next;
	struct uib_outstanding *out;
	uint64_t acknowledged = 0;

	pthread_mutex_lock(&qp->lock);
	for (out = qp->out_head; out; out = next) {
		next = out->next;

		if (psn_cmp(out->psn, ack_psn) > 0)
			break;
		if (out->is_final) {
			emit_send_wc(qp, out->wr_id, out->wr_opcode, out->signaled,
				     UIB_WC_SUCCESS, 0);
		}
		if (out->congestion_inflight)
			acknowledged += out->nominal_len;
		event.tx_time_ns = out->tx_time_ns;
		event.tx_retries = out->retries;
		event.retransmit = out->retries != 0;
		outstanding_unlink(qp, out);
		free_outstanding(out);
	}
	if (acknowledged) {
		event.newly_received = acknowledged > UINT32_MAX ? UINT32_MAX :
			(uint32_t)acknowledged;
		uib_congestion_on_ack(&qp->congestion, &event);
	}
	qp->last_acked_psn = ack_psn;
	outstanding_retry_reschedule(qp);
	pthread_mutex_unlock(&qp->lock);
}

static void rocev2_congestion_cnp(struct uib_qp *qp)
{
	struct uib_congestion_ack event = {
		.ack_time_ns = now_ns(),
		.ecn_marked = 1,
	};
	struct uib_outstanding *out;

	pthread_mutex_lock(&qp->lock);
	out = qp->out_head ? qp->out_head : qp->read_head;
	if (out) {
		event.tx_time_ns = out->tx_time_ns;
		event.tx_retries = out->retries;
		event.retransmit = out->retries != 0;
		uib_congestion_on_ack(&qp->congestion, &event);
	}
	pthread_mutex_unlock(&qp->lock);
}

static void fail_outstanding(struct uib_qp *qp, enum uib_wc_status st)
{
	struct uib_outstanding *out;

	pthread_mutex_lock(&qp->lock);
	while ((out = qp->out_head) != NULL) {
		if (out->congestion_inflight)
			uib_congestion_on_loss(&qp->congestion,
					       out->nominal_len);
		if (out->is_final)
			emit_send_wc(qp, out->wr_id, out->wr_opcode, 1, st, 0);
		outstanding_unlink(qp, out);
		free_outstanding(out);
	}
	while ((out = qp->read_head) != NULL) {
		if (out->congestion_inflight)
			uib_congestion_on_loss(&qp->congestion,
					       out->nominal_len);
		emit_send_wc(qp, out->wr_id, out->wr_opcode, 1, st, 0);
		outstanding_unlink(qp, out);
		free_outstanding(out);
	}
	qp->frag_read_out = NULL;
	qp->frag_read_offset = 0;
	uib_qp_retry_cancel(qp);
	pthread_mutex_unlock(&qp->lock);
}

static void send_read_response(struct uib_qp *qp, const struct sockaddr_in *dst,
			       uint32_t start_psn, uint64_t va, uint32_t rkey,
			       uint32_t length)
{
	uint8_t packet[UIB_PKT_MAX];
	struct uib_mr *mr;
	uint32_t mtu;
	uint32_t n;
	uint32_t i;

	mr = uib_mr_check_remote(qp->pd, rkey, va, length,
				  UIB_ACCESS_REMOTE_READ);
	if (!mr) {
		send_ack(qp, dst, start_psn, UIB_AETH_NAK_REM_ACC);
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
			send_ack(qp, dst, start_psn, UIB_AETH_NAK_REM_ACC);
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

		uib_transport_send(qp->ctx, dst, pkt, pkt_len);
	}
out:
	uib_mr_put(mr);
}

static struct uib_outstanding *find_read_by_psn(struct uib_qp *qp, uint32_t psn)
{
	struct uib_outstanding *out;

	pthread_mutex_lock(&qp->lock);
	out = uib_psn_index_lookup(&qp->out_index,
				   psn & UIB_BTH_PSN_MASK);
	if (!out) {
		for (out = qp->read_head; out; out = out->next) {
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
	if (target == qp->read_head || target->prev || target->next) {
		if (target->congestion_inflight) {
			struct uib_congestion_ack event = {
				.ack_time_ns = now_ns(),
				.tx_time_ns = target->tx_time_ns,
				.newly_received = target->nominal_len,
				.tx_retries = target->retries,
				.retransmit = target->retries != 0,
			};

			uib_congestion_on_ack(&qp->congestion, &event);
		}
		outstanding_unlink(qp, target);
		outstanding_retry_reschedule(qp);
	}
	pthread_mutex_unlock(&qp->lock);
}

int uib_rocev2_handle_packet(struct uib_qp *qp,
			    const struct sockaddr_in *src,
			    const void *packet, size_t len)
{
	const uint8_t *p = packet;

	if (verify_icrc(p, len) < 0)
		return -EBADMSG;

	const struct uib_bth *bth = (const struct uib_bth *)p;
	uint8_t op;
	uint32_t dqpn, psn;

	uib_bth_unpack(bth, &op, &dqpn, &psn, NULL);
	(void)dqpn;

	const uint8_t *body = p + UIB_BTH_BYTES;
	size_t body_len = len - UIB_BTH_BYTES - UIB_ICRC_BYTES;
	uint8_t pad = uib_bth_pad(bth);

	if (pad > body_len)
		return -EBADMSG;
	body_len -= pad;

	int is_rc = (qp->qp_type == UIB_QPT_RC);

	int is_resp = (op == UIB_OP_ACKNOWLEDGE || op == UIB_OP_CNP
		       || op == UIB_OP_RDMA_READ_RESP_FIRST
		       || op == UIB_OP_RDMA_READ_RESP_MIDDLE
		       || op == UIB_OP_RDMA_READ_RESP_LAST
		       || op == UIB_OP_RDMA_READ_RESP_ONLY);

	if (is_rc && !is_resp) {
		uint32_t expected = qp->rq_psn & UIB_BTH_PSN_MASK;
		int order = psn_cmp(psn, expected);

		if (order < 0) {
			send_ack(qp, src, qp->last_rx_psn, UIB_AETH_ACK);
			return 0;
		}
		if (order > 0) {
			send_ack(qp, src, expected, UIB_AETH_NAK_PSN_SEQ);
			return 0;
		}
	}

	switch (op) {
	case UIB_OP_CNP:
		rocev2_congestion_cnp(qp);
		return 0;
	case UIB_OP_SEND_ONLY:
	case UIB_OP_SEND_ONLY_WITH_IMM: {
		struct uib_rq_slot slot;
		size_t consumed = 0;
		uint32_t imm_data = 0;
		int rc;

		if (op == UIB_OP_SEND_ONLY_WITH_IMM) {
			if (body_len < sizeof(imm_data))
				return -EBADMSG;
			memcpy(&imm_data, body, sizeof(imm_data));
			body += sizeof(imm_data);
			body_len -= sizeof(imm_data);
		}

		if (pop_recv_slot(qp, &slot) < 0) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_RNR);
			return -ENOENT;
		}
		rc = uib_qp_scatter_recv(qp, &slot, 0, body, body_len,
					 &consumed);
		uib_qp_emit_recv_slot(qp, &slot,
				      rc < 0 ? UIB_WC_LOC_PROT_ERR :
				      UIB_WC_SUCCESS, UIB_WC_RECV,
				      (uint32_t)consumed, qp->dest_qp_num,
				      imm_data);
		uib_qp_release_recv(&slot);
		qp->last_rx_psn = psn;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn,
				 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
		return rc;
	}
	case UIB_OP_SEND_FIRST: {
		struct uib_rq_slot slot;
		size_t consumed = 0;
		int rc;

		if (pop_recv_slot(qp, &slot) < 0) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_RNR);
			return -ENOENT;
		}
		uib_mr_put(qp->frag_recv_mr);
		qp->frag_recv_mr = NULL;
		if (slot.num_sge == 1)
			qp->frag_recv_mr = uib_mr_lookup_local(
				qp->pd, slot.sg_list[0].lkey,
				(const void *)(uintptr_t)slot.sg_list[0].addr,
				slot.sg_list[0].length, 0);
		rc = scatter_recv_fragment(qp, &slot, 0, body, body_len,
					   &consumed);
		qp->frag_recv = slot;
		if (!qp->frag_recv.heap_sge)
			qp->frag_recv.sg_list = qp->frag_recv.inline_sge;
		qp->frag_recv_active = 1;
		qp->frag_recv_offset = (uint32_t)consumed;
		qp->last_rx_psn = psn;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + 1;
		if (is_rc && rc < 0)
			send_ack(qp, src, psn,
				 UIB_AETH_NAK_INV_REQ);
		return rc;
	}
	case UIB_OP_SEND_MIDDLE: {
		if (!qp->frag_recv_active) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		size_t consumed = 0;
		int rc = scatter_recv_fragment(qp, &qp->frag_recv,
					       qp->frag_recv_offset, body,
					       body_len, &consumed);
		qp->frag_recv_offset += (uint32_t)consumed;
		qp->last_rx_psn = psn;
		qp->rq_psn = psn + 1;
		if (is_rc && rc < 0)
			send_ack(qp, src, psn,
				 UIB_AETH_NAK_INV_REQ);
		return rc;
	}
	case UIB_OP_SEND_LAST:
	case UIB_OP_SEND_LAST_WITH_IMM: {
		uint32_t imm_data = 0;

		if (!qp->frag_recv_active) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		if (op == UIB_OP_SEND_LAST_WITH_IMM) {
			if (body_len < sizeof(imm_data))
				return -EBADMSG;
			memcpy(&imm_data, body, sizeof(imm_data));
			body += sizeof(imm_data);
			body_len -= sizeof(imm_data);
		}
		size_t consumed = 0;
		int rc = scatter_recv_fragment(qp, &qp->frag_recv,
					       qp->frag_recv_offset, body,
					       body_len, &consumed);
		uint32_t total = qp->frag_recv_offset + (uint32_t)consumed;

		uib_qp_emit_recv_slot(qp, &qp->frag_recv,
				      rc < 0 ? UIB_WC_LOC_PROT_ERR :
				      UIB_WC_SUCCESS, UIB_WC_RECV, total,
				      qp->dest_qp_num, imm_data);
		uib_qp_release_recv(&qp->frag_recv);
		uib_mr_put(qp->frag_recv_mr);
		qp->frag_recv_mr = NULL;
		qp->frag_recv_active = 0;
		qp->frag_recv_offset = 0;
		qp->last_rx_psn = psn;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn,
				 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
		return rc;
	}
	case UIB_OP_UD_SEND_ONLY:
	case UIB_OP_UD_SEND_ONLY_WITH_IMM: {
		if (body_len < UIB_DETH_BYTES)
			return -EBADMSG;
		const struct uib_deth *deth = (const struct uib_deth *)body;
		uint32_t qkey, sqp, imm_data = 0;

		uib_deth_unpack(deth, &qkey, &sqp);
		if (qkey != qp->qkey)
			return -EACCES;
		body     += UIB_DETH_BYTES;
		body_len -= UIB_DETH_BYTES;
		if (op == UIB_OP_UD_SEND_ONLY_WITH_IMM) {
			if (body_len < sizeof(imm_data))
				return -EBADMSG;
			memcpy(&imm_data, body, sizeof(imm_data));
			body += sizeof(imm_data);
			body_len -= sizeof(imm_data);
		}
		struct uib_rq_slot slot;

		if (pop_recv_slot(qp, &slot) < 0)
			return -ENOENT;
		size_t consumed = 0;
		int rc = uib_qp_scatter_recv(qp, &slot, 0, body, body_len,
					     &consumed);
		uib_qp_emit_recv_slot(qp, &slot,
				      rc < 0 ? UIB_WC_LOC_PROT_ERR :
				      UIB_WC_SUCCESS, UIB_WC_RECV,
				      (uint32_t)consumed, sqp, imm_data);
		uib_qp_release_recv(&slot);
		qp->rq_psn++;
		return rc;
	}
	case UIB_OP_RDMA_WRITE_ONLY:
	case UIB_OP_RDMA_WRITE_ONLY_WITH_IMM: {
		struct uib_rq_slot slot;
		uint32_t imm_data = 0;
		int with_imm_op = op == UIB_OP_RDMA_WRITE_ONLY_WITH_IMM;

		if (body_len < UIB_RETH_BYTES) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		const struct uib_reth *reth = (const struct uib_reth *)body;
		uint64_t va; uint32_t rkey, rlen;

		uib_reth_unpack(reth, &va, &rkey, &rlen);
		body     += UIB_RETH_BYTES;
		body_len -= UIB_RETH_BYTES;
		if (with_imm_op) {
			if (body_len < sizeof(imm_data))
				return -EBADMSG;
			memcpy(&imm_data, body, sizeof(imm_data));
			body += sizeof(imm_data);
			body_len -= sizeof(imm_data);
			if (pop_recv_slot(qp, &slot) < 0) {
				if (is_rc)
					send_ack(qp, src, psn, UIB_AETH_RNR);
				return -ENOENT;
			}
		}
		if (rlen > body_len)
			rlen = body_len;
		struct uib_mr *mr = uib_mr_check_remote(qp->pd, rkey, va, rlen,
							UIB_ACCESS_REMOTE_WRITE);
		if (!mr) {
			if (with_imm_op)
				uib_qp_release_recv(&slot);
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EACCES;
		}
		if (uib_mr_place(mr, va, body, rlen) < 0) {
			uib_mr_put(mr);
			if (with_imm_op)
				uib_qp_release_recv(&slot);
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EIO;
		}
		qp->last_rx_psn = psn;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn, UIB_AETH_ACK);
		if (with_imm_op) {
			uib_qp_emit_recv_slot(qp, &slot, UIB_WC_SUCCESS,
					      UIB_WC_RECV_RDMA_WITH_IMM,
					      rlen, qp->dest_qp_num, imm_data);
			uib_qp_release_recv(&slot);
		}
		uib_mr_put(mr);
		return 0;
	}
	case UIB_OP_RDMA_WRITE_FIRST: {
		if (body_len < UIB_RETH_BYTES) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		const struct uib_reth *reth = (const struct uib_reth *)body;
		uint64_t va; uint32_t rkey, rlen;

		uib_reth_unpack(reth, &va, &rkey, &rlen);
		body     += UIB_RETH_BYTES;
		body_len -= UIB_RETH_BYTES;
		struct uib_mr *mr = uib_mr_check_remote(qp->pd, rkey, va, rlen,
							UIB_ACCESS_REMOTE_WRITE);
		if (!mr) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EACCES;
		}
		size_t take = body_len > rlen ? rlen : body_len;

		if (uib_mr_place(mr, va, body, take) < 0) {
			uib_mr_put(mr);
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EIO;
		}
		uib_mr_put(qp->frag_write_mr);
		qp->frag_write_va     = va;
		qp->frag_write_total  = rlen;
		qp->frag_write_offset = (uint32_t)take;
		qp->frag_write_active = 1;
		qp->frag_write_mr     = mr;
		qp->last_rx_psn = psn;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + 1;
		return 0;
	}
	case UIB_OP_RDMA_WRITE_MIDDLE: {
		if (!qp->frag_write_active) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		uint32_t remain = qp->frag_write_total - qp->frag_write_offset;
		size_t take = body_len > remain ? remain : body_len;

		if (uib_mr_place(qp->frag_write_mr,
				 qp->frag_write_va + qp->frag_write_offset,
				 body, take) < 0) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EIO;
		}
		qp->frag_write_offset += (uint32_t)take;
		qp->last_rx_psn = psn;
		qp->rq_psn = psn + 1;
		return 0;
	}
	case UIB_OP_RDMA_WRITE_LAST:
	case UIB_OP_RDMA_WRITE_LAST_WITH_IMM: {
		struct uib_rq_slot slot;
		uint32_t imm_data = 0;
		int with_imm_op = op == UIB_OP_RDMA_WRITE_LAST_WITH_IMM;

		if (!qp->frag_write_active) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		if (with_imm_op) {
			if (body_len < sizeof(imm_data))
				return -EBADMSG;
			memcpy(&imm_data, body, sizeof(imm_data));
			body += sizeof(imm_data);
			body_len -= sizeof(imm_data);
			if (pop_recv_slot(qp, &slot) < 0) {
				if (is_rc)
					send_ack(qp, src, psn, UIB_AETH_RNR);
				return -ENOENT;
			}
		}
		uint32_t remain = qp->frag_write_total - qp->frag_write_offset;
		size_t take = body_len > remain ? remain : body_len;

		if (uib_mr_place(qp->frag_write_mr,
				 qp->frag_write_va + qp->frag_write_offset,
				 body, take) < 0) {
			if (with_imm_op)
				uib_qp_release_recv(&slot);
			return -EIO;
		}
		qp->frag_write_offset += (uint32_t)take;
		qp->frag_write_active = 0;
		uib_mr_put(qp->frag_write_mr);
		qp->frag_write_mr = NULL;
		qp->last_rx_psn = psn;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn, UIB_AETH_ACK);
		if (with_imm_op) {
			uib_qp_emit_recv_slot(qp, &slot, UIB_WC_SUCCESS,
					      UIB_WC_RECV_RDMA_WITH_IMM,
					      qp->frag_write_total,
					      qp->dest_qp_num, imm_data);
			uib_qp_release_recv(&slot);
		}
		return 0;
	}
	case UIB_OP_RDMA_READ_REQ: {
		if (body_len < UIB_RETH_BYTES)
			return -EBADMSG;
		const struct uib_reth *reth = (const struct uib_reth *)body;
		uint64_t va; uint32_t rkey, rlen;

		uib_reth_unpack(reth, &va, &rkey, &rlen);
		send_read_response(qp, &qp->remote_addr, psn, va, rkey, rlen);
		uint32_t mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
		uint32_t span = (rlen + mtu - 1) / mtu;

		if (span == 0)
			span = 1;
		qp->last_rx_psn = psn + span - 1;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + span;
		return 0;
	}
	case UIB_OP_RDMA_READ_RESP_ONLY: {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		body += UIB_AETH_BYTES; body_len -= UIB_AETH_BYTES;
		struct uib_outstanding *o = find_read_by_psn(qp, psn);

		if (!o)
			return 0;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, o->read_sgl, o->read_num_sge,
				    0, body, body_len, &consumed);
		detach_outstanding(qp, o);
		emit_send_wc(qp, o->wr_id, o->wr_opcode, o->signaled,
			     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			     (uint32_t)consumed);
		free_outstanding(o);
		return rc;
	}
	case UIB_OP_RDMA_READ_RESP_FIRST: {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		body += UIB_AETH_BYTES; body_len -= UIB_AETH_BYTES;
		struct uib_outstanding *o = find_read_by_psn(qp, psn);

		if (!o)
			return 0;
		qp->frag_read_out = o;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, o->read_sgl, o->read_num_sge,
				    0, body, body_len, &consumed);
		qp->frag_read_offset = (uint32_t)consumed;
		return rc;
	}
	case UIB_OP_RDMA_READ_RESP_MIDDLE: {
		struct uib_outstanding *o = qp->frag_read_out;

		if (!o)
			return 0;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, o->read_sgl, o->read_num_sge,
				    qp->frag_read_offset, body, body_len, &consumed);
		qp->frag_read_offset += (uint32_t)consumed;
		return rc;
	}
	case UIB_OP_RDMA_READ_RESP_LAST: {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		body += UIB_AETH_BYTES; body_len -= UIB_AETH_BYTES;
		struct uib_outstanding *o = qp->frag_read_out;

		if (!o)
			return 0;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, o->read_sgl, o->read_num_sge,
				    qp->frag_read_offset, body, body_len, &consumed);
		uint32_t total = qp->frag_read_offset + (uint32_t)consumed;

		detach_outstanding(qp, o);
		emit_send_wc(qp, o->wr_id, o->wr_opcode, o->signaled,
			     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			     total);
		free_outstanding(o);
		qp->frag_read_out = NULL;
		qp->frag_read_offset = 0;
		return rc;
	}
	case UIB_OP_ACKNOWLEDGE: {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		const struct uib_aeth *aeth = (const struct uib_aeth *)body;
		uint8_t syndrome; uint32_t msn;

		uib_aeth_unpack(aeth, &syndrome, &msn);
		(void)msn;
		if ((syndrome & UIB_AETH_CODE_MASK) == UIB_AETH_ACK_CODE) {
			ack_outstanding(qp, psn);
		} else if ((syndrome & UIB_AETH_CODE_MASK) == UIB_AETH_RNR_CODE) {
		} else if (syndrome != UIB_AETH_NAK_PSN_SEQ) {
			pthread_mutex_lock(&qp->lock);
			qp->state = UIB_QPS_ERR;
			pthread_mutex_unlock(&qp->lock);
			fail_outstanding(qp,
					 syndrome == UIB_AETH_NAK_REM_ACC
					     ? UIB_WC_REM_ACCESS_ERR
					     : UIB_WC_REM_OP_ERR);
		}
		return 0;
	}
	default:
		return -ENOTSUP;
	}
}

/**
 * uib_qp_handle_packet - Dispatch a received protocol packet to a QP.
 * @qp: Queue pair
 * @src: Source transport address
 * @pkt: Received protocol packet
 * @len: Length of the byte range
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_qp_handle_packet(struct uib_qp *qp, const struct sockaddr_in *src,
			 const void *pkt, size_t len)
{
	if (!qp || !qp->ctx->pops || !qp->ctx->pops->handle_packet)
		return -EOPNOTSUPP;

	return qp->ctx->pops->handle_packet(qp, src, pkt, len);
}

static void rocev2_retransmit_list(struct uib_qp *qp,
				  struct uib_outstanding *head,
				  uint64_t now, uint64_t *next_deadline)
{
	struct uib_outstanding *next;
	struct uib_outstanding *out;

	for (out = head; out; out = next) {
		next = out->next;
		if ((int64_t)(now - out->deadline_ns) >= 0) {
			if (out->retries >= UIB_MAX_RETRIES) {
				if (out->congestion_inflight)
					uib_congestion_on_loss(&qp->congestion,
							       out->nominal_len);
				if (out->is_final || out->is_read)
					emit_send_wc(qp, out->wr_id,
						     out->wr_opcode, 1,
						     UIB_WC_RETRY_EXC_ERR, 0);
				outstanding_unlink(qp, out);
				free_outstanding(out);
				qp->state = UIB_QPS_ERR;
				continue;
			}
			if (out->congestion_inflight) {
				uib_congestion_on_loss(&qp->congestion,
						       out->nominal_len);
				out->congestion_inflight = 0;
			}
			if (!uib_congestion_can_send(&qp->congestion,
						     out->nominal_len)) {
				out->deadline_ns = now + UIB_RTO_NS;
				if (out->deadline_ns < *next_deadline)
					*next_deadline = out->deadline_ns;
				continue;
			}
			out->tx_time_ns = now;
			uib_congestion_on_send(&qp->congestion,
					       out->nominal_len);
			out->congestion_inflight = 1;
			if (send_outstanding(qp, out) < 0) {
				uib_congestion_on_loss(&qp->congestion,
						       out->nominal_len);
				out->congestion_inflight = 0;
				out->deadline_ns = now + UIB_RTO_NS;
				if (out->deadline_ns < *next_deadline)
					*next_deadline = out->deadline_ns;
				continue;
			}
			out->retries++;
			out->deadline_ns = now + UIB_RTO_NS;
		}
		if (out->deadline_ns < *next_deadline)
			*next_deadline = out->deadline_ns;
	}
}

void uib_rocev2_retransmit_tick(struct uib_qp *qp, uint64_t now_ns)
{
	uint64_t next_deadline = UINT64_MAX;

	pthread_mutex_lock(&qp->lock);
	rocev2_retransmit_list(qp, qp->out_head, now_ns, &next_deadline);
	rocev2_retransmit_list(qp, qp->read_head, now_ns, &next_deadline);
	if (next_deadline != UINT64_MAX)
		uib_qp_retry_schedule(qp, next_deadline);
	else
		uib_qp_retry_cancel(qp);
	pthread_mutex_unlock(&qp->lock);
}

/**
 * uib_qp_retransmit_tick - Run protocol retransmission work for a QP.
 * @qp: Queue pair
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_retransmit_tick(struct uib_qp *qp, uint64_t now_ns)
{
	if (qp && qp->ctx->pops && qp->ctx->pops->retransmit_tick)
		qp->ctx->pops->retransmit_tick(qp, now_ns);
}
