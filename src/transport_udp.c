/* SPDX-License-Identifier: Apache-2.0 */
#include <limits.h>

#include "transport_udp_internal.h"

UIB_UDP_INTERNAL int test_drop_packet(struct uib_udp_backend *be)
{
	unsigned int remaining;

	remaining = __atomic_load_n(&be->test_drop_tx, __ATOMIC_RELAXED);
	while (remaining) {
		if (__atomic_compare_exchange_n(&be->test_drop_tx, &remaining,
					remaining - 1, 0, __ATOMIC_RELAXED,
					__ATOMIC_RELAXED))
			return 1;
	}
	return 0;
}

static void dispatch_packet(struct uib_context *ctx, const void *buffer,
			    size_t length,
			    const struct sockaddr_in *source, uint8_t tos,
			    uint8_t integrity_validated)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_rx_worker *worker;
	struct uib_udp_rx_work *work;
	uint64_t position;
	uint32_t qpn;
	unsigned int index;
	int was_empty;
	int rc;

	if (!ctx->pops || !ctx->pops->packet_qpn ||
	    length < ctx->pops->min_packet ||
	    ctx->pops->packet_qpn(buffer, length, &qpn) < 0)
		return;
	index = qpn % be->rx_worker_count;
	worker = &be->rx_workers[index];
	work = rx_work_alloc(worker, length);
	if (!work)
		goto drop;
	work->source = *source;
	work->rx_time_ns = uib_now_ns();
	work->length = length;
	work->tos = tos;
	work->integrity_validated = integrity_validated;
	memcpy(work->data, buffer, length);
	rc = uib_ptr_ring_reserve(&worker->queue, 1, &position, &was_empty);
	if (rc < 0) {
		rx_work_release(worker, work);
		goto drop;
	}
	uib_ptr_ring_publish(&worker->queue, position, work);
	if (was_empty) {
		pthread_mutex_lock(&worker->lock);
		pthread_cond_signal(&worker->cond);
		pthread_mutex_unlock(&worker->lock);
	}
	return;

drop:
	__atomic_fetch_add(&be->stats.rx_dispatch_drops, 1,
			   __ATOMIC_RELAXED);
}

static int raw_decode(struct uib_context *ctx, void *buffer, size_t frame_len,
		      struct sockaddr_in *source, void **rdma_out,
		      size_t *rdma_len_out, uint8_t *tos,
		      uint8_t *integrity_validated)
{
	struct uib_wire_meta meta;
	uint8_t *frame = buffer;
	struct udphdr *udp;
	struct iphdr *ip;
	uint8_t *rdma;
	size_t rdma_len;
	size_t available_len;
	size_t udp_len;
	size_t ip_len;
	uint32_t src_ip;
	uint16_t src_port;

	if (frame_len < sizeof(*ip) + sizeof(*udp))
		return -1;
	ip = (struct iphdr *)frame;
	if (ip->version != 4 || ip->protocol != IPPROTO_UDP)
		return -1;
	ip_len = (size_t)ip->ihl * 4;
	if (ip_len < sizeof(*ip) || ip_len + sizeof(*udp) > frame_len)
		return -1;
	udp = (struct udphdr *)(frame + ip_len);
	if (udp->dest != ctx->local_addr.sin_port)
		return -1;
	if (ctx->local_addr.sin_addr.s_addr != htonl(INADDR_ANY) &&
	    ip->daddr != ctx->local_addr.sin_addr.s_addr)
		return -1;
	udp_len = ntohs(udp->len);
	available_len = frame_len - ip_len;
	if (udp_len < sizeof(*udp) + ctx->pops->min_packet ||
	    available_len < sizeof(*udp) + ctx->pops->min_packet)
		return -1;
	rdma = (uint8_t *)(udp + 1);
	rdma_len = min_t(size_t, udp_len, available_len) - sizeof(*udp);
	meta.src_ip = ip->saddr;
	meta.dst_ip = ip->daddr;
	meta.src_port = udp->source;
	meta.dst_port = udp->dest;
	meta.ip_id = ntohs(ip->id);
	*tos = ip->tos;
	*integrity_validated = 0;
	if (ctx->pops->wire_decode && udp_len <= available_len) {
		if (ctx->pops->wire_decode(&meta, rdma, rdma_len) < 0)
			return -1;
		*integrity_validated = 1;
	}

	src_ip = ip->saddr;
	src_port = udp->source;
	memmove(frame, rdma, rdma_len);
	memset(source, 0, sizeof(*source));
	source->sin_family = AF_INET;
	source->sin_addr.s_addr = src_ip;
	source->sin_port = src_port;
	*rdma_out = frame;
	*rdma_len_out = rdma_len;
	return 0;
}

static void deliver_packet(struct uib_context *ctx, void *buffer, size_t length,
			   const struct sockaddr_in *source, uint8_t tos)
{
	struct uib_udp_backend *be = ctx->backend;
	struct sockaddr_in decoded_source = *source;
	void *rdma = buffer;
	size_t rdma_len = length;
	uint8_t integrity_validated = 0;

	if (be->raw_mode &&
	    raw_decode(ctx, buffer, length, &decoded_source, &rdma,
		       &rdma_len, &tos, &integrity_validated) < 0)
		return;
	dispatch_packet(ctx, rdma, rdma_len, &decoded_source, tos,
			integrity_validated);
	__atomic_fetch_add(&be->stats.rx_packets, 1, __ATOMIC_RELAXED);
}

