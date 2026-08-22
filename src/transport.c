/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <urcu-bp.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/env.h"
#include "uib/qp.h"
#include "uib/protocol.h"
#include "host_zc_internal.h"
#include "utils.h"

struct uib_direct_delivery {
	uib_transport_release_fn release;
	const uint8_t           *data;
	void                    *opaque;
	size_t                   len;
	int                      claimed;
};

static _Thread_local struct uib_direct_delivery *direct_delivery;

/**
 * uib_transport_deliver - Route a received packet to its destination QP.
 * @ctx: UIB device context
 * @src: Source transport address
 * @buf: Received protocol packet
 * @len: Length of the byte range
 * @uib-api: Packet transports
 */
void uib_transport_deliver(struct uib_context *ctx,
			   const struct sockaddr_in *src, const void *buf,
			   size_t len)
{
	struct uib_qp *qp;
	uint32_t dqpn;

	rcu_read_lock();
	if (!ctx->pops || !ctx->pops->packet_qpn ||
	    len < ctx->pops->min_packet ||
	    ctx->pops->packet_qpn(buf, len, &dqpn) < 0) {
		rcu_read_unlock();
		return;
	}
	qp = uib_lookup_qp_rcu(ctx, dqpn);

	if (!qp) {
		rcu_read_unlock();
		return;
	}
	uib_qp_handle_packet(qp, src, buf, len);
	rcu_read_unlock();
}

void uib_transport_deliver_direct(struct uib_context *ctx,
				  const struct sockaddr_in *src,
				  const void *buf, size_t len,
				  uib_transport_release_fn release,
				  void *opaque)
{
	struct uib_direct_delivery delivery;

	delivery.release = release;
	delivery.data = buf;
	delivery.opaque = opaque;
	delivery.len = len;
	delivery.claimed = 0;
	direct_delivery = &delivery;
	uib_transport_deliver(ctx, src, buf, len);
	direct_delivery = NULL;
	if (!delivery.claimed && release)
		release(opaque);
}

void uib_transport_direct_scope(const void *data, size_t len,
				uib_transport_release_fn release,
				void *release_opaque,
				uib_direct_consume_fn consume,
				void *consume_opaque)
{
	struct uib_direct_delivery delivery;

	delivery.release = release;
	delivery.data = data;
	delivery.opaque = release_opaque;
	delivery.len = len;
	delivery.claimed = 0;
	direct_delivery = &delivery;
	consume(consume_opaque);
	direct_delivery = NULL;
	if (!delivery.claimed && release)
		release(release_opaque);
}

int uib_transport_claim_direct(const void *data, size_t len,
			       uib_transport_release_fn *release, void **opaque)
{
	const uint8_t *end;
	const uint8_t *start;

	if (!direct_delivery || direct_delivery->claimed || !release || !opaque)
		return -ENOENT;
	start = data;
	end = start + len;
	if (start < direct_delivery->data || end < start ||
	    end > direct_delivery->data + direct_delivery->len)
		return -ERANGE;
	direct_delivery->claimed = 1;
	*release = direct_delivery->release;
	*opaque = direct_delivery->opaque;
	return 0;
}

/**
 * uib_transport_retransmit_walk - Process every expired retry deadline.
 * @ctx: UIB device context
 * @uib-api: Packet transports
 */
void uib_transport_retransmit_walk(struct uib_context *ctx)
{
	uint64_t now = uib_now_ns();
	struct uib_qp *qp;

	rcu_read_lock();
	while ((qp = uib_retry_pop_due(ctx, now)) != NULL)
		uib_qp_retransmit_tick(qp, now);
	rcu_read_unlock();
}

static const char *mode_name(enum uib_transport_mode m)
{
	switch (m) {
	case UIB_MODE_UDP_IO_URING:
		return "udp-io-uring";
	case UIB_MODE_UDP_SYNC:
		return "udp-sync";
	case UIB_MODE_UDP_ZC_AFXDP:
		return "udp-zc-afxdp";
	case UIB_MODE_TCP_DEVMEM:
		return "tcp-devmem";
	}
	return "?";
}

/**
 * uib_transport_mode_from_env - Select transport mode from the environment.
 * @uib-api: Packet transports
 *
 * Return: The selected transport mode; invalid values fall back to
 * UIB_MODE_UDP_IO_URING.
 */
enum uib_transport_mode uib_transport_mode_from_env(void)
{
	const char *s = getenv(UIB_ENV_TRANSPORT_MODE);

	if (!s || !*s)
		return UIB_MODE_UDP_IO_URING;
	if (!strcmp(s, "udp-io-uring"))
		return UIB_MODE_UDP_IO_URING;
	if (!strcmp(s, "udp-sync") || !strcmp(s, "udp-blocking"))
		return UIB_MODE_UDP_SYNC;
	if (!strcmp(s, "udp-zc-afxdp"))
		return UIB_MODE_UDP_ZC_AFXDP;
	if (!strcmp(s, "tcp-devmem"))
		return UIB_MODE_TCP_DEVMEM;
	return UIB_MODE_UDP_IO_URING;
}

