/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>
#include <urcu-bp.h>

#include "uib/bth.h"
#include "uib/dcqcn.h"
#include "uib/env.h"
#include "uib/mr.h"
#include "uib/pd.h"
#include "uib/qp.h"
#include "uib/transport.h"
#include "uib/uet_nscc.h"
#include "uib/context.h"
#include "congestion_internal.h"
#include "ptr_ring.h"
#include "qp_internal.h"
#include "rocev2_internal.h"
#include "utils.h"

static void rocev2_free_send_batch(struct uib_transport_ref *refs,
				    uint32_t built);
static void rocev2_send_data_release(struct rocev2_send_data *data);
static void enqueue_outstanding(struct uib_qp *qp,
				struct uib_outstanding *out);
static int post_rdma_read(struct uib_qp *qp, struct uib_send_wr *wr,
			  size_t total);
static int post_atomic(struct uib_qp *qp, struct uib_send_wr *wr);
static int rocev2_send_total(const struct uib_send_wr *wr, size_t *total);
static int rocev2_packet_count(size_t total, uint32_t mtu,
			       uint32_t *packets);
static int rocev2_prepare_send_data(struct uib_qp *qp,
				    struct uib_send_wr *wr,
				    struct rocev2_send_data *data);
static int rocev2_build_send_batch(struct uib_qp *qp,
				   struct uib_send_wr *wr,
				   const struct rocev2_send_data *data,
				   struct uib_transport_ref *refs,
				   uint32_t packets, uint32_t mtu,
				   uint32_t first_psn,
				   uint32_t *built);

static uint16_t rocev2_udp_sport(const struct uib_qp *qp)
{
	return uib_rocev2_udp_sport(qp->qp_num, qp->dest_qp_num);
}

int rocev2_transport_send(struct uib_qp *qp,
			  const struct sockaddr_in *dst,
			  const void *packet, size_t length)
{
	return uib_transport_send_entropy(qp->ctx, dst, packet, length,
					  rocev2_udp_sport(qp));
}

static int rocev2_transport_send_ref(struct uib_qp *qp,
				      struct uib_outstanding *out)
{
	struct uib_transport_ref ref;

	ref.dst = &out->dst;
	ref.buf = out->pkt;
	ref.release = rocev2_free_outstanding;
	ref.opaque = out;
	ref.len = out->pkt_len;
	ref.entropy = rocev2_udp_sport(qp);
	return uib_transport_send_ref_batch(qp->ctx, &ref, 1);
}

static size_t outstanding_pool_capacity(uint8_t pool_class)
{
	static const size_t capacities[UIB_ROCEV2_OUT_POOL_CLASSES] = {
		256,
		2048,
		UIB_ROCEV2_PKT_MAX,
	};

	return capacities[pool_class];
}

