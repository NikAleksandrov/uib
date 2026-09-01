/* SPDX-License-Identifier: Apache-2.0 */
#include "transport_udp_internal.h"

static size_t rx_work_capacity(uint8_t pool_class)
{
	static const size_t capacities[UIB_UDP_RX_POOL_CLASSES] = {
		256,
		2048,
		8192,
		UIB_RECV_BUF_LEN,
	};

	return capacities[pool_class];
}

static uint8_t rx_work_pool_class(size_t length)
{
	uint8_t pool_class;

	for (pool_class = 0; pool_class < UIB_UDP_RX_POOL_CLASSES;
	     pool_class++) {
		if (length <= rx_work_capacity(pool_class))
			return pool_class;
	}
	return UIB_UDP_RX_POOL_CLASSES;
}

static struct uib_udp_rx_work *rx_work_new(uint8_t pool_class)
{
	struct uib_udp_rx_work *work;

	work = malloc(sizeof(*work) + rx_work_capacity(pool_class));
	if (work)
		work->pool_class = pool_class;
	return work;
}

UIB_UDP_INTERNAL struct uib_udp_rx_work *
rx_work_alloc(struct uib_udp_rx_worker *worker, size_t length)
{
	struct uib_udp_rx_work *work;
	uint8_t pool_class;

	pool_class = rx_work_pool_class(length);
	if (pool_class == UIB_UDP_RX_POOL_CLASSES)
		return NULL;
	work = uib_ptr_ring_pop(&worker->pool[pool_class]);
	if (work) {
		__atomic_fetch_add(&worker->backend->stats.rx_pool_hits, 1,
				   __ATOMIC_RELAXED);
	} else {
		__atomic_fetch_add(&worker->backend->stats.rx_pool_misses, 1,
				   __ATOMIC_RELAXED);
		work = rx_work_new(pool_class);
	}
	return work;
}

UIB_UDP_INTERNAL void rx_work_release(struct uib_udp_rx_worker *worker,
			    struct uib_udp_rx_work *work)
{
	if (!work)
		return;
	if (work->pool_class < worker->pools_initialized &&
	    !uib_ptr_ring_push(&worker->pool[work->pool_class], work))
		return;
	free(work);
}

static void rx_worker_signal_init(struct uib_udp_rx_worker *worker, int rc)
{
	pthread_mutex_lock(&worker->lock);
	worker->init_rc = rc;
	worker->init_done = 1;
	pthread_cond_signal(&worker->cond);
	pthread_mutex_unlock(&worker->lock);
}

UIB_UDP_INTERNAL void *rx_worker_loop(void *argument)
{
	struct uib_udp_rx_worker *worker = argument;
	struct uib_udp_backend *be = worker->backend;
	struct uib_udp_rx_work *work;
	cpu_set_t cpus;
	int rc;

	rc = 0;
	if (worker->cpu_set) {
		CPU_ZERO(&cpus);
		CPU_SET(worker->cpu, &cpus);
		rc = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
	}
	rx_worker_signal_init(worker, rc ? -rc : 0);
	if (rc)
		return NULL;
	for (;;) {
		work = uib_ptr_ring_pop(&worker->queue);
		if (work) {
			if (work->integrity_validated)
				uib_transport_deliver_validated(be->ctx, &work->source,
					work->data, work->length, work->tos,
					work->rx_time_ns);
			else
				uib_transport_deliver_ecn(be->ctx, &work->source,
					work->data, work->length, work->tos,
					work->rx_time_ns);
			rx_work_release(worker, work);
			continue;
		}
		pthread_mutex_lock(&worker->lock);
		while (__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE) &&
		       !uib_ptr_ring_count(&worker->queue))
			pthread_cond_wait(&worker->cond, &worker->lock);
		rc = __atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE);
		pthread_mutex_unlock(&worker->lock);
		if (!rc && !uib_ptr_ring_count(&worker->queue))
			break;
	}
	return NULL;
}