static const struct uib_transport_ops *ops_for_mode(enum uib_transport_mode m)
{
	switch (m) {
	case UIB_MODE_UDP_IO_URING:
		return &uib_udp_ops;
	case UIB_MODE_UDP_SYNC:
		return &uib_udp_sync_ops;
	case UIB_MODE_UDP_ZC_AFXDP:
		return &uib_afxdp_ops;
	case UIB_MODE_TCP_DEVMEM:
		return &uib_tcp_devmem_ops;
	}
	return NULL;
}

/**
 * uib_transport_init - Open the selected transport for a context.
 * @ctx: UIB device context
 * @uib-api: Packet transports
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_transport_init(struct uib_context *ctx)
{
	const struct uib_transport_ops *ops = ops_for_mode(ctx->mode);

	if (!ops || (ops->probe && !ops->probe())) {
		if (ctx->mode != UIB_MODE_UDP_IO_URING)
			fprintf(stderr,
				"uib: transport mode '%s' unavailable, using udp-io-uring\n",
				mode_name(ctx->mode));
		ops = &uib_udp_ops;
		ctx->mode = UIB_MODE_UDP_IO_URING;
	}
	ctx->tops = ops;
	return ops->init(ctx);
}

/**
 * uib_transport_fini - Stop and release a context's transport.
 * @ctx: UIB device context
 * @uib-api: Packet transports
 */
void uib_transport_fini(struct uib_context *ctx)
{
	if (ctx && ctx->tops && ctx->tops->fini)
		ctx->tops->fini(ctx);
}

/**
 * uib_transport_send - Send a packet over the selected transport.
 * @ctx: UIB device context
 * @dst: Destination transport address
 * @buf: Protocol packet to send
 * @len: Length of the byte range
 * @uib-api: Packet transports
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_transport_send(struct uib_context *ctx, const struct sockaddr_in *dst,
		       const void *buf, size_t len)
{
	return ctx->tops->send(ctx, dst, buf, len);
}

/**
 * uib_transport_send_entropy - Send a packet with path-selection entropy.
 * @ctx: UIB device context
 * @dst: Destination transport address
 * @buf: Protocol packet to send
 * @len: Length of the byte range
 * @entropy: Transport entropy value
 * @uib-api: Packet transports
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_transport_send_entropy(struct uib_context *ctx,
			       const struct sockaddr_in *dst,
			       const void *buf, size_t len, uint16_t entropy)
{
	if (ctx->tops->send_entropy)
		return ctx->tops->send_entropy(ctx, dst, buf, len, entropy);
	return ctx->tops->send(ctx, dst, buf, len);
}

/**
 * uib_transport_send_ref - Send while retaining a caller-owned buffer.
 * @ctx: UIB device context
 * @dst: Destination transport address
 * @buf: Referenced protocol packet to send
 * @len: Length of the byte range
 * @release: Callback invoked when the transport releases the buffer
 * @opaque: Caller-defined release callback argument
 * @uib-api: Packet transports
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_transport_send_ref(struct uib_context *ctx,
			   const struct sockaddr_in *dst, const void *buf,
			   size_t len, uib_transport_release_fn release,
			   void *opaque)
{
	int rc;

	if (ctx->tops->send_ref)
		return ctx->tops->send_ref(ctx, dst, buf, len, release, opaque);
	rc = ctx->tops->send(ctx, dst, buf, len);
	release(opaque);
	return rc;
}

/**
 * uib_transport_send_ref_batch - Send retained buffers as one transport batch.
 * @ctx: UIB device context
 * @refs: Array of referenced send requests
 * @count: Number of array entries
 * @uib-api: Packet transports
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_transport_send_ref_batch(struct uib_context *ctx,
				 const struct uib_transport_ref *refs,
				 size_t count)
{
	size_t i;
	int rc;

	if (!count)
		return 0;
	if (ctx->tops->send_ref_batch)
		return ctx->tops->send_ref_batch(ctx, refs, count);
	for (i = 0; i < count; i++) {
		if (refs[i].entropy)
			rc = uib_transport_send_entropy(ctx, refs[i].dst,
						refs[i].buf, refs[i].len,
						refs[i].entropy);
		else
			rc = ctx->tops->send(ctx, refs[i].dst, refs[i].buf,
					     refs[i].len);
		refs[i].release(refs[i].opaque);
		if (rc < 0) {
			for (i++; i < count; i++)
				refs[i].release(refs[i].opaque);
			return rc;
		}
	}
	return 0;
}

/**
 * uib_transport_get_stats - Read transport, batching, and packet counters.
 * @ctx: UIB device context
 * @stats: Statistics structure to fill
 * @uib-api: Packet transports
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_transport_get_stats(struct uib_context *ctx,
			    struct uib_transport_stats *stats)
{
	if (!ctx || !stats)
		return -EINVAL;
	if (!ctx->tops || !ctx->tops->get_stats)
		return -EOPNOTSUPP;
	return ctx->tops->get_stats(ctx, stats);
}
