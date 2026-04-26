#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "uib/context.h"
#include "uib/qp.h"
#include "uib/transport.h"
#include "uib/bth.h"

struct uib_context *uib_open_device(const char *bind_ip, uint16_t bind_port)
{
	struct uib_context *ctx = calloc(1, sizeof(*ctx));

	if (!ctx)
		return NULL;

	ctx->mode      = uib_transport_mode_from_env();
	ctx->bind_port = bind_port;
	if (bind_ip && *bind_ip)
		snprintf(ctx->bind_ip, sizeof(ctx->bind_ip), "%s", bind_ip);

	pthread_mutex_init(&ctx->qp_lock, NULL);
	ctx->qp_table_size = UIB_DEFAULT_QP_TABLE;
	ctx->qp_table = calloc(ctx->qp_table_size, sizeof(*ctx->qp_table));
	if (!ctx->qp_table)
		goto err_lock;
	ctx->qp_next_hint = 1;

	if (uib_transport_init(ctx) < 0)
		goto err_table;

	return ctx;

err_table:
	free(ctx->qp_table);
err_lock:
	pthread_mutex_destroy(&ctx->qp_lock);
	free(ctx);
	return NULL;
}

int uib_close_device(struct uib_context *ctx)
{
	if (!ctx)
		return -EINVAL;
	uib_transport_fini(ctx);
	pthread_mutex_destroy(&ctx->qp_lock);
	free(ctx->qp_table);
	free(ctx);
	return 0;
}

uint32_t uib_register_qp(struct uib_context *ctx, struct uib_qp *qp)
{
	pthread_mutex_lock(&ctx->qp_lock);
	uint32_t i;

	for (i = ctx->qp_next_hint; i < ctx->qp_table_size; i++) {
		if (!ctx->qp_table[i])
			goto found;
	}
	for (i = 1; i < ctx->qp_next_hint; i++) {
		if (!ctx->qp_table[i])
			goto found;
	}
	uint32_t old_size = ctx->qp_table_size;
	uint32_t new_size = old_size * 2;
	struct uib_qp **nt = realloc(ctx->qp_table, new_size * sizeof(*nt));

	if (!nt) {
		pthread_mutex_unlock(&ctx->qp_lock);
		return 0;
	}
	memset(nt + old_size, 0, (new_size - old_size) * sizeof(*nt));
	ctx->qp_table      = nt;
	ctx->qp_table_size = new_size;
	i = old_size;
found:
	ctx->qp_table[i] = qp;
	ctx->qp_next_hint = i + 1;
	pthread_mutex_unlock(&ctx->qp_lock);
	return i;
}

void uib_unregister_qp(struct uib_context *ctx, struct uib_qp *qp)
{
	if (!qp || !qp->qp_num || qp->qp_num >= ctx->qp_table_size)
		return;
	pthread_mutex_lock(&ctx->qp_lock);
	if (ctx->qp_table[qp->qp_num] == qp)
		ctx->qp_table[qp->qp_num] = NULL;
	if (qp->qp_num < ctx->qp_next_hint)
		ctx->qp_next_hint = qp->qp_num;
	pthread_mutex_unlock(&ctx->qp_lock);
}

struct uib_qp *uib_lookup_qp(struct uib_context *ctx, uint32_t qpn)
{
	if (qpn == 0 || qpn >= ctx->qp_table_size)
		return NULL;
	pthread_mutex_lock(&ctx->qp_lock);
	struct uib_qp *qp = ctx->qp_table[qpn];

	pthread_mutex_unlock(&ctx->qp_lock);
	return qp;
}
