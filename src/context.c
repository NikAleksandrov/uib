#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "uib/context.h"
#include "uib/qp.h"
#include "uib/transport.h"
#include "uib/protocol.h"

struct uib_context *uib_open_device(const char *bind_ip, uint16_t bind_port)
{
	const char *protocol = getenv("UIB_PROTOCOL");

	return uib_open_device_protocol(bind_ip, bind_port, protocol);
}

struct uib_context *uib_open_device_protocol(const char *bind_ip,
					     uint16_t bind_port,
					     const char *protocol)
{
	struct uib_context *ctx = calloc(1, sizeof(*ctx));

	if (!ctx)
		return NULL;

	ctx->pops = uib_protocol_find(protocol);
	if (!ctx->pops) {
		errno = EPROTONOSUPPORT;
		free(ctx);
		return NULL;
	}
	ctx->mode = uib_transport_mode_from_env();
	uib_zerocopy_init(ctx);
	ctx->bind_port = bind_port ? bind_port : ctx->pops->default_port;
	if (bind_ip && *bind_ip)
		snprintf(ctx->bind_ip, sizeof(ctx->bind_ip), "%s", bind_ip);

	pthread_mutex_init(&ctx->qp_lock, NULL);
	pthread_rwlock_init(&ctx->qp_io_lock, NULL);
	ctx->qp_table_size = UIB_DEFAULT_QP_TABLE;
	ctx->qp_table = calloc(ctx->qp_table_size, sizeof(*ctx->qp_table));
	ctx->qp_free_ids = calloc(ctx->qp_table_size,
				  sizeof(*ctx->qp_free_ids));
	ctx->retry_heap = calloc(ctx->qp_table_size, sizeof(*ctx->retry_heap));
	if (!ctx->qp_table || !ctx->qp_free_ids || !ctx->retry_heap)
		goto err_table;
	ctx->qp_next_id = 1;
	if (ctx->pops->init && ctx->pops->init(ctx) < 0)
		goto err_table;

	if (uib_transport_init(ctx) < 0)
		goto err_protocol;

	return ctx;

err_protocol:
	if (ctx->pops->fini)
		ctx->pops->fini(ctx);
err_table:
	free(ctx->retry_heap);
	free(ctx->qp_free_ids);
	free(ctx->qp_table);
	pthread_rwlock_destroy(&ctx->qp_io_lock);
	pthread_mutex_destroy(&ctx->qp_lock);
	pthread_mutex_destroy(&ctx->zerocopy_lock);
	free(ctx);
	return NULL;
}

int uib_close_device(struct uib_context *ctx)
{
	if (!ctx)
		return -EINVAL;
	uib_transport_fini(ctx);
	if (ctx->pops && ctx->pops->fini)
		ctx->pops->fini(ctx);
	pthread_rwlock_destroy(&ctx->qp_io_lock);
	pthread_mutex_destroy(&ctx->qp_lock);
	pthread_mutex_destroy(&ctx->zerocopy_lock);
	free(ctx->retry_heap);
	free(ctx->qp_free_ids);
	free(ctx->qp_table);
	free(ctx);
	return 0;
}

uint32_t uib_register_qp(struct uib_context *ctx, struct uib_qp *qp)
{
	struct uib_qp **new_retry;
	struct uib_qp **new_table;
	uint32_t *new_free;
	uint32_t new_size;
	uint32_t i;

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
		free(ctx->qp_table);
		ctx->retry_heap = new_retry;
		ctx->qp_free_ids = new_free;
		ctx->qp_table = new_table;
		ctx->qp_table_size = new_size;
	}
	ctx->qp_next_id++;
found:
	ctx->qp_table[i] = qp;
	pthread_mutex_unlock(&ctx->qp_lock);
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

void uib_unregister_qp(struct uib_context *ctx, struct uib_qp *qp)
{
	if (!qp || !qp->qp_num || qp->qp_num >= ctx->qp_table_size)
		return;
	pthread_mutex_lock(&ctx->qp_lock);
	if (ctx->qp_table[qp->qp_num] == qp) {
		retry_heap_remove_locked(ctx, qp);
		ctx->qp_table[qp->qp_num] = NULL;
		ctx->qp_free_ids[ctx->qp_free_count++] = qp->qp_num;
	}
	pthread_mutex_unlock(&ctx->qp_lock);
}

struct uib_qp *uib_lookup_qp(struct uib_context *ctx, uint32_t qpn)
{
	if (qpn == 0 || qpn >= ctx->qp_table_size)
		return NULL;
	pthread_mutex_lock(&ctx->qp_lock);
	struct uib_qp *qp = ctx->qp_table[qpn];

	pthread_mutex_unlock(&ctx->qp_lock);
	return qp;
}
