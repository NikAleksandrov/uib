/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <urcu-bp.h>

#include "uib/context.h"
#include "uib/env.h"
#include "uib/qp.h"
#include "uib/transport.h"
#include "uib/protocol.h"
#include "observability.h"
#include "utils.h"

#define UIB_RETRY_TIMEOUT_DEFAULT_US 20000ULL
#define UIB_RETRY_TIMEOUT_MIN_US      1000ULL
#define UIB_RETRY_TIMEOUT_MAX_US  60000000ULL

static int retry_timeout_init(struct uib_context *ctx)
{
	const char *setting;
	uint64_t timeout_us;

	setting = getenv(UIB_ENV_RETRY_TIMEOUT_US);
	if (!setting) {
		ctx->retry_timeout_ns = UIB_RETRY_TIMEOUT_DEFAULT_US * 1000ULL;
		return 0;
	}
	if (uib_parse_u64(setting, 10, &timeout_us) < 0 ||
	    timeout_us < UIB_RETRY_TIMEOUT_MIN_US ||
	    timeout_us > UIB_RETRY_TIMEOUT_MAX_US)
		return -EINVAL;
	ctx->retry_timeout_ns = timeout_us * 1000ULL;
	return 0;
}

/**
 * uib_open_device - Open a context using the environment-selected protocol.
 * @bind_ip: Local IPv4 address in presentation format
 * @bind_port: Local transport port in host byte order
 * @uib-api: Contexts and QP dispatch
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_context *uib_open_device(const char *bind_ip, uint16_t bind_port)
{
	const char *protocol = getenv(UIB_ENV_PROTOCOL);

	return uib_open_device_protocol(bind_ip, bind_port, protocol);
}

/**
 * uib_open_device_protocol - Open a context using an explicit wire protocol.
 * @bind_ip: Local IPv4 address in presentation format
 * @bind_port: Local transport port in host byte order
 * @protocol: Wire protocol name
 * @uib-api: Contexts and QP dispatch
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_context *uib_open_device_protocol(const char *bind_ip,
					     uint16_t bind_port,
					     const char *protocol)
{
	struct uib_context *ctx = calloc(1, sizeof(*ctx));
	int rc;

	if (!ctx)
		return NULL;

	ctx->pops = uib_protocol_find(protocol);
	if (!ctx->pops) {
		errno = EPROTONOSUPPORT;
		free(ctx);
		return NULL;
	}
	ctx->mode = uib_transport_mode_from_env();
	rc = retry_timeout_init(ctx);
	if (rc < 0) {
		errno = -rc;
		goto err_context;
	}
	uib_zerocopy_init(ctx);
	ctx->bind_port = bind_port ? bind_port : ctx->pops->default_port;
	if (bind_ip && *bind_ip)
		snprintf(ctx->bind_ip, sizeof(ctx->bind_ip), "%s", bind_ip);

	pthread_mutex_init(&ctx->qp_lock, NULL);
	ctx->qp_table_size = UIB_DEFAULT_QP_TABLE;
	ctx->qp_table = calloc(ctx->qp_table_size, sizeof(*ctx->qp_table));
	ctx->qp_free_ids = calloc(ctx->qp_table_size,
				  sizeof(*ctx->qp_free_ids));
	ctx->retry_heap = calloc(ctx->qp_table_size, sizeof(*ctx->retry_heap));
	if (!ctx->qp_table || !ctx->qp_free_ids || !ctx->retry_heap)
		goto err_table;
	ctx->qp_next_id = 1;
	if (ctx->pops->init) {
		rc = ctx->pops->init(ctx);
		if (rc < 0)
			goto err_errno;
	}

	rc = uib_transport_init(ctx);
	if (rc < 0)
		goto err_protocol;

	return ctx;

err_protocol:
	if (ctx->pops->fini)
		ctx->pops->fini(ctx);
err_errno:
	errno = -rc;
err_table:
	free(ctx->retry_heap);
	free(ctx->qp_free_ids);
	free(ctx->qp_table);
	pthread_mutex_destroy(&ctx->qp_lock);
	pthread_mutex_destroy(&ctx->zerocopy_lock);
err_context:
	free(ctx);
	return NULL;
}

/**
 * uib_close_device - Stop transport workers and release a context.
 * @ctx: UIB device context
 * @uib-api: Contexts and QP dispatch
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_close_device(struct uib_context *ctx)
{
	if (!ctx)
		return -EINVAL;
	uib_observability_emit(ctx);
	uib_transport_fini(ctx);
	if (ctx->pops && ctx->pops->fini)
		ctx->pops->fini(ctx);
	pthread_mutex_destroy(&ctx->qp_lock);
	pthread_mutex_destroy(&ctx->zerocopy_lock);
	free(ctx->retry_heap);
	free(ctx->qp_free_ids);
	free(ctx->qp_table);
	free(ctx);
	return 0;
}

/**
 * uib_register_qp - Assign a QPN and publish a QP in its context.
 * @ctx: UIB device context
 * @qp: Queue pair
 * @uib-api: Contexts and QP dispatch
 *
 * Return: The assigned nonzero QP number on success, or 0 on failure.
 */
