/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdlib.h>
#include <sched.h>

#include "uib/bth.h"
#include "uib/env.h"
#include "uib/mr.h"
#include "uib/protocol.h"
#include "uib/qp.h"
#include "uib/context.h"
#include "ptr_ring.h"
#include "rocev2_internal.h"
#include "utils.h"

void rocev2_sequence_notify(struct uib_qp *qp)
{
	struct uib_rocev2_context *context;
	struct uib_rocev2_qp *rq;
	int rc;

	context = qp->ctx->protocol_private;
	rq = rocev2_qp(qp);
	__atomic_store_n(&rq->sequence_pending, 1, __ATOMIC_RELEASE);
	if (__atomic_exchange_n(&rq->sequence_queued, 1,
				__ATOMIC_ACQ_REL))
		return;
	if (context->flat_sequence) {
		rocev2_sequence_progress(qp);
		return;
	}
	for (;;) {
		rc = uib_ptr_ring_push(&context->sequence_ready, qp);
		if (!rc)
			break;
		sched_yield();
	}
	pthread_mutex_lock(&context->sequence_lock);
	pthread_cond_signal(&context->sequence_cond);
	pthread_mutex_unlock(&context->sequence_lock);
}

static void *rocev2_prep_worker(void *argument)
{
	struct uib_rocev2_worker *worker;
	struct uib_rocev2_context *context;
	struct uib_rocev2_task *task;

	worker = argument;
	context = worker->context;
	for (;;) {
		task = uib_ptr_ring_pop(&worker->ready);
		if (task) {
			rocev2_process_task(task);
			continue;
		}
		pthread_mutex_lock(&worker->lock);
		while (__atomic_load_n(&context->run, __ATOMIC_ACQUIRE) &&
		       !uib_ptr_ring_count(&worker->ready))
			pthread_cond_wait(&worker->cond, &worker->lock);
		if (!__atomic_load_n(&context->run, __ATOMIC_ACQUIRE) &&
		    !uib_ptr_ring_count(&worker->ready)) {
			pthread_mutex_unlock(&worker->lock);
			break;
		}
		pthread_mutex_unlock(&worker->lock);
	}
	return NULL;
}

static void *rocev2_sequence_worker(void *argument)
{
	struct uib_rocev2_context *context;
	struct uib_qp *qp;
	int run;

	context = argument;
	for (;;) {
		qp = uib_ptr_ring_pop(&context->sequence_ready);
		if (qp) {
			rocev2_sequence_progress(qp);
			continue;
		}
		pthread_mutex_lock(&context->sequence_lock);
		while (__atomic_load_n(&context->sequence_run,
					       __ATOMIC_ACQUIRE) &&
		       !uib_ptr_ring_count(&context->sequence_ready))
			pthread_cond_wait(&context->sequence_cond,
					  &context->sequence_lock);
		run = __atomic_load_n(&context->sequence_run,
				      __ATOMIC_ACQUIRE);
		pthread_mutex_unlock(&context->sequence_lock);
		if (!run && !uib_ptr_ring_count(&context->sequence_ready))
			break;
	}
	return NULL;
}

static int rocev2_worker_init(struct uib_rocev2_context *context,
			       struct uib_rocev2_worker *worker)
{
	unsigned int i;
	int rc;

	worker->context = context;
	rc = pthread_mutex_init(&worker->lock, NULL);
	if (rc)
		return -rc;
	rc = pthread_cond_init(&worker->cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&worker->lock);
		return -rc;
	}
	worker->initialized = 1;
	rc = uib_ptr_ring_init(&worker->ready, UIB_ROCEV2_PREP_QUEUE_SIZE);
	if (rc < 0)
		return rc;
	worker->ready_initialized = 1;
	rc = uib_ptr_ring_init(&worker->free_tasks,
			       UIB_ROCEV2_PREP_QUEUE_SIZE);
	if (rc < 0)
		return rc;
	worker->free_initialized = 1;
	worker->tasks = calloc(UIB_ROCEV2_PREP_QUEUE_SIZE,
			       sizeof(*worker->tasks));
	if (!worker->tasks)
		return -ENOMEM;
	for (i = 0; i < UIB_ROCEV2_PREP_QUEUE_SIZE; i++) {
		worker->tasks[i].worker = worker;
		uib_ptr_ring_push(&worker->free_tasks, &worker->tasks[i]);
	}
	return 0;
}