UIB_UDP_INTERNAL void inflight_add(struct uib_udp_backend *be, struct uib_udp_tx *tx)
{
	tx->inflight_prev = NULL;
	tx->inflight_next = be->tx_inflight;
	if (be->tx_inflight)
		be->tx_inflight->inflight_prev = tx;
	be->tx_inflight = tx;
}

UIB_UDP_INTERNAL void inflight_remove(struct uib_udp_backend *be, struct uib_udp_tx *tx)
{
	if (tx->inflight_prev)
		tx->inflight_prev->inflight_next = tx->inflight_next;
	else
		be->tx_inflight = tx->inflight_next;
	if (tx->inflight_next)
		tx->inflight_next->inflight_prev = tx->inflight_prev;
	tx->inflight_next = NULL;
	tx->inflight_prev = NULL;
}

static void init_tx(struct uib_udp_tx *tx)
{
	tx->op.type = UIB_UDP_OP_TX;
	tx->message.msg_control = NULL;
	tx->message.msg_controllen = 0;
	tx->message.msg_flags = 0;
	tx->message.msg_iovlen = 1;
}

static void reset_tx(struct uib_udp_tx *tx)
{
	tx->release = NULL;
	tx->release_list = NULL;
	tx->release_opaque = NULL;
	tx->release_count = 0;
	tx->packets = 1;
	tx->iov_list = tx->iov;
	tx->iov_capacity = UIB_UDP_REF_INLINE_IOVS;
	tx->iov_allocated = 0;
	tx->gso_size = 0;
	tx->zc_result = 0;
	tx->zerocopy = 0;
	tx->message.msg_control = NULL;
	tx->message.msg_controllen = 0;
	tx->message.msg_iovlen = 1;
}

static struct uib_udp_tx *new_ref_tx(void)
{
	struct uib_udp_tx *tx;

	tx = malloc(sizeof(*tx));
	if (tx)
		init_tx(tx);
	return tx;
}

UIB_UDP_INTERNAL struct uib_udp_tx *alloc_ref_tx(struct uib_udp_backend *be)
{
	struct uib_udp_tx *tx;

	tx = uib_ptr_ring_pop(&be->tx_ref_pool);
	if (!tx)
		tx = new_ref_tx();
	if (tx) {
		tx->pool = UIB_UDP_TX_REF_POOL;
		tx->storage_capacity = 0;
		reset_tx(tx);
	}
	return tx;
}

static struct uib_udp_tx *new_copy_tx(size_t capacity)
{
	struct uib_udp_tx *tx;

	tx = malloc(sizeof(*tx) + capacity);
	if (tx)
		init_tx(tx);
	return tx;
}

UIB_UDP_INTERNAL struct uib_udp_tx *alloc_copy_tx(struct uib_udp_backend *be,
						size_t storage_len)
{
	struct uib_udp_tx *tx;
	size_t capacity;

	tx = NULL;
	capacity = storage_len <= UIB_UDP_COPY_CAPACITY ?
		UIB_UDP_COPY_CAPACITY : storage_len;
	if (capacity == UIB_UDP_COPY_CAPACITY)
		tx = uib_ptr_ring_pop(&be->tx_copy_pool);
	if (!tx)
		tx = new_copy_tx(capacity);
	if (!tx)
		return NULL;
	tx->pool = capacity == UIB_UDP_COPY_CAPACITY ?
		UIB_UDP_TX_COPY_POOL : UIB_UDP_TX_UNPOOLED;
	tx->storage_capacity = capacity;
	reset_tx(tx);
	return tx;
}

static struct uib_udp_tx *new_gso_tx(void)
{
	struct uib_udp_tx *tx;
	size_t allocation;

	allocation = sizeof(*tx) +
		UIB_UDP_GSO_MAX_IOVS * sizeof(*tx->iov_list) +
		UIB_UDP_GSO_MAX_SEGMENTS * sizeof(*tx->release_list);
	tx = malloc(allocation);
	if (tx)
		init_tx(tx);
	return tx;
}

