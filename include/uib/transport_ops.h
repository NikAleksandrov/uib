#ifndef UIB_TRANSPORT_OPS_H
#define UIB_TRANSPORT_OPS_H

#include <stddef.h>
#include <netinet/in.h>

struct uib_context;
struct uib_mr;

enum uib_transport_mode {
	UIB_MODE_UDP_BLOCKING = 0,
	UIB_MODE_UDP_ZC_AFXDP,
	UIB_MODE_TCP_DEVMEM,
};

struct uib_transport_ops {
	const char *name;
	int  (*probe)(void);
	int  (*init)(struct uib_context *ctx);
	void (*fini)(struct uib_context *ctx);
	int  (*send)(struct uib_context *ctx, const struct sockaddr_in *dst,
		     const void *buf, size_t len);
	int  (*mr_register)(struct uib_context *ctx, struct uib_mr *mr);
	void (*mr_deregister)(struct uib_context *ctx, struct uib_mr *mr);
};

extern const struct uib_transport_ops uib_udp_ops;

#endif