static void rocev2_context_cleanup(struct uib_rocev2_context *context)
{
	struct uib_rocev2_worker *worker;
	struct uib_rocev2_task *task;
	unsigned int i;

	if (!context)
		return;
	__atomic_store_n(&context->run, 0, __ATOMIC_RELEASE);
	for (i = 0; i < context->worker_count; i++) {
		worker = &context->workers[i];
		if (!worker->initialized)
			continue;
		pthread_mutex_lock(&worker->lock);
		pthread_cond_broadcast(&worker->cond);
		pthread_mutex_unlock(&worker->lock);
	}
	for (i = 0; i < context->worker_count; i++) {
		worker = &context->workers[i];
		if (worker->started)
			pthread_join(worker->thread, NULL);
	}
	__atomic_store_n(&context->sequence_run, 0, __ATOMIC_RELEASE);
	if (context->sequence_sync_initialized) {
		pthread_mutex_lock(&context->sequence_lock);
		pthread_cond_broadcast(&context->sequence_cond);
		pthread_mutex_unlock(&context->sequence_lock);
	}
	if (context->sequence_started)
		pthread_join(context->sequence_thread, NULL);
	for (i = 0; i < context->worker_count; i++) {
		worker = &context->workers[i];
		while (worker->ready_initialized &&
		       (task = uib_ptr_ring_pop(&worker->ready)) != NULL) {
			rocev2_task_release(task);
			if (task->refs != task->inline_refs)
				free(task->refs);
		}
		if (worker->free_initialized)
			uib_ptr_ring_destroy(&worker->free_tasks);
		if (worker->ready_initialized)
			uib_ptr_ring_destroy(&worker->ready);
		if (worker->initialized) {
			pthread_cond_destroy(&worker->cond);
			pthread_mutex_destroy(&worker->lock);
		}
		free(worker->tasks);
	}
	if (context->sequence_ready_initialized)
		uib_ptr_ring_destroy(&context->sequence_ready);
	if (context->sequence_sync_initialized) {
		pthread_cond_destroy(&context->sequence_cond);
		pthread_mutex_destroy(&context->sequence_lock);
	}
	free(context->workers);
	free(context);
}

static int rocev2_context_resources_init(struct uib_rocev2_context *context,
					 unsigned int count)
{
	int rc;

	if (!context->flat_sequence) {
		rc = pthread_mutex_init(&context->sequence_lock, NULL);
		if (rc)
			return -rc;
		rc = pthread_cond_init(&context->sequence_cond, NULL);
		if (rc) {
			pthread_mutex_destroy(&context->sequence_lock);
			return -rc;
		}
		context->sequence_sync_initialized = 1;
		rc = uib_ptr_ring_init(&context->sequence_ready,
				       UIB_ROCEV2_SEQUENCE_QUEUE_SIZE);
		if (rc < 0)
			return rc;
		context->sequence_ready_initialized = 1;
	}
	context->workers = calloc(count, sizeof(*context->workers));
	if (!context->workers)
		return -ENOMEM;
	context->worker_count = count;
	return 0;
}

static int rocev2_context_init(struct uib_context *ctx)
{
	struct uib_rocev2_context *context;
	struct uib_rocev2_worker *worker;
	cpu_set_t allowed;
	unsigned int count = 0;
	unsigned int cpu_start;
	unsigned int i;
	int affinity;
	int rc;

	rc = uib_worker_configuration(&count, &allowed, &affinity,
				      UIB_ENV_TX_WORKERS,
				      UIB_ENV_TX_WORKER_AFFINITY,
				      UIB_ROCEV2_WORKERS_AUTO_MAX);
	if (rc < 0)
		return rc;
	context = calloc(1, sizeof(*context));
	if (!context)
		return -ENOMEM;
	context->flat_sequence = count > 1;
	cpu_start = affinity ? uib_worker_cpu_reserve(count +
						!context->flat_sequence) : 0;
	context->run = 1;
	context->sequence_run = 1;
	rc = rocev2_context_resources_init(context, count);
	if (rc < 0)
		goto error;
	if (!context->flat_sequence) {
		rc = uib_worker_thread_start(&context->sequence_thread,
					     rocev2_sequence_worker, context,
					     UIB_ROCEV2_WORKER_STACK_SIZE,
					     &allowed, cpu_start, affinity);
		if (rc < 0)
			goto error;
		context->sequence_started = 1;
	}
	for (i = 0; i < count; i++) {
		worker = &context->workers[i];
		rc = rocev2_worker_init(context, worker);
		if (rc < 0)
			goto error;
		rc = uib_worker_thread_start(&worker->thread,
					     rocev2_prep_worker, worker,
					     UIB_ROCEV2_WORKER_STACK_SIZE,
					     &allowed, cpu_start + i +
					     !context->flat_sequence, affinity);
		if (rc < 0)
			goto error;
		worker->started = 1;
	}
	ctx->protocol_private = context;
	return 0;

error:
	rocev2_context_cleanup(context);
	return rc;
}

