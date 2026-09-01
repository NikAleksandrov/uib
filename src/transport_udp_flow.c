/* SPDX-License-Identifier: Apache-2.0 */
#include "transport_udp_internal.h"

static void udp_flow_account_tx(struct uib_udp_backend *be,
				struct uib_udp_tx *tx, ssize_t sent)
{
	__atomic_fetch_add(&be->stats.tx_submitted, tx->packets,
			   __ATOMIC_RELAXED);
	if (sent >= 0 && (size_t)sent == tx->length) {
		__atomic_fetch_add(&be->stats.tx_completed, tx->packets,
				   __ATOMIC_RELAXED);
		if (tx->gso_size) {
			__atomic_fetch_add(&be->stats.tx_gso_batches, 1,
					   __ATOMIC_RELAXED);
			__atomic_fetch_add(&be->stats.tx_gso_packets,
					   tx->packets, __ATOMIC_RELAXED);
		}
		uib_zerocopy_account_copy(be->ctx, tx->length);
	} else {
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
	}
}

static void udp_flow_send_batch(struct uib_udp_flow *flow,
				struct uib_udp_tx **txs, unsigned int count)
{
	struct mmsghdr messages[UIB_UDP_FLOW_TX_BATCH];
	struct uib_udp_backend *be;
	unsigned int completed;
	unsigned int i;
	int sent;

	be = flow->backend;
	memset(messages, 0, count * sizeof(*messages));
	for (i = 0; i < count; i++)
		messages[i].msg_hdr = txs[i]->message;
	sent = sendmmsg(flow->tx_fd, messages, count, MSG_NOSIGNAL);
	completed = sent > 0 ? (unsigned int)sent : 0;
	for (i = 0; i < completed; i++)
		udp_flow_account_tx(be, txs[i], messages[i].msg_len);
	for (i = completed; i < count; i++) {
		sent = sendmsg(flow->tx_fd, &txs[i]->message, MSG_NOSIGNAL);
		udp_flow_account_tx(be, txs[i], sent);
	}
	for (i = 0; i < count; i++)
		free_tx(be, txs[i]);
}

static void *udp_flow_tx_loop(void *argument)
{
	struct uib_udp_tx *txs[UIB_UDP_FLOW_TX_BATCH];
	struct uib_udp_flow *flow;
	unsigned int count;
	int run;

	flow = argument;
	for (;;) {
		count = 0;
		while (count < UIB_UDP_FLOW_TX_BATCH) {
			txs[count] = uib_ptr_ring_pop(&flow->tx_queue);
			if (!txs[count])
				break;
			count++;
		}
		if (count) {
			pthread_mutex_lock(&flow->lock);
			pthread_cond_broadcast(&flow->cond);
			pthread_mutex_unlock(&flow->lock);
			udp_flow_send_batch(flow, txs, count);
			continue;
		}
		pthread_mutex_lock(&flow->lock);
		while (__atomic_load_n(&flow->run, __ATOMIC_ACQUIRE) &&
		       !uib_ptr_ring_count(&flow->tx_queue))
			pthread_cond_wait(&flow->cond, &flow->lock);
		run = __atomic_load_n(&flow->run, __ATOMIC_ACQUIRE);
		pthread_mutex_unlock(&flow->lock);
		if (!run && !uib_ptr_ring_count(&flow->tx_queue))
			break;
	}
	return NULL;
}

static void *udp_flow_rx_loop(void *argument)
{
	struct uib_udp_flow *flow;
	struct uib_udp_backend *be;
	struct sockaddr_in source;
	struct msghdr message;
	struct iovec vector;
	uint8_t control[UIB_UDP_RX_CONTROL_SPACE];
	uint16_t gro_size;
	uint8_t tos;
	ssize_t received;

	flow = argument;
	be = flow->backend;
	memset(&message, 0, sizeof(message));
	vector.iov_base = flow->rx_buffer;
	vector.iov_len = UIB_RECV_BUF_LEN;
	message.msg_name = &source;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	while (__atomic_load_n(&flow->run, __ATOMIC_ACQUIRE)) {
		message.msg_namelen = sizeof(source);
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		message.msg_flags = 0;
		received = recvmsg(flow->rx_fd, &message, 0);
		if (received < 0) {
			if (errno == EINTR)
				continue;
			if (!__atomic_load_n(&flow->run, __ATOMIC_ACQUIRE))
				break;
			continue;
		}
		if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
			continue;
		gro_size = message_gro_size(&message);
		tos = message_tos(&message);
		deliver_udp_payload(be->ctx, flow->rx_buffer,
				    (size_t)received, &source, gro_size, tos);
	}
	return NULL;
}