static uint8_t outstanding_pool_class(size_t packet_len)
{
	uint8_t pool_class;

	for (pool_class = 0; pool_class < UIB_ROCEV2_OUT_POOL_CLASSES; pool_class++) {
		if (packet_len <= outstanding_pool_capacity(pool_class))
			return pool_class;
	}
	return UIB_ROCEV2_OUT_POOL_CLASSES;
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

void rocev2_outstanding_pool_destroy(struct uib_outstanding_pool *pool)
{
	struct uib_outstanding *out;
	uint8_t pool_class;

	if (!pool)
		return;
	__atomic_store_n(&pool->closing, 1, __ATOMIC_RELEASE);
	synchronize_rcu();
	for (pool_class = 0; pool_class < UIB_ROCEV2_OUT_POOL_CLASSES; pool_class++) {
		while ((out = uib_ptr_ring_pop(&pool->free[pool_class])) != NULL) {
			free(out);
			outstanding_pool_put(pool);
		}
		uib_ptr_ring_destroy(&pool->free[pool_class]);
	}
	outstanding_pool_put(pool);
}

struct uib_outstanding_pool *
rocev2_outstanding_pool_create(uint32_t max_send_wr)
{
	struct uib_outstanding_pool *pool;
	uint32_t ring_capacity;
	uint8_t pool_class;
	uint32_t limit;
	int rc;

	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return NULL;
	limit = max_send_wr > UIB_ROCEV2_OUT_POOL_MAX / 16 ? UIB_ROCEV2_OUT_POOL_MAX :
		max_send_wr * 16;
	if (!max_send_wr)
		limit = UIB_ROCEV2_OUT_POOL_MAX;
	if (limit < 64)
		limit = 64;
	if (limit > UIB_ROCEV2_OUT_POOL_MAX)
		limit = UIB_ROCEV2_OUT_POOL_MAX;
	ring_capacity = 64;
	while (ring_capacity < limit)
		ring_capacity <<= 1;
	limit = ring_capacity;
	for (pool_class = 0; pool_class < UIB_ROCEV2_OUT_POOL_CLASSES; pool_class++) {
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

void rocev2_free_outstanding(void *opaque)
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
	struct uib_outstanding_pool *pool = rocev2_qp(qp)->out_pool;
	struct uib_outstanding *out;
	uint8_t pool_class;

	pool_class = outstanding_pool_class(packet_len);
	if (pool_class == UIB_ROCEV2_OUT_POOL_CLASSES)
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

int rocev2_send_outstanding(struct uib_qp *qp,
			    struct uib_outstanding *out)
{
	__atomic_add_fetch(&out->refs, 1, __ATOMIC_RELAXED);
	return rocev2_transport_send_ref(qp, out);
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

int rocev2_congestion_init(struct uib_qp *qp)
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
		uint64_t value;

		uib_uet_nscc_config_default(&nscc_config, mtu);
		max_wnd = getenv(UIB_ENV_ROCEV2_NSCC_MAX_WND);
		if (max_wnd) {
			if (uib_parse_u64(max_wnd, 0, &value) < 0 || value < mtu ||
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
	return uib_congestion_init(&rocev2_qp(qp)->congestion, cc,
				   configuration, mtu,
				   uib_now_ns());
}

static void rocev2_free_send_batch(struct uib_transport_ref *refs,
				    uint32_t built)
{
	struct uib_outstanding *out;
	uint32_t i;

	for (i = 0; i < built; i++) {
		out = refs[i].opaque;
		rocev2_free_outstanding(out);
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

static struct uib_rocev2_task *
rocev2_task_reserve(struct uib_rocev2_context *context,
			     struct uib_rocev2_worker **selected,
			     uint64_t *position, int *was_empty)
{
	struct uib_rocev2_worker *worker;
	struct uib_rocev2_task *task;
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

static void rocev2_task_recycle(struct uib_rocev2_task *task)
{
	struct uib_rocev2_worker *worker;

	worker = task->worker;
	if (task->refs && task->refs != task->inline_refs)
		free(task->refs);
	if (!worker) {
		free(task);
		return;
	}
	memset(task, 0, sizeof(*task));
	task->worker = worker;
	uib_ptr_ring_push(&worker->free_tasks, task);
}

void rocev2_task_release(struct uib_rocev2_task *task)
{
	rocev2_free_send_batch(task->refs, task->built);
	rocev2_send_data_release(&task->data);
}

void rocev2_process_task(struct uib_rocev2_task *task)
{
	int rc;

	rc = rocev2_build_send_batch(task->qp, &task->wr, &task->data,
				     task->refs, task->packets, task->mtu,
				     task->first_psn, &task->built);
	if (rc < 0) {
		rocev2_free_send_batch(task->refs, task->built);
		task->built = 0;
	}
	rocev2_send_data_release(&task->data);
	memset(&task->data, 0, sizeof(task->data));
	task->rc = rc;
	__atomic_store_n(&task->ready, 1, __ATOMIC_RELEASE);
	rocev2_sequence_notify(task->qp);
}

static int rocev2_commit_ready_locked(struct uib_qp *qp)
{
	struct uib_rocev2_task *task;
	struct uib_rocev2_qp *rq;
	int failed;

	rq = rocev2_qp(qp);
	failed = 0;
	while (rq->prep_head &&
	       __atomic_load_n(&rq->prep_head->ready, __ATOMIC_ACQUIRE)) {
		task = rq->prep_head;
		rq->prep_head = task->next;
		if (!rq->prep_head)
			rq->prep_tail = NULL;
		if (task->rc < 0) {
			qp->state = UIB_QPS_ERR;
			failed = 1;
			uib_qp_emit_send_wc(qp, task->wr.wr_id,
					     task->wr.opcode,
					     task->wr.send_flags & UIB_SEND_SIGNALED,
					     UIB_WC_LOC_PROT_ERR, 0);
		} else if (qp->state == UIB_QPS_ERR) {
			rocev2_free_send_batch(task->refs, task->built);
			task->built = 0;
			uib_qp_emit_send_wc(qp, task->wr.wr_id,
					     task->wr.opcode,
					     task->wr.send_flags & UIB_SEND_SIGNALED,
					     UIB_WC_WR_FLUSH_ERR, 0);
		} else {
			rocev2_commit_send_batch(qp, task->refs, task->built);
			task->built = 0;
		}
		if (rq->prep_pending)
			rq->prep_pending--;
		rocev2_task_recycle(task);
	}
	if (!rq->prep_pending)
		pthread_cond_broadcast(&rq->prep_cond);
	return failed;
}

void rocev2_sequence_progress(struct uib_qp *qp)
{
	struct uib_rocev2_qp *rq;
	int failed;
	int pending;
	int owned;

	rq = rocev2_qp(qp);
	failed = 0;
	owned = 1;
	while (owned) {
		do {
			__atomic_store_n(&rq->sequence_pending, 0,
					 __ATOMIC_RELEASE);
			pthread_mutex_lock(&qp->lock);
			failed |= rocev2_commit_ready_locked(qp);
			pthread_mutex_unlock(&qp->lock);
			pending = __atomic_exchange_n(&rq->sequence_pending, 0,
						      __ATOMIC_ACQ_REL);
		} while (pending);
		__atomic_store_n(&rq->sequence_queued, 0, __ATOMIC_RELEASE);
		if (!__atomic_load_n(&rq->sequence_pending,
					     __ATOMIC_ACQUIRE))
			break;
		owned = !__atomic_exchange_n(&rq->sequence_queued, 1,
					     __ATOMIC_ACQ_REL);
	}
	pthread_mutex_lock(&qp->lock);
	pthread_cond_broadcast(&rq->prep_cond);
	pthread_mutex_unlock(&qp->lock);
	if (failed)
		rocev2_fail_outstanding(qp, UIB_WC_WR_FLUSH_ERR);
}

static int rocev2_queue_send(struct uib_qp *qp, struct uib_send_wr *wr)
{
	struct uib_rocev2_context *context;
	struct uib_rocev2_worker *worker;
	struct uib_rocev2_task *task;
	struct rocev2_send_data data;
	struct uib_transport_ref *refs;
	struct uib_rocev2_qp *rq;
	uint64_t position;
	uint32_t packets;
	uint32_t mtu;
	int was_empty;
	int queued;
	int rc;

	memset(&data, 0, sizeof(data));
	refs = NULL;
	rq = rocev2_qp(qp);
	pthread_mutex_lock(&qp->lock);
	if (rq->prep_closing) {
		pthread_mutex_unlock(&qp->lock);
		return -ESHUTDOWN;
	}
	pthread_mutex_unlock(&qp->lock);
	rc = rocev2_send_total(wr, &data.total);
	if (rc < 0)
		return rc;
	mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
	rc = rocev2_packet_count(data.total, mtu, &packets);
	if (rc < 0)
		goto out_data;
	rc = rocev2_prepare_send_data(qp, wr, &data);
	if (rc < 0)
		goto out_data;
	if (packets > UIB_ROCEV2_TX_BATCH_STACK) {
		refs = calloc(packets, sizeof(*refs));
		if (!refs) {
			rc = -ENOMEM;
			goto out_data;
		}
	}
	context = qp->ctx->protocol_private;
	task = rocev2_task_reserve(context, &worker, &position, &was_empty);
	if (!task) {
		task = calloc(1, sizeof(*task));
		if (!task) {
			rc = -ENOMEM;
			free(refs);
			goto out_data;
		}
		worker = NULL;
		queued = 0;
	} else {
		queued = 1;
	}
	memset(task, 0, sizeof(*task));
	task->worker = worker;
	task->qp = qp;
	task->wr = *wr;
	task->wr.next = NULL;
	task->wr.sg_list = NULL;
	task->data = data;
	memset(&data, 0, sizeof(data));
	task->packets = packets;
	task->mtu = mtu;
	task->refs = refs ? refs : task->inline_refs;
	pthread_mutex_lock(&qp->lock);
	task->first_psn = qp->sq_psn;
	qp->sq_psn += packets;
	if (rq->prep_tail)
		rq->prep_tail->next = task;
	else
		rq->prep_head = task;
	rq->prep_tail = task;
	rq->prep_pending++;
	pthread_mutex_unlock(&qp->lock);
	if (!queued) {
		rocev2_process_task(task);
		return 0;
	}
	uib_ptr_ring_publish(&worker->ready, position, task);
	if (was_empty) {
		pthread_mutex_lock(&worker->lock);
		pthread_cond_signal(&worker->cond);
		pthread_mutex_unlock(&worker->lock);
	}
	return 0;

out_data:
	rocev2_send_data_release(&data);
	return rc;
}

static int post_one_send(struct uib_qp *qp, struct uib_send_wr *wr)
{
	struct uib_transport_ref stack_refs[UIB_ROCEV2_TX_BATCH_STACK];
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
	if (qp->qp_type == UIB_QPT_RC &&
	    (wr->opcode == UIB_WR_SEND ||
	     wr->opcode == UIB_WR_SEND_WITH_IMM ||
	     wr->opcode == UIB_WR_RDMA_WRITE ||
	     wr->opcode == UIB_WR_RDMA_WRITE_WITH_IMM))
		return rocev2_queue_send(qp, wr);
	mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
	if (qp->qp_type == UIB_QPT_UD && data.total > mtu)
		return -EMSGSIZE;
	rc = rocev2_prepare_send_data(qp, wr, &data);
	if (rc < 0)
		goto out_data;
	rc = rocev2_packet_count(data.total, mtu, &packets);
	if (rc < 0)
		goto out_data;
	is_rc = qp->qp_type == UIB_QPT_RC;
	if (is_rc && packets > UIB_ROCEV2_TX_BATCH_STACK) {
		refs = malloc((size_t)packets * sizeof(*refs));
		if (!refs) {
			rc = -ENOMEM;
			goto out_data;
		}
	}
	if (is_rc)
		pthread_mutex_lock(&qp->lock);
	start_psn = qp->sq_psn;
	qp->sq_psn += packets;
	rc = rocev2_build_send_batch(qp, wr, &data, refs, packets, mtu,
				     start_psn,
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
		uib_qp_emit_send_wc(qp, wr->wr_id, wr->opcode,
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
			uib_qp_emit_send_wc(qp, cur->wr_id, cur->opcode,
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

static void outstanding_index_add(struct uib_qp *qp,
				  struct uib_outstanding *out)
{
	if (!out->is_read ||
	    uib_psn_index_insert(&rocev2_qp(qp)->out_index,
				 out->psn & UIB_BTH_PSN_MASK, out))
		return;
	out->indexed = 1;
}

static void outstanding_index_remove(struct uib_qp *qp,
				     struct uib_outstanding *out)
{
	if (!out->indexed)
		return;
	uib_psn_index_remove(&rocev2_qp(qp)->out_index,
			     out->psn & UIB_BTH_PSN_MASK, out);
	out->indexed = 0;
}

void rocev2_outstanding_unlink(struct uib_qp *qp,
			       struct uib_outstanding *out)
{
	struct uib_outstanding **tail;
	struct uib_outstanding **head;

	if (out->is_read) {
		head = &rocev2_qp(qp)->read_head;
		tail = &rocev2_qp(qp)->read_tail;
	} else {
		head = &rocev2_qp(qp)->out_head;
		tail = &rocev2_qp(qp)->out_tail;
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
	rocev2_qp(qp)->out_count--;
}

static void enqueue_outstanding(struct uib_qp *qp, struct uib_outstanding *out)
{
	struct uib_outstanding **tail;
	struct uib_outstanding **head;

	if (out->is_read) {
		head = &rocev2_qp(qp)->read_head;
		tail = &rocev2_qp(qp)->read_tail;
	} else {
		head = &rocev2_qp(qp)->out_head;
		tail = &rocev2_qp(qp)->out_tail;
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
	rocev2_qp(qp)->out_count++;
}

void rocev2_outstanding_retry_reschedule(struct uib_qp *qp)
{
	struct uib_outstanding *out;
	uint64_t deadline = UINT64_MAX;

	for (out = rocev2_qp(qp)->out_head; out; out = out->next) {
		if (out->sent && out->deadline_ns < deadline)
			deadline = out->deadline_ns;
	}
	for (out = rocev2_qp(qp)->read_head; out; out = out->next) {
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
	struct uib_outstanding *read = first_unsent(rocev2_qp(qp)->read_head);
	struct uib_outstanding *send = first_unsent(rocev2_qp(qp)->out_head);

	if (!send)
		return read;
	if (!read)
		return send;
	return rocev2_psn_cmp(send->psn, read->psn) <= 0 ? send : read;
}

void rocev2_request_ack(struct uib_outstanding *out)
{
	struct uib_bth *bth = out->pkt;
	uint32_t apsn = ntohl(bth->apsn) | UIB_BTH_ACK_REQ;
	uint32_t crc;

	bth->apsn = htonl(apsn);
	crc = htonl(uib_icrc(out->pkt, out->pkt_len - UIB_ICRC_BYTES));
	memcpy((uint8_t *)out->pkt + out->pkt_len - UIB_ICRC_BYTES, &crc,
	       sizeof(crc));
}

int rocev2_drain_locked(struct uib_qp *qp)
{
	struct uib_transport_ref refs[UIB_ROCEV2_TX_BATCH_STACK];
	struct uib_outstanding *outstanding[UIB_ROCEV2_TX_BATCH_STACK];
	struct uib_congestion congestion_before =
		rocev2_qp(qp)->congestion;
	struct uib_outstanding *out;
	uint64_t transmit_time = uib_now_ns();
	uint32_t count = 0;
	uint32_t i;
	int rc;

	while (count < UIB_ROCEV2_TX_BATCH_STACK) {
		out = rocev2_next_unsent(qp);
		if (!out || !uib_congestion_can_send(
					       &rocev2_qp(qp)->congestion,
						       out->nominal_len))
			break;
		out->sent = 1;
		out->tx_time_ns = transmit_time;
		uib_congestion_on_send(&rocev2_qp(qp)->congestion,
				       out->nominal_len);
		out->congestion_inflight = 1;
		if (uib_congestion_ack_request(&rocev2_qp(qp)->congestion,
					       out->nominal_len))
			rocev2_request_ack(out);
		refs[count].dst = &out->dst;
		refs[count].buf = out->pkt;
		refs[count].release = rocev2_free_outstanding;
		refs[count].opaque = out;
		refs[count].len = out->pkt_len;
		refs[count].entropy = rocev2_udp_sport(qp);
		outstanding[count] = out;
		__atomic_add_fetch(&out->refs, 1, __ATOMIC_RELAXED);
		count++;
	}
	if (!count) {
		if (rocev2_next_unsent(qp)) {
			uint64_t next = uib_congestion_next_send_ns(
				&rocev2_qp(qp)->congestion);

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
		rocev2_qp(qp)->congestion = congestion_before;
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
		uint64_t next = uib_congestion_next_send_ns(
			&rocev2_qp(qp)->congestion);

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

void rocev2_send_ack(struct uib_qp *qp, const struct sockaddr_in *dst,
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
	rocev2_transport_send(qp, &target, pkt, sizeof(pkt));
}

void rocev2_send_cnp(struct uib_qp *qp, const struct sockaddr_in *dst)
{
	uint8_t packet[UIB_BTH_BYTES + UIB_ICRC_BYTES];
	struct sockaddr_in target;
	uint32_t wire_crc;

	response_target(qp, dst, &target);
	uib_bth_pack((struct uib_bth *)packet, UIB_OP_CNP,
		     UIB_DEFAULT_PKEY, qp->dest_qp_num, 0, 0);
	wire_crc = htonl(uib_icrc(packet, UIB_BTH_BYTES));
	memcpy(packet + UIB_BTH_BYTES, &wire_crc, sizeof(wire_crc));
	rocev2_transport_send(qp, &target, packet, sizeof(packet));
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
			   size_t capacity, uint32_t assigned_psn,
			   uint32_t *out_psn,
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
	psn = assigned_psn;

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
			rc = rocev2_send_outstanding(qp, out);
	} else {
		rc = rocev2_transport_send(qp, &dst, pkt, *out_pkt_len);
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
	uint32_t first_psn;
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
			rocev2_free_outstanding(out);
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
	first_psn = qp->sq_psn;
	qp->sq_psn += n_resp;
	out->nominal_len = rocev2_nominal_len(qp, packet_len);
	rc = send_one_packet(qp, wr, UIB_OP_RDMA_READ_REQ, 1, 1, NULL, 0,
			     wr->rdma.remote_addr, wr->rdma.rkey,
			     (uint32_t)total, out, out->pkt, out->pkt_len,
			     first_psn, &psn, &out->pkt_len, &dst, 1);
	if (rc < 0) {
		qp->sq_psn = first_psn;
		pthread_mutex_unlock(&qp->lock);
		rocev2_free_outstanding(out);
		return rc;
	}
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
	    wr->sg_list[0].length < UIB_ROCEV2_ATOMIC_ACK_BYTES ||
	    (wr->sg_list[0].addr & (UIB_ROCEV2_ATOMIC_ACK_BYTES - 1)) ||
	    (wr->atomic.remote_addr & (UIB_ROCEV2_ATOMIC_ACK_BYTES - 1)))
		return -EINVAL;
	mr = uib_mr_lookup_local(qp->pd, wr->sg_list[0].lkey,
				 (void *)(uintptr_t)wr->sg_list[0].addr,
				 UIB_ROCEV2_ATOMIC_ACK_BYTES, UIB_ACCESS_LOCAL_WRITE);
	if (!mr)
		return -EACCES;
	uib_mr_put(mr);

	packet_len = UIB_BTH_BYTES + sizeof(*atomic) + UIB_ICRC_BYTES;
	out = alloc_outstanding(qp, packet_len);
	if (!out)
		return -ENOMEM;
	out->read_sgl = calloc(1, sizeof(*out->read_sgl));
	if (!out->read_sgl) {
		rocev2_free_outstanding(out);
		return -ENOMEM;
	}
	*out->read_sgl = wr->sg_list[0];
	out->read_num_sge = 1;
	out->read_total_len = UIB_ROCEV2_ATOMIC_ACK_BYTES;

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

static int rocev2_packet_count(size_t total, uint32_t mtu,
			       uint32_t *packets)
{
	size_t count;

	count = total ? 1 + (total - 1) / mtu : 1;
	if (count > UINT32_MAX)
		return -EMSGSIZE;
	*packets = (uint32_t)count;
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
	rc = uib_qp_gather_total(qp->pd, wr->sg_list, wr->num_sge,
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
				    uint32_t first_psn,
				    uint32_t *built)
{
	uint8_t stack_packet[UIB_ROCEV2_PKT_MAX];
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
				     packet, packet_len, first_psn + i, &psn,
				     &packet_len, &dst, is_rc);
		if (rc < 0) {
			rocev2_free_outstanding(out);
			return rc;
		}
		if (!is_rc)
			continue;
		rocev2_init_outstanding(qp, wr, out, &dst, packet_len, psn,
					   opcode, is_last);
		refs[*built].dst = &out->dst;
		refs[*built].buf = out->pkt;
		refs[*built].release = rocev2_free_outstanding;
		refs[*built].opaque = out;
		refs[*built].len = out->pkt_len;
		refs[*built].entropy = rocev2_udp_sport(qp);
		(*built)++;
	}
	return 0;
}