static void prepare_gso_tx(struct uib_udp_tx *tx)
{
	tx->pool = UIB_UDP_TX_GSO_POOL;
	tx->storage_capacity = 0;
	reset_tx(tx);
	tx->iov_list = (struct iovec *)tx->storage;
	tx->iov_capacity = UIB_UDP_GSO_MAX_IOVS;
	tx->release_list = (struct uib_udp_release *)
		(tx->iov_list + UIB_UDP_GSO_MAX_IOVS);
}

UIB_UDP_INTERNAL struct uib_udp_tx *alloc_gso_tx(struct uib_udp_backend *be)
{
	struct uib_udp_tx *tx;

	tx = uib_ptr_ring_pop(&be->tx_gso_pool);
	if (!tx)
		tx = new_gso_tx();
	if (!tx)
		return NULL;
	prepare_gso_tx(tx);
	return tx;
}

UIB_UDP_INTERNAL void free_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx);

UIB_UDP_INTERNAL size_t buffer_page_count(const struct uib_udp_backend *be,
				const void *buffer, size_t length)
{
	size_t offset;

	if (!length)
		return 0;
	offset = (uintptr_t)buffer & (be->page_size - 1);
	return (offset + length + be->page_size - 1) / be->page_size;
}

UIB_UDP_INTERNAL int gso_zerocopy_eligible(const struct uib_context *ctx,
				 const struct uib_udp_tx *tx)
{
	const struct uib_udp_backend *be = ctx->backend;
	size_t pages = 0;
	size_t iov_count;
	size_t i;

	if (!be->zerocopy_enabled || ctx->zerocopy.mode == UIB_ZEROCOPY_OFF ||
	    tx->length < ctx->zerocopy.threshold)
		return 0;
	iov_count = (size_t)tx->message.msg_iovlen;
	for (i = 0; i < iov_count; i++) {
		pages += buffer_page_count(be, tx->iov_list[i].iov_base,
					   tx->iov_list[i].iov_len);
		if (pages > UIB_UDP_ZC_MAX_FRAGS)
			return 0;
	}
	return 1;
}

UIB_UDP_INTERNAL size_t limit_zerocopy_run(const struct uib_context *ctx,
				 const struct uib_transport_ref *refs,
				 size_t first, size_t run)
{
	const struct uib_udp_backend *be = ctx->backend;
	size_t pages = 0;
	size_t i;

	if (!be->zerocopy_enabled || ctx->zerocopy.mode == UIB_ZEROCOPY_OFF ||
	    run * refs[first].len < ctx->zerocopy.threshold)
		return run;
	for (i = 0; i < run; i++) {
		pages += buffer_page_count(be, refs[first + i].buf,
					   refs[first + i].len);
		if (pages > UIB_UDP_ZC_MAX_FRAGS)
			return i ? i : 1;
	}
	return run;
}

UIB_UDP_INTERNAL int gso_compatible(const struct uib_udp_tx *head,
			  const struct uib_udp_tx *next)
{
	if (!head->gso_size || head->gso_size != next->gso_size)
		return 0;
	if (head->zerocopy || next->zerocopy)
		return 0;
	if (memcmp(&head->destination, &next->destination,
		   sizeof(head->destination)))
		return 0;
	if (head->packets + next->packets > UIB_UDP_GSO_MAX_SEGMENTS)
		return 0;
	if (head->length + next->length > UIB_UDP_GSO_CAPACITY)
		return 0;
	if ((size_t)head->message.msg_iovlen +
	    (size_t)next->message.msg_iovlen > head->iov_capacity)
		return 0;
	return 1;
}

UIB_UDP_INTERNAL void merge_gso_tx(struct uib_udp_backend *be,
			 struct uib_udp_tx *head, struct uib_udp_tx *next)
{
	unsigned int release_offset = head->release_count;
	size_t iov_offset = head->message.msg_iovlen;
	size_t iov_count;
	size_t i;