static int udp_flow_configure_socket(struct uib_udp_backend *be, int fd,
				     int receive, int reuse_port)
{
	int socket_buffer;
	int gso_size;
	int one;
	int tos;

	socket_buffer = UIB_SOCKET_BUFFER_SIZE;
	gso_size = 0;
	one = 1;
	tos = IPTOS_ECN_ECT0;
	if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0)
		return -errno;
	if (receive &&
	    setsockopt(fd, IPPROTO_IP, IP_RECVTOS, &one, sizeof(one)) < 0)
		return -errno;
	if (be->gso_enabled &&
	    setsockopt(fd, IPPROTO_UDP, UDP_SEGMENT, &gso_size,
		       sizeof(gso_size)) < 0)
		return -errno;
	if (receive && be->gro_enabled &&
	    setsockopt(fd, IPPROTO_UDP, UDP_GRO, &one, sizeof(one)) < 0)
		return -errno;
	if (reuse_port &&
	    (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
	     setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0))
		return -errno;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer,
		   sizeof(socket_buffer));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer,
		   sizeof(socket_buffer));
	return 0;
}

static int udp_multipath_flow_socket(struct uib_udp_backend *be,
				     struct uib_udp_flow *flow)
{
	struct sockaddr_in address;
	unsigned int port;
	int rc;

	address = be->local_addr;
	port = ntohs(address.sin_port) +
		(flow->index + 1) * UIB_UDP_FLOW_PORT_STRIDE;
	if (port > UINT16_MAX)
		return -ERANGE;
	address.sin_port = htons((uint16_t)port);
	flow->rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (flow->rx_fd < 0)
		return -errno;
	rc = udp_flow_configure_socket(be, flow->rx_fd, 1, 0);
	if (rc < 0)
		return rc;
	if (bind(flow->rx_fd, (struct sockaddr *)&address,
		 sizeof(address)) < 0)
		return -errno;
	flow->tx_fd = flow->rx_fd;
	return 0;
}

static int udp_fixed_flow_rx_socket(struct uib_udp_backend *be,
				    struct uib_udp_flow *flow)
{
	int rc;

	if (!flow->index) {
		flow->rx_fd = be->fd;
		return 0;
	}
	flow->rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (flow->rx_fd < 0)
		return -errno;
	rc = udp_flow_configure_socket(be, flow->rx_fd, 1, 1);
	if (rc < 0)
		return rc;
	if (bind(flow->rx_fd, (struct sockaddr *)&be->local_addr,
		 sizeof(be->local_addr)) < 0)
		return -errno;
	return 0;
}

static int udp_fixed_flow_tx_socket(struct uib_udp_backend *be,
				    struct uib_udp_flow *flow)
{
	struct sockaddr_in address;
	unsigned int local_port;
	unsigned int port;
	int rc;

	local_port = ntohs(be->local_addr.sin_port);
	port = UIB_UDP_FLOW_SOURCE_BASE + flow->index;
	if (local_port >= UIB_UDP_FLOW_SOURCE_BASE &&
	    local_port < UIB_UDP_FLOW_SOURCE_BASE +
			 be->requested_flow_count)
		port += be->requested_flow_count;
	if (port > UINT16_MAX)
		return -ERANGE;
	flow->tx_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (flow->tx_fd < 0)
		return -errno;
	rc = udp_flow_configure_socket(be, flow->tx_fd, 0, 1);
	if (rc < 0)
		return rc;
	address = be->local_addr;
	address.sin_port = htons((uint16_t)port);
	if (bind(flow->tx_fd, (struct sockaddr *)&address,
		 sizeof(address)) < 0)
		return -errno;
	return 0;
}

