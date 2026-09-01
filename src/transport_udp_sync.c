/* SPDX-License-Identifier: Apache-2.0 */
#include "transport_udp_internal.h"

#define UIB_UDP_SYNC_IOV_STACK 512U

UIB_UDP_INTERNAL int udp_sync_send_entropy(struct uib_context *ctx,
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

UIB_UDP_INTERNAL int udp_sync_send(struct uib_context *ctx,
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

UIB_UDP_INTERNAL int udp_sync_send_iov_ref_batch(
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

UIB_UDP_INTERNAL int udp_sync_send_ref_batch(struct uib_context *ctx,
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
	/* GSO takes precedence over automatic zero copy. */
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

static int udp_sync_send_iov_ref_sendmmsg(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count, size_t vector_count)
{
	struct mmsghdr messages[UIB_URING_BATCH_MAX];
	struct iovec stack_vectors[UIB_UDP_SYNC_IOV_STACK];
	struct uib_udp_backend *be;
	struct iovec *vectors;
	size_t vector_offset;
	size_t batch;
	size_t offset;
	size_t i;
	int sent;
	int rc;

	be = ctx->backend;
	rc = 0;
	vectors = stack_vectors;
	if (vector_count > UIB_UDP_SYNC_IOV_STACK) {
		vectors = malloc(vector_count * sizeof(*vectors));
		if (!vectors) {
			for (i = 0; i < count; i++)
				refs[i].release(refs[i].opaque);
			return -ENOMEM;
		}
	}
	offset = 0;
	vector_offset = 0;
	while (offset < count) {
		batch = min_t(size_t, count - offset, UIB_URING_BATCH_MAX);
		memset(messages, 0, batch * sizeof(*messages));
		for (i = 0; i < batch; i++) {
			memcpy(&vectors[vector_offset], refs[offset + i].iov,
			       refs[offset + i].iov_count * sizeof(*vectors));
			messages[i].msg_hdr.msg_name = (void *)refs[offset + i].dst;
			messages[i].msg_hdr.msg_namelen =
				sizeof(*refs[offset + i].dst);
			messages[i].msg_hdr.msg_iov = &vectors[vector_offset];
			messages[i].msg_hdr.msg_iovlen = refs[offset + i].iov_count;
			vector_offset += refs[offset + i].iov_count;
		}
		sent = sendmmsg(be->fd, messages, (unsigned int)batch,
				MSG_NOSIGNAL);
		if (sent < 0) {
			rc = -errno;
			sent = 0;
		} else {
			rc = (size_t)sent == batch ? 0 : -EIO;
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
			break;
		}
	}
	if (vectors != stack_vectors)
		free(vectors);
	return rc;
}

UIB_UDP_INTERNAL int udp_sync_send_iov_ref_batch(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count)
{
	struct uib_udp_backend *be = ctx->backend;
	size_t vector_count;
	size_t batch;
	size_t offset;
	size_t i;
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
	return udp_sync_send_iov_ref_sendmmsg(ctx, refs, count, vector_count);
}
