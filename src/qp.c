/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/ip.h>

#include <urcu-bp.h>

#include "uib/qp.h"
#include "uib/pd.h"
#include "uib/mr.h"
#include "uib/cq.h"
#include "uib/context.h"
#include "uib/env.h"
#include "uib/transport.h"
#include "transport_internal.h"
#include "uib/bth.h"
#include "uib/dcqcn.h"
#include "uib/uet_nscc.h"
#include "congestion_internal.h"
#include "host_zc_internal.h"
#include "ptr_ring.h"
#include "qp_internal.h"
#include "utils.h"

#define UIB_PKT_MAX (UIB_PMTU + 64)
#define UIB_OUT_POOL_MAX 1024
#define UIB_OUT_POOL_CLASSES 3
#define UIB_TX_BATCH_STACK 64
#define UIB_ATOMIC_ACK_BYTES 8

struct uib_atomic_eth {
	uint64_t va;
	uint32_t rkey;
	uint64_t swap_data;
	uint64_t compare_data;
} __packed;

struct uib_outstanding_pool {
	struct uib_ptr_ring free[UIB_OUT_POOL_CLASSES];
	uint32_t            refs;
	int                 closing;
};

struct uib_srq {
	struct uib_pd      *pd;
	struct uib_rq_slot *rq;
	uint32_t            rq_size;
	uint32_t            rq_head;
	uint32_t            rq_tail;
	uint32_t            max_sge;
	uint32_t            srq_limit;
	uint32_t            refcnt;
	pthread_mutex_t     lock;
};

static int psn_cmp(uint32_t a, uint32_t b);

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
	qp->srq          = attr->srq;
	qp->qp_type      = attr->qp_type;
	qp->state        = UIB_QPS_RESET;
	qp->max_send_sge = attr->max_send_sge ? attr->max_send_sge : 1;
	qp->max_recv_sge = attr->srq ? attr->srq->max_sge :
		(attr->max_recv_sge ? attr->max_recv_sge : 1);
	qp->path_mtu     = UIB_PMTU;
	qp->out_pool     = outstanding_pool_create(attr->max_send_wr);
	if (!qp->out_pool) {
		free(qp);
		return NULL;
	}

	if (attr->srq) {
		if (attr->srq->pd != pd) {
			outstanding_pool_destroy(qp->out_pool);
			free(qp);
			errno = EINVAL;
			return NULL;
		}
		pthread_mutex_lock(&attr->srq->lock);
		attr->srq->refcnt++;
		pthread_mutex_unlock(&attr->srq->lock);
	} else {
		qp->rq_size = attr->max_recv_wr ?
			(int)attr->max_recv_wr + 1 : 65;
		qp->rq = calloc(qp->rq_size, sizeof(*qp->rq));
		if (!qp->rq) {
			outstanding_pool_destroy(qp->out_pool);
			free(qp);
			return NULL;
		}
	}

	pthread_mutex_init(&qp->lock, NULL);

	qp->qp_num = uib_register_qp(pd->ctx, qp);
	if (qp->qp_num == 0) {
		if (qp->srq) {
			pthread_mutex_lock(&qp->srq->lock);
			qp->srq->refcnt--;
			pthread_mutex_unlock(&qp->srq->lock);
		}
		outstanding_pool_destroy(qp->out_pool);
		free(qp->rq);
		free(qp);
		errno = ENOSPC;
		return NULL;
	}
	rc = qp->ctx->pops->qp_init ? qp->ctx->pops->qp_init(qp) : 0;
	if (rc < 0) {
		uib_unregister_qp(pd->ctx, qp);
		if (qp->srq) {
			pthread_mutex_lock(&qp->srq->lock);
			qp->srq->refcnt--;
			pthread_mutex_unlock(&qp->srq->lock);
		}
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
	out->sent = 0;
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
	struct uib_uet_nscc_config nscc_config;
	struct uib_dcqcn_config dcqcn_config;
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

		uib_uet_nscc_config_default(&nscc_config, mtu);
		max_wnd = getenv(UIB_ENV_ROCEV2_NSCC_MAX_WND);
		if (max_wnd) {
			errno = 0;
			value = strtoull(max_wnd, &end, 0);
			if (errno || end == max_wnd || *end || value < mtu ||
			    value > UINT32_MAX)
				return -EINVAL;
			nscc_config.max_wnd = (uint32_t)value;
			nscc_config.bdp = nscc_config.max_wnd;
		}
		configuration = &nscc_config;
	} else if (!strcmp(cc, "dcqcn")) {
		if (uib_dcqcn_config_from_env(&dcqcn_config, mtu))
			return -EINVAL;
		configuration = &dcqcn_config;
	}
	return uib_congestion_init(&qp->congestion, cc, configuration, mtu,
				   uib_now_ns());
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
	if (!qp->srq) {
		for (i = 0; i < qp->rq_size; i++) {
			if (qp->rq[i].valid)
				uib_qp_release_recv(&qp->rq[i]);
		}
	}
	pthread_mutex_unlock(&qp->lock);

	pthread_mutex_destroy(&qp->lock);
	if (qp->srq) {
		pthread_mutex_lock(&qp->srq->lock);
		qp->srq->refcnt--;
		pthread_mutex_unlock(&qp->srq->lock);
	}
	if (qp->pd)
		qp->pd->refcnt--;
	free(qp->rq);
	outstanding_pool_destroy(qp->out_pool);
	free(qp);
	return 0;
}

