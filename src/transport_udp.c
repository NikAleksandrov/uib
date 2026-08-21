/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <liburing.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/env.h"
#include "uib/protocol.h"
#include "uib/afxdp_frame.h"

#define UIB_URING_DEPTH              256
#define UIB_URING_BATCH_DEFAULT      64
#define UIB_URING_BATCH_MAX          256
#define UIB_URING_RX_BUFFERS         128
#define UIB_URING_RX_BUFFER_GROUP    7
#define UIB_URING_TX_BATCH_DEFAULT   64
#define UIB_URING_TX_LIMIT           4096
#define UIB_SOCKET_BUFFER_SIZE       (4 * 1024 * 1024)

enum uib_udp_op_type {
	UIB_UDP_OP_RX_MULTISHOT = 0,
	UIB_UDP_OP_RX_ONESHOT,
	UIB_UDP_OP_TX,
	UIB_UDP_OP_TIMER,
	UIB_UDP_OP_WAKE,
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
	int                armed;
};

struct uib_udp_tx {
	struct uib_udp_op  op;
	struct uib_udp_tx *inflight_next;
	struct uib_udp_tx *inflight_prev;
	struct uib_udp_tx *queue_next;
	struct sockaddr_in destination;
	struct iovec       iov;
	struct msghdr      message;
	void              *data;
	size_t             length;
	int                zerocopy;
};

struct uib_udp_backend {
	struct uib_udp_tx       *tx_inflight;
	struct uib_udp_tx       *tx_tail;
	struct uib_udp_tx       *tx_head;
	struct io_uring_buf_ring *buffer_ring;
	struct uib_udp_rx_slot  *rx_slots;
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
	pthread_mutex_t          tx_lock;
	pthread_t                worker;
	void                    *rx_buffers;
	size_t                   rx_buffer_len;
	size_t                   tx_count;
	unsigned int             rx_batch;
	unsigned int             tx_batch;
	unsigned int             test_drop_tx;
	uint32_t                 zerocopy_sequence;
	int                      zerocopy_enabled;
	int                      multishot_required;
	int                      multishot_armed;
	int                      timer_armed;
	int                      wake_armed;
	int                      worker_run;
	int                      stop_requested;
	int                      accepting;
	int                      init_done;
	int                      init_rc;
	int                      raw_mode;
	int                      wake_fd;
	int                      fd;
};

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

static int raw_decode(struct uib_context *ctx, void *buffer, size_t frame_len,
		      struct sockaddr_in *source, void **rdma_out,
		      size_t *rdma_len_out)
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
	if (ctx->pops->wire_decode &&
	    ctx->pops->wire_decode(&meta, rdma, rdma_len) < 0)
		return -1;

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
			   const struct sockaddr_in *source)
{
	struct uib_udp_backend *be = ctx->backend;
	struct sockaddr_in decoded_source = *source;
	void *rdma = buffer;
	size_t rdma_len = length;

	if (be->raw_mode &&
	    raw_decode(ctx, buffer, length, &decoded_source, &rdma,
		       &rdma_len) < 0)
		return;
	uib_transport_deliver(ctx, &decoded_source, rdma, rdma_len);
	__atomic_fetch_add(&be->stats.rx_packets, 1, __ATOMIC_RELAXED);
}

static struct io_uring_sqe *get_sqe(struct uib_udp_backend *be)
{
	return io_uring_get_sqe(&be->ring);
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
		slot->message.msg_iov = &slot->iov;
		slot->message.msg_iovlen = 1;
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

static void free_tx(struct uib_udp_tx *tx)
{
	free(tx->data);
	free(tx);
}

static struct uib_udp_tx *dequeue_tx(struct uib_udp_backend *be)
{
	struct uib_udp_tx *tx;

	pthread_mutex_lock(&be->tx_lock);
	tx = be->tx_head;
	if (tx) {
		be->tx_head = tx->queue_next;
		if (!be->tx_head)
			be->tx_tail = NULL;
		be->tx_count--;
		tx->queue_next = NULL;
	}
	pthread_mutex_unlock(&be->tx_lock);
	return tx;
}

static void requeue_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx)
{
	pthread_mutex_lock(&be->tx_lock);
	tx->queue_next = be->tx_head;
	be->tx_head = tx;
	if (!be->tx_tail)
		be->tx_tail = tx;
	be->tx_count++;
	pthread_mutex_unlock(&be->tx_lock);
}