static void rocev2_context_fini(struct uib_context *ctx)
{
	struct uib_rocev2_context *context;

	context = ctx->protocol_private;
	ctx->protocol_private = NULL;
	rocev2_context_cleanup(context);
}

int uib_rocev2_qp_init(struct uib_qp *qp)
{
	struct uib_rocev2_qp *rq;
	int rc;

	rq = calloc(1, sizeof(*rq));
	if (!rq)
		return -ENOMEM;
	rq->out_pool = rocev2_outstanding_pool_create(qp->max_send_wr);
	if (!rq->out_pool) {
		free(rq);
		return -ENOMEM;
	}
	rc = pthread_cond_init(&rq->prep_cond, NULL);
	if (rc) {
		rocev2_outstanding_pool_destroy(rq->out_pool);
		free(rq);
		return -rc;
	}
	qp->protocol_private = rq;
	rc = rocev2_congestion_init(qp);
	if (rc < 0) {
		pthread_cond_destroy(&rq->prep_cond);
		rocev2_outstanding_pool_destroy(rq->out_pool);
		qp->protocol_private = NULL;
		free(rq);
		return rc;
	}
	return 0;
}

void uib_rocev2_qp_modified(struct uib_qp *qp, int attr_mask)
{
	struct uib_rocev2_qp *rq = rocev2_qp(qp);

	/*
	 * The acknowledged PSN follows whichever sequence number was set, and
	 * a later send sequence supersedes a receive sequence in the same
	 * request, matching the order the attributes are applied in.
	 */
	if (attr_mask & UIB_QP_RQ_PSN)
		rq->last_acked_psn = qp->rq_psn ? qp->rq_psn - 1 :
			UIB_BTH_PSN_MASK;
	if (attr_mask & UIB_QP_SQ_PSN)
		rq->last_acked_psn = qp->sq_psn ? qp->sq_psn - 1 :
			UIB_BTH_PSN_MASK;
	if (attr_mask & UIB_QP_PATH_MTU) {
		struct uib_congestion previous = rq->congestion;

		if (rocev2_congestion_init(qp))
			rq->congestion = previous;
	}
}

void uib_rocev2_qp_fini(struct uib_qp *qp)
{
	struct uib_rocev2_qp *rq = rocev2_qp(qp);
	struct uib_outstanding *next;
	struct uib_outstanding *o;

	if (!rq)
		return;
	pthread_mutex_lock(&qp->lock);
	rq->prep_closing = 1;
	while (rq->prep_pending ||
	       __atomic_load_n(&rq->sequence_queued, __ATOMIC_ACQUIRE))
		pthread_cond_wait(&rq->prep_cond, &qp->lock);
	pthread_mutex_unlock(&qp->lock);
	for (o = rq->out_head; o; o = next) {
		next = o->next;
		rocev2_free_outstanding(o);
	}
	for (o = rq->read_head; o; o = next) {
		next = o->next;
		rocev2_free_outstanding(o);
	}
	rq->out_head = rq->out_tail = NULL;
	rq->read_head = rq->read_tail = NULL;
	if (rq->frag_recv_active)
		uib_qp_release_recv(&rq->frag_recv);
	uib_mr_put(rq->frag_recv_mr);
	rq->frag_recv_mr = NULL;
	uib_mr_put(rq->frag_write_mr);
	rq->frag_write_mr = NULL;
	uib_psn_index_destroy(&rq->out_index);
	pthread_cond_destroy(&rq->prep_cond);
	rocev2_outstanding_pool_destroy(rq->out_pool);
	qp->protocol_private = NULL;
	free(rq);
}
const struct uib_protocol_ops uib_rocev2_protocol_ops = {
	.name = "rocev2",
	.default_port = UIB_ROCE_V2_UDP_DPORT,
	.source_port_entropy = 1,
	.flow_key_offset = offsetof(struct uib_bth, dqpn),
	.path_mtu_valid = rocev2_path_mtu_valid,
	.min_packet = UIB_BTH_BYTES + UIB_ICRC_BYTES,
	.init = rocev2_context_init,
	.fini = rocev2_context_fini,
	.qp_init = uib_rocev2_qp_init,
	.qp_fini = uib_rocev2_qp_fini,
	.qp_modified = uib_rocev2_qp_modified,
	.packet_qpn = rocev2_packet_qpn,
	.post_send = uib_rocev2_post_send,
	.handle_packet = uib_rocev2_handle_packet_meta,
	.retransmit_tick = uib_rocev2_retransmit_tick,
	.wire_encode = rocev2_wire_encode,
	.wire_decode = rocev2_wire_decode,
};