/**
 * uib_create_srq - Create a receive queue shared by multiple QPs.
 * @pd: Protection domain
 * @attr: Shared receive queue capacity and scatter/gather limits
 * @uib-api: Queue pairs and data transfer
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_srq *uib_create_srq(struct uib_pd *pd,
			       const struct uib_srq_init_attr *attr)
{
	struct uib_srq *srq;

	if (!pd || !attr || !attr->max_wr || !attr->max_sge ||
	    attr->srq_limit > attr->max_wr ||
	    attr->max_wr == UINT32_MAX) {
		errno = EINVAL;
		return NULL;
	}
	srq = calloc(1, sizeof(*srq));
	if (!srq)
		return NULL;
	srq->rq_size = attr->max_wr + 1;
	srq->rq = calloc(srq->rq_size, sizeof(*srq->rq));
	if (!srq->rq) {
		free(srq);
		return NULL;
	}
	srq->pd = pd;
	srq->max_sge = attr->max_sge;
	srq->srq_limit = attr->srq_limit;
	pthread_mutex_init(&srq->lock, NULL);
	pd->refcnt++;
	return srq;
}

/**
 * uib_modify_srq - Change supported shared receive queue attributes.
 * @srq: Shared receive queue
 * @attr: New attribute values
 * @attr_mask: Bitmask selecting attributes to change
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_modify_srq(struct uib_srq *srq, const struct uib_srq_attr *attr,
		   int attr_mask)
{
	if (!srq || !attr ||
	    (attr_mask & ~(UIB_SRQ_MAX_WR | UIB_SRQ_LIMIT)))
		return -EINVAL;
	pthread_mutex_lock(&srq->lock);
	if ((attr_mask & UIB_SRQ_MAX_WR) &&
	    attr->max_wr != srq->rq_size - 1) {
		pthread_mutex_unlock(&srq->lock);
		return -EOPNOTSUPP;
	}
	if ((attr_mask & UIB_SRQ_LIMIT) &&
	    attr->srq_limit > srq->rq_size - 1) {
		pthread_mutex_unlock(&srq->lock);
		return -EINVAL;
	}
	if (attr_mask & UIB_SRQ_LIMIT)
		srq->srq_limit = attr->srq_limit;
	pthread_mutex_unlock(&srq->lock);
	return 0;
}

/**
 * uib_query_srq - Read shared receive queue capacity and limits.
 * @srq: Shared receive queue
 * @attr: Output attribute structure
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_query_srq(struct uib_srq *srq, struct uib_srq_attr *attr)
{
	if (!srq || !attr)
		return -EINVAL;
	pthread_mutex_lock(&srq->lock);
	attr->max_wr = srq->rq_size - 1;
	attr->max_sge = srq->max_sge;
	attr->srq_limit = srq->srq_limit;
	pthread_mutex_unlock(&srq->lock);
	return 0;
}

/**
 * uib_destroy_srq - Destroy an unused shared receive queue.
 * @srq: Shared receive queue
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success, or -EBUSY while a QP still uses the SRQ.
 */