static int udp_flow_socket(struct uib_udp_backend *be,
			   struct uib_udp_flow *flow)
{
	int rc;

	if (be->flow_mode == UIB_UDP_FLOW_MULTIPATH)
		return udp_multipath_flow_socket(be, flow);
	rc = udp_fixed_flow_rx_socket(be, flow);
	if (rc < 0)
		return rc;
	return udp_fixed_flow_tx_socket(be, flow);
}

static int udp_attach_flow_filter(struct uib_udp_backend *be)
{
	struct sock_filter code[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 be->ctx->pops->flow_key_offset),
		BPF_STMT(BPF_ALU | BPF_MOD, be->flow_count),
		BPF_STMT(BPF_RET | BPF_A, 0),
	};
	struct sock_fprog program = {
		.len = sizeof(code) / sizeof(code[0]),
		.filter = code,
	};

	if (setsockopt(be->fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF,
		       &program, sizeof(program)) < 0)
		return -errno;
	return 0;
}

UIB_UDP_INTERNAL int udp_flow_count(unsigned int *count)
{
	const char *setting;
	uint64_t value;

	*count = 1;
	setting = getenv(UIB_ENV_UDP_FLOWS);
	if (!setting || !*setting)
		return 0;
	if (uib_parse_u64(setting, 10, &value) < 0 || !value ||
	    value > UIB_UDP_FLOW_MAX)
		return -EINVAL;
	*count = (unsigned int)value;
	return 0;
}

UIB_UDP_INTERNAL int init_udp_flows(struct uib_context *ctx,
			  struct uib_udp_backend *be)
{
	struct uib_udp_flow *flow;
	cpu_set_t allowed;
	unsigned int count;
	unsigned int cpu_index;
	unsigned int cpu_start;
	unsigned int thread_count;
	unsigned int i;
	int affinity;
	int rc;

	count = be->requested_flow_count;
	if (count == 1)
		return 0;
	if (be->raw_mode || be->tx_batch)
		return -ENOTSUP;
	if (ctx->zerocopy.mode == UIB_ZEROCOPY_REQUIRED)
		return -ENOTSUP;
	rc = uib_env_bool(UIB_ENV_UDP_WORKER_AFFINITY, 1, &affinity);
	if (rc)
		return rc;
	CPU_ZERO(&allowed);
	if (affinity && sched_getaffinity(0, sizeof(allowed), &allowed) < 0)
		return -errno;
	thread_count = count * 2;
	if (be->flow_mode == UIB_UDP_FLOW_FIXED_PORT)
		thread_count--;
	cpu_start = affinity ? uib_worker_cpu_reserve(thread_count) : 0;
	cpu_index = 0;
	be->flows = calloc(count, sizeof(*be->flows));
	if (!be->flows)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		be->flows[i].rx_fd = -1;
		be->flows[i].tx_fd = -1;
	}
	for (i = 0; i < count; i++) {
		flow = &be->flows[i];
		flow->backend = be;
		flow->index = i;
		flow->rx_fd = -1;
		flow->tx_fd = -1;
		flow->run = 1;
		be->flow_count = i + 1;
		rc = pthread_mutex_init(&flow->lock, NULL);
		if (rc)
			return -rc;
		rc = pthread_cond_init(&flow->cond, NULL);
		if (rc) {
			pthread_mutex_destroy(&flow->lock);
			return -rc;
		}
		flow->sync_initialized = 1;
		rc = uib_ptr_ring_init(&flow->tx_queue,
				       UIB_UDP_FLOW_QUEUE_SIZE);
		if (rc < 0)
			return rc;
		flow->queue_initialized = 1;
		rc = udp_flow_socket(be, flow);
		if (rc < 0)
			return rc;
		if (flow->rx_fd != be->fd) {
			flow->rx_buffer = malloc(UIB_RECV_BUF_LEN);
			if (!flow->rx_buffer)
				return -ENOMEM;
			rc = uib_worker_thread_start(&flow->rx_thread,
						     udp_flow_rx_loop, flow,
						     UIB_UDP_RX_WORKER_STACK,
						     &allowed,
						     cpu_start + cpu_index,
						     affinity);
			if (rc < 0)
				return rc;
			flow->rx_started = 1;
			cpu_index++;
		}
		rc = uib_worker_thread_start(&flow->tx_thread,
					     udp_flow_tx_loop, flow,
					     UIB_UDP_RX_WORKER_STACK, &allowed,
					     cpu_start + cpu_index, affinity);
		if (rc < 0)
			return rc;
		flow->tx_started = 1;
		cpu_index++;
	}
	if (be->flow_mode == UIB_UDP_FLOW_FIXED_PORT)
		return udp_attach_flow_filter(be);
	return 0;
}

