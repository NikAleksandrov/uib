/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <linux/filter.h>
#include <linux/udp.h>

#include <liburing.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/env.h"
#include "uib/protocol.h"
#include "uib/afxdp_frame.h"
#include "ptr_ring.h"
#include "transport_internal.h"
#include "utils.h"

#define UIB_URING_DEPTH              256
#define UIB_URING_BATCH_DEFAULT      64
#define UIB_URING_BATCH_MAX          256
#define UIB_URING_RX_BUFFERS         128
#define UIB_URING_RX_BUFFER_GROUP    7
#define UIB_URING_TX_BATCH_DEFAULT   64
#define UIB_URING_TX_LARGE_BATCH     16
#define UIB_URING_TX_LARGE_PACKET    2048
#define UIB_URING_TX_LIMIT           4096
#define UIB_URING_TX_POOL_LIMIT      4096
#define UIB_URING_COPY_POOL_LIMIT    1024
#define UIB_URING_GSO_POOL_LIMIT     256
#define UIB_URING_TX_POOL_WARM       1024
#define UIB_URING_COPY_POOL_WARM     256
#define UIB_URING_GSO_POOL_WARM      UIB_URING_GSO_POOL_LIMIT
#define UIB_UDP_GSO_MAX_SEGMENTS     64
#define UIB_UDP_REF_INLINE_IOVS      2
#define UIB_UDP_GSO_MAX_IOVS         (UIB_UDP_GSO_MAX_SEGMENTS * 2)
#define UIB_UDP_GSO_CAPACITY         65520
/* Stay below the common Linux MAX_SKB_FRAGS limit of 17. */
#define UIB_UDP_ZC_MAX_FRAGS         16
#define UIB_UDP_RX_CONTROL_SPACE \
	(CMSG_SPACE(sizeof(uint16_t)) + CMSG_SPACE(sizeof(uint8_t)))
#define UIB_URING_SQPOLL_IDLE_DEFAULT 2000
#define UIB_SOCKET_BUFFER_SIZE       (4 * 1024 * 1024)
#define UIB_UDP_RX_WORKERS_AUTO_MAX  2U
#define UIB_UDP_RX_WORK_QUEUE        1024
#define UIB_UDP_RX_POOL_CLASSES      4
#define UIB_UDP_RX_POOL_LIMIT        256
#define UIB_UDP_RX_LARGE_POOL_LIMIT  16
#define UIB_UDP_RX_POOL_WARM_MIN     4
#define UIB_UDP_RX_POOL_WARM_TOTAL   2048
#define UIB_UDP_RX_WORKER_STACK      (512 * 1024)
#define UIB_UDP_COPY_CAPACITY        (8192 + sizeof(struct iphdr) + \
				       sizeof(struct udphdr))
#define UIB_UDP_FLOW_MAX             64U
#define UIB_UDP_FLOW_PORT_STRIDE     128U
#define UIB_UDP_FLOW_SOURCE_BASE     0xc000U
#define UIB_UDP_FLOW_QUEUE_SIZE      256U
#define UIB_UDP_FLOW_TX_BATCH        32U

enum uib_udp_op_type {
	UIB_UDP_OP_RX_MULTISHOT = 0,
	UIB_UDP_OP_RX_ONESHOT,
	UIB_UDP_OP_TX,
	UIB_UDP_OP_TIMER,
	UIB_UDP_OP_WAKE,
};

enum uib_udp_tx_pool {
	UIB_UDP_TX_UNPOOLED = 0,
	UIB_UDP_TX_REF_POOL,
	UIB_UDP_TX_COPY_POOL,
	UIB_UDP_TX_GSO_POOL,
};

enum uib_udp_flow_mode {
	UIB_UDP_FLOW_NONE = 0,
	UIB_UDP_FLOW_MULTIPATH,
	UIB_UDP_FLOW_FIXED_PORT,
};

struct uib_udp_release {
	uib_transport_release_fn release;
	void                    *opaque;
	unsigned int             iov_count;
};

struct uib_udp_op {
	enum uib_udp_op_type type;
};

struct uib_udp_rx_slot {
	struct uib_udp_op  op;
	struct sockaddr_in source;
	struct iovec       iov;
	struct msghdr      message;
	void              *buffer;
	union {
		struct cmsghdr header;
		uint8_t bytes[UIB_UDP_RX_CONTROL_SPACE];
	} control;
	int                armed;
};

struct uib_udp_tx {
	struct uib_udp_op  op;
	struct uib_udp_tx *inflight_next;
	struct uib_udp_tx *inflight_prev;
	struct uib_udp_tx *queue_next;
	struct sockaddr_in destination;
	struct iovec      *iov_list;
	struct iovec       iov[UIB_UDP_REF_INLINE_IOVS];
	struct msghdr      message;
	void              *data;
	uib_transport_release_fn release;
	struct uib_udp_release  *release_list;
	void              *release_opaque;
	size_t             length;
	size_t             storage_capacity;
	unsigned int       release_count;
	unsigned int       packets;
	unsigned int       iov_capacity;
	int                iov_allocated;
	uint16_t           gso_size;
	int                zc_result;
	int                zerocopy;
	enum uib_udp_tx_pool pool;
	union {
		struct cmsghdr header;
		uint8_t bytes[CMSG_SPACE(sizeof(uint16_t))];
	} control;
	uint8_t            storage[];
};

struct uib_udp_backend;

struct uib_udp_flow {
	struct uib_udp_backend *backend;
	struct uib_ptr_ring     tx_queue;
	pthread_cond_t          cond;
	pthread_mutex_t         lock;
	pthread_t               tx_thread;
	pthread_t               rx_thread;
	void                   *rx_buffer;
	unsigned int            index;
	int                     rx_fd;
	int                     tx_fd;
	int                     run;
	int                     queue_initialized;
	int                     sync_initialized;
	int                     tx_started;
	int                     rx_started;
};

struct uib_udp_rx_work {
	struct sockaddr_in source;
	uint64_t           rx_time_ns;
	size_t             length;
	uint8_t            ecn;
	uint8_t            integrity_validated;
	uint8_t            pool_class;
	uint8_t            data[];
};

struct uib_udp_rx_worker {
	struct uib_udp_backend *backend;
	struct uib_ptr_ring     queue;
	struct uib_ptr_ring     pool[UIB_UDP_RX_POOL_CLASSES];
	pthread_cond_t          cond;
	pthread_mutex_t         lock;
	pthread_t               thread;
	unsigned int            cpu;
	unsigned int            pools_initialized;
	int                     cpu_set;
	int                     started;
	int                     initialized;
	int                     queue_initialized;
	int                     init_done;
	int                     init_rc;
};

