#ifndef UIB_TRANSPORT_H
#define UIB_TRANSPORT_H

#include <stddef.h>
#include <netinet/in.h>

#include "uib/transport_ops.h"

struct uib_context;

extern const struct uib_transport_ops uib_afxdp_ops;
extern const struct uib_transport_ops uib_tcp_devmem_ops;

#define UIB_UD_TAG_RECV  1
#define UIB_UD_TAG_SEND  2
#define UIB_UD_TAG_TIMER 3

#define UIB_TIMER_TICK_MS 50

int uib_transport_init(struct uib_context *ctx);
void uib_transport_fini(struct uib_context *ctx);
int uib_transport_send(struct uib_context *ctx, const struct sockaddr_in *dst,
		       const void *buf, size_t len);
int uib_transport_send_entropy(struct uib_context *ctx,
			       const struct sockaddr_in *dst,
			       const void *buf, size_t len, uint16_t entropy);

void uib_transport_deliver(struct uib_context *ctx, const struct sockaddr_in *src,
			   const void *buf, size_t len);
void uib_transport_retransmit_walk(struct uib_context *ctx);

enum uib_transport_mode uib_transport_mode_from_env(void);

#endif