UIB_UDP_INTERNAL void cleanup_udp_flows(struct uib_udp_backend *be)
{
	struct uib_udp_flow *flow;
	struct uib_udp_tx *tx;
	unsigned int i;

	if (!be->flows)
		return;
	for (i = 0; i < be->flow_count; i++) {
		flow = &be->flows[i];
		__atomic_store_n(&flow->run, 0, __ATOMIC_RELEASE);
		if (flow->sync_initialized) {
			pthread_mutex_lock(&flow->lock);
			pthread_cond_broadcast(&flow->cond);
			pthread_mutex_unlock(&flow->lock);
		}
		if (flow->rx_fd >= 0 && flow->rx_fd != be->fd)
			shutdown(flow->rx_fd, SHUT_RDWR);
	}
	for (i = 0; i < be->flow_count; i++) {
		flow = &be->flows[i];
		if (flow->tx_started)
			pthread_join(flow->tx_thread, NULL);
		if (flow->rx_started)
			pthread_join(flow->rx_thread, NULL);
		while (flow->queue_initialized &&
		       (tx = uib_ptr_ring_pop(&flow->tx_queue)) != NULL)
			free_tx(be, tx);
		if (flow->queue_initialized)
			uib_ptr_ring_destroy(&flow->tx_queue);
		if (flow->sync_initialized) {
			pthread_cond_destroy(&flow->cond);
			pthread_mutex_destroy(&flow->lock);
		}
		if (flow->rx_fd >= 0 && flow->rx_fd != be->fd)
			close(flow->rx_fd);
		if (flow->tx_fd >= 0 && flow->tx_fd != flow->rx_fd)
			close(flow->tx_fd);
		free(flow->rx_buffer);
	}
	free(be->flows);
	be->flows = NULL;
	be->flow_count = 0;
}

static int udp_flow_enqueue(struct uib_udp_flow *flow,
			    struct uib_udp_tx *tx)
{
	uint64_t position;
	int was_empty;
	int rc;

	for (;;) {
		rc = uib_ptr_ring_reserve(&flow->tx_queue, 1, &position,
					  &was_empty);
		if (!rc)
			break;
		pthread_mutex_lock(&flow->lock);
		while (__atomic_load_n(&flow->run, __ATOMIC_ACQUIRE) &&
		       uib_ptr_ring_count(&flow->tx_queue) >=
		       UIB_UDP_FLOW_QUEUE_SIZE)
			pthread_cond_wait(&flow->cond, &flow->lock);
		if (!__atomic_load_n(&flow->run, __ATOMIC_ACQUIRE)) {
			pthread_mutex_unlock(&flow->lock);
			return -ESHUTDOWN;
		}
		pthread_mutex_unlock(&flow->lock);
	}
	uib_ptr_ring_publish(&flow->tx_queue, position, tx);
	if (was_empty) {
		pthread_mutex_lock(&flow->lock);
		pthread_cond_signal(&flow->cond);
		pthread_mutex_unlock(&flow->lock);
	}
	return 0;
}