UIB_UDP_INTERNAL void deliver_udp_payload(struct uib_context *ctx, void *buffer,
				size_t length,
				const struct sockaddr_in *source,
				uint16_t gro_size, uint8_t tos)
{
	struct uib_udp_backend *be = ctx->backend;
	uint8_t *payload = buffer;
	size_t segment;
	size_t offset;

	if (!gro_size || gro_size >= length) {
		deliver_packet(ctx, buffer, length, source, tos);
		return;
	}
	__atomic_fetch_add(&be->stats.rx_gro_batches, 1, __ATOMIC_RELAXED);
	for (offset = 0; offset < length; offset += segment) {
		segment = length - offset;
		if (segment > gro_size)
			segment = gro_size;
		deliver_packet(ctx, payload + offset, segment, source, tos);
	}
}

static int udp_probe(void)
{
	return 1;
}

static void cleanup_rx_workers(struct uib_udp_backend *be)
{
	struct uib_udp_rx_worker *worker;
	struct uib_udp_rx_work *work;
	unsigned int i;

	if (!be->rx_workers)
		return;
	__atomic_store_n(&be->worker_run, 0, __ATOMIC_RELEASE);
	for (i = 0; i < be->rx_worker_count; i++) {
		worker = &be->rx_workers[i];
		if (!worker->initialized)
			continue;
		pthread_mutex_lock(&worker->lock);
		pthread_cond_signal(&worker->cond);
		pthread_mutex_unlock(&worker->lock);
	}
	for (i = 0; i < be->rx_worker_count; i++) {
		worker = &be->rx_workers[i];
		if (worker->started)
			pthread_join(worker->thread, NULL);
		if (!worker->initialized)
			continue;
		if (worker->queue_initialized) {
			while ((work = uib_ptr_ring_pop(&worker->queue)) != NULL)
				free(work);
			uib_ptr_ring_destroy(&worker->queue);
		}
		cleanup_rx_work_pools(worker);
		pthread_cond_destroy(&worker->cond);
		pthread_mutex_destroy(&worker->lock);
	}
	free(be->rx_workers);
	be->rx_workers = NULL;
	be->rx_worker_count = 0;
}

static int init_rx_workers(struct uib_udp_backend *be)
{
	struct uib_udp_rx_worker *worker;
	pthread_attr_t attr;
	cpu_set_t allowed;
	unsigned int count = 0;
	unsigned int cpu_start;
	unsigned int i;
	int affinity;
	int attr_ready;
	int cpu;
	int rc;

	rc = uib_worker_configuration(&count, &allowed, &affinity,
				      UIB_ENV_UDP_WORKERS,
				      UIB_ENV_UDP_WORKER_AFFINITY,
				      UIB_UDP_RX_WORKERS_AUTO_MAX);
	if (rc < 0)
		return rc;
	cpu_start = affinity ? uib_worker_cpu_reserve(count) : 0;
	be->rx_workers = calloc(count, sizeof(*be->rx_workers));
	if (!be->rx_workers)
		return -ENOMEM;
	be->rx_worker_count = count;
	for (i = 0; i < count; i++) {
		worker = &be->rx_workers[i];
		worker->backend = be;
		pthread_mutex_init(&worker->lock, NULL);
		pthread_cond_init(&worker->cond, NULL);
		worker->initialized = 1;
		rc = uib_ptr_ring_init(&worker->queue,
				       UIB_UDP_RX_WORK_QUEUE);
		if (rc < 0)
			return rc;
		worker->queue_initialized = 1;
		rc = init_rx_work_pools(worker);
		if (rc < 0)
			return rc;
		if (affinity) {
			cpu = uib_worker_cpu(&allowed, cpu_start + i);
			if (cpu < 0)
				return -EINVAL;
			worker->cpu = (unsigned int)cpu;
			worker->cpu_set = 1;
		}
	}
	attr_ready = !pthread_attr_init(&attr);
	if (attr_ready)
		pthread_attr_setstacksize(&attr, UIB_UDP_RX_WORKER_STACK);
	for (i = 0; i < count; i++) {
		worker = &be->rx_workers[i];
		rc = pthread_create(&worker->thread, attr_ready ? &attr : NULL,
				    rx_worker_loop, worker);
		if (rc) {
			rc = -rc;
			goto out;
		}
		worker->started = 1;
		pthread_mutex_lock(&worker->lock);
		while (!worker->init_done)
			pthread_cond_wait(&worker->cond, &worker->lock);
		rc = worker->init_rc;
		pthread_mutex_unlock(&worker->lock);
		if (rc < 0)
			goto out;
	}

out:
	if (attr_ready)
		pthread_attr_destroy(&attr);
	return rc;
}

static int parse_batch_size(const char *name, unsigned int *value)
{
	const char *setting = getenv(name);
	uint64_t parsed;

	if (!setting || !*setting)
		return 0;
	if (uib_parse_u64(setting, 10, &parsed) < 0 || !parsed ||
	    parsed > UIB_URING_BATCH_MAX)
		return -EINVAL;
	*value = (unsigned int)parsed;
	return 0;
}

static int parse_sqpoll(struct uib_udp_backend *be)
{
	const char *setting = getenv(UIB_ENV_URING_SQPOLL);
	const char *cpu = getenv(UIB_ENV_URING_SQPOLL_CPU);
	const char *idle = getenv(UIB_ENV_URING_SQPOLL_IDLE);
	uint64_t value;

	be->sqpoll_idle = UIB_URING_SQPOLL_IDLE_DEFAULT;
	if (setting && *setting && strcmp(setting, "off") &&
	    strcmp(setting, "0")) {
		if (strcmp(setting, "on") && strcmp(setting, "1"))
			return -EINVAL;
		be->sqpoll_requested = 1;
	}
	if (idle && *idle) {
		if (uib_parse_u64(idle, 10, &value) < 0 || !value ||
		    value > UINT32_MAX)
			return -EINVAL;
		be->sqpoll_idle = (unsigned int)value;
	}
	if (cpu && *cpu) {
		if (uib_parse_u64(cpu, 10, &value) < 0 || value > UINT32_MAX)
			return -EINVAL;
		be->sqpoll_cpu = (unsigned int)value;
		be->sqpoll_cpu_set = 1;
	}
	return 0;
}

