/* SPDX-License-Identifier: Apache-2.0 */
#include "transport_udp_internal.h"

UIB_UDP_INTERNAL uint16_t message_gro_size(const struct msghdr *message)
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

UIB_UDP_INTERNAL uint8_t message_tos(const struct msghdr *message)
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
		return tos;
	}
	return 0;
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

static uint8_t multishot_tos(struct io_uring_recvmsg_out *output,
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
			return tos;
		}
		control = io_uring_recvmsg_cmsg_nexthdr(output, message,
						      control);
	}
	return 0;
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

UIB_UDP_INTERNAL int zerocopy_retryable(int result)
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
	uint8_t tos;

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
	tos = multishot_tos(output, &be->multishot_message);
	deliver_udp_payload(ctx, payload, output->payloadlen, &source,
			    gro_size, tos);

recycle:
	recycle_multishot_buffer(be, id);
}

static void complete_oneshot(struct uib_context *ctx,
			     struct uib_udp_rx_slot *slot,
			     const struct io_uring_cqe *cqe)
{
	uint16_t gro_size;
	uint8_t tos;

	slot->armed = 0;
	if (cqe->res <= 0)
		return;
	gro_size = message_gro_size(&slot->message);
	tos = message_tos(&slot->message);
	deliver_udp_payload(ctx, slot->buffer, (size_t)cqe->res,
			    &slot->source, gro_size, tos);
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

UIB_UDP_INTERNAL void free_all_tx(struct uib_udp_backend *be)
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

UIB_UDP_INTERNAL void *worker_loop(void *argument)
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