struct uib_udp_backend {
	struct uib_udp_tx       *tx_inflight;
	struct uib_udp_tx       *tx_retry_head;
	struct io_uring_buf_ring *buffer_ring;
	struct uib_udp_rx_slot  *rx_slots;
	struct uib_ptr_ring      tx_queue;
	struct uib_ptr_ring      tx_ref_pool;
	struct uib_ptr_ring      tx_copy_pool;
	struct uib_ptr_ring      tx_gso_pool;
	struct io_uring          ring;
	struct uib_udp_op        multishot_op;
	struct uib_udp_op        timer_op;
	struct uib_udp_op        wake_op;
	struct msghdr            multishot_message;
	struct __kernel_timespec timeout;
	struct uib_transport_stats stats;
	struct sockaddr_in       local_addr;
	pthread_cond_t           init_cond;
	pthread_mutex_t          init_lock;
	pthread_t                worker;
	struct uib_context      *ctx;
	struct uib_udp_rx_worker *rx_workers;
	struct uib_udp_flow     *flows;
	void                    *rx_buffers;
	size_t                   rx_buffer_len;
	size_t                   page_size;
	unsigned int             rx_batch;
	unsigned int             tx_batch;
	unsigned int             sqpoll_idle;
	unsigned int             sqpoll_cpu;
	unsigned int             test_drop_tx;
	unsigned int             rx_worker_count;
	unsigned int             flow_count;
	unsigned int             requested_flow_count;
	uint32_t                 zerocopy_sequence;
	int                      zerocopy_enabled;
	int                      multishot_required;
	int                      multishot_armed;
	int                      timer_armed;
	int                      wake_armed;
	int                      worker_run;
	int                      stop_requested;
	int                      accepting;
	int                      worker_waiting;
	int                      init_done;
	int                      init_rc;
	int                      raw_mode;
	int                      fixed_file;
	int                      sqpoll_requested;
	int                      sqpoll_active;
	int                      sqpoll_cpu_set;
	int                      tx_batch_set;
	int                      gso_enabled;
	int                      gso_required;
	int                      gro_enabled;
	enum uib_udp_flow_mode   flow_mode;
	int                      wake_fd;
	int                      fd;
};

static int init_udp_flows(struct uib_context *ctx,
			  struct uib_udp_backend *be);
static void cleanup_udp_flows(struct uib_udp_backend *be);
static int udp_flow_count(unsigned int *count);

static int test_drop_packet(struct uib_udp_backend *be)
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

static struct uib_udp_rx_work *
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

