#ifndef UIB_CONTEXT_H
#define UIB_CONTEXT_H

#include <stdint.h>
#include <pthread.h>
#include <netinet/in.h>

#include "uib/transport_ops.h"

struct uib_qp;

struct uib_context {
	enum uib_transport_mode         mode;
	const struct uib_transport_ops *tops;
	void                           *backend;

	char                bind_ip[64];
	uint16_t            bind_port;
	struct sockaddr_in  local_addr;

	pthread_mutex_t     qp_lock;
	struct uib_qp     **qp_table;
	uint32_t            qp_table_size;
	uint32_t            qp_next_hint;
};

#define UIB_DEFAULT_QP_TABLE 256
#define UIB_RECV_BUF_LEN     (64 * 1024)

struct uib_context *uib_open_device(const char *bind_ip, uint16_t bind_port);
int uib_close_device(struct uib_context *ctx);

uint32_t uib_register_qp(struct uib_context *ctx, struct uib_qp *qp);
void     uib_unregister_qp(struct uib_context *ctx, struct uib_qp *qp);
struct uib_qp *uib_lookup_qp(struct uib_context *ctx, uint32_t qpn);

#endif