	iov_count = (size_t)next->message.msg_iovlen;
	for (i = 0; i < iov_count; i++)
		head->iov_list[iov_offset + i] = next->iov_list[i];
	for (i = 0; i < (size_t)next->release_count; i++)
		head->release_list[release_offset + i] = next->release_list[i];
	head->message.msg_iovlen += next->message.msg_iovlen;
	head->release_count += next->release_count;
	head->packets += next->packets;
	head->length += next->length;
	next->release_count = 0;
	next->release = NULL;
	free_tx(be, next);
}

UIB_UDP_INTERNAL void free_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx)
{
	uib_transport_release_fn release = tx->release;
	void *opaque = tx->release_opaque;
	unsigned int i;

	if (release)
		release(opaque);
	for (i = 0; i < tx->release_count; i++)
		tx->release_list[i].release(tx->release_list[i].opaque);
	if (tx->iov_allocated)
		free(tx->iov_list);
	if (tx->pool == UIB_UDP_TX_REF_POOL) {
		tx->release = NULL;
		tx->release_opaque = NULL;
		if (!uib_ptr_ring_push(&be->tx_ref_pool, tx))
			tx = NULL;
	} else if (tx->pool == UIB_UDP_TX_COPY_POOL) {
		if (!uib_ptr_ring_push(&be->tx_copy_pool, tx))
			tx = NULL;
	} else if (tx->pool == UIB_UDP_TX_GSO_POOL) {
		if (!uib_ptr_ring_push(&be->tx_gso_pool, tx))
			tx = NULL;
	}
	free(tx);
}

UIB_UDP_INTERNAL void requeue_tx_list(struct uib_udp_backend *be,
			    struct uib_udp_tx *head)
{
	struct uib_udp_tx *tail;

	if (!head)
		return;
	tail = head;
	while (tail->queue_next)
		tail = tail->queue_next;
	tail->queue_next = be->tx_retry_head;
	be->tx_retry_head = head;
}

UIB_UDP_INTERNAL void requeue_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx)
{
	tx->queue_next = NULL;
	requeue_tx_list(be, tx);
}

static struct uib_udp_tx *next_queued_tx(struct uib_udp_backend *be)
{
	struct uib_udp_tx *tx;

	tx = be->tx_retry_head;
	if (tx) {
		be->tx_retry_head = tx->queue_next;
		tx->queue_next = NULL;
		return tx;
	}
	return uib_ptr_ring_pop(&be->tx_queue);
}

UIB_UDP_INTERNAL unsigned int dequeue_tx_batch(struct uib_udp_backend *be,
				     struct uib_udp_tx **head,
				     unsigned int limit)
{
	struct uib_udp_tx *tail;
	struct uib_udp_tx *tx;
	size_t packet_len;
	unsigned int count;
	unsigned int packet_limit;
	unsigned int packets;

	*head = NULL;
	tail = NULL;
	count = 0;
	packets = 0;
	while (count < limit) {
		tx = next_queued_tx(be);
		if (!tx)
			break;
		packet_len = tx->gso_size ? tx->gso_size : tx->length;
		packet_limit = packet_len > UIB_URING_TX_LARGE_PACKET ?
			UIB_URING_TX_LARGE_BATCH : UIB_URING_TX_BATCH_DEFAULT;
		if (!be->tx_batch_set && count &&
		    packets + tx->packets > packet_limit) {
			requeue_tx(be, tx);
			break;
		}
		tx->queue_next = NULL;
		if (tail)
			tail->queue_next = tx;
		else
			*head = tx;
		tail = tx;
		packets += tail->packets;
		count++;
	}
	return count;
}