static void rx_work_release(struct uib_udp_rx_worker *worker,
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

static void *rx_worker_loop(void *argument)
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
					work->data, work->length, work->ecn,
					work->rx_time_ns);
			else
				uib_transport_deliver_ecn(be->ctx, &work->source,
					work->data, work->length, work->ecn,
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

static void dispatch_packet(struct uib_context *ctx, const void *buffer,
			    size_t length,
			    const struct sockaddr_in *source, uint8_t ecn,
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
	work->ecn = ecn;
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
		      size_t *rdma_len_out, uint8_t *ecn,
		      uint8_t *integrity_validated)
{
	struct uib_wire_meta meta;
	uint8_t *frame = buffer;
	struct udphdr *udp;
	struct iphdr *ip;
	uint8_t *rdma;
	size_t rdma_len;
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
	if (udp_len < sizeof(*udp) + ctx->pops->min_packet ||
	    ip_len + udp_len > frame_len)
		return -1;
	rdma = (uint8_t *)(udp + 1);
	rdma_len = udp_len - sizeof(*udp);
	meta.src_ip = ip->saddr;
	meta.dst_ip = ip->daddr;
	meta.src_port = udp->source;
	meta.dst_port = udp->dest;
	meta.ip_id = ntohs(ip->id);
	*ecn = IPTOS_ECN(ip->tos);
	*integrity_validated = 0;
	if (ctx->pops->wire_decode) {
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
			   const struct sockaddr_in *source, uint8_t ecn)
{
	struct uib_udp_backend *be = ctx->backend;
	struct sockaddr_in decoded_source = *source;
	void *rdma = buffer;
	size_t rdma_len = length;
	uint8_t integrity_validated = 0;

	if (be->raw_mode &&
	    raw_decode(ctx, buffer, length, &decoded_source, &rdma,
		       &rdma_len, &ecn, &integrity_validated) < 0)
		return;
	dispatch_packet(ctx, rdma, rdma_len, &decoded_source, ecn,
			integrity_validated);
	__atomic_fetch_add(&be->stats.rx_packets, 1, __ATOMIC_RELAXED);
}

static void deliver_udp_payload(struct uib_context *ctx, void *buffer,
				size_t length,
				const struct sockaddr_in *source,
				uint16_t gro_size, uint8_t ecn)
{
	struct uib_udp_backend *be = ctx->backend;
	uint8_t *payload = buffer;
	size_t segment;
	size_t offset;

	if (!gro_size || gro_size >= length) {
		deliver_packet(ctx, buffer, length, source, ecn);
		return;
	}
	__atomic_fetch_add(&be->stats.rx_gro_batches, 1, __ATOMIC_RELAXED);
	for (offset = 0; offset < length; offset += segment) {
		segment = length - offset;
		if (segment > gro_size)
			segment = gro_size;
		deliver_packet(ctx, payload + offset, segment, source, ecn);
	}
}

static uint16_t message_gro_size(const struct msghdr *message)
{
	struct cmsghdr *control;
	uint16_t size;

	for (control = CMSG_FIRSTHDR(message); control;
	     control = CMSG_NXTHDR((struct msghdr *)message, control)) {
		if (control->cmsg_level != IPPROTO_UDP ||
		    control->cmsg_type != UDP_GRO ||
		    control->cmsg_len < CMSG_LEN(sizeof(size)))
			continue;
		memcpy(&size, CMSG_DATA(control), sizeof(size));
		return size;
	}
	return 0;
}

static uint8_t message_ecn(const struct msghdr *message)
{
	struct cmsghdr *control;
	uint8_t tos;

	for (control = CMSG_FIRSTHDR(message); control;
	     control = CMSG_NXTHDR((struct msghdr *)message, control)) {
		if (control->cmsg_level != IPPROTO_IP ||
		    control->cmsg_type != IP_TOS ||
		    control->cmsg_len < CMSG_LEN(sizeof(tos)))
			continue;
		memcpy(&tos, CMSG_DATA(control), sizeof(tos));
		return IPTOS_ECN(tos);
	}
	return IPTOS_ECN_NOT_ECT;
}

static uint16_t multishot_gro_size(struct io_uring_recvmsg_out *output,
				   struct msghdr *message)
{
	struct cmsghdr *control;
	uint16_t size;

	control = io_uring_recvmsg_cmsg_firsthdr(output, message);
	while (control) {
		if (control->cmsg_level == IPPROTO_UDP &&
		    control->cmsg_type == UDP_GRO &&
		    control->cmsg_len >= CMSG_LEN(sizeof(size))) {
			memcpy(&size, CMSG_DATA(control), sizeof(size));
			return size;
		}
		control = io_uring_recvmsg_cmsg_nexthdr(output, message,
						      control);
	}
	return 0;
}

static uint8_t multishot_ecn(struct io_uring_recvmsg_out *output,
			     struct msghdr *message)
{
	struct cmsghdr *control;
	uint8_t tos;

	control = io_uring_recvmsg_cmsg_firsthdr(output, message);
	while (control) {
		if (control->cmsg_level == IPPROTO_IP &&
		    control->cmsg_type == IP_TOS &&
		    control->cmsg_len >= CMSG_LEN(sizeof(tos))) {
			memcpy(&tos, CMSG_DATA(control), sizeof(tos));
			return IPTOS_ECN(tos);
		}
		control = io_uring_recvmsg_cmsg_nexthdr(output, message,
						      control);
	}
	return IPTOS_ECN_NOT_ECT;
}

static struct io_uring_sqe *get_sqe(struct uib_udp_backend *be)
{
	return io_uring_get_sqe(&be->ring);
}

static int ring_socket(const struct uib_udp_backend *be)
{
	return be->fixed_file ? 0 : be->fd;
}

static void mark_socket_sqe(const struct uib_udp_backend *be,
			    struct io_uring_sqe *sqe)
{
	if (be->fixed_file)
		sqe->flags |= IOSQE_FIXED_FILE;
}

static int arm_timer(struct uib_udp_backend *be)
{
	struct io_uring_sqe *sqe;

	sqe = get_sqe(be);
	if (!sqe)
		return -EAGAIN;
	be->timeout.tv_sec = UIB_TIMER_TICK_MS / 1000;
	be->timeout.tv_nsec = (UIB_TIMER_TICK_MS % 1000) * 1000000;
	io_uring_prep_timeout(sqe, &be->timeout, 0, 0);
	io_uring_sqe_set_data(sqe, &be->timer_op);
	be->timer_armed = 1;
	return 0;
}

static int arm_wake(struct uib_udp_backend *be)
{
	struct io_uring_sqe *sqe;

	sqe = get_sqe(be);
	if (!sqe)
		return -EAGAIN;
	io_uring_prep_poll_add(sqe, be->wake_fd, POLLIN);
	io_uring_sqe_set_data(sqe, &be->wake_op);
	be->wake_armed = 1;
	return 0;
}

static void *multishot_buffer(struct uib_udp_backend *be, unsigned int id)
{
	return (uint8_t *)be->rx_buffers + id * be->rx_buffer_len;
}

static void recycle_multishot_buffer(struct uib_udp_backend *be,
				     unsigned int id)
{
	unsigned int mask;

	mask = io_uring_buf_ring_mask(UIB_URING_RX_BUFFERS);
	io_uring_buf_ring_add(be->buffer_ring, multishot_buffer(be, id),
			      (unsigned int)be->rx_buffer_len, (unsigned short)id,
			      mask, 0);
	io_uring_buf_ring_advance(be->buffer_ring, 1);
}

static int setup_multishot(struct uib_udp_backend *be)
{
	struct io_uring_buf_ring *buffer_ring;
	unsigned int i;
	int rc;

	be->rx_buffer_len = UIB_RECV_BUF_LEN + sizeof(struct sockaddr_in) +
			    UIB_UDP_RX_CONTROL_SPACE +
			    sizeof(struct io_uring_recvmsg_out);
	be->rx_buffers = calloc(UIB_URING_RX_BUFFERS, be->rx_buffer_len);
	if (!be->rx_buffers)
		return -ENOMEM;
	buffer_ring = io_uring_setup_buf_ring(&be->ring, UIB_URING_RX_BUFFERS,
					       UIB_URING_RX_BUFFER_GROUP, 0,
					       &rc);
	if (!buffer_ring) {
		free(be->rx_buffers);
		be->rx_buffers = NULL;
		return rc;
	}
	be->buffer_ring = buffer_ring;
	for (i = 0; i < UIB_URING_RX_BUFFERS; i++)
		recycle_multishot_buffer(be, i);
	memset(&be->multishot_message, 0, sizeof(be->multishot_message));
	be->multishot_message.msg_namelen = sizeof(struct sockaddr_in);
	be->multishot_message.msg_controllen = UIB_UDP_RX_CONTROL_SPACE;
	be->stats.rx_mode = UIB_URING_RX_MULTISHOT;
	return 0;
}

static void cleanup_multishot(struct uib_udp_backend *be)
{
	if (be->buffer_ring)
		io_uring_free_buf_ring(&be->ring, be->buffer_ring,
				       UIB_URING_RX_BUFFERS,
				       UIB_URING_RX_BUFFER_GROUP);
	be->buffer_ring = NULL;
	free(be->rx_buffers);
	be->rx_buffers = NULL;
}

static int setup_batched_rx(struct uib_udp_backend *be)
{
	struct uib_udp_rx_slot *slot;
	unsigned int i;

	be->rx_slots = calloc(be->rx_batch, sizeof(*be->rx_slots));
	if (!be->rx_slots)
		return -ENOMEM;
	for (i = 0; i < be->rx_batch; i++) {
		slot = &be->rx_slots[i];
		slot->buffer = malloc(UIB_RECV_BUF_LEN);
		if (!slot->buffer) {
			while (i)
				free(be->rx_slots[--i].buffer);
			free(be->rx_slots);
			be->rx_slots = NULL;
			return -ENOMEM;
		}
		slot->op.type = UIB_UDP_OP_RX_ONESHOT;
		slot->iov.iov_base = slot->buffer;
		slot->iov.iov_len = UIB_RECV_BUF_LEN;
		slot->message.msg_name = &slot->source;
		slot->message.msg_namelen = sizeof(slot->source);
		slot->message.msg_controllen = sizeof(slot->control.bytes);
		slot->message.msg_iov = &slot->iov;
		slot->message.msg_iovlen = 1;
		slot->message.msg_control = slot->control.bytes;
		slot->message.msg_controllen = sizeof(slot->control.bytes);
	}
	be->stats.rx_mode = UIB_URING_RX_BATCHED;
	return 0;
}

static void cleanup_batched_rx(struct uib_udp_backend *be)
{
	unsigned int i;

	if (!be->rx_slots)
		return;
	for (i = 0; i < be->rx_batch; i++)
		free(be->rx_slots[i].buffer);
	free(be->rx_slots);
	be->rx_slots = NULL;
}

static int arm_multishot(struct uib_udp_backend *be)
{
	struct io_uring_sqe *sqe;

	sqe = get_sqe(be);
	if (!sqe)
		return -EAGAIN;
	io_uring_prep_recvmsg_multishot(sqe, be->fd,
					 &be->multishot_message, MSG_TRUNC);
	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = UIB_URING_RX_BUFFER_GROUP;
	io_uring_sqe_set_data(sqe, &be->multishot_op);
	be->multishot_armed = 1;
	return 0;
}

static void arm_batched_rx(struct uib_udp_backend *be)
{
	struct uib_udp_rx_slot *slot;
	struct io_uring_sqe *sqe;
	unsigned int i;

	for (i = 0; i < be->rx_batch; i++) {
		slot = &be->rx_slots[i];
		if (slot->armed)
			continue;
		sqe = get_sqe(be);
		if (!sqe)
			return;
		slot->message.msg_namelen = sizeof(slot->source);
		slot->message.msg_controllen = sizeof(slot->control.bytes);
		slot->message.msg_flags = 0;
		io_uring_prep_recvmsg(sqe, be->fd, &slot->message, 0);
		io_uring_sqe_set_data(sqe, &slot->op);
		slot->armed = 1;
	}
}

static void inflight_add(struct uib_udp_backend *be, struct uib_udp_tx *tx)
{
	tx->inflight_prev = NULL;
	tx->inflight_next = be->tx_inflight;
	if (be->tx_inflight)
		be->tx_inflight->inflight_prev = tx;
	be->tx_inflight = tx;
}

static void inflight_remove(struct uib_udp_backend *be, struct uib_udp_tx *tx)
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

static struct uib_udp_tx *alloc_ref_tx(struct uib_udp_backend *be)
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

static struct uib_udp_tx *alloc_copy_tx(struct uib_udp_backend *be,
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

static struct uib_udp_tx *alloc_gso_tx(struct uib_udp_backend *be)
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

static void free_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx);

static size_t buffer_page_count(const struct uib_udp_backend *be,
				const void *buffer, size_t length)
{
	size_t offset;

	if (!length)
		return 0;
	offset = (uintptr_t)buffer & (be->page_size - 1);
	return (offset + length + be->page_size - 1) / be->page_size;
}

static int gso_zerocopy_eligible(const struct uib_context *ctx,
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

static size_t limit_zerocopy_run(const struct uib_context *ctx,
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

static int gso_compatible(const struct uib_udp_tx *head,
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

static void merge_gso_tx(struct uib_udp_backend *be,
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

static void free_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx)
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

static void requeue_tx_list(struct uib_udp_backend *be,
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

static void requeue_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx)
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

static unsigned int dequeue_tx_batch(struct uib_udp_backend *be,
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

static void submit_tx(struct uib_context *ctx)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_tx *batch;
	struct uib_udp_tx *next;
	struct uib_udp_tx *merged;
	struct io_uring_sqe *sqe;
	struct uib_udp_tx *tx;
	unsigned int available;

	available = io_uring_sq_space_left(&be->ring);
	if (available > be->tx_batch)
		available = be->tx_batch;
	if (!available)
		return;
	dequeue_tx_batch(be, &batch, available);
	for (tx = batch; tx; tx = next) {
		next = tx->queue_next;
		tx->queue_next = NULL;
		while (next && gso_compatible(tx, next)) {
			merged = next;
			next = merged->queue_next;
			merge_gso_tx(be, tx, merged);
		}
		sqe = get_sqe(be);
		if (!sqe) {
			tx->queue_next = next;
			requeue_tx_list(be, tx);
			break;
		}
#ifdef UIB_HAVE_ZEROCOPY
		if (tx->zerocopy) {
			io_uring_prep_sendmsg_zc(sqe, ring_socket(be),
						   &tx->message, MSG_NOSIGNAL);
			sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
		} else
#endif
			io_uring_prep_sendmsg(sqe, ring_socket(be),
						  &tx->message, MSG_NOSIGNAL);
		mark_socket_sqe(be, sqe);
		io_uring_sqe_set_data(sqe, &tx->op);
		inflight_add(be, tx);
		__atomic_fetch_add(&be->stats.tx_submitted, tx->packets,
				   __ATOMIC_RELAXED);
	}
}

static int tx_drained(struct uib_udp_backend *be)
{
	return !uib_ptr_ring_count(&be->tx_queue) && !be->tx_retry_head &&
		!be->tx_inflight;
}

static int tx_queued(struct uib_udp_backend *be)
{
	return uib_ptr_ring_count(&be->tx_queue) || be->tx_retry_head;
}

static int fallback_gso_send(struct uib_context *ctx,
			     struct uib_udp_tx *tx)
{
	struct uib_udp_backend *be = ctx->backend;
	struct msghdr message;
	unsigned int iov_offset;
	ssize_t sent;
	unsigned int i;

	memset(&message, 0, sizeof(message));
	message.msg_name = &tx->destination;
	message.msg_namelen = sizeof(tx->destination);
	iov_offset = 0;
	for (i = 0; i < tx->release_count; i++) {
		message.msg_iov = &tx->iov_list[iov_offset];
		message.msg_iovlen = tx->release_list[i].iov_count;
		sent = sendmsg(be->fd, &message, MSG_NOSIGNAL);
		if (sent < 0)
			return -errno;
		if ((size_t)sent != tx->gso_size)
			return -EIO;
		iov_offset += tx->release_list[i].iov_count;
	}
	return 0;
}

static int zerocopy_retryable(int result)
{
	return result == -EINVAL || result == -EOPNOTSUPP ||
		result == -EMSGSIZE;
}

static void complete_tx(struct uib_context *ctx, struct uib_udp_tx *tx,
			const struct io_uring_cqe *cqe)
{
	struct uib_udp_backend *be = ctx->backend;

	if (cqe->flags & IORING_CQE_F_NOTIF) {
		__atomic_fetch_add(&ctx->zerocopy.completions, 1,
				   __ATOMIC_RELAXED);
		if ((uint32_t)cqe->res & IORING_NOTIF_USAGE_ZC_COPIED)
			__atomic_fetch_add(&ctx->zerocopy.deferred_copies, 1,
					   __ATOMIC_RELAXED);
		inflight_remove(be, tx);
		if (tx->zc_result) {
			if (ctx->zerocopy.mode != UIB_ZEROCOPY_REQUIRED &&
			    zerocopy_retryable(tx->zc_result)) {
				tx->zerocopy = 0;
				tx->zc_result = 0;
				requeue_tx(be, tx);
				return;
			}
			__atomic_fetch_add(&be->stats.tx_errors, 1,
					   __ATOMIC_RELAXED);
			__atomic_fetch_add(&ctx->zerocopy.errors, 1,
					   __ATOMIC_RELAXED);
		}
		free_tx(be, tx);
		return;
	}
	if (cqe->res < 0) {
		if (tx->zerocopy && cqe->flags & IORING_CQE_F_MORE) {
			tx->zc_result = cqe->res;
			return;
		}
#ifdef UIB_HAVE_ZEROCOPY
		if (tx->zerocopy && ctx->zerocopy.mode != UIB_ZEROCOPY_REQUIRED &&
		    zerocopy_retryable(cqe->res)) {
			tx->zerocopy = 0;
			inflight_remove(be, tx);
			requeue_tx(be, tx);
			return;
		}
#endif
		if (tx->gso_size && !be->gso_required &&
		    (cqe->res == -EINVAL || cqe->res == -EOPNOTSUPP ||
		     cqe->res == -EIO)) {
			__atomic_store_n(&be->gso_enabled, 0, __ATOMIC_RELEASE);
			if (!fallback_gso_send(ctx, tx)) {
				__atomic_fetch_add(
					&be->stats.tx_completed, tx->packets,
					__ATOMIC_RELAXED);
				uib_zerocopy_account_copy(ctx, tx->length);
				inflight_remove(be, tx);
				free_tx(be, tx);
				return;
			}
		}
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&ctx->zerocopy.errors, 1, __ATOMIC_RELAXED);
		inflight_remove(be, tx);
		free_tx(be, tx);
		return;
	}
	if ((size_t)cqe->res != tx->length) {
		if (tx->zerocopy && cqe->flags & IORING_CQE_F_MORE) {
			tx->zc_result = -EIO;
			return;
		}
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		inflight_remove(be, tx);
		free_tx(be, tx);
		return;
	}
	__atomic_fetch_add(&be->stats.tx_completed, tx->packets,
			   __ATOMIC_RELAXED);
	if (tx->gso_size) {
		__atomic_fetch_add(&be->stats.tx_gso_batches, 1,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&be->stats.tx_gso_packets, tx->packets,
				   __ATOMIC_RELAXED);
	}
	if (tx->zerocopy) {
		uib_zerocopy_account_direct(ctx, tx->length,
					    UIB_ZEROCOPY_CAP_MSG);
		if (cqe->flags & IORING_CQE_F_MORE)
			return;
	} else {
		uib_zerocopy_account_copy(ctx, tx->length);
	}
	inflight_remove(be, tx);
	free_tx(be, tx);
}

static int switch_to_batched_rx(struct uib_udp_backend *be)
{
	int rc;

	cleanup_multishot(be);
	be->multishot_armed = 0;
	rc = setup_batched_rx(be);
	return rc;
}

static void complete_multishot(struct uib_context *ctx,
			       const struct io_uring_cqe *cqe)
{
	struct uib_udp_backend *be = ctx->backend;
	struct io_uring_recvmsg_out *output;
	struct sockaddr_in source;
	unsigned int captured;
	unsigned int id;
	void *buffer;
	void *payload;
	uint16_t gro_size;
	uint8_t ecn;

	if (!(cqe->flags & IORING_CQE_F_MORE))
		be->multishot_armed = 0;
	if (cqe->res == -EINVAL || cqe->res == -EOPNOTSUPP) {
		if (be->multishot_required || switch_to_batched_rx(be) < 0)
			__atomic_store_n(&be->worker_run, 0, __ATOMIC_RELEASE);
		return;
	}
	if (cqe->res < 0 || !(cqe->flags & IORING_CQE_F_BUFFER))
		return;
	id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
	if (id >= UIB_URING_RX_BUFFERS)
		return;
	buffer = multishot_buffer(be, id);
	output = io_uring_recvmsg_validate(buffer, cqe->res,
					   &be->multishot_message);
	if (!output)
		goto recycle;
	payload = io_uring_recvmsg_payload(output, &be->multishot_message);
	captured = io_uring_recvmsg_payload_length(output, cqe->res,
						   &be->multishot_message);
	if (output->namelen < sizeof(source) || output->flags & MSG_TRUNC ||
	    output->payloadlen > captured)
		goto recycle;
	memcpy(&source, io_uring_recvmsg_name(output), sizeof(source));
	gro_size = multishot_gro_size(output, &be->multishot_message);
	ecn = multishot_ecn(output, &be->multishot_message);
	deliver_udp_payload(ctx, payload, output->payloadlen, &source,
			    gro_size, ecn);

recycle:
	recycle_multishot_buffer(be, id);
}

static void complete_oneshot(struct uib_context *ctx,
			     struct uib_udp_rx_slot *slot,
			     const struct io_uring_cqe *cqe)
{
	uint16_t gro_size;
	uint8_t ecn;

	slot->armed = 0;
	if (cqe->res <= 0)
		return;
	gro_size = message_gro_size(&slot->message);
	ecn = message_ecn(&slot->message);
	deliver_udp_payload(ctx, slot->buffer, (size_t)cqe->res,
			    &slot->source, gro_size, ecn);
}

static void drain_wake_fd(struct uib_udp_backend *be)
{
	uint64_t value;

	while (read(be->wake_fd, &value, sizeof(value)) == sizeof(value))
		;
}

static void update_largest_batch(struct uib_udp_backend *be,
				 unsigned int received)
{
	uint32_t largest;

	largest = __atomic_load_n(&be->stats.rx_largest_batch,
				  __ATOMIC_RELAXED);
	while (received > largest &&
	       !__atomic_compare_exchange_n(&be->stats.rx_largest_batch, &largest,
					    received, 0, __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED))
		;
}

static void process_completions(struct uib_context *ctx,
				struct io_uring_cqe **cqes,
				unsigned int count)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_rx_slot *slot;
	struct uib_udp_tx *tx;
	struct uib_udp_op *op;
	unsigned int received = 0;
	unsigned int i;

	for (i = 0; i < count; i++) {
		op = io_uring_cqe_get_data(cqes[i]);
		if (!op)
			goto seen;
		switch (op->type) {
		case UIB_UDP_OP_RX_MULTISHOT:
			complete_multishot(ctx, cqes[i]);
			if (cqes[i]->res > 0)
				received++;
			break;
		case UIB_UDP_OP_RX_ONESHOT:
			slot = (struct uib_udp_rx_slot *)op;
			complete_oneshot(ctx, slot, cqes[i]);
			if (cqes[i]->res > 0)
				received++;
			break;
		case UIB_UDP_OP_TX:
			tx = (struct uib_udp_tx *)op;
			complete_tx(ctx, tx, cqes[i]);
			break;
		case UIB_UDP_OP_TIMER:
			be->timer_armed = 0;
			uib_transport_retransmit_walk(ctx);
			break;
		case UIB_UDP_OP_WAKE:
			be->wake_armed = 0;
			drain_wake_fd(be);
			break;
		}
seen:
		io_uring_cqe_seen(&be->ring, cqes[i]);
	}
	if (received) {
		__atomic_fetch_add(&be->stats.rx_completion_batches, 1,
				   __ATOMIC_RELAXED);
		update_largest_batch(be, received);
	}
}

static int setup_rx(struct uib_udp_backend *be)
{
	const char *mode = getenv(UIB_ENV_URING_RX_MODE);
	int rc;

	if (mode && !strcmp(mode, "batch"))
		return setup_batched_rx(be);
	be->multishot_required = mode && !strcmp(mode, "multishot");
	rc = setup_multishot(be);
	if (!rc)
		return 0;
	if (be->multishot_required)
		return rc;
	return setup_batched_rx(be);
}

static void signal_worker_init(struct uib_udp_backend *be, int rc)
{
	pthread_mutex_lock(&be->init_lock);
	be->init_rc = rc;
	be->init_done = 1;
	pthread_cond_signal(&be->init_cond);
	pthread_mutex_unlock(&be->init_lock);
}

static void free_all_tx(struct uib_udp_backend *be)
{
	struct uib_udp_tx *next;
	struct uib_udp_tx *tx;

	tx = be->tx_inflight;
	while (tx) {
		next = tx->inflight_next;
		free_tx(be, tx);
		tx = next;
	}
	be->tx_inflight = NULL;
	tx = be->tx_retry_head;
	while (tx) {
		next = tx->queue_next;
		free_tx(be, tx);
		tx = next;
	}
	be->tx_retry_head = NULL;
	while ((tx = uib_ptr_ring_pop(&be->tx_queue)) != NULL)
		free_tx(be, tx);
}

static void *worker_loop(void *argument)
{
	struct io_uring_cqe *cqes[UIB_URING_BATCH_MAX];
	struct uib_context *ctx = argument;
	struct uib_udp_backend *be = ctx->backend;
	struct io_uring_params params;
	unsigned int count;
	int rc;

	memset(&params, 0, sizeof(params));
	if (be->sqpoll_requested) {
		params.flags = IORING_SETUP_SQPOLL;
		params.sq_thread_idle = be->sqpoll_idle;
		if (be->sqpoll_cpu_set) {
			params.flags |= IORING_SETUP_SQ_AFF;
			params.sq_thread_cpu = be->sqpoll_cpu;
		}
	}
	rc = io_uring_queue_init_params(UIB_URING_DEPTH, &be->ring, &params);
	if (rc < 0 && be->sqpoll_requested) {
		memset(&params, 0, sizeof(params));
		rc = io_uring_queue_init_params(UIB_URING_DEPTH, &be->ring,
						&params);
	} else if (!rc && be->sqpoll_requested) {
		be->sqpoll_active = 1;
	}
	if (rc < 0) {
		signal_worker_init(be, rc);
		return NULL;
	}
	if (!io_uring_register_files(&be->ring, &be->fd, 1))
		be->fixed_file = 1;
	rc = setup_rx(be);
	if (rc < 0) {
		if (be->fixed_file)
			io_uring_unregister_files(&be->ring);
		io_uring_queue_exit(&be->ring);
		signal_worker_init(be, rc);
		return NULL;
	}
	signal_worker_init(be, 0);

	while (__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE)) {
		if (__atomic_load_n(&be->stop_requested, __ATOMIC_ACQUIRE) &&
		    tx_drained(be))
			break;
		if (be->stats.rx_mode == UIB_URING_RX_MULTISHOT) {
			if (!be->multishot_armed)
				arm_multishot(be);
		} else {
			arm_batched_rx(be);
		}
		if (!be->timer_armed)
			arm_timer(be);
		if (!be->wake_armed)
			arm_wake(be);
		submit_tx(ctx);
		__atomic_store_n(&be->worker_waiting, 1, __ATOMIC_SEQ_CST);
		if (tx_queued(be)) {
			__atomic_store_n(&be->worker_waiting, 0,
					 __ATOMIC_RELEASE);
			rc = io_uring_submit(&be->ring);
			if (rc < 0 && rc != -EINTR)
				break;
			continue;
		}
		rc = io_uring_submit_and_wait(&be->ring, 1);
		__atomic_store_n(&be->worker_waiting, 0, __ATOMIC_RELEASE);
		if (rc < 0 && rc != -EINTR)
			break;
		count = io_uring_peek_batch_cqe(&be->ring, cqes,
						be->rx_batch);
		if (count)
			process_completions(ctx, cqes, count);
	}

	cleanup_multishot(be);
	cleanup_batched_rx(be);
	if (be->fixed_file)
		io_uring_unregister_files(&be->ring);
	io_uring_queue_exit(&be->ring);
	free_all_tx(be);
	__atomic_store_n(&be->worker_run, 0, __ATOMIC_RELEASE);
	return NULL;
}

static int udp_probe(void)
{
	return 1;
}

static int init_rx_work_pools(struct uib_udp_rx_worker *worker)
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

static void cleanup_rx_work_pools(struct uib_udp_rx_worker *worker)
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
	char *end;
	unsigned long parsed;

	if (!setting || !*setting)
		return 0;
	errno = 0;
	parsed = strtoul(setting, &end, 10);
	if (errno || *end || !parsed || parsed > UIB_URING_BATCH_MAX)
		return -EINVAL;
	*value = (unsigned int)parsed;
	return 0;
}

static int parse_sqpoll(struct uib_udp_backend *be)
{
	const char *setting = getenv(UIB_ENV_URING_SQPOLL);
	const char *cpu = getenv(UIB_ENV_URING_SQPOLL_CPU);
	const char *idle = getenv(UIB_ENV_URING_SQPOLL_IDLE);
	char *end;
	unsigned long value;

	be->sqpoll_idle = UIB_URING_SQPOLL_IDLE_DEFAULT;
	if (setting && *setting && strcmp(setting, "off") &&
	    strcmp(setting, "0")) {
		if (strcmp(setting, "on") && strcmp(setting, "1"))
			return -EINVAL;
		be->sqpoll_requested = 1;
	}
	if (idle && *idle) {
		errno = 0;
		value = strtoul(idle, &end, 10);
		if (errno || *end || !value || value > UINT32_MAX)
			return -EINVAL;
		be->sqpoll_idle = (unsigned int)value;
	}
	if (cpu && *cpu) {
		errno = 0;
		value = strtoul(cpu, &end, 10);
		if (errno || *end || value > UINT32_MAX)
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

static int init_tx_rings(struct uib_udp_backend *be)
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

static void warm_tx_pools(struct uib_udp_backend *be)
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

static int udp_init_common(struct uib_context *ctx, int asynchronous)
{
	const char *drop_tx = getenv(UIB_ENV_TEST_DROP_TX);
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
	if (drop_tx)
		be->test_drop_tx = (unsigned int)strtoul(drop_tx, NULL, 10);
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
	ip->tos = IPTOS_ECN_ECT0;
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

static int build_raw_packet(struct uib_context *ctx,
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

static int build_raw_iov_packet(
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

static int udp_send_ref_fallback(struct uib_context *ctx,
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

static size_t udp_ref_run(struct uib_context *ctx,
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

static struct uib_udp_tx *udp_ref_gso_tx(
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

static struct uib_udp_tx *udp_ref_one_tx(
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

static int udp_publish_ref_batch(struct uib_udp_backend *be,
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

static int udp_iov_ref_validate(const struct uib_transport_iov_ref *ref)
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

static int udp_send_iov_ref_fallback(
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

static size_t udp_iov_ref_run(
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

static struct uib_udp_tx *udp_iov_ref_gso_tx(
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

static struct uib_udp_tx *udp_iov_ref_one_tx(
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
	uint8_t ecn;
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
		ecn = message_ecn(&message);
		deliver_udp_payload(be->ctx, flow->rx_buffer,
				    (size_t)received, &source, gro_size, ecn);
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

static int udp_flow_count(unsigned int *count)
{
	const char *setting;
	unsigned long value;
	char *end;

	*count = 1;
	setting = getenv(UIB_ENV_UDP_FLOWS);
	if (!setting || !*setting)
		return 0;
	errno = 0;
	value = strtoul(setting, &end, 10);
	if (errno || end == setting || *end || !value ||
	    value > UIB_UDP_FLOW_MAX)
		return -EINVAL;
	*count = (unsigned int)value;
	return 0;
}

static int init_udp_flows(struct uib_context *ctx,
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

static void cleanup_udp_flows(struct uib_udp_backend *be)
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

static int udp_flow_send_iov_ref_batch(
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

static int udp_send_iov_ref_batch(
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

static int udp_send_ref_batch(struct uib_context *ctx,
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

static int udp_sync_send_entropy(struct uib_context *ctx,
				 const struct sockaddr_in *destination,
				 const void *buffer, size_t length,
				 uint16_t entropy)
{
	struct uib_udp_backend *be = ctx->backend;
	struct sockaddr_in flow_destination;
	struct uib_udp_tx *tx = NULL;
	struct msghdr message;
	struct iovec iov;
	unsigned int base_port;
	unsigned int flow_port;
	unsigned int delta;
	unsigned int lane;
	ssize_t sent;
	int socket_fd;
	int rc;

	if (test_drop_packet(be))
		return 0;
	flow_destination = *destination;
	socket_fd = be->fd;
	if (be->flow_mode == UIB_UDP_FLOW_FIXED_PORT) {
		lane = entropy % be->flow_count;
		socket_fd = be->flows[lane].tx_fd;
	}
	base_port = ntohs(destination->sin_port);
	flow_port = entropy;
	if (be->flow_mode == UIB_UDP_FLOW_MULTIPATH &&
	    flow_port >= base_port +
					       UIB_UDP_FLOW_PORT_STRIDE) {
		delta = flow_port - base_port;
		lane = delta / UIB_UDP_FLOW_PORT_STRIDE - 1;
		if (!(delta % UIB_UDP_FLOW_PORT_STRIDE) &&
		    lane < be->flow_count) {
			flow_destination.sin_port = htons((uint16_t)flow_port);
			socket_fd = be->flows[lane].tx_fd;
		}
	}
	if (be->raw_mode) {
		tx = alloc_copy_tx(be, sizeof(struct iphdr) +
				   sizeof(struct udphdr) + length);
		if (!tx)
			return -ENOMEM;
		tx->data = tx->storage;
		rc = build_raw_packet(ctx, destination, buffer, length, entropy,
				      tx);
		if (rc < 0)
			goto out;
		sent = sendto(socket_fd, tx->data, tx->length, MSG_NOSIGNAL,
			      (const struct sockaddr *)&flow_destination,
			      sizeof(flow_destination));
		if (sent >= 0)
			uib_zerocopy_account_copy(ctx, (size_t)sent);
		else
			sent = -errno;
		length = tx->length;
	} else {
		memset(&message, 0, sizeof(message));
		iov.iov_base = (void *)buffer;
		iov.iov_len = length;
		message.msg_name = &flow_destination;
		message.msg_namelen = sizeof(flow_destination);
		message.msg_iov = &iov;
		message.msg_iovlen = 1;
		sent = uib_zerocopy_sendmsg(ctx, socket_fd, &message,
					    MSG_NOSIGNAL,
					    be->zerocopy_enabled,
					    &be->zerocopy_sequence);
	}
	__atomic_fetch_add(&be->stats.tx_submitted, 1, __ATOMIC_RELAXED);
	if (sent < 0) {
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		rc = (int)sent;
	} else if ((size_t)sent != length) {
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		rc = -EIO;
	} else {
		__atomic_fetch_add(&be->stats.tx_completed, 1, __ATOMIC_RELAXED);
		rc = 0;
	}
out:
	if (tx)
		free_tx(be, tx);
	return rc;
}

static int udp_sync_send(struct uib_context *ctx,
			 const struct sockaddr_in *destination,
			 const void *buffer, size_t length)
{
	return udp_sync_send_entropy(ctx, destination, buffer, length, 0);
}

static int udp_sync_send_ref_batch_single(struct uib_context *ctx,
					  const struct uib_transport_ref *refs,
					  size_t count)
{
	size_t i;
	int rc;

	for (i = 0; i < count; i++) {
		rc = udp_sync_send_entropy(ctx, refs[i].dst, refs[i].buf,
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

static int udp_sync_send_iov_ref_batch(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count);

static int udp_sync_send_ref_batch_iov(struct uib_context *ctx,
				       const struct uib_transport_ref *refs,
				       size_t count)
{
	struct uib_transport_iov_ref iov_refs[UIB_URING_BATCH_MAX];
	struct iovec vectors[UIB_URING_BATCH_MAX];
	size_t batch;
	size_t offset;
	size_t i;
	int rc;

	offset = 0;
	while (offset < count) {
		batch = count - offset;
		if (batch > UIB_URING_BATCH_MAX)
			batch = UIB_URING_BATCH_MAX;
		memset(iov_refs, 0, batch * sizeof(*iov_refs));
		for (i = 0; i < batch; i++) {
			vectors[i].iov_base = (void *)refs[offset + i].buf;
			vectors[i].iov_len = refs[offset + i].len;
			iov_refs[i].dst = refs[offset + i].dst;
			iov_refs[i].iov = &vectors[i];
			iov_refs[i].iov_count = 1;
			iov_refs[i].len = refs[offset + i].len;
			iov_refs[i].release = refs[offset + i].release;
			iov_refs[i].opaque = refs[offset + i].opaque;
			iov_refs[i].flow = refs[offset + i].entropy;
			iov_refs[i].entropy = refs[offset + i].entropy;
		}
		rc = udp_sync_send_iov_ref_batch(ctx, iov_refs, batch);
		offset += batch;
		if (rc < 0) {
			for (i = offset; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
	}
	return 0;
}

static int udp_sync_send_ref_batch(struct uib_context *ctx,
				   const struct uib_transport_ref *refs,
				   size_t count)
{
	struct mmsghdr messages[UIB_URING_BATCH_MAX];
	struct iovec vectors[UIB_URING_BATCH_MAX];
	struct uib_udp_backend *be = ctx->backend;
	size_t batch;
	size_t offset;
	size_t i;
	int sent;
	int rc;

	if (be->raw_mode || be->test_drop_tx)
		return udp_sync_send_ref_batch_single(ctx, refs, count);
	if (be->flow_mode == UIB_UDP_FLOW_FIXED_PORT)
		return udp_sync_send_ref_batch_iov(ctx, refs, count);
	if (__atomic_load_n(&be->gso_enabled, __ATOMIC_ACQUIRE))
		return udp_sync_send_ref_batch_iov(ctx, refs, count);
	if (ctx->zerocopy.mode != UIB_ZEROCOPY_OFF)
		return udp_sync_send_ref_batch_single(ctx, refs, count);

	offset = 0;
	while (offset < count) {
		batch = count - offset;
		if (batch > UIB_URING_BATCH_MAX)
			batch = UIB_URING_BATCH_MAX;
		memset(messages, 0, batch * sizeof(*messages));
		for (i = 0; i < batch; i++) {
			vectors[i].iov_base = (void *)refs[offset + i].buf;
			vectors[i].iov_len = refs[offset + i].len;
			messages[i].msg_hdr.msg_name =
				(void *)refs[offset + i].dst;
			messages[i].msg_hdr.msg_namelen =
				sizeof(*refs[offset + i].dst);
			messages[i].msg_hdr.msg_iov = &vectors[i];
			messages[i].msg_hdr.msg_iovlen = 1;
		}
		sent = sendmmsg(be->fd, messages, (unsigned int)batch,
				MSG_NOSIGNAL);
		if (sent < 0) {
			rc = -errno;
			sent = 0;
		} else if ((size_t)sent != batch) {
			rc = -EIO;
		} else {
			rc = 0;
		}
		__atomic_fetch_add(&be->stats.tx_submitted, batch,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&be->stats.tx_completed, (size_t)sent,
				   __ATOMIC_RELAXED);
		if (rc < 0)
			__atomic_fetch_add(&be->stats.tx_errors, 1,
					   __ATOMIC_RELAXED);
		for (i = 0; i < batch; i++)
			refs[offset + i].release(refs[offset + i].opaque);
		for (i = 0; i < (size_t)sent; i++)
			uib_zerocopy_account_copy(ctx, refs[offset + i].len);
		offset += batch;
		if (rc < 0) {
			for (i = offset; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
	}
	return 0;
}

static int udp_sync_send_iov_ref_one(
	struct uib_context *ctx, const struct uib_transport_iov_ref *ref)
{
	struct uib_udp_backend *be = ctx->backend;
	struct msghdr message;
	ssize_t sent;
	int rc;

	if (test_drop_packet(be))
		return 0;
	memset(&message, 0, sizeof(message));
	message.msg_name = (void *)ref->dst;
	message.msg_namelen = sizeof(*ref->dst);
	message.msg_iov = (struct iovec *)ref->iov;
	message.msg_iovlen = ref->iov_count;
	sent = uib_zerocopy_sendmsg(ctx, be->fd, &message, MSG_NOSIGNAL,
				    be->zerocopy_enabled,
				    &be->zerocopy_sequence);
	__atomic_fetch_add(&be->stats.tx_submitted, 1, __ATOMIC_RELAXED);
	if (sent < 0) {
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		return (int)sent;
	}
	if ((size_t)sent != ref->len) {
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		return -EIO;
	}
	__atomic_fetch_add(&be->stats.tx_completed, 1, __ATOMIC_RELAXED);
	rc = 0;
	return rc;
}

static int udp_sync_send_raw_iov_ref(
	struct uib_context *ctx, const struct uib_transport_iov_ref *ref)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_tx *tx;
	size_t storage_len;
	ssize_t sent;
	int rc;

	if (test_drop_packet(be))
		return 0;
	storage_len = sizeof(struct iphdr) + sizeof(struct udphdr) + ref->len;
	tx = alloc_copy_tx(be, storage_len);
	if (!tx)
		return -ENOMEM;
	tx->data = tx->storage;
	rc = build_raw_iov_packet(ctx, ref, tx);
	if (rc < 0)
		goto out;
	sent = sendto(be->fd, tx->data, tx->length, MSG_NOSIGNAL,
		      (const struct sockaddr *)ref->dst, sizeof(*ref->dst));
	__atomic_fetch_add(&be->stats.tx_submitted, 1, __ATOMIC_RELAXED);
	if (sent < 0) {
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		rc = -errno;
	} else if ((size_t)sent != tx->length) {
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		rc = -EIO;
	} else {
		__atomic_fetch_add(&be->stats.tx_completed, 1,
				   __ATOMIC_RELAXED);
		uib_zerocopy_account_copy(ctx, (size_t)sent);
		rc = 0;
	}

out:
	free_tx(be, tx);
	return rc;
}

static int udp_sync_send_iov_ref_fallback(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count)
{
	struct uib_udp_backend *be = ctx->backend;
	size_t i;
	int rc;

	for (i = 0; i < count; i++) {
		if (be->raw_mode)
			rc = udp_sync_send_raw_iov_ref(ctx, &refs[i]);
		else
			rc = udp_sync_send_iov_ref_one(ctx, &refs[i]);
		refs[i].release(refs[i].opaque);
		if (rc < 0) {
			for (i++; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
	}
	return 0;
}

static int udp_sync_send_iov_ref_plain(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_iov_ref *ref)
{
	struct msghdr message;
	ssize_t sent;

	memset(&message, 0, sizeof(message));
	message.msg_name = (void *)ref->dst;
	message.msg_namelen = sizeof(*ref->dst);
	message.msg_iov = (struct iovec *)ref->iov;
	message.msg_iovlen = ref->iov_count;
	sent = uib_zerocopy_sendmsg(ctx, be->fd, &message, MSG_NOSIGNAL,
				    be->zerocopy_enabled,
				    &be->zerocopy_sequence);
	if (sent < 0)
		return (int)sent;
	if ((size_t)sent != ref->len)
		return -EIO;
	return 0;
}

static int udp_sync_send_iov_ref_gso(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count)
{
	struct iovec vectors[UIB_UDP_GSO_MAX_IOVS];
	struct uib_udp_backend *be = ctx->backend;
	union {
		struct cmsghdr header;
		uint8_t bytes[CMSG_SPACE(sizeof(uint16_t))];
	} control;
	struct cmsghdr *header;
	struct msghdr message;
	uint16_t segment_size;
	size_t completed;
	size_t vector_count;
	size_t length;
	ssize_t sent;
	size_t i;
	int rc;

	vector_count = 0;
	length = 0;
	for (i = 0; i < count; i++) {
		memcpy(&vectors[vector_count], refs[i].iov,
		       refs[i].iov_count * sizeof(*vectors));
		vector_count += refs[i].iov_count;
		length += refs[i].len;
	}
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	segment_size = (uint16_t)refs[0].len;
	message.msg_name = (void *)refs[0].dst;
	message.msg_namelen = sizeof(*refs[0].dst);
	message.msg_iov = vectors;
	message.msg_iovlen = vector_count;
	message.msg_control = control.bytes;
	message.msg_controllen = CMSG_SPACE(sizeof(segment_size));
	header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = IPPROTO_UDP;
	header->cmsg_type = UDP_SEGMENT;
	header->cmsg_len = CMSG_LEN(sizeof(segment_size));
	memcpy(CMSG_DATA(header), &segment_size, sizeof(segment_size));
	if (ctx->zerocopy.mode == UIB_ZEROCOPY_AUTO &&
	    segment_size < ctx->zerocopy.threshold) {
		sent = sendmsg(be->fd, &message, MSG_NOSIGNAL);
		if (sent >= 0)
			uib_zerocopy_account_copy(ctx, (size_t)sent);
		else
			sent = -errno;
	} else {
		sent = uib_zerocopy_sendmsg(ctx, be->fd, &message,
					    MSG_NOSIGNAL,
					    be->zerocopy_enabled,
					    &be->zerocopy_sequence);
	}
	__atomic_fetch_add(&be->stats.tx_submitted, count, __ATOMIC_RELAXED);
	if (sent >= 0 && (size_t)sent == length) {
		completed = count;
		__atomic_fetch_add(&be->stats.tx_gso_batches, 1,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&be->stats.tx_gso_packets, count,
				   __ATOMIC_RELAXED);
		rc = 0;
		goto account;
	}
	rc = sent < 0 ? (int)sent : -EIO;
	if (be->gso_required || !zerocopy_retryable(rc)) {
		completed = 0;
		goto account;
	}
	__atomic_store_n(&be->gso_enabled, 0, __ATOMIC_RELEASE);
	completed = 0;
	for (i = 0; i < count; i++) {
		rc = udp_sync_send_iov_ref_plain(ctx, be, &refs[i]);
		if (rc < 0)
			break;
		completed++;
	}

account:
	__atomic_fetch_add(&be->stats.tx_completed, completed,
			   __ATOMIC_RELAXED);
	if (rc < 0)
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
	return rc;
}

static int udp_sync_send_iov_ref_batch(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count)
{
	struct mmsghdr messages[UIB_URING_BATCH_MAX];
	struct uib_udp_backend *be = ctx->backend;
	struct iovec *vectors;
	size_t vector_count;
	size_t vector_offset;
	size_t batch;
	size_t offset;
	size_t i;
	int sent;
	int rc;

	if (be->flow_count && !be->test_drop_tx)
		return udp_flow_send_iov_ref_batch(ctx, refs, count);
	vector_count = 0;
	for (i = 0; i < count; i++) {
		rc = udp_iov_ref_validate(&refs[i]);
		if (rc < 0 || SIZE_MAX - vector_count < refs[i].iov_count) {
			if (rc >= 0)
				rc = -EOVERFLOW;
			for (i = 0; i < count; i++) {
				if (refs[i].release)
					refs[i].release(refs[i].opaque);
			}
			return rc;
		}
		vector_count += refs[i].iov_count;
	}
	if (be->raw_mode || be->test_drop_tx)
		return udp_sync_send_iov_ref_fallback(ctx, refs, count);
	if (__atomic_load_n(&be->gso_enabled, __ATOMIC_ACQUIRE)) {
		offset = 0;
		while (offset < count) {
			batch = udp_iov_ref_run(ctx, be, refs, offset, count);
			if (batch > 1)
				rc = udp_sync_send_iov_ref_gso(ctx,
							       &refs[offset], batch);
			else
				rc = udp_sync_send_iov_ref_one(ctx, &refs[offset]);
			for (i = 0; i < batch; i++)
				refs[offset + i].release(refs[offset + i].opaque);
			offset += batch;
			if (rc < 0) {
				for (i = offset; i < count; i++)
					refs[i].release(refs[i].opaque);
				return rc;
			}
		}
		return 0;
	}
	if (ctx->zerocopy.mode != UIB_ZEROCOPY_OFF) {
		for (i = 0; i < count; i++) {
			rc = udp_sync_send_iov_ref_one(ctx, &refs[i]);
			refs[i].release(refs[i].opaque);
			if (rc < 0) {
				for (i++; i < count; i++)
					refs[i].release(refs[i].opaque);
				return rc;
			}
		}
		return 0;
	}
	vectors = malloc((vector_count ? vector_count : 1) *
			 sizeof(*vectors));
	if (!vectors) {
		for (i = 0; i < count; i++)
			refs[i].release(refs[i].opaque);
		return -ENOMEM;
	}
	offset = 0;
	vector_offset = 0;
	while (offset < count) {
		batch = count - offset;
		if (batch > UIB_URING_BATCH_MAX)
			batch = UIB_URING_BATCH_MAX;
		memset(messages, 0, batch * sizeof(*messages));
		for (i = 0; i < batch; i++) {
			memcpy(&vectors[vector_offset], refs[offset + i].iov,
			       refs[offset + i].iov_count * sizeof(*vectors));
			messages[i].msg_hdr.msg_name =
				(void *)refs[offset + i].dst;
			messages[i].msg_hdr.msg_namelen =
				sizeof(*refs[offset + i].dst);
			messages[i].msg_hdr.msg_iov = &vectors[vector_offset];
			messages[i].msg_hdr.msg_iovlen =
				refs[offset + i].iov_count;
			vector_offset += refs[offset + i].iov_count;
		}
		sent = sendmmsg(be->fd, messages, (unsigned int)batch,
				MSG_NOSIGNAL);
		if (sent < 0) {
			rc = -errno;
			sent = 0;
		} else if ((size_t)sent != batch) {
			rc = -EIO;
		} else {
			rc = 0;
		}
		__atomic_fetch_add(&be->stats.tx_submitted, batch,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&be->stats.tx_completed, (size_t)sent,
				   __ATOMIC_RELAXED);
		if (rc < 0)
			__atomic_fetch_add(&be->stats.tx_errors, 1,
					   __ATOMIC_RELAXED);
		for (i = 0; i < batch; i++)
			refs[offset + i].release(refs[offset + i].opaque);
		for (i = 0; i < (size_t)sent; i++)
			uib_zerocopy_account_copy(ctx, refs[offset + i].len);
		offset += batch;
		if (rc < 0) {
			for (i = offset; i < count; i++)
				refs[i].release(refs[i].opaque);
			free(vectors);
			return rc;
		}
	}
	free(vectors);
	return 0;
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