UIB_UDP_INTERNAL int udp_flow_send_iov_ref_batch(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count)
{
	struct uib_udp_backend *be;
	struct uib_udp_flow *flow;
	struct uib_udp_tx *tx;
	unsigned int lane;
	unsigned int port;
	size_t run;
	size_t i;
	int rc;

	be = ctx->backend;
	for (i = 0; i < count; i++) {
		rc = udp_iov_ref_validate(&refs[i]);
		if (rc < 0) {
			for (i = 0; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
	}
	i = 0;
	while (i < count) {
		run = udp_iov_ref_run(ctx, be, refs, i, count);
		tx = run > 1 ? udp_iov_ref_gso_tx(ctx, be, refs, i, run) :
			udp_iov_ref_one_tx(ctx, be, &refs[i]);
		if (!tx) {
			for (; i < count; i++)
				refs[i].release(refs[i].opaque);
			return -ENOMEM;
		}
		lane = refs[i].flow % be->flow_count;
		flow = &be->flows[lane];
		if (be->flow_mode == UIB_UDP_FLOW_MULTIPATH) {
			port = ntohs(tx->destination.sin_port) +
				(lane + 1) * UIB_UDP_FLOW_PORT_STRIDE;
			if (port > UINT16_MAX) {
				free_tx(be, tx);
				for (i += run; i < count; i++)
					refs[i].release(refs[i].opaque);
				return -ERANGE;
			}
			tx->destination.sin_port = htons((uint16_t)port);
		}
		rc = udp_flow_enqueue(flow, tx);
		if (rc < 0) {
			free_tx(be, tx);
			for (i += run; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
		i += run;
	}
	return 0;
}

UIB_UDP_INTERNAL int udp_send_iov_ref_batch(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_tx *head = NULL;
	struct uib_udp_tx *tail = NULL;
	struct uib_udp_tx *tx;
	size_t queued = 0;
	size_t run;
	size_t i = 0;
	int rc;

	if (!count)
		return 0;
	for (i = 0; i < count; i++) {
		rc = udp_iov_ref_validate(&refs[i]);
		if (rc < 0) {
			for (i = 0; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
	}
	i = 0;
	if (be->raw_mode || be->test_drop_tx)
		return udp_send_iov_ref_fallback(ctx, refs, count);
	if (!__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&be->accepting, __ATOMIC_ACQUIRE)) {
		rc = -ESHUTDOWN;
		goto release;
	}
	while (i < count) {
		run = udp_iov_ref_run(ctx, be, refs, i, count);
		tx = run > 1 ? udp_iov_ref_gso_tx(ctx, be, refs, i, run) :
			udp_iov_ref_one_tx(ctx, be, &refs[i]);
		if (!tx) {
			rc = -ENOMEM;
			goto release;
		}
		tx->queue_next = NULL;
		if (tail)
			tail->queue_next = tx;
		else
			head = tx;
		tail = tx;
		queued++;
		i += run;
	}
	rc = udp_publish_ref_batch(be, head, queued, count);
	if (!rc)
		return 0;

release:
	while (head) {
		tx = head;
		head = head->queue_next;
		free_tx(be, tx);
	}
	for (; i < count; i++)
		refs[i].release(refs[i].opaque);
	return rc;
}

UIB_UDP_INTERNAL int udp_send_ref_batch(struct uib_context *ctx,
			      const struct uib_transport_ref *refs,
			      size_t count)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_tx *head = NULL;
	struct uib_udp_tx *tail = NULL;
	struct uib_udp_tx *tx;
	size_t queued = 0;
	size_t run;
	size_t i = 0;
	int rc;

	if (!count)
		return 0;
	if (be->raw_mode || be->test_drop_tx)
		return udp_send_ref_fallback(ctx, refs, count);
	if (!__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&be->accepting, __ATOMIC_ACQUIRE)) {
		rc = -ESHUTDOWN;
		goto release;
	}
	while (i < count) {
		run = udp_ref_run(ctx, be, refs, i, count);
		tx = run > 1 ? udp_ref_gso_tx(ctx, be, refs, i, run) :
			udp_ref_one_tx(ctx, be, &refs[i]);
		if (!tx) {
			rc = -ENOMEM;
			goto release;
		}
		tx->queue_next = NULL;
		if (tail)
			tail->queue_next = tx;
		else
			head = tx;
		tail = tx;
		queued++;
		i += run;
	}
	rc = udp_publish_ref_batch(be, head, queued, count);
	if (!rc)
		return 0;

release:
	while (head) {
		tx = head;
		head = head->queue_next;
		free_tx(be, tx);
	}
	for (; i < count; i++)
		refs[i].release(refs[i].opaque);
	return rc;
}