static void submit_tx(struct uib_udp_backend *be)
{
	struct io_uring_sqe *sqe;
	struct uib_udp_tx *tx;
	unsigned int submitted = 0;

	while (submitted < be->tx_batch) {
		tx = dequeue_tx(be);
		if (!tx)
			return;
		sqe = get_sqe(be);
		if (!sqe) {
			requeue_tx(be, tx);
			return;
		}
#ifdef UIB_HAVE_ZEROCOPY
		if (tx->zerocopy)
			io_uring_prep_sendmsg_zc(sqe, be->fd, &tx->message,
						   MSG_NOSIGNAL);
		else
#endif
			io_uring_prep_sendmsg(sqe, be->fd, &tx->message,
					      MSG_NOSIGNAL);
		io_uring_sqe_set_data(sqe, &tx->op);
		inflight_add(be, tx);
		__atomic_fetch_add(&be->stats.tx_submitted, 1,
				   __ATOMIC_RELAXED);
		submitted++;
	}
}

static int tx_drained(struct uib_udp_backend *be)
{
	int drained;

	pthread_mutex_lock(&be->tx_lock);
	drained = !be->tx_head && !be->tx_inflight;
	pthread_mutex_unlock(&be->tx_lock);
	return drained;
}

static void complete_tx(struct uib_context *ctx, struct uib_udp_tx *tx,
			const struct io_uring_cqe *cqe)
{
	struct uib_udp_backend *be = ctx->backend;

	if (cqe->flags & IORING_CQE_F_NOTIF) {
		__atomic_fetch_add(&ctx->zerocopy.completions, 1,
				   __ATOMIC_RELAXED);
		inflight_remove(be, tx);
		free_tx(tx);
		return;
	}
	if (cqe->res < 0) {
#ifdef UIB_HAVE_ZEROCOPY
		if (tx->zerocopy && ctx->zerocopy.mode != UIB_ZEROCOPY_REQUIRED &&
		    (cqe->res == -EINVAL || cqe->res == -EOPNOTSUPP)) {
			tx->zerocopy = 0;
			inflight_remove(be, tx);
			requeue_tx(be, tx);
			return;
		}
#endif
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&ctx->zerocopy.errors, 1, __ATOMIC_RELAXED);
		inflight_remove(be, tx);
		free_tx(tx);
		return;
	}
	if ((size_t)cqe->res != tx->length) {
		__atomic_fetch_add(&be->stats.tx_errors, 1, __ATOMIC_RELAXED);
		inflight_remove(be, tx);
		free_tx(tx);
		return;
	}
	__atomic_fetch_add(&be->stats.tx_completed, 1, __ATOMIC_RELAXED);
	if (tx->zerocopy) {
		uib_zerocopy_account_direct(ctx, tx->length,
					    UIB_ZEROCOPY_CAP_MSG);
		if (cqe->flags & IORING_CQE_F_MORE)
			return;
	} else {
		uib_zerocopy_account_copy(ctx, tx->length);
	}
	inflight_remove(be, tx);
	free_tx(tx);
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
	deliver_packet(ctx, payload, output->payloadlen, &source);

recycle:
	recycle_multishot_buffer(be, id);
}

static void complete_oneshot(struct uib_context *ctx,
			     struct uib_udp_rx_slot *slot,
			     const struct io_uring_cqe *cqe)
{
	slot->armed = 0;
	if (cqe->res > 0)
		deliver_packet(ctx, slot->buffer, (size_t)cqe->res,
			       &slot->source);
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
		free_tx(tx);
		tx = next;
	}
	be->tx_inflight = NULL;
	tx = be->tx_head;
	while (tx) {
		next = tx->queue_next;
		free_tx(tx);
		tx = next;
	}
	be->tx_head = NULL;
	be->tx_tail = NULL;
}

static void *worker_loop(void *argument)
{
	struct io_uring_cqe *cqes[UIB_URING_BATCH_MAX];
	struct uib_context *ctx = argument;
	struct uib_udp_backend *be = ctx->backend;
	unsigned int count;
	int rc;

	rc = io_uring_queue_init(UIB_URING_DEPTH, &be->ring, 0);
	if (rc < 0) {
		signal_worker_init(be, rc);
		return NULL;
	}
	rc = setup_rx(be);
	if (rc < 0) {
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
		submit_tx(be);
		rc = io_uring_submit_and_wait(&be->ring, 1);
		if (rc < 0 && rc != -EINTR)
			break;
		count = io_uring_peek_batch_cqe(&be->ring, cqes,
						be->rx_batch);
		if (count)
			process_completions(ctx, cqes, count);
	}

	cleanup_multishot(be);
	cleanup_batched_rx(be);
	io_uring_queue_exit(&be->ring);
	free_all_tx(be);
	__atomic_store_n(&be->worker_run, 0, __ATOMIC_RELEASE);
	return NULL;
}