UIB_UDP_INTERNAL int init_rx_work_pools(struct uib_udp_rx_worker *worker)
{
	struct uib_udp_rx_work *work;
	unsigned int warm_count;
	unsigned int pool_class;
	unsigned int i;
	int rc;

	warm_count = UIB_UDP_RX_POOL_WARM_TOTAL /
		worker->backend->rx_worker_count;
	if (warm_count < UIB_UDP_RX_POOL_WARM_MIN)
		warm_count = UIB_UDP_RX_POOL_WARM_MIN;
	if (warm_count > UIB_UDP_RX_POOL_LIMIT)
		warm_count = UIB_UDP_RX_POOL_LIMIT;
	for (pool_class = 0; pool_class < UIB_UDP_RX_POOL_CLASSES;
	     pool_class++) {
		rc = uib_ptr_ring_init(&worker->pool[pool_class],
				       pool_class == UIB_UDP_RX_POOL_CLASSES - 1 ?
				       UIB_UDP_RX_LARGE_POOL_LIMIT :
				       UIB_UDP_RX_POOL_LIMIT);
		if (rc < 0)
			return rc;
		worker->pools_initialized++;
	}
	for (pool_class = 0; pool_class < UIB_UDP_RX_POOL_CLASSES - 1;
	     pool_class++) {
		for (i = 0; i < warm_count; i++) {
			work = rx_work_new((uint8_t)pool_class);
			if (!work)
				break;
			if (uib_ptr_ring_push(&worker->pool[pool_class], work)) {
				free(work);
				break;
			}
		}
	}
	return 0;
}

UIB_UDP_INTERNAL void cleanup_rx_work_pools(struct uib_udp_rx_worker *worker)
{
	struct uib_udp_rx_work *work;
	unsigned int pool_class;

	for (pool_class = 0; pool_class < worker->pools_initialized;
	     pool_class++) {
		while ((work = uib_ptr_ring_pop(&worker->pool[pool_class])) !=
		       NULL)
			free(work);
		uib_ptr_ring_destroy(&worker->pool[pool_class]);
	}
	worker->pools_initialized = 0;
}

UIB_UDP_INTERNAL int init_tx_rings(struct uib_udp_backend *be)
{
	int rc;

	rc = uib_ptr_ring_init(&be->tx_queue, UIB_URING_TX_LIMIT);
	if (rc < 0)
		return rc;
	rc = uib_ptr_ring_init(&be->tx_ref_pool, UIB_URING_TX_POOL_LIMIT);
	if (rc < 0)
		return rc;
	rc = uib_ptr_ring_init(&be->tx_copy_pool, UIB_URING_COPY_POOL_LIMIT);
	if (rc < 0)
		return rc;
	return uib_ptr_ring_init(&be->tx_gso_pool,
				 UIB_URING_GSO_POOL_LIMIT);
}

UIB_UDP_INTERNAL void warm_tx_pools(struct uib_udp_backend *be)
{
	struct uib_udp_tx *tx;
	unsigned int i;

	for (i = 0; i < UIB_URING_TX_POOL_WARM; i++) {
		tx = new_ref_tx();
		if (!tx)
			break;
		tx->pool = UIB_UDP_TX_REF_POOL;
		tx->storage_capacity = 0;
		reset_tx(tx);
		if (uib_ptr_ring_push(&be->tx_ref_pool, tx) < 0) {
			free(tx);
			break;
		}
	}
	for (i = 0; i < UIB_URING_COPY_POOL_WARM; i++) {
		tx = new_copy_tx(UIB_UDP_COPY_CAPACITY);
		if (!tx)
			break;
		tx->pool = UIB_UDP_TX_COPY_POOL;
		tx->storage_capacity = UIB_UDP_COPY_CAPACITY;
		reset_tx(tx);
		if (uib_ptr_ring_push(&be->tx_copy_pool, tx) < 0) {
			free(tx);
			break;
		}
	}
	for (i = 0; i < UIB_URING_GSO_POOL_WARM; i++) {
		tx = new_gso_tx();
		if (!tx)
			break;
		prepare_gso_tx(tx);
		if (uib_ptr_ring_push(&be->tx_gso_pool, tx) < 0) {
			free(tx);
			break;
		}
	}
}
