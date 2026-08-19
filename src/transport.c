#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/qp.h"
#include "uib/protocol.h"
#include "utils.h"

void uib_transport_deliver(struct uib_context *ctx, const struct sockaddr_in *src,
			   const void *buf, size_t len)
{
	uint32_t dqpn;

	if (!ctx->pops || !ctx->pops->packet_qpn ||
	    len < ctx->pops->min_packet ||
	    ctx->pops->packet_qpn(buf, len, &dqpn) < 0)
		return;
	struct uib_qp *qp = uib_lookup_qp(ctx, dqpn);

	if (!qp)
		return;
	uib_qp_handle_packet(qp, src, buf, len);
}

void uib_transport_retransmit_walk(struct uib_context *ctx)
{
	uint64_t now = uib_now_ns();
	pthread_mutex_lock(&ctx->qp_lock);
	uint32_t n = ctx->qp_table_size;
	struct uib_qp **snap = malloc(n * sizeof(*snap));

	if (snap)
		memcpy(snap, ctx->qp_table, n * sizeof(*snap));
	pthread_mutex_unlock(&ctx->qp_lock);
	if (!snap)
		return;
	for (uint32_t i = 1; i < n; i++) {
		struct uib_qp *qp = snap[i];

		if (qp && qp->qp_type == UIB_QPT_RC)
			uib_qp_retransmit_tick(qp, now);
	}
	free(snap);
}

static const char *mode_name(enum uib_transport_mode m)
{
	switch (m) {
	case UIB_MODE_UDP_BLOCKING:
		return "udp-blocking";
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
		return UIB_MODE_UDP_BLOCKING;
	if (!strcmp(s, "udp-zc-afxdp"))
		return UIB_MODE_UDP_ZC_AFXDP;
	if (!strcmp(s, "tcp-devmem"))
		return UIB_MODE_TCP_DEVMEM;
	return UIB_MODE_UDP_BLOCKING;
}

static const struct uib_transport_ops *ops_for_mode(enum uib_transport_mode m)
{
	switch (m) {
	case UIB_MODE_UDP_BLOCKING:
		return &uib_udp_ops;
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
		if (ctx->mode != UIB_MODE_UDP_BLOCKING)
			fprintf(stderr,
				"uib: transport mode '%s' unavailable, using udp-blocking\n",
				mode_name(ctx->mode));
		ops = &uib_udp_ops;
		ctx->mode = UIB_MODE_UDP_BLOCKING;
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