uint32_t uib_register_qp(struct uib_context *ctx, struct uib_qp *qp)
{
	struct uib_qp **new_retry;
	struct uib_qp **new_table;
	struct uib_qp **old_table;
	uint32_t *new_free;
	uint32_t new_size;
	uint32_t i;

	old_table = NULL;
	pthread_mutex_lock(&ctx->qp_lock);
	if (ctx->qp_free_count) {
		i = ctx->qp_free_ids[--ctx->qp_free_count];
		goto found;
	}
	i = ctx->qp_next_id;
	if (i >= ctx->qp_table_size) {
		new_size = ctx->qp_table_size * 2;
		if (new_size <= ctx->qp_table_size)
			goto no_space;
		new_table = calloc(new_size, sizeof(*new_table));
		new_free = calloc(new_size, sizeof(*new_free));
		new_retry = calloc(new_size, sizeof(*new_retry));
		if (!new_table || !new_free || !new_retry) {
			free(new_retry);
			free(new_free);
			free(new_table);
			goto no_space;
		}
		memcpy(new_table, ctx->qp_table,
		       ctx->qp_table_size * sizeof(*new_table));
		memcpy(new_free, ctx->qp_free_ids,
		       ctx->qp_free_count * sizeof(*new_free));
		memcpy(new_retry, ctx->retry_heap,
		       ctx->retry_heap_size * sizeof(*new_retry));
		free(ctx->retry_heap);
		free(ctx->qp_free_ids);
		old_table = ctx->qp_table;
		ctx->retry_heap = new_retry;
		ctx->qp_free_ids = new_free;
		rcu_assign_pointer(ctx->qp_table, new_table);
		__atomic_store_n(&ctx->qp_table_size, new_size,
				 __ATOMIC_RELEASE);
	}
	ctx->qp_next_id++;
found:
	rcu_assign_pointer(ctx->qp_table[i], qp);
	pthread_mutex_unlock(&ctx->qp_lock);
	if (old_table) {
		synchronize_rcu();
		free(old_table);
	}
	return i;

no_space:
	pthread_mutex_unlock(&ctx->qp_lock);
	return 0;
}

static void retry_heap_swap(struct uib_context *ctx, uint32_t a, uint32_t b)
{
	struct uib_qp *qp = ctx->retry_heap[a];

	ctx->retry_heap[a] = ctx->retry_heap[b];
	ctx->retry_heap[b] = qp;
	ctx->retry_heap[a]->retry_heap_index = a;
	ctx->retry_heap[b]->retry_heap_index = b;
}

static void retry_heap_up(struct uib_context *ctx, uint32_t index)
{
	uint32_t parent;

	while (index) {
		parent = (index - 1) / 2;
		if (ctx->retry_heap[parent]->retry_deadline_ns <=
		    ctx->retry_heap[index]->retry_deadline_ns)
			break;
		retry_heap_swap(ctx, parent, index);
		index = parent;
	}
}

static void retry_heap_down(struct uib_context *ctx, uint32_t index)
{
	uint32_t smallest;
	uint32_t right;
	uint32_t left;

	for (;;) {
		left = index * 2 + 1;
		if (left >= ctx->retry_heap_size)
			break;
		right = left + 1;
		smallest = left;
		if (right < ctx->retry_heap_size &&
		    ctx->retry_heap[right]->retry_deadline_ns <
		    ctx->retry_heap[left]->retry_deadline_ns)
			smallest = right;
		if (ctx->retry_heap[index]->retry_deadline_ns <=
		    ctx->retry_heap[smallest]->retry_deadline_ns)
			break;
		retry_heap_swap(ctx, index, smallest);
		index = smallest;
	}
}

static void retry_heap_remove_locked(struct uib_context *ctx,
				     struct uib_qp *qp)
{
	uint32_t index;

	if (!qp->retry_scheduled)
		return;
	index = qp->retry_heap_index;
	ctx->retry_heap_size--;
	qp->retry_scheduled = 0;
	if (index == ctx->retry_heap_size)
		return;
	ctx->retry_heap[index] = ctx->retry_heap[ctx->retry_heap_size];
	ctx->retry_heap[index]->retry_heap_index = index;
	if (index &&
	    ctx->retry_heap[index]->retry_deadline_ns <
	    ctx->retry_heap[(index - 1) / 2]->retry_deadline_ns)
		retry_heap_up(ctx, index);
	else
		retry_heap_down(ctx, index);
}