static int setup_socket(struct uib_context *ctx, struct uib_udp_backend *be)
{
	const char *udp_mode = getenv(UIB_ENV_UDP_MODE);
	const char *gso_mode = getenv(UIB_ENV_UDP_GSO);
	int socket_buffer = UIB_SOCKET_BUFFER_SIZE;
	int tos = IPTOS_ECN_ECT0;
	int gso_size = 0;
	int one = 1;
	int allow_raw;

	ctx->local_addr.sin_family = AF_INET;
	ctx->local_addr.sin_port = htons(ctx->bind_port ? ctx->bind_port :
					 ctx->pops->default_port);
	if (ctx->bind_ip[0]) {
		if (inet_pton(AF_INET, ctx->bind_ip,
			      &ctx->local_addr.sin_addr) != 1)
			return -EINVAL;
	} else {
		ctx->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	}

	/* Raw IPv4 exposes the outer headers required by RoCEv2 ICRC. */
	allow_raw = udp_mode && !strcmp(udp_mode, "raw");
	if (ctx->zerocopy.mode == UIB_ZEROCOPY_ON ||
	    ctx->zerocopy.mode == UIB_ZEROCOPY_REQUIRED)
		allow_raw = 0;
	be->fd = allow_raw ? socket(AF_INET, SOCK_RAW, IPPROTO_UDP) : -1;
	if (be->fd >= 0) {
		int one = 1;

		if (setsockopt(be->fd, IPPROTO_IP, IP_HDRINCL, &one,
			       sizeof(one)) < 0)
			return -errno;
		be->raw_mode = 1;
		return 0;
	}
	if (udp_mode && !strcmp(udp_mode, "raw"))
		return -errno;
	be->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (be->fd < 0)
		return -errno;
	if (setsockopt(be->fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0 ||
	    setsockopt(be->fd, IPPROTO_IP, IP_RECVTOS, &one, sizeof(one)) < 0)
		return -errno;
	if (gso_mode && strcmp(gso_mode, "auto") &&
	    strcmp(gso_mode, "off") && strcmp(gso_mode, "on"))
		return -EINVAL;
	be->gso_required = gso_mode && !strcmp(gso_mode, "on");
	if (!gso_mode || strcmp(gso_mode, "off")) {
		if (!setsockopt(be->fd, IPPROTO_UDP, UDP_SEGMENT, &gso_size,
				sizeof(gso_size)))
			be->gso_enabled = 1;
		else if (be->gso_required)
			return -errno;
	}
	if (be->gso_enabled &&
	    !setsockopt(be->fd, IPPROTO_UDP, UDP_GRO, &one, sizeof(one)))
		be->gro_enabled = 1;
	{
		int one = 1;

		setsockopt(be->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (be->flow_mode == UIB_UDP_FLOW_FIXED_PORT &&
		    setsockopt(be->fd, SOL_SOCKET, SO_REUSEPORT, &one,
			       sizeof(one)) < 0)
			return -errno;
	}
	setsockopt(be->fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer,
		   sizeof(socket_buffer));
	setsockopt(be->fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer,
		   sizeof(socket_buffer));
	if (bind(be->fd, (struct sockaddr *)&ctx->local_addr,
		 sizeof(ctx->local_addr)) < 0)
		return -errno;
	{
		socklen_t length = sizeof(ctx->local_addr);

		if (getsockname(be->fd, (struct sockaddr *)&ctx->local_addr,
				&length) < 0)
			return -errno;
	}
	be->local_addr = ctx->local_addr;
	be->zerocopy_enabled = uib_zerocopy_enable_socket(ctx, be->fd);
	if (be->zerocopy_enabled < 0)
		return be->zerocopy_enabled;
	return 0;
}

static void cleanup_backend(struct uib_udp_backend *be)
{
	struct uib_udp_tx *tx;

	cleanup_udp_flows(be);
	cleanup_rx_workers(be);
	if (be->fd >= 0)
		close(be->fd);
	if (be->wake_fd >= 0)
		close(be->wake_fd);
	free_all_tx(be);
	while ((tx = uib_ptr_ring_pop(&be->tx_ref_pool)) != NULL)
		free(tx);
	while ((tx = uib_ptr_ring_pop(&be->tx_copy_pool)) != NULL)
		free(tx);
	while ((tx = uib_ptr_ring_pop(&be->tx_gso_pool)) != NULL)
		free(tx);
	uib_ptr_ring_destroy(&be->tx_gso_pool);
	uib_ptr_ring_destroy(&be->tx_copy_pool);
	uib_ptr_ring_destroy(&be->tx_ref_pool);
	uib_ptr_ring_destroy(&be->tx_queue);
	pthread_cond_destroy(&be->init_cond);
	pthread_mutex_destroy(&be->init_lock);
	free(be);
}

static int udp_test_drop_init(struct uib_udp_backend *be)
{
	const char *setting;
	uint64_t count;

	setting = getenv(UIB_ENV_TEST_DROP_TX);
	if (!setting)
		return 0;
	if (uib_parse_u64(setting, 10, &count) < 0 || count > UINT_MAX)
		return -EINVAL;
	be->test_drop_tx = (unsigned int)count;
	return 0;
}

static int udp_init_common(struct uib_context *ctx, int asynchronous)
{
	struct uib_udp_backend *be;
	long page_size;
	int thread_rc;
	int rc;

	be = calloc(1, sizeof(*be));
	if (!be)
		return -ENOMEM;
	be->fd = -1;
	be->wake_fd = -1;
	be->ctx = ctx;
	page_size = sysconf(_SC_PAGESIZE);
	be->page_size = page_size > 0 ? (size_t)page_size : 4096;
	pthread_mutex_init(&be->init_lock, NULL);
	pthread_cond_init(&be->init_cond, NULL);
	rc = init_tx_rings(be);
	if (rc < 0)
		goto error;
	warm_tx_pools(be);
	be->rx_batch = UIB_URING_BATCH_DEFAULT;
	if (asynchronous)
		be->tx_batch = UIB_URING_TX_BATCH_DEFAULT;
	rc = parse_batch_size(UIB_ENV_URING_RX_BATCH, &be->rx_batch);
	if (rc < 0)
		goto error;
	if (asynchronous) {
		be->tx_batch_set = getenv(UIB_ENV_URING_TX_BATCH) != NULL;
		rc = parse_batch_size(UIB_ENV_URING_TX_BATCH, &be->tx_batch);
		if (rc < 0)
			goto error;
	}
	rc = parse_sqpoll(be);
	if (rc < 0)
		goto error;
	be->multishot_op.type = UIB_UDP_OP_RX_MULTISHOT;
	be->timer_op.type = UIB_UDP_OP_TIMER;
	be->wake_op.type = UIB_UDP_OP_WAKE;
	rc = udp_test_drop_init(be);
	if (rc < 0)
		goto error;
	ctx->backend = be;
	rc = udp_flow_count(&be->requested_flow_count);
	if (rc < 0)
		goto error;
	if (be->requested_flow_count > 1) {
		if (ctx->pops && ctx->pops->multipath_ports)
			be->flow_mode = UIB_UDP_FLOW_MULTIPATH;
		else if (ctx->pops && ctx->pops->source_port_entropy)
			be->flow_mode = UIB_UDP_FLOW_FIXED_PORT;
		else {
			rc = -EPROTONOSUPPORT;
			goto error;
		}
	}
	rc = setup_socket(ctx, be);
	if (rc < 0)
		goto error;
	rc = init_udp_flows(ctx, be);
	if (rc < 0)
		goto error;
	be->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (be->wake_fd < 0) {
		rc = -errno;
		goto error;
	}
	__atomic_store_n(&be->worker_run, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&be->accepting, 1, __ATOMIC_RELEASE);
	rc = init_rx_workers(be);
	if (rc < 0)
		goto error;
	thread_rc = pthread_create(&be->worker, NULL, worker_loop, ctx);
	if (thread_rc) {
		rc = -thread_rc;
		goto error;
	}
	pthread_mutex_lock(&be->init_lock);
	while (!be->init_done)
		pthread_cond_wait(&be->init_cond, &be->init_lock);
	rc = be->init_rc;
	pthread_mutex_unlock(&be->init_lock);
	if (rc < 0) {
		__atomic_store_n(&be->worker_run, 0, __ATOMIC_RELEASE);
		pthread_join(be->worker, NULL);
		goto error;
	}
	return 0;

error:
	cleanup_backend(be);
	ctx->backend = NULL;
	return rc;
}

static int udp_init(struct uib_context *ctx)
{
	return udp_init_common(ctx, 1);
}

static int udp_sync_init(struct uib_context *ctx)
{
	return udp_init_common(ctx, 0);
}

static void udp_fini(struct uib_context *ctx)
{
	struct uib_udp_backend *be = ctx->backend;
	uint64_t wake = 1;

	if (!be)
		return;
	__atomic_store_n(&be->accepting, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&be->stop_requested, 1, __ATOMIC_RELEASE);
	if (__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE)) {
		if (write(be->wake_fd, &wake, sizeof(wake)) < 0 && errno != EAGAIN)
			shutdown(be->fd, SHUT_RDWR);
	}
	pthread_join(be->worker, NULL);
	cleanup_backend(be);
	ctx->backend = NULL;
}

static int resolve_source_address(struct uib_context *ctx,
				  const struct sockaddr_in *destination,
				  uint32_t *source_ip)
{
	struct sockaddr_in local;
	socklen_t local_len = sizeof(local);
	int route_fd;
	int saved;

	*source_ip = ctx->local_addr.sin_addr.s_addr;
	if (*source_ip != htonl(INADDR_ANY))
		return 0;
	route_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (route_fd < 0)
		return -errno;
	if (connect(route_fd, (const struct sockaddr *)destination,
		    sizeof(*destination)) < 0 ||
	    getsockname(route_fd, (struct sockaddr *)&local, &local_len) < 0) {
		saved = errno;
		close(route_fd);
		return -saved;
	}
	close(route_fd);
	*source_ip = local.sin_addr.s_addr;
	return 0;
}

static int prepare_raw_packet(struct uib_context *ctx,
			      const struct sockaddr_in *destination,
			      size_t length, uint16_t entropy,
			      struct uib_udp_tx *tx)
{
	struct uib_wire_meta meta;
	struct udphdr *udp;
	struct iphdr *ip;
	size_t packet_len;
	uint32_t source_ip;
	int rc;

	if (length < ctx->pops->min_packet ||
	    length > UINT16_MAX - sizeof(*ip) - sizeof(*udp))
		return -EMSGSIZE;
	rc = resolve_source_address(ctx, destination, &source_ip);
	if (rc < 0)
		return rc;
	packet_len = sizeof(*ip) + sizeof(*udp) + length;
	tx->length = packet_len;
	ip = tx->data;
	udp = (struct udphdr *)(ip + 1);
	memset(ip, 0, sizeof(*ip));
	ip->version = 4;
	ip->ihl = 5;
	ip->tos = ctx->pops->ipv4_tos ?
		ctx->pops->ipv4_tos(ctx, udp + 1, length) : IPTOS_ECN_ECT0;
	ip->tot_len = htons((uint16_t)packet_len);
	ip->frag_off = htons(0x4000);
	ip->ttl = 64;
	ip->protocol = IPPROTO_UDP;
	ip->saddr = source_ip;
	ip->daddr = destination->sin_addr.s_addr;
	ip->check = htons(uib_inet_csum(ip, sizeof(*ip)));
	memset(udp, 0, sizeof(*udp));
	udp->source = entropy ? htons(entropy) : ctx->local_addr.sin_port;
	udp->dest = destination->sin_port ? destination->sin_port :
		htons(ctx->pops->default_port);
	udp->len = htons((uint16_t)(sizeof(*udp) + length));
	meta.src_ip = ip->saddr;
	meta.dst_ip = ip->daddr;
	meta.src_port = udp->source;
	meta.dst_port = udp->dest;
	meta.ip_id = 0;
	if (ctx->pops->wire_encode &&
	    ctx->pops->wire_encode(&meta, udp + 1, length) < 0)
		return -EBADMSG;
	return 0;
}

UIB_UDP_INTERNAL int build_raw_packet(struct uib_context *ctx,
			    const struct sockaddr_in *destination,
			    const void *buffer, size_t length,
			    uint16_t entropy, struct uib_udp_tx *tx)
{
	struct udphdr *udp;
	struct iphdr *ip;

	ip = tx->data;
	udp = (struct udphdr *)(ip + 1);
	memcpy(udp + 1, buffer, length);
	return prepare_raw_packet(ctx, destination, length, entropy, tx);
}

UIB_UDP_INTERNAL int build_raw_iov_packet(
	struct uib_context *ctx, const struct uib_transport_iov_ref *ref,
	struct uib_udp_tx *tx)
{
	struct udphdr *udp;
	struct iphdr *ip;
	uint8_t *payload;
	size_t offset;
	size_t i;

	ip = tx->data;
	udp = (struct udphdr *)(ip + 1);
	payload = (uint8_t *)(udp + 1);
	offset = 0;
	for (i = 0; i < ref->iov_count; i++) {
		memcpy(payload + offset, ref->iov[i].iov_base,
		       ref->iov[i].iov_len);
		offset += ref->iov[i].iov_len;
	}
	return prepare_raw_packet(ctx, ref->dst, ref->len, ref->entropy, tx);
}

static int enqueue_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx)
{
	uint64_t position;
	uint64_t wake = 1;
	int wake_worker;
	int rc;

	if (!__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&be->accepting, __ATOMIC_ACQUIRE))
		return -ESHUTDOWN;
	rc = uib_ptr_ring_reserve(&be->tx_queue, 1, &position, NULL);
	if (rc < 0) {
		if (rc == -ENOSPC)
			return -EAGAIN;
		return rc;
	}
	tx->queue_next = NULL;
	uib_ptr_ring_publish(&be->tx_queue, position, tx);
	wake_worker = __atomic_load_n(&be->worker_waiting, __ATOMIC_SEQ_CST) &&
		!pthread_equal(pthread_self(), be->worker);
	__atomic_fetch_add(&be->stats.tx_queued, 1, __ATOMIC_RELAXED);
	if (wake_worker && write(be->wake_fd, &wake, sizeof(wake)) < 0 &&
	    errno != EAGAIN)
		shutdown(be->fd, SHUT_RDWR);
	return 0;
}

