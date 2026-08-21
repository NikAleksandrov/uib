/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_TRANSPORT_OPS_H
#define UIB_TRANSPORT_OPS_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

struct uib_context;
struct uib_mr;
struct uib_transport_stats;

/*
 * Selectable datapath backends. UDP_IO_URING is the default and is always
 * available; the others probe at init and fall back to it when unusable.
 */
enum uib_transport_mode {
	UIB_MODE_UDP_IO_URING = 0,
	UIB_MODE_UDP_SYNC,
	UIB_MODE_UDP_ZC_AFXDP,
	UIB_MODE_TCP_DEVMEM,
};

/* Backends carry opaque protocol packets; MR callbacks are optional. */
struct uib_transport_ops {
	const char *name;
	uint32_t mr_capabilities;
	int  (*probe)(void);
	int  (*init)(struct uib_context *ctx);
	void (*fini)(struct uib_context *ctx);
	int  (*send)(struct uib_context *ctx, const struct sockaddr_in *dst,
		     const void *buf, size_t len);
	int  (*send_entropy)(struct uib_context *ctx,
			     const struct sockaddr_in *dst, const void *buf,
			     size_t len, uint16_t entropy);
	int  (*mr_register)(struct uib_context *ctx, struct uib_mr *mr);
	void (*mr_deregister)(struct uib_context *ctx, struct uib_mr *mr);
	int  (*get_stats)(struct uib_context *ctx,
			  struct uib_transport_stats *stats);
};

extern const struct uib_transport_ops uib_udp_ops;
extern const struct uib_transport_ops uib_udp_sync_ops;

#endif