/**
 * uib_qp_retry_schedule - Add or update a QP retransmission deadline.
 * @qp: Queue pair
 * @deadline_ns: Absolute retry deadline in nanoseconds
 * @uib-api: Contexts and QP dispatch
 */
void uib_qp_retry_schedule(struct uib_qp *qp, uint64_t deadline_ns)
{
	struct uib_context *ctx;
	uint64_t old_deadline;
	uint32_t index;

	if (!qp)
		return;
	ctx = qp->ctx;
	pthread_mutex_lock(&ctx->qp_lock);
	if (qp->retry_scheduled) {
		old_deadline = qp->retry_deadline_ns;
		qp->retry_deadline_ns = deadline_ns;
		index = qp->retry_heap_index;
		if (deadline_ns < old_deadline)
			retry_heap_up(ctx, index);
		else if (deadline_ns > old_deadline)
			retry_heap_down(ctx, index);
	} else {
		index = ctx->retry_heap_size++;
		qp->retry_deadline_ns = deadline_ns;
		qp->retry_heap_index = index;
		qp->retry_scheduled = 1;
		ctx->retry_heap[index] = qp;
		retry_heap_up(ctx, index);
	}
	pthread_mutex_unlock(&ctx->qp_lock);
}

/**
 * uib_qp_retry_cancel - Remove a QP from the retransmission heap.
 * @qp: Queue pair
 * @uib-api: Contexts and QP dispatch
 */
void uib_qp_retry_cancel(struct uib_qp *qp)
{
	struct uib_context *ctx;

	if (!qp)
		return;
	ctx = qp->ctx;
	pthread_mutex_lock(&ctx->qp_lock);
	retry_heap_remove_locked(ctx, qp);
	pthread_mutex_unlock(&ctx->qp_lock);
}

/**
 * uib_retry_pop_due - Remove and return the next due retransmission QP.
 * @ctx: UIB device context
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Contexts and QP dispatch
 *
 * Return: The next QP whose retry deadline has passed, or NULL if none is due.
 */
struct uib_qp *uib_retry_pop_due(struct uib_context *ctx, uint64_t now_ns)
{
	struct uib_qp *qp = NULL;

	pthread_mutex_lock(&ctx->qp_lock);
	if (ctx->retry_heap_size &&
	    ctx->retry_heap[0]->retry_deadline_ns <= now_ns) {
		qp = ctx->retry_heap[0];
		retry_heap_remove_locked(ctx, qp);
	}
	pthread_mutex_unlock(&ctx->qp_lock);
	return qp;
}

/**
 * uib_unregister_qp - Remove a QP from context lookup structures.
 * @ctx: UIB device context
 * @qp: Queue pair
 * @uib-api: Contexts and QP dispatch
 */
void uib_unregister_qp(struct uib_context *ctx, struct uib_qp *qp)
{
	int removed;

	if (!qp || !qp->qp_num || qp->qp_num >= ctx->qp_table_size)
		return;
	removed = 0;
	pthread_mutex_lock(&ctx->qp_lock);
	if (ctx->qp_table[qp->qp_num] == qp) {
		retry_heap_remove_locked(ctx, qp);
		rcu_assign_pointer(ctx->qp_table[qp->qp_num], NULL);
		removed = 1;
	}
	pthread_mutex_unlock(&ctx->qp_lock);
	if (!removed)
		return;
	synchronize_rcu();
	pthread_mutex_lock(&ctx->qp_lock);
	ctx->qp_free_ids[ctx->qp_free_count++] = qp->qp_num;
	pthread_mutex_unlock(&ctx->qp_lock);
}

/**
 * uib_lookup_qp - Find a QP by QPN with internal synchronization.
 * @ctx: UIB device context
 * @qpn: Queue pair number
 * @uib-api: Contexts and QP dispatch
 *
 * Return: The matching QP, or NULL if @qpn is not registered.
 */
struct uib_qp *uib_lookup_qp(struct uib_context *ctx, uint32_t qpn)
{
	struct uib_qp *qp;

	rcu_read_lock();
	qp = uib_lookup_qp_rcu(ctx, qpn);
	rcu_read_unlock();
	return qp;
}

/**
 * uib_lookup_qp_rcu - Find a QP while the caller holds an RCU read lock.
 * @ctx: UIB device context
 * @qpn: Queue pair number
 * @uib-api: Contexts and QP dispatch
 *
 * Return: The matching QP, or NULL if @qpn is not registered.
 */
struct uib_qp *uib_lookup_qp_rcu(struct uib_context *ctx, uint32_t qpn)
{
	if (qpn == 0 ||
	    qpn >= __atomic_load_n(&ctx->qp_table_size, __ATOMIC_ACQUIRE))
		return NULL;
	return rcu_dereference(ctx->qp_table)[qpn];
}