static int udp_probe(void)
{
	return 1;
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

static int setup_socket(struct uib_context *ctx, struct uib_udp_backend *be)
{
	const char *udp_mode = getenv(UIB_ENV_UDP_MODE);
	int socket_buffer = UIB_SOCKET_BUFFER_SIZE;
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
	{
		int one = 1;

		setsockopt(be->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
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
	if (be->fd >= 0)
		close(be->fd);
	if (be->wake_fd >= 0)
		close(be->wake_fd);
	pthread_cond_destroy(&be->init_cond);
	pthread_mutex_destroy(&be->init_lock);
	pthread_mutex_destroy(&be->tx_lock);
	free(be);
}

static int udp_init_common(struct uib_context *ctx, int asynchronous)
{
	const char *drop_tx = getenv(UIB_ENV_TEST_DROP_TX);
	struct uib_udp_backend *be;
	int thread_rc;
	int rc;

	be = calloc(1, sizeof(*be));
	if (!be)
		return -ENOMEM;
	be->fd = -1;
	be->wake_fd = -1;
	pthread_mutex_init(&be->tx_lock, NULL);
	pthread_mutex_init(&be->init_lock, NULL);
	pthread_cond_init(&be->init_cond, NULL);
	be->rx_batch = UIB_URING_BATCH_DEFAULT;
	if (asynchronous)
		be->tx_batch = UIB_URING_TX_BATCH_DEFAULT;
	rc = parse_batch_size(UIB_ENV_URING_RX_BATCH, &be->rx_batch);
	if (rc < 0)
		goto error;
	if (asynchronous) {
		rc = parse_batch_size(UIB_ENV_URING_TX_BATCH, &be->tx_batch);
		if (rc < 0)
			goto error;
	}
	be->multishot_op.type = UIB_UDP_OP_RX_MULTISHOT;
	be->timer_op.type = UIB_UDP_OP_TIMER;
	be->wake_op.type = UIB_UDP_OP_WAKE;
	if (drop_tx)
		be->test_drop_tx = (unsigned int)strtoul(drop_tx, NULL, 10);
	ctx->backend = be;
	rc = setup_socket(ctx, be);
	if (rc < 0)
		goto error;
	be->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (be->wake_fd < 0) {
		rc = -errno;
		goto error;
	}
	__atomic_store_n(&be->worker_run, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&be->accepting, 1, __ATOMIC_RELEASE);
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

static int build_raw_packet(struct uib_context *ctx,
			    const struct sockaddr_in *destination,
			    const void *buffer, size_t length,
			    uint16_t entropy, struct uib_udp_tx *tx)
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
	tx->data = malloc(packet_len);
	if (!tx->data)
		return -ENOMEM;
	tx->length = packet_len;
	ip = tx->data;
	udp = (struct udphdr *)(ip + 1);
	memset(ip, 0, sizeof(*ip));
	ip->version = 4;
	ip->ihl = 5;
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
	memcpy(udp + 1, buffer, length);
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

static int enqueue_tx(struct uib_udp_backend *be, struct uib_udp_tx *tx)
{
	uint64_t wake = 1;

	pthread_mutex_lock(&be->tx_lock);
	if (!__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&be->accepting, __ATOMIC_ACQUIRE)) {
		pthread_mutex_unlock(&be->tx_lock);
		return -ESHUTDOWN;
	}
	if (be->tx_count >= UIB_URING_TX_LIMIT) {
		pthread_mutex_unlock(&be->tx_lock);
		return -EAGAIN;
	}
	tx->queue_next = NULL;
	if (be->tx_tail)
		be->tx_tail->queue_next = tx;
	else
		be->tx_head = tx;
	be->tx_tail = tx;
	be->tx_count++;
	pthread_mutex_unlock(&be->tx_lock);
	__atomic_fetch_add(&be->stats.tx_queued, 1, __ATOMIC_RELAXED);
	if (write(be->wake_fd, &wake, sizeof(wake)) < 0 && errno != EAGAIN)
		return -errno;
	return 0;
}

static int udp_send_entropy(struct uib_context *ctx,
			    const struct sockaddr_in *destination,
			    const void *buffer, size_t length,
			    uint16_t entropy)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_tx *tx;
	int rc;

	if (test_drop_packet(be))
		return 0;
	tx = calloc(1, sizeof(*tx));
	if (!tx)
		return -ENOMEM;
	tx->op.type = UIB_UDP_OP_TX;
	tx->destination = *destination;
	if (be->raw_mode) {
		rc = build_raw_packet(ctx, destination, buffer, length, entropy, tx);
		if (rc < 0)
			goto error;
	} else {
		tx->data = malloc(length);
		if (!tx->data) {
			rc = -ENOMEM;
			goto error;
		}
		memcpy(tx->data, buffer, length);
		tx->length = length;
		if (be->zerocopy_enabled &&
		    ctx->zerocopy.mode != UIB_ZEROCOPY_OFF &&
		    length >= ctx->zerocopy.threshold)
			tx->zerocopy = 1;
	}
	tx->iov.iov_base = tx->data;
	tx->iov.iov_len = tx->length;
	tx->message.msg_name = &tx->destination;
	tx->message.msg_namelen = sizeof(tx->destination);
	tx->message.msg_iov = &tx->iov;
	tx->message.msg_iovlen = 1;
	rc = enqueue_tx(be, tx);
	if (rc < 0)
		goto error;
	return 0;

error:
	free_tx(tx);
	return rc;
}

static int udp_send(struct uib_context *ctx,
		    const struct sockaddr_in *destination,
		    const void *buffer, size_t length)
{
	return udp_send_entropy(ctx, destination, buffer, length, 0);
}

static int udp_sync_send_entropy(struct uib_context *ctx,
				 const struct sockaddr_in *destination,
				 const void *buffer, size_t length,
				 uint16_t entropy)
{
	struct uib_udp_backend *be = ctx->backend;
	struct uib_udp_tx *tx = NULL;
	struct msghdr message;
	struct iovec iov;
	ssize_t sent;
	int rc;

	if (test_drop_packet(be))
		return 0;
	if (be->raw_mode) {
		tx = calloc(1, sizeof(*tx));
		if (!tx)
			return -ENOMEM;
		rc = build_raw_packet(ctx, destination, buffer, length, entropy,
				      tx);
		if (rc < 0)
			goto out;
		sent = sendto(be->fd, tx->data, tx->length, MSG_NOSIGNAL,
			      (const struct sockaddr *)destination,
			      sizeof(*destination));
		if (sent >= 0)
			uib_zerocopy_account_copy(ctx, (size_t)sent);
		else
			sent = -errno;
		length = tx->length;
	} else {
		memset(&message, 0, sizeof(message));
		iov.iov_base = (void *)buffer;
		iov.iov_len = length;
		message.msg_name = (void *)destination;
		message.msg_namelen = sizeof(*destination);
		message.msg_iov = &iov;
		message.msg_iovlen = 1;
		sent = uib_zerocopy_sendmsg(ctx, be->fd, &message,
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
		free_tx(tx);
	return rc;
}

static int udp_sync_send(struct uib_context *ctx,
			 const struct sockaddr_in *destination,
			 const void *buffer, size_t length)
{
	return udp_sync_send_entropy(ctx, destination, buffer, length, 0);
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
	stats->rx_packets = __atomic_load_n(&be->stats.rx_packets,
					   __ATOMIC_RELAXED);
	stats->rx_completion_batches =
		__atomic_load_n(&be->stats.rx_completion_batches,
				__ATOMIC_RELAXED);
	stats->rx_largest_batch = __atomic_load_n(&be->stats.rx_largest_batch,
						  __ATOMIC_RELAXED);
	stats->rx_mode = be->stats.rx_mode;
	stats->rx_batch = be->rx_batch;
	stats->tx_batch = be->tx_batch;
	return 0;
}

const struct uib_transport_ops uib_udp_ops = {
	.name = "udp-io-uring",
	.probe = udp_probe,
	.init = udp_init,
	.fini = udp_fini,
	.send = udp_send,
	.send_entropy = udp_send_entropy,
	.get_stats = udp_get_stats,
};

const struct uib_transport_ops uib_udp_sync_ops = {
	.name = "udp-sync",
	.probe = udp_probe,
	.init = udp_sync_init,
	.fini = udp_fini,
	.send = udp_sync_send,
	.send_entropy = udp_sync_send_entropy,
	.get_stats = udp_get_stats,
};