static int udp_send_entropy(struct uib_context *ctx,
			    const struct sockaddr_in *destination,
			    const void *buffer, size_t length,
			    uint16_t entropy)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_tx *tx;
	size_t storage_len;
	int rc;

	if (test_drop_packet(be))
		return 0;
	storage_len = length;
	if (be->raw_mode)
		storage_len += sizeof(struct iphdr) + sizeof(struct udphdr);
	tx = alloc_copy_tx(be, storage_len);
	if (!tx)
		return -ENOMEM;
	tx->destination = *destination;
	tx->data = tx->storage;
	if (be->raw_mode) {
		rc = build_raw_packet(ctx, destination, buffer, length, entropy, tx);
		if (rc < 0)
			goto error;
	} else {
		memcpy(tx->data, buffer, length);
		tx->length = length;
		if (be->zerocopy_enabled &&
		    ctx->zerocopy.mode != UIB_ZEROCOPY_OFF &&
		    length >= ctx->zerocopy.threshold)
			tx->zerocopy = 1;
	}
	tx->iov[0].iov_base = tx->data;
	tx->iov[0].iov_len = tx->length;
	tx->message.msg_name = &tx->destination;
	tx->message.msg_namelen = sizeof(tx->destination);
	tx->message.msg_iov = tx->iov;
	tx->message.msg_iovlen = 1;
	rc = enqueue_tx(be, tx);
	if (rc < 0)
		goto error;
	return 0;

