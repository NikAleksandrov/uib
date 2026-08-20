#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/qp.h"
#include "uib/protocol.h"
#include "utils.h"

void uib_transport_deliver(struct uib_context *ctx,
			   const struct sockaddr_in *src, const void *buf,
			   size_t len)
{
	struct uib_qp *qp;
	uint32_t dqpn;

	pthread_rwlock_rdlock(&ctx->qp_io_lock);
	if (!ctx->pops || !ctx->pops->packet_qpn ||
	    len < ctx->pops->min_packet ||
	    ctx->pops->packet_qpn(buf, len, &dqpn) < 0) {
		pthread_rwlock_unlock(&ctx->qp_io_lock);
		return;
	}
	qp = uib_lookup_qp_locked(ctx, dqpn);

	if (!qp) {
		pthread_rwlock_unlock(&ctx->qp_io_lock);
		return;
	}
	uib_qp_handle_packet(qp, src, buf, len);
	pthread_rwlock_unlock(&ctx->qp_io_lock);
}

void uib_transport_retransmit_walk(struct uib_context *ctx)
{
	uint64_t now = uib_now_ns();
	struct uib_qp *qp;

	pthread_rwlock_rdlock(&ctx->qp_io_lock);
	while ((qp = uib_retry_pop_due(ctx, now)) != NULL)
		uib_qp_retransmit_tick(qp, now);
	pthread_rwlock_unlock(&ctx->qp_io_lock);
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

enum uib_transport_mode uib_transport_mode_from_env(void)
{
	const char *s = getenv("UIB_TRANSPORT_MODE");

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

void uib_transport_fini(struct uib_context *ctx)
{
	if (ctx && ctx->tops && ctx->tops->fini)
		ctx->tops->fini(ctx);
}

int uib_transport_send(struct uib_context *ctx, const struct sockaddr_in *dst,
		       const void *buf, size_t len)
{
	return ctx->tops->send(ctx, dst, buf, len);
}

int uib_transport_send_entropy(struct uib_context *ctx,
			       const struct sockaddr_in *dst,
			       const void *buf, size_t len, uint16_t entropy)
{
	if (ctx->tops->send_entropy)
		return ctx->tops->send_entropy(ctx, dst, buf, len, entropy);
	return ctx->tops->send(ctx, dst, buf, len);
}

int uib_transport_get_stats(struct uib_context *ctx,
			    struct uib_transport_stats *stats)
{
	if (!ctx || !stats)
		return -EINVAL;
	if (!ctx->tops || !ctx->tops->get_stats)
		return -EOPNOTSUPP;
	return ctx->tops->get_stats(ctx, stats);
}