int uib_destroy_srq(struct uib_srq *srq)
{
	uint32_t i;

	if (!srq)
		return -EINVAL;
	pthread_mutex_lock(&srq->lock);
	if (srq->refcnt) {
		pthread_mutex_unlock(&srq->lock);
		return -EBUSY;
	}
	for (i = 0; i < srq->rq_size; i++) {
		if (srq->rq[i].valid)
			uib_qp_release_recv(&srq->rq[i]);
	}
	pthread_mutex_unlock(&srq->lock);
	pthread_mutex_destroy(&srq->lock);
	if (srq->pd)
		srq->pd->refcnt--;
	free(srq->rq);
	free(srq);
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
	size_t off_in_sge;
	size_t consumed = 0;
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
	off_in_sge = skip;

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

void qp_emit_send_vendor(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status st, uint32_t byte_len,
			 uint32_t vendor_err)
{
	struct uib_wc wc = {0};

	if (!qp->send_cq)
		return;
	if (st == UIB_WC_SUCCESS && !signaled)
		return;
	wc.wr_id    = wr_id;
	wc.status   = st;
	wc.qp_num   = qp->qp_num;
	wc.byte_len = byte_len;
	wc.vendor_err = vendor_err;
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

static void emit_send_wc(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status status, uint32_t byte_len)
{
	qp_emit_send_vendor(qp, wr_id, wr_op, signaled, status, byte_len, 0);
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
	struct uib_wc wc = {0};

	if (!qp->recv_cq)
		return -EINVAL;

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
	out->deadline_ns = 0;
	if (*tail)
		(*tail)->next = out;
	else
		*head = out;
	*tail = out;
	outstanding_index_add(qp, out);
	qp->out_count++;
}

static void outstanding_retry_reschedule(struct uib_qp *qp)
{
	struct uib_outstanding *out;
	uint64_t deadline = UINT64_MAX;

	for (out = qp->out_head; out; out = out->next) {
		if (out->sent && out->deadline_ns < deadline)
			deadline = out->deadline_ns;
	}
	for (out = qp->read_head; out; out = out->next) {
		if (out->sent && out->deadline_ns < deadline)
			deadline = out->deadline_ns;
	}
	if (deadline != UINT64_MAX)
		uib_qp_retry_schedule(qp, deadline);
	else
		uib_qp_retry_cancel(qp);
}

static struct uib_outstanding *first_unsent(struct uib_outstanding *out)
{
	while (out && out->sent)
		out = out->next;
	return out;
}

static struct uib_outstanding *rocev2_next_unsent(struct uib_qp *qp)
{
	struct uib_outstanding *read = first_unsent(qp->read_head);
	struct uib_outstanding *send = first_unsent(qp->out_head);

	if (!send)
		return read;
	if (!read)
		return send;
	return psn_cmp(send->psn, read->psn) <= 0 ? send : read;
}

static void rocev2_request_ack(struct uib_outstanding *out)
{
	struct uib_bth *bth = out->pkt;
	uint32_t apsn = ntohl(bth->apsn) | UIB_BTH_ACK_REQ;
	uint32_t crc;

	bth->apsn = htonl(apsn);
	crc = htonl(uib_icrc(out->pkt, out->pkt_len - UIB_ICRC_BYTES));
	memcpy((uint8_t *)out->pkt + out->pkt_len - UIB_ICRC_BYTES, &crc,
	       sizeof(crc));
}

static int rocev2_drain_locked(struct uib_qp *qp)
{
	struct uib_transport_ref refs[UIB_TX_BATCH_STACK];
	struct uib_outstanding *outstanding[UIB_TX_BATCH_STACK];
	struct uib_congestion congestion_before = qp->congestion;
	struct uib_outstanding *out;
	uint64_t transmit_time = uib_now_ns();
	uint32_t count = 0;
	uint32_t i;
	int rc;

	while (count < UIB_TX_BATCH_STACK) {
		out = rocev2_next_unsent(qp);
		if (!out || !uib_congestion_can_send(&qp->congestion,
						       out->nominal_len))
			break;
		out->sent = 1;
		out->tx_time_ns = transmit_time;
		uib_congestion_on_send(&qp->congestion, out->nominal_len);
		out->congestion_inflight = 1;
		if (uib_congestion_ack_request(&qp->congestion,
					       out->nominal_len))
			rocev2_request_ack(out);
		refs[count].dst = &out->dst;
		refs[count].buf = out->pkt;
		refs[count].release = free_outstanding;
		refs[count].opaque = out;
		refs[count].len = out->pkt_len;
		refs[count].entropy = 0;
		outstanding[count] = out;
		__atomic_add_fetch(&out->refs, 1, __ATOMIC_RELAXED);
		count++;
	}
	if (!count) {
		if (rocev2_next_unsent(qp)) {
			uint64_t next = uib_congestion_next_send_ns(
				&qp->congestion);

			if (!next)
				next = transmit_time + qp->ctx->retry_timeout_ns;
			else if (next <= transmit_time)
				next = transmit_time + 1;
			uib_qp_retry_schedule(qp, next);
		}
		return 0;
	}
	rc = uib_transport_send_ref_batch(qp->ctx, refs, count);
	if (rc < 0) {
		qp->congestion = congestion_before;
		for (i = 0; i < count; i++) {
			outstanding[i]->sent = 0;
			outstanding[i]->congestion_inflight = 0;
		}
		uib_qp_retry_schedule(qp, transmit_time +
				      qp->ctx->retry_timeout_ns);
		return rc;
	}
	for (i = 0; i < count; i++) {
		outstanding[i]->deadline_ns = transmit_time +
					      qp->ctx->retry_timeout_ns;
		if (!qp->retry_scheduled ||
		    outstanding[i]->deadline_ns < qp->retry_deadline_ns)
			uib_qp_retry_schedule(qp,
					      outstanding[i]->deadline_ns);
	}
	if (rocev2_next_unsent(qp)) {
		uint64_t next = uib_congestion_next_send_ns(&qp->congestion);

		if (next && next < qp->retry_deadline_ns)
			uib_qp_retry_schedule(qp, next);
	}
	return 0;
}

static void response_target(const struct uib_qp *qp,
			    const struct sockaddr_in *source,
			    struct sockaddr_in *target)
{
	*target = *source;
	if (qp->remote_addr.sin_family == AF_INET) {
		target->sin_addr = qp->remote_addr.sin_addr;
		target->sin_port = qp->remote_addr.sin_port;
	}
}

static void send_ack(struct uib_qp *qp, const struct sockaddr_in *dst,
		     uint32_t ack_psn, uint8_t syndrome)
{
	struct sockaddr_in target;
	uint8_t pkt[UIB_BTH_BYTES + UIB_AETH_BYTES + UIB_ICRC_BYTES];
	struct uib_bth *bth = (struct uib_bth *)pkt;
	struct uib_aeth *aeth = (struct uib_aeth *)(pkt + UIB_BTH_BYTES);
	uint32_t crc;
	uint32_t bcrc;

	response_target(qp, dst, &target);

	uib_bth_pack(bth, UIB_OP_ACKNOWLEDGE, UIB_DEFAULT_PKEY,
		     qp->dest_qp_num, ack_psn, 0);
	uib_aeth_pack(aeth, syndrome, ack_psn);
	crc = uib_icrc(pkt, UIB_BTH_BYTES + UIB_AETH_BYTES);
	bcrc = htonl(crc);

	memcpy(pkt + UIB_BTH_BYTES + UIB_AETH_BYTES, &bcrc, UIB_ICRC_BYTES);
	uib_transport_send(qp->ctx, &target, pkt, sizeof(pkt));
}

static void send_cnp(struct uib_qp *qp, const struct sockaddr_in *dst)
{
	uint8_t packet[UIB_BTH_BYTES + UIB_ICRC_BYTES];
	struct sockaddr_in target;
	uint32_t wire_crc;

	response_target(qp, dst, &target);
	uib_bth_pack((struct uib_bth *)packet, UIB_OP_CNP,
		     UIB_DEFAULT_PKEY, qp->dest_qp_num, 0, 0);
	wire_crc = htonl(uib_icrc(packet, UIB_BTH_BYTES));
	memcpy(packet + UIB_BTH_BYTES, &wire_crc, sizeof(wire_crc));
	uib_transport_send(qp->ctx, &target, packet, sizeof(packet));
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
	struct uib_outstanding *out;
	struct sockaddr_in dst;
	size_t packet_len;
	uint32_t n_resp;
	uint32_t mtu;
	uint32_t psn;
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
	out->nominal_len = rocev2_nominal_len(qp, packet_len);
	rc = send_one_packet(qp, wr, UIB_OP_RDMA_READ_REQ, 1, 1, NULL, 0,
			     wr->rdma.remote_addr, wr->rdma.rkey,
			     (uint32_t)total, out, out->pkt, out->pkt_len, &psn,
			     &out->pkt_len, &dst, 1);
	if (rc < 0) {
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
	rocev2_drain_locked(qp);
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static void atomic_eth_pack(struct uib_atomic_eth *atomic,
			    const struct uib_send_wr *wr)
{
	atomic->va = htobe64(wr->atomic.remote_addr);
	atomic->rkey = htonl(wr->atomic.rkey);
	atomic->swap_data = htobe64(wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ?
		wr->atomic.compare_add : wr->atomic.swap);
	atomic->compare_data = htobe64(wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ?
		wr->atomic.compare_add : 0);
}

static int post_atomic(struct uib_qp *qp, struct uib_send_wr *wr)
{
	struct uib_atomic_eth *atomic;
	struct uib_outstanding *out;
	struct uib_mr *mr;
	struct uib_bth *bth;
	uint32_t wire_crc;
	uint32_t crc;
	uint32_t psn;
	uint8_t op;
	size_t packet_len;

	if (qp->qp_type != UIB_QPT_RC || wr->num_sge != 1 ||
	    wr->sg_list[0].length < UIB_ATOMIC_ACK_BYTES ||
	    (wr->sg_list[0].addr & (UIB_ATOMIC_ACK_BYTES - 1)) ||
	    (wr->atomic.remote_addr & (UIB_ATOMIC_ACK_BYTES - 1)))
		return -EINVAL;
	mr = uib_mr_lookup_local(qp->pd, wr->sg_list[0].lkey,
				 (void *)(uintptr_t)wr->sg_list[0].addr,
				 UIB_ATOMIC_ACK_BYTES, UIB_ACCESS_LOCAL_WRITE);
	if (!mr)
		return -EACCES;
	uib_mr_put(mr);

	packet_len = UIB_BTH_BYTES + sizeof(*atomic) + UIB_ICRC_BYTES;
	out = alloc_outstanding(qp, packet_len);
	if (!out)
		return -ENOMEM;
	out->read_sgl = calloc(1, sizeof(*out->read_sgl));
	if (!out->read_sgl) {
		free_outstanding(out);
		return -ENOMEM;
	}
	*out->read_sgl = wr->sg_list[0];
	out->read_num_sge = 1;
	out->read_total_len = UIB_ATOMIC_ACK_BYTES;

	op = wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ? UIB_OP_FETCH_ADD :
		UIB_OP_COMPARE_SWAP;
	bth = out->pkt;
	atomic = (struct uib_atomic_eth *)((uint8_t *)out->pkt +
					     UIB_BTH_BYTES);
	pthread_mutex_lock(&qp->lock);
	psn = qp->sq_psn++;
	uib_bth_pack(bth, op, UIB_DEFAULT_PKEY, qp->dest_qp_num, psn, 1);
	atomic_eth_pack(atomic, wr);
	crc = uib_icrc(out->pkt, packet_len - UIB_ICRC_BYTES);
	wire_crc = htonl(crc);
	memcpy((uint8_t *)out->pkt + packet_len - UIB_ICRC_BYTES,
	       &wire_crc, sizeof(wire_crc));
	out->nominal_len = rocev2_nominal_len(qp, packet_len);
	out->psn = psn;
	out->wire_opcode = op;
	out->signaled = !!(wr->send_flags & UIB_SEND_SIGNALED);
	out->is_read = 1;
	out->is_final = 1;
	out->wr_id = wr->wr_id;
	out->wr_opcode = wr->opcode;
	out->dst = qp->remote_addr;
	out->pkt_len = packet_len;
	enqueue_outstanding(qp, out);
	rocev2_drain_locked(qp);
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static uint8_t pick_opcode(enum uib_qp_type qpt, enum uib_wr_opcode op,
			   int with_imm, uint32_t n, uint32_t i)
{
	int is_ud = (qpt == UIB_QPT_UD);
	int is_only = n == 1;
	int is_first = i == 0;
	int is_last = i == n - 1;

	if (is_ud)
		return with_imm ? UIB_OP_UD_SEND_ONLY_WITH_IMM :
			UIB_OP_UD_SEND_ONLY;

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

struct rocev2_send_data {
	const uint8_t *message;
	uint8_t *owned_message;
	struct uib_mr *mr;
	size_t total;
};

static int rocev2_send_total(const struct uib_send_wr *wr, size_t *total)
{
	uint32_t i;

	*total = 0;
	for (i = 0; i < (uint32_t)wr->num_sge; i++) {
		if (SIZE_MAX - *total < wr->sg_list[i].length)
			return -EMSGSIZE;
		*total += wr->sg_list[i].length;
	}
	return 0;
}

static void rocev2_send_data_release(struct rocev2_send_data *data)
{
	uib_mr_put(data->mr);
	free(data->owned_message);
}

static int rocev2_prepare_send_data(struct uib_qp *qp,
				    struct uib_send_wr *wr,
				    struct rocev2_send_data *data)
{
	size_t copied;
	uint32_t i;
	int rc;

	if (!data->total)
		return 0;
	if (wr->send_flags & UIB_SEND_INLINE) {
		if (wr->num_sge == 1) {
			data->message = (const uint8_t *)(uintptr_t)
				wr->sg_list[0].addr;
			return 0;
		}
		data->owned_message = malloc(data->total);
		if (!data->owned_message)
			return -ENOMEM;
		copied = 0;
		for (i = 0; i < (uint32_t)wr->num_sge; i++) {
			memcpy(data->owned_message + copied,
			       (const void *)(uintptr_t)wr->sg_list[i].addr,
			       wr->sg_list[i].length);
			copied += wr->sg_list[i].length;
		}
		data->message = data->owned_message;
		return 0;
	}
	if (wr->num_sge == 1) {
		data->mr = uib_mr_lookup_local(
			qp->pd, wr->sg_list[0].lkey,
			(const void *)(uintptr_t)wr->sg_list[0].addr,
			wr->sg_list[0].length, 0);
		if (!data->mr)
			return -EACCES;
		if (data->mr->kind == UIB_MR_HOST) {
			data->message = (const uint8_t *)(uintptr_t)
				wr->sg_list[0].addr;
			return 0;
		}
		uib_mr_put(data->mr);
		data->mr = NULL;
	}
	data->owned_message = malloc(data->total);
	if (!data->owned_message)
		return -ENOMEM;
	rc = gather_total(qp->pd, wr->sg_list, wr->num_sge,
			  data->owned_message, data->total);
	if (rc < 0)
		return rc;
	data->message = data->owned_message;
	return 0;
}

static void rocev2_init_outstanding(struct uib_qp *qp,
				    struct uib_send_wr *wr,
				    struct uib_outstanding *out,
				    const struct sockaddr_in *dst,
				    size_t packet_len, uint32_t psn,
				    uint8_t opcode, int is_last)
{
	out->nominal_len = rocev2_nominal_len(qp, packet_len);
	out->psn = psn;
	out->wire_opcode = opcode;
	out->signaled = !!(wr->send_flags & UIB_SEND_SIGNALED);
	out->is_final = is_last;
	out->wr_id = wr->wr_id;
	out->wr_opcode = wr->opcode;
	out->dst = *dst;
	out->pkt_len = packet_len;
}

static int rocev2_build_send_batch(struct uib_qp *qp,
				    struct uib_send_wr *wr,
				    const struct rocev2_send_data *data,
				    struct uib_transport_ref *refs,
				    uint32_t packets, uint32_t mtu,
				    uint32_t *built)
{
	uint8_t stack_packet[UIB_PKT_MAX];
	struct uib_outstanding *out;
	struct sockaddr_in dst;
	uint8_t *packet;
	size_t packet_len;
	size_t chunk;
	size_t offset;
	uint32_t psn;
	uint32_t i;
	uint8_t opcode;
	int is_first;
	int is_last;
	int is_rc;
	int with_imm;
	int rc;

	is_rc = qp->qp_type == UIB_QPT_RC;
	with_imm = wr->opcode == UIB_WR_SEND_WITH_IMM ||
		wr->opcode == UIB_WR_RDMA_WRITE_WITH_IMM;
	*built = 0;
	for (i = 0; i < packets; i++) {
		offset = (size_t)i * mtu;
		chunk = offset + mtu > data->total ? data->total - offset : mtu;
		is_first = i == 0;
		is_last = i == packets - 1;
		opcode = pick_opcode(qp->qp_type, wr->opcode, with_imm,
				     packets, i);
		if (opcode == UIB_BTH_INVALID_OPCODE)
			return -EINVAL;
		packet_len = packet_length(qp, opcode, is_first, chunk);
		out = NULL;
		packet = stack_packet;
		if (is_rc) {
			out = alloc_outstanding(qp, packet_len);
			if (!out)
				return -ENOMEM;
			packet = out->pkt;
		}
		rc = send_one_packet(qp, wr, opcode, is_first, is_last,
				     data->message ? data->message + offset : NULL,
				     chunk, wr->rdma.remote_addr,
				     wr->rdma.rkey, (uint32_t)data->total, out,
				     packet, packet_len, &psn, &packet_len, &dst,
				     is_rc);
		if (rc < 0) {
			free_outstanding(out);
			return rc;
		}
		if (!is_rc)
			continue;
		rocev2_init_outstanding(qp, wr, out, &dst, packet_len, psn,
					   opcode, is_last);
		refs[*built].dst = &out->dst;
		refs[*built].buf = out->pkt;
		refs[*built].release = free_outstanding;
		refs[*built].opaque = out;
		refs[*built].len = out->pkt_len;
		refs[*built].entropy = 0;
		(*built)++;
	}
	return 0;
}

static void rocev2_free_send_batch(struct uib_transport_ref *refs,
				    uint32_t built)
{
	struct uib_outstanding *out;
	uint32_t i;

	for (i = 0; i < built; i++) {
		out = refs[i].opaque;
		free_outstanding(out);
	}
}

static void rocev2_commit_send_batch(struct uib_qp *qp,
				      struct uib_transport_ref *refs,
				      uint32_t built)
{
	struct uib_outstanding *out;
	uint32_t i;

	for (i = 0; i < built; i++) {
		out = refs[i].opaque;
		enqueue_outstanding(qp, out);
	}
	rocev2_drain_locked(qp);
}

static int post_one_send(struct uib_qp *qp, struct uib_send_wr *wr)
{
	struct uib_transport_ref stack_refs[UIB_TX_BATCH_STACK];
	struct rocev2_send_data data = {0};
	struct uib_transport_ref *refs = stack_refs;
	uint32_t start_psn;
	uint32_t packets;
	uint32_t built;
	uint32_t mtu;
	int is_rc;
	int rc;

	rc = rocev2_send_total(wr, &data.total);
	if (rc < 0)
		return rc;
	if (wr->opcode == UIB_WR_RDMA_READ)
		return post_rdma_read(qp, wr, data.total);
	if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    wr->opcode == UIB_WR_ATOMIC_CMP_SWAP)
		return post_atomic(qp, wr);
	mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
	if (qp->qp_type == UIB_QPT_UD && data.total > mtu)
		return -EMSGSIZE;
	rc = rocev2_prepare_send_data(qp, wr, &data);
	if (rc < 0)
		goto out_data;
	packets = data.total <= mtu ? 1 :
		(uint32_t)((data.total + mtu - 1) / mtu);
	is_rc = qp->qp_type == UIB_QPT_RC;
	if (is_rc && packets > UIB_TX_BATCH_STACK) {
		refs = malloc((size_t)packets * sizeof(*refs));
		if (!refs) {
			rc = -ENOMEM;
			goto out_data;
		}
	}
	if (is_rc)
		pthread_mutex_lock(&qp->lock);
	start_psn = qp->sq_psn;
	rc = rocev2_build_send_batch(qp, wr, &data, refs, packets, mtu,
				     &built);
	if (is_rc && rc < 0)
		qp->sq_psn = start_psn;
	if (is_rc && !rc) {
		rocev2_commit_send_batch(qp, refs, built);
		built = 0;
	}
	rocev2_free_send_batch(refs, built);
	if (is_rc)
		pthread_mutex_unlock(&qp->lock);
	if (!is_rc && !rc)
		emit_send_wc(qp, wr->wr_id, wr->opcode,
			     wr->send_flags & UIB_SEND_SIGNALED,
			     UIB_WC_SUCCESS, (uint32_t)data.total);
	if (refs != stack_refs)
		free(refs);
out_data:
	rocev2_send_data_release(&data);
	return rc;
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

static int post_recv_queue(struct uib_rq_slot *rq, uint32_t rq_size,
			   uint32_t *rq_head, uint32_t *rq_tail,
			   uint32_t max_sge, pthread_mutex_t *lock,
			   struct uib_recv_wr *wr,
			   struct uib_recv_wr **bad, void *private_data,
			   void (*private_release)(void *private_data))
{
	struct uib_rq_slot *slot;
	struct uib_recv_wr *cur;
	uint32_t next;

	if (!wr)
		return -EINVAL;

	pthread_mutex_lock(lock);
	for (cur = wr; cur; cur = cur->next) {
		if (cur->num_sge < 0 || (uint32_t)cur->num_sge > max_sge ||
		    (cur->num_sge && !cur->sg_list)) {
			pthread_mutex_unlock(lock);
			if (bad)
				*bad = cur;
			return -EINVAL;
		}
		next = (*rq_head + 1) % rq_size;

		if (next == *rq_tail) {
			pthread_mutex_unlock(lock);
			if (bad)
				*bad = cur;
			return -ENOSPC;
		}
		slot = &rq[*rq_head];

		slot->wr_id = cur->wr_id;
		slot->num_sge = cur->num_sge;
		slot->heap_sge = cur->num_sge > UIB_RQ_INLINE_SGE;
		if (slot->heap_sge) {
			slot->sg_list = malloc((size_t)cur->num_sge *
					       sizeof(*slot->sg_list));
			if (!slot->sg_list) {
				pthread_mutex_unlock(lock);
				if (bad)
					*bad = cur;
				return -ENOMEM;
			}
		} else {
			slot->sg_list = slot->inline_sge;
		}
		if (cur->num_sge)
			memcpy(slot->sg_list, cur->sg_list,
			       cur->num_sge * sizeof(struct uib_sge));
		if (private_data) {
			slot->private_data = private_data;
			slot->private_release = private_release;
			private_data = NULL;
			private_release = NULL;
		}
		slot->valid = 1;
		*rq_head = next;
	}
	pthread_mutex_unlock(lock);
	return 0;
}

static int post_recv_internal(struct uib_qp *qp, struct uib_recv_wr *wr,
			      struct uib_recv_wr **bad, void *private_data,
			      void (*private_release)(void *private_data))
{
	if (!qp || qp->srq)
		return -EINVAL;
	return post_recv_queue(qp->rq, qp->rq_size, &qp->rq_head,
			       &qp->rq_tail, qp->max_recv_sge, &qp->lock,
			       wr, bad, private_data, private_release);
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

/**
 * uib_post_srq_recv - Post receive requests to a shared receive queue.
 * @srq: Shared receive queue
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_post_srq_recv(struct uib_srq *srq, struct uib_recv_wr *wr,
		      struct uib_recv_wr **bad)
{
	if (!srq)
		return -EINVAL;
	return post_recv_queue(srq->rq, srq->rq_size, &srq->rq_head,
			       &srq->rq_tail, srq->max_sge, &srq->lock,
			       wr, bad, NULL, NULL);
}

int uib_post_recv_owned(struct uib_qp *qp, struct uib_recv_wr *request,
			void *private_data,
			void (*private_release)(void *private_data))
{
	return post_recv_internal(qp, request, NULL, private_data,
				  private_release);
}

static int pop_recv_queue(struct uib_rq_slot *rq, uint32_t rq_size,
			  uint32_t *rq_head, uint32_t *rq_tail,
			  pthread_mutex_t *lock, struct uib_rq_slot *out)
{
	pthread_mutex_lock(lock);
	if (*rq_tail == *rq_head) {
		pthread_mutex_unlock(lock);
		return -ENOENT;
	}
	*out = rq[*rq_tail];
	if (!out->heap_sge)
		out->sg_list = out->inline_sge;
	rq[*rq_tail].valid = 0;
	rq[*rq_tail].sg_list = NULL;
	rq[*rq_tail].private_data = NULL;
	rq[*rq_tail].private_release = NULL;
	*rq_tail = (*rq_tail + 1) % rq_size;
	pthread_mutex_unlock(lock);
	return 0;
}

static int pop_recv_slot(struct uib_qp *qp, struct uib_rq_slot *out)
{
	if (qp->srq)
		return pop_recv_queue(qp->srq->rq, qp->srq->rq_size,
				      &qp->srq->rq_head, &qp->srq->rq_tail,
				      &qp->srq->lock, out);
	return pop_recv_queue(qp->rq, qp->rq_size, &qp->rq_head,
			      &qp->rq_tail, &qp->lock, out);
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
	uint32_t want;
	uint32_t got;

	if (len < UIB_ICRC_BYTES)
		return -1;

	memcpy(&want, pkt + len - UIB_ICRC_BYTES, UIB_ICRC_BYTES);
	want = ntohl(want);
	got = uib_icrc(pkt, len - UIB_ICRC_BYTES);
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
		.ack_time_ns = uib_now_ns(),
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
	if (qp->sequence_retry_active &&
	    psn_cmp(ack_psn, qp->sequence_retry_psn) >= 0)
		qp->sequence_retry_active = 0;
	qp->last_acked_psn = ack_psn;
	outstanding_retry_reschedule(qp);
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

static void send_atomic_ack(struct uib_qp *qp, uint32_t psn, uint64_t value)
{
	uint8_t packet[UIB_BTH_BYTES + UIB_AETH_BYTES +
		       UIB_ATOMIC_ACK_BYTES + UIB_ICRC_BYTES];
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
	uib_transport_send(qp->ctx, &qp->remote_addr, packet, sizeof(packet));
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
				.ack_time_ns = uib_now_ns(),
				.tx_time_ns = target->tx_time_ns,
				.newly_received = target->nominal_len,
				.tx_retries = target->retries,
				.retransmit = target->retries != 0,
			};

			uib_congestion_on_ack(&qp->congestion, &event);
		}
		outstanding_unlink(qp, target);
		outstanding_retry_reschedule(qp);
		rocev2_drain_locked(qp);
	}
	pthread_mutex_unlock(&qp->lock);
}

int uib_rocev2_handle_packet(struct uib_qp *qp,
			    const struct sockaddr_in *src,
			    const void *packet, size_t len)
{
	return uib_rocev2_handle_packet_meta(qp, src, packet, len, NULL);
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
	rx->qp->last_rx_psn = rx->psn;
	rx->qp->last_rx_valid = 1;
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
	order = psn_cmp(rx->psn, expected);
	if (order < 0) {
		if ((rx->opcode == UIB_OP_FETCH_ADD ||
		     rx->opcode == UIB_OP_COMPARE_SWAP) &&
		    rx->qp->atomic_ack_valid &&
		    !psn_cmp(rx->psn, rx->qp->atomic_ack_psn))
			send_atomic_ack(rx->qp, rx->psn,
					rx->qp->atomic_ack_value);
		else
			send_ack(rx->qp, rx->src, rx->qp->last_rx_psn,
				 UIB_AETH_ACK);
		return 1;
	}
	if (order > 0) {
		send_ack(rx->qp, rx->src, expected, UIB_AETH_NAK_PSN_SEQ);
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
	if (pop_recv_slot(rx->qp, &slot) < 0) {
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_RNR);
		return -ENOENT;
	}
	rc = uib_qp_scatter_recv(rx->qp, &slot, 0, body, body_len,
				 &consumed);
	uib_qp_emit_recv_slot(rx->qp, &slot,
			      rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			      UIB_WC_RECV, (uint32_t)consumed,
			      rx->qp->dest_qp_num, imm_data);
	uib_qp_release_recv(&slot);
	rocev2_rx_advance(rx);
	if (rx->is_rc)
		send_ack(rx->qp, rx->src, rx->psn,
			 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
	return rc;
}

static int rocev2_handle_send_first(struct rocev2_rx_packet *rx)
{
	struct uib_rq_slot slot;
	size_t consumed = 0;
	int rc;

	if (pop_recv_slot(rx->qp, &slot) < 0) {
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_RNR);
		return -ENOENT;
	}
	uib_mr_put(rx->qp->frag_recv_mr);
	rx->qp->frag_recv_mr = NULL;
	if (slot.num_sge == 1)
		rx->qp->frag_recv_mr = uib_mr_lookup_local(
			rx->qp->pd, slot.sg_list[0].lkey,
			(const void *)(uintptr_t)slot.sg_list[0].addr,
			slot.sg_list[0].length, 0);
	rc = scatter_recv_fragment(rx->qp, &slot, 0, rx->body,
				   rx->body_len, &consumed);
	rx->qp->frag_recv = slot;
	if (!rx->qp->frag_recv.heap_sge)
		rx->qp->frag_recv.sg_list = rx->qp->frag_recv.inline_sge;
	rx->qp->frag_recv_active = 1;
	rx->qp->frag_recv_offset = (uint32_t)consumed;
	rocev2_rx_advance(rx);
	if (rx->is_rc && (rc < 0 || rx->ack_request))
		send_ack(rx->qp, rx->src, rx->psn,
			 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
	return rc;
}

static int rocev2_handle_send_middle(struct rocev2_rx_packet *rx)
{
	size_t consumed = 0;
	int rc;

	if (!rx->qp->frag_recv_active) {
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	rc = scatter_recv_fragment(rx->qp, &rx->qp->frag_recv,
				   rx->qp->frag_recv_offset, rx->body,
				   rx->body_len, &consumed);
	rx->qp->frag_recv_offset += (uint32_t)consumed;
	rocev2_rx_advance(rx);
	if (rx->is_rc && (rc < 0 || rx->ack_request))
		send_ack(rx->qp, rx->src, rx->psn,
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

	if (!rx->qp->frag_recv_active) {
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn,
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
	rc = scatter_recv_fragment(rx->qp, &rx->qp->frag_recv,
				   rx->qp->frag_recv_offset, body, body_len,
				   &consumed);
	total = rx->qp->frag_recv_offset + (uint32_t)consumed;
	uib_qp_emit_recv_slot(rx->qp, &rx->qp->frag_recv,
			      rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			      UIB_WC_RECV, total, rx->qp->dest_qp_num,
			      imm_data);
	uib_qp_release_recv(&rx->qp->frag_recv);
	uib_mr_put(rx->qp->frag_recv_mr);
	rx->qp->frag_recv_mr = NULL;
	rx->qp->frag_recv_active = 0;
	rx->qp->frag_recv_offset = 0;
	rocev2_rx_advance(rx);
	if (rx->is_rc)
		send_ack(rx->qp, rx->src, rx->psn,
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
	if (pop_recv_slot(rx->qp, &slot) < 0)
		return -ENOENT;
	rc = uib_qp_scatter_recv(rx->qp, &slot, 0, body, body_len,
				 &consumed);
	uib_qp_emit_recv_slot(rx->qp, &slot,
			      rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			      UIB_WC_RECV, (uint32_t)consumed, sqp, imm_data);
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
			send_ack(rx->qp, rx->src, rx->psn,
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
		if (pop_recv_slot(rx->qp, &slot) < 0) {
			if (rx->is_rc)
				send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_RNR);
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
			send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EACCES;
	}
	if (uib_mr_place(mr, va, body, rlen) < 0) {
		uib_mr_put(mr);
		if (with_imm)
			uib_qp_release_recv(&slot);
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EIO;
	}
	rocev2_rx_advance(rx);
	if (rx->is_rc)
		send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_ACK);
	if (with_imm) {
		uib_qp_emit_recv_slot(rx->qp, &slot, UIB_WC_SUCCESS,
				      UIB_WC_RECV_RDMA_WITH_IMM, rlen,
				      rx->qp->dest_qp_num, imm_data);
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
			send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	reth = (const struct uib_reth *)rx->body;
	uib_reth_unpack(reth, &va, &rkey, &rlen);
	mr = uib_mr_check_remote(rx->qp->pd, rkey, va, rlen,
				 UIB_ACCESS_REMOTE_WRITE);
	if (!mr) {
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EACCES;
	}
	take = rx->body_len - UIB_RETH_BYTES;
	if (take > rlen)
		take = rlen;
	if (uib_mr_place(mr, va, rx->body + UIB_RETH_BYTES, take) < 0) {
		uib_mr_put(mr);
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EIO;
	}
	uib_mr_put(rx->qp->frag_write_mr);
	rx->qp->frag_write_va = va;
	rx->qp->frag_write_total = rlen;
	rx->qp->frag_write_offset = (uint32_t)take;
	rx->qp->frag_write_active = 1;
	rx->qp->frag_write_mr = mr;
	rocev2_rx_advance(rx);
	if (rx->is_rc && rx->ack_request)
		send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_ACK);
	return 0;
}

static int rocev2_handle_write_middle(struct rocev2_rx_packet *rx)
{
	size_t take;
	uint32_t remain;

	if (!rx->qp->frag_write_active) {
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_INV_REQ);
		return -EBADMSG;
	}
	remain = rx->qp->frag_write_total - rx->qp->frag_write_offset;
	take = rx->body_len > remain ? remain : rx->body_len;
	if (uib_mr_place(rx->qp->frag_write_mr,
			 rx->qp->frag_write_va + rx->qp->frag_write_offset,
			 rx->body, take) < 0) {
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn,
				 UIB_AETH_NAK_REM_ACC);
		return -EIO;
	}
	rx->qp->frag_write_offset += (uint32_t)take;
	rocev2_rx_advance(rx);
	if (rx->is_rc && rx->ack_request)
		send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_ACK);
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

	if (!rx->qp->frag_write_active) {
		if (rx->is_rc)
			send_ack(rx->qp, rx->src, rx->psn,
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
		if (pop_recv_slot(rx->qp, &slot) < 0) {
			if (rx->is_rc)
				send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_RNR);
			return -ENOENT;
		}
	}
	remain = rx->qp->frag_write_total - rx->qp->frag_write_offset;
	take = body_len > remain ? remain : body_len;
	if (uib_mr_place(rx->qp->frag_write_mr,
			 rx->qp->frag_write_va + rx->qp->frag_write_offset,
			 body, take) < 0) {
		if (with_imm)
			uib_qp_release_recv(&slot);
		return -EIO;
	}
	rx->qp->frag_write_offset += (uint32_t)take;
	rx->qp->frag_write_active = 0;
	uib_mr_put(rx->qp->frag_write_mr);
	rx->qp->frag_write_mr = NULL;
	rocev2_rx_advance(rx);
	if (rx->is_rc)
		send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_ACK);
	if (with_imm) {
		uib_qp_emit_recv_slot(rx->qp, &slot, UIB_WC_SUCCESS,
				      UIB_WC_RECV_RDMA_WITH_IMM,
				      rx->qp->frag_write_total,
				      rx->qp->dest_qp_num, imm_data);
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
	rx->qp->last_rx_psn = rx->psn + span - 1;
	rx->qp->last_rx_valid = 1;
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
	if ((va & (UIB_ATOMIC_ACK_BYTES - 1)) ||
	    !(rx->qp->access_flags & UIB_ACCESS_REMOTE_ATOMIC)) {
		send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_NAK_REM_ACC);
		return -EACCES;
	}
	mr = uib_mr_check_remote(rx->qp->pd, rkey, va, UIB_ATOMIC_ACK_BYTES,
				 UIB_ACCESS_REMOTE_ATOMIC);
	if (!mr) {
		send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_NAK_REM_ACC);
		return -EACCES;
	}
	if (mr->kind != UIB_MR_HOST) {
		uib_mr_put(mr);
		send_ack(rx->qp, rx->src, rx->psn, UIB_AETH_NAK_REM_OP);
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
	rx->qp->atomic_ack_psn = rx->psn;
	rx->qp->atomic_ack_value = old;
	rx->qp->atomic_ack_valid = 1;
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

	if (rx->body_len < UIB_AETH_BYTES + UIB_ATOMIC_ACK_BYTES)
		return -EBADMSG;
	memcpy(&wire_value, rx->body + UIB_AETH_BYTES, sizeof(wire_value));
	value = be64toh(wire_value);
	out = find_read_by_psn(rx->qp, rx->psn);
	if (!out)
		return 0;
	rc = scatter_at(rx->qp->pd, out->read_sgl, out->read_num_sge, 0,
			(const uint8_t *)&value, sizeof(value), &consumed);
	detach_outstanding(rx->qp, out);
	emit_send_wc(rx->qp, out->wr_id, out->wr_opcode, out->signaled,
		     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
		     (uint32_t)consumed);
	free_outstanding(out);
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
		find_read_by_psn(rx->qp, rx->psn) : rx->qp->frag_read_out;
	if (!out)
		return 0;
	if (rx->opcode == UIB_OP_RDMA_READ_RESP_FIRST)
		rx->qp->frag_read_out = out;
	rc = scatter_at(rx->qp->pd, out->read_sgl, out->read_num_sge,
			rx->opcode == UIB_OP_RDMA_READ_RESP_ONLY ||
			rx->opcode == UIB_OP_RDMA_READ_RESP_FIRST ? 0 :
			rx->qp->frag_read_offset,
			body, body_len, &consumed);
	if (rx->opcode == UIB_OP_RDMA_READ_RESP_FIRST) {
		rx->qp->frag_read_offset = (uint32_t)consumed;
		return rc;
	}
	if (rx->opcode == UIB_OP_RDMA_READ_RESP_MIDDLE) {
		rx->qp->frag_read_offset += (uint32_t)consumed;
		return rc;
	}
	total = rx->opcode == UIB_OP_RDMA_READ_RESP_ONLY ?
		(uint32_t)consumed :
		rx->qp->frag_read_offset + (uint32_t)consumed;
	detach_outstanding(rx->qp, out);
	emit_send_wc(rx->qp, out->wr_id, out->wr_opcode, out->signaled,
		     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS, total);
	free_outstanding(out);
	if (rx->opcode == UIB_OP_RDMA_READ_RESP_LAST) {
		rx->qp->frag_read_out = NULL;
		rx->qp->frag_read_offset = 0;
	}
	return rc;
}

static int rocev2_prepare_retry_list_locked(struct uib_qp *qp,
					    struct uib_outstanding *head,
					    uint32_t expected_psn)
{
	struct uib_outstanding *out;

	for (out = head; out; out = out->next) {
		if (!out->sent || psn_cmp(out->psn, expected_psn) < 0)
			continue;
		if (!psn_cmp(out->psn, expected_psn) &&
		    out->retries >= UIB_MAX_RETRIES)
			return -ETIMEDOUT;
		if (out->congestion_inflight) {
			uib_congestion_on_loss(&qp->congestion,
					       out->nominal_len);
			out->congestion_inflight = 0;
		}
		rocev2_request_ack(out);
		out->sent = 0;
		if (!psn_cmp(out->psn, expected_psn))
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
	if (qp->sequence_retry_active &&
	    psn_cmp(expected_psn, qp->sequence_retry_psn) <= 0) {
		pthread_mutex_unlock(&qp->lock);
		return;
	}
	qp->sequence_retry_psn = expected_psn;
	qp->sequence_retry_active = 1;
	rc = rocev2_prepare_retry_list_locked(qp, qp->out_head, expected_psn);
	if (!rc)
		rc = rocev2_prepare_retry_list_locked(qp, qp->read_head,
						       expected_psn);
	if (rc < 0)
		qp->state = UIB_QPS_ERR;
	else {
		uib_qp_retry_cancel(qp);
		rocev2_drain_locked(qp);
	}
	pthread_mutex_unlock(&qp->lock);
	if (rc < 0)
		fail_outstanding(qp, UIB_WC_RETRY_EXC_ERR);
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
		fail_outstanding(rx->qp,
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

	if (verify_icrc(packet, len) < 0)
		return -EBADMSG;
	bth = packet;
	memset(&rx, 0, sizeof(rx));
	rx.qp = qp;
	rx.src = src;
	uib_bth_unpack(bth, &rx.opcode, &dqpn, &rx.psn, &rx.ack_request);
	rx.body = (const uint8_t *)packet + UIB_BTH_BYTES;
	rx.body_len = len - UIB_BTH_BYTES - UIB_ICRC_BYTES;
	rx.is_rc = qp->qp_type == UIB_QPT_RC;
	pad = uib_bth_pad(bth);
	if (pad > rx.body_len)
		return -EBADMSG;
	rx.body_len -= pad;
	if (meta && meta->ecn == IPTOS_ECN_CE && rx.opcode != UIB_OP_CNP)
		send_cnp(qp, src);
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
	return uib_qp_handle_packet_meta(qp, src, pkt, len, NULL);
}

int uib_qp_handle_packet_meta(struct uib_qp *qp,
			      const struct sockaddr_in *src, const void *pkt,
			      size_t len, const struct uib_rx_meta *meta)
{
	if (!qp || !qp->ctx->pops)
		return -EOPNOTSUPP;
	if (qp->ctx->pops->handle_packet)
		return qp->ctx->pops->handle_packet(qp, src, pkt, len, meta);
	return -EOPNOTSUPP;
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
				out->deadline_ns = now + qp->ctx->retry_timeout_ns;
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
	rocev2_retransmit_list(qp, qp->out_head, now_ns, &next_deadline);
	rocev2_retransmit_list(qp, qp->read_head, now_ns, &next_deadline);
	if (next_deadline != UINT64_MAX)
		uib_qp_retry_schedule(qp, next_deadline);
	else
		uib_qp_retry_cancel(qp);
	rocev2_drain_locked(qp);
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