error:
	free_tx(be, tx);
	return rc;
}

static int udp_send(struct uib_context *ctx,
		    const struct sockaddr_in *destination,
		    const void *buffer, size_t length)
{
	return udp_send_entropy(ctx, destination, buffer, length, 0);
}

static int udp_send_ref(struct uib_context *ctx,
			const struct sockaddr_in *destination,
			const void *buffer, size_t length,
			uib_transport_release_fn release, void *opaque)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_tx *tx;
	int rc;

	if (be->raw_mode) {
		rc = udp_send(ctx, destination, buffer, length);
		release(opaque);
		return rc;
	}
	if (test_drop_packet(be)) {
		release(opaque);
		return 0;
	}
	tx = alloc_ref_tx(be);
	if (!tx) {
		release(opaque);
		return -ENOMEM;
	}
	tx->destination = *destination;
	tx->data = (void *)buffer;
	tx->release = release;
	tx->release_opaque = opaque;
	tx->length = length;
	if (be->zerocopy_enabled && ctx->zerocopy.mode != UIB_ZEROCOPY_OFF &&
	    length >= ctx->zerocopy.threshold)
		tx->zerocopy = 1;
	tx->iov[0].iov_base = tx->data;
	tx->iov[0].iov_len = tx->length;
	tx->message.msg_name = &tx->destination;
	tx->message.msg_namelen = sizeof(tx->destination);
	tx->message.msg_iov = tx->iov;
	tx->message.msg_iovlen = 1;
	rc = enqueue_tx(be, tx);
	if (rc < 0)
		free_tx(be, tx);
	return rc;
}

UIB_UDP_INTERNAL int udp_send_ref_fallback(struct uib_context *ctx,
				 const struct uib_transport_ref *refs,
				 size_t count)
{
	size_t i;
	int rc;

	for (i = 0; i < count; i++) {
		rc = udp_send_entropy(ctx, refs[i].dst, refs[i].buf,
				      refs[i].len, refs[i].entropy);
		refs[i].release(refs[i].opaque);
		if (rc < 0) {
			for (i++; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
	}
	return 0;
}

UIB_UDP_INTERNAL size_t udp_ref_run(struct uib_context *ctx,
			  struct uib_udp_backend *be,
			  const struct uib_transport_ref *refs,
			  size_t start, size_t count)
{
	size_t run;

	run = 1;
	if (__atomic_load_n(&be->gso_enabled, __ATOMIC_ACQUIRE) &&
	    refs[start].len && refs[start].len <= UINT16_MAX) {
		while (start + run < count && run < UIB_UDP_GSO_MAX_SEGMENTS &&
		       (run + 1) * refs[start].len <= UIB_UDP_GSO_CAPACITY &&
		       refs[start + run].len == refs[start].len &&
		       !memcmp(refs[start + run].dst, refs[start].dst,
			       sizeof(*refs[start].dst)))
			run++;
	}
	return limit_zerocopy_run(ctx, refs, start, run);
}

UIB_UDP_INTERNAL struct uib_udp_tx *udp_ref_gso_tx(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_ref *refs, size_t start, size_t run)
{
	struct uib_udp_tx *tx;
	struct cmsghdr *control;
	size_t i;

	tx = alloc_gso_tx(be);
	if (!tx)
		return NULL;
	tx->destination = *refs[start].dst;
	tx->length = run * refs[start].len;
	tx->packets = (unsigned int)run;
	tx->gso_size = (uint16_t)refs[start].len;
	for (i = 0; i < run; i++) {
		tx->iov_list[i].iov_base = (void *)refs[start + i].buf;
		tx->iov_list[i].iov_len = refs[start + i].len;
		tx->release_list[i].release = refs[start + i].release;
		tx->release_list[i].opaque = refs[start + i].opaque;
		tx->release_list[i].iov_count = 1;
		tx->release_count++;
	}
	tx->message.msg_name = &tx->destination;
	tx->message.msg_namelen = sizeof(tx->destination);
	tx->message.msg_iov = tx->iov_list;
	tx->message.msg_iovlen = run;
	tx->zerocopy = gso_zerocopy_eligible(ctx, tx);
	control = (struct cmsghdr *)tx->control.bytes;
	tx->message.msg_control = control;
	tx->message.msg_controllen = CMSG_SPACE(sizeof(tx->gso_size));
	control->cmsg_level = IPPROTO_UDP;
	control->cmsg_type = UDP_SEGMENT;
	control->cmsg_len = CMSG_LEN(sizeof(tx->gso_size));
	memcpy(CMSG_DATA(control), &tx->gso_size, sizeof(tx->gso_size));
	return tx;
}

UIB_UDP_INTERNAL struct uib_udp_tx *udp_ref_one_tx(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_ref *ref)
{
	struct uib_udp_tx *tx;

	tx = alloc_ref_tx(be);
	if (!tx)
		return NULL;
	tx->destination = *ref->dst;
	tx->data = (void *)ref->buf;
	tx->release = ref->release;
	tx->release_opaque = ref->opaque;
	tx->length = ref->len;
	if (be->zerocopy_enabled && ctx->zerocopy.mode != UIB_ZEROCOPY_OFF &&
	    ref->len >= ctx->zerocopy.threshold)
		tx->zerocopy = 1;
	tx->iov[0].iov_base = tx->data;
	tx->iov[0].iov_len = tx->length;
	tx->message.msg_name = &tx->destination;
	tx->message.msg_namelen = sizeof(tx->destination);
	tx->message.msg_iov = tx->iov;
	return tx;
}

UIB_UDP_INTERNAL int udp_publish_ref_batch(struct uib_udp_backend *be,
				 struct uib_udp_tx *head, size_t queued,
				 size_t packet_count)
{
	struct uib_udp_tx *tx;
	uint64_t position;
	uint64_t wake;
	int wake_worker;
	int rc;

	if (queued > UINT32_MAX) {
		return -EAGAIN;
	}
	rc = uib_ptr_ring_reserve(&be->tx_queue, (uint32_t)queued,
				  &position, NULL);
	if (rc < 0) {
		if (rc == -ENOSPC || rc == -EINVAL)
			rc = -EAGAIN;
		return rc;
	}
	while (head) {
		tx = head;
		head = head->queue_next;
		tx->queue_next = NULL;
		uib_ptr_ring_publish(&be->tx_queue, position, tx);
		position++;
	}
	wake_worker = __atomic_load_n(&be->worker_waiting, __ATOMIC_SEQ_CST);
	__atomic_fetch_add(&be->stats.tx_queued, packet_count, __ATOMIC_RELAXED);
	if (wake_worker && !pthread_equal(pthread_self(), be->worker)) {
		wake = 1;
		if (write(be->wake_fd, &wake, sizeof(wake)) < 0 &&
		    errno != EAGAIN)
			shutdown(be->fd, SHUT_RDWR);
	}
	return 0;
}

UIB_UDP_INTERNAL int udp_iov_ref_validate(const struct uib_transport_iov_ref *ref)
{
	size_t length;
	size_t i;

	if (!ref->iov || !ref->iov_count || !ref->release)
		return -EINVAL;
	length = 0;
	for (i = 0; i < ref->iov_count; i++) {
		if (ref->iov[i].iov_len > ref->len - length)
			return -EINVAL;
		length += ref->iov[i].iov_len;
	}
	return length == ref->len ? 0 : -EINVAL;
}

static int udp_send_iov_ref_copy(struct uib_context *ctx,
				 const struct uib_transport_iov_ref *ref)
{
	uint8_t *buffer;
	size_t offset;
	size_t i;
	int rc;

	rc = udp_iov_ref_validate(ref);
	if (rc < 0)
		return rc;
	buffer = malloc(ref->len ? ref->len : 1);
	if (!buffer)
		return -ENOMEM;
	offset = 0;
	for (i = 0; i < ref->iov_count; i++) {
		memcpy(buffer + offset, ref->iov[i].iov_base,
		       ref->iov[i].iov_len);
		offset += ref->iov[i].iov_len;
	}
	rc = udp_send_entropy(ctx, ref->dst, buffer, ref->len,
			      ref->entropy);
	free(buffer);
	return rc;
}

UIB_UDP_INTERNAL int udp_send_iov_ref_fallback(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count)
{
	size_t i;
	int rc;

	for (i = 0; i < count; i++) {
		rc = udp_send_iov_ref_copy(ctx, &refs[i]);
		refs[i].release(refs[i].opaque);
		if (rc < 0) {
			for (i++; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
	}
	return 0;
}

static size_t udp_iov_ref_page_count(
	const struct uib_udp_backend *be,
	const struct uib_transport_iov_ref *ref)
{
	size_t pages;
	size_t i;

	pages = 0;
	for (i = 0; i < ref->iov_count; i++)
		pages += buffer_page_count(be, ref->iov[i].iov_base,
					   ref->iov[i].iov_len);
	return pages;
}

UIB_UDP_INTERNAL size_t udp_iov_ref_run(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_iov_ref *refs, size_t start, size_t count)
{
	size_t iov_count;
	size_t pages;
	size_t run;

	run = 1;
	iov_count = refs[start].iov_count;
	if (__atomic_load_n(&be->gso_enabled, __ATOMIC_ACQUIRE) &&
	    refs[start].len && refs[start].len <= UINT16_MAX) {
		while (start + run < count &&
		       run < UIB_UDP_GSO_MAX_SEGMENTS &&
		       (run + 1) * refs[start].len <= UIB_UDP_GSO_CAPACITY &&
		       iov_count + refs[start + run].iov_count <=
		       UIB_UDP_GSO_MAX_IOVS &&
		       refs[start + run].len == refs[start].len &&
		       refs[start + run].flow == refs[start].flow &&
		       !memcmp(refs[start + run].dst, refs[start].dst,
			       sizeof(*refs[start].dst))) {
			iov_count += refs[start + run].iov_count;
			run++;
		}
	}
	if (!be->zerocopy_enabled || ctx->zerocopy.mode == UIB_ZEROCOPY_OFF ||
	    run * refs[start].len < ctx->zerocopy.threshold)
		return run;
	pages = 0;
	for (iov_count = 0; iov_count < run; iov_count++) {
		pages += udp_iov_ref_page_count(be, &refs[start + iov_count]);
		if (pages > UIB_UDP_ZC_MAX_FRAGS)
			return iov_count ? iov_count : 1;
	}
	return run;
}

UIB_UDP_INTERNAL struct uib_udp_tx *udp_iov_ref_gso_tx(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_iov_ref *refs, size_t start, size_t run)
{
	struct uib_udp_tx *tx;
	struct cmsghdr *control;
	size_t iov_offset;
	size_t i;

	tx = alloc_gso_tx(be);
	if (!tx)
		return NULL;
	tx->destination = *refs[start].dst;
	tx->length = run * refs[start].len;
	tx->packets = (unsigned int)run;
	tx->gso_size = (uint16_t)refs[start].len;
	iov_offset = 0;
	for (i = 0; i < run; i++) {
		memcpy(&tx->iov_list[iov_offset], refs[start + i].iov,
		       refs[start + i].iov_count * sizeof(*tx->iov_list));
		tx->release_list[i].release = refs[start + i].release;
		tx->release_list[i].opaque = refs[start + i].opaque;
		tx->release_list[i].iov_count =
			(unsigned int)refs[start + i].iov_count;
		tx->release_count++;
		iov_offset += refs[start + i].iov_count;
	}
	tx->message.msg_name = &tx->destination;
	tx->message.msg_namelen = sizeof(tx->destination);
	tx->message.msg_iov = tx->iov_list;
	tx->message.msg_iovlen = iov_offset;
	tx->zerocopy = gso_zerocopy_eligible(ctx, tx);
	control = (struct cmsghdr *)tx->control.bytes;
	tx->message.msg_control = control;
	tx->message.msg_controllen = CMSG_SPACE(sizeof(tx->gso_size));
	control->cmsg_level = IPPROTO_UDP;
	control->cmsg_type = UDP_SEGMENT;
	control->cmsg_len = CMSG_LEN(sizeof(tx->gso_size));
	memcpy(CMSG_DATA(control), &tx->gso_size, sizeof(tx->gso_size));
	return tx;
}

UIB_UDP_INTERNAL struct uib_udp_tx *udp_iov_ref_one_tx(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_iov_ref *ref)
{
	struct uib_udp_tx *tx;

	tx = alloc_ref_tx(be);
	if (!tx)
		return NULL;
	if (ref->iov_count > tx->iov_capacity) {
		tx->iov_list = malloc(ref->iov_count * sizeof(*tx->iov_list));
		if (!tx->iov_list) {
			free_tx(be, tx);
			return NULL;
		}
		tx->iov_capacity = (unsigned int)ref->iov_count;
		tx->iov_allocated = 1;
	}
	memcpy(tx->iov_list, ref->iov,
	       ref->iov_count * sizeof(*tx->iov_list));
	tx->destination = *ref->dst;
	tx->release = ref->release;
	tx->release_opaque = ref->opaque;
	tx->length = ref->len;
	if (be->zerocopy_enabled && ctx->zerocopy.mode != UIB_ZEROCOPY_OFF &&
	    ref->len >= ctx->zerocopy.threshold)
		tx->zerocopy = 1;
	tx->message.msg_name = &tx->destination;
	tx->message.msg_namelen = sizeof(tx->destination);
	tx->message.msg_iov = tx->iov_list;
	tx->message.msg_iovlen = ref->iov_count;
	return tx;
}

static int udp_get_stats(struct uib_context *ctx,
			 struct uib_transport_stats *stats)
{
	struct uib_udp_backend *be = ctx->backend;

	memset(stats, 0, sizeof(*stats));
	stats->tx_queued = __atomic_load_n(&be->stats.tx_queued,
					   __ATOMIC_RELAXED);
	stats->tx_submitted = __atomic_load_n(&be->stats.tx_submitted,
					      __ATOMIC_RELAXED);
	stats->tx_completed = __atomic_load_n(&be->stats.tx_completed,
					      __ATOMIC_RELAXED);
	stats->tx_errors = __atomic_load_n(&be->stats.tx_errors,
					   __ATOMIC_RELAXED);
	stats->tx_gso_batches = __atomic_load_n(&be->stats.tx_gso_batches,
					       __ATOMIC_RELAXED);
	stats->tx_gso_packets = __atomic_load_n(&be->stats.tx_gso_packets,
					       __ATOMIC_RELAXED);
	stats->rx_packets = __atomic_load_n(&be->stats.rx_packets,
					   __ATOMIC_RELAXED);
	stats->rx_gro_batches = __atomic_load_n(&be->stats.rx_gro_batches,
					       __ATOMIC_RELAXED);
	stats->rx_completion_batches =
		__atomic_load_n(&be->stats.rx_completion_batches,
				__ATOMIC_RELAXED);
	stats->rx_dispatch_drops =
		__atomic_load_n(&be->stats.rx_dispatch_drops,
				__ATOMIC_RELAXED);
	stats->rx_pool_hits = __atomic_load_n(&be->stats.rx_pool_hits,
					     __ATOMIC_RELAXED);
	stats->rx_pool_misses = __atomic_load_n(&be->stats.rx_pool_misses,
					       __ATOMIC_RELAXED);
	stats->rx_largest_batch = __atomic_load_n(&be->stats.rx_largest_batch,
						  __ATOMIC_RELAXED);
	stats->rx_mode = be->stats.rx_mode;
	stats->rx_batch = be->rx_batch;
	stats->rx_workers = be->rx_worker_count;
	stats->rx_workers_pinned = be->rx_worker_count &&
		be->rx_workers[0].cpu_set ? be->rx_worker_count : 0;
	stats->tx_batch = be->tx_batch;
	stats->sqpoll_requested = be->sqpoll_requested;
	stats->sqpoll_active = be->sqpoll_active;
	stats->gso_enabled = __atomic_load_n(&be->gso_enabled,
					    __ATOMIC_ACQUIRE);
	stats->gro_enabled = be->gro_enabled;
	return 0;
}

const struct uib_transport_ops uib_udp_ops = {
	.name = "udp-io-uring",
	.probe = udp_probe,
	.init = udp_init,
	.fini = udp_fini,
	.send = udp_send,
	.send_entropy = udp_send_entropy,
	.send_ref = udp_send_ref,
	.send_ref_batch = udp_send_ref_batch,
	.send_iov_ref_batch = udp_send_iov_ref_batch,
	.get_stats = udp_get_stats,
};

const struct uib_transport_ops uib_udp_sync_ops = {
	.name = "udp-sync",
	.probe = udp_probe,
	.init = udp_sync_init,
	.fini = udp_fini,
	.send = udp_sync_send,
	.send_entropy = udp_sync_send_entropy,
	.send_ref_batch = udp_sync_send_ref_batch,
	.send_iov_ref_batch = udp_sync_send_iov_ref_batch,
	.get_stats = udp_get_stats,
};
