/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_CONTEXT_H
#define UIB_CONTEXT_H

#include <stdint.h>
#include <pthread.h>
#include <netinet/in.h>

#include "uib/transport_ops.h"
#include "uib/protocol.h"
#include "uib/zerocopy.h"

struct uib_qp;

struct uib_context {
	enum uib_transport_mode         mode;
	const struct uib_transport_ops *tops;
	const struct uib_protocol_ops  *pops;
	void                           *backend;
	void                           *protocol_private;

	char                bind_ip[64];
	uint16_t            bind_port;
	struct sockaddr_in  local_addr;

	pthread_mutex_t     qp_lock;
	pthread_mutex_t     zerocopy_lock;
	pthread_rwlock_t    qp_io_lock;
	struct uib_qp     **qp_table;
	uint32_t           *qp_free_ids;
	struct uib_qp     **retry_heap;
	uint32_t            qp_table_size;
	uint32_t            qp_next_id;
	uint32_t            qp_free_count;
	uint32_t            retry_heap_size;
	struct uib_zerocopy_stats zerocopy;
};

#define UIB_DEFAULT_QP_TABLE 256
#define UIB_RECV_BUF_LEN     (64 * 1024)

struct uib_context *uib_open_device(const char *bind_ip, uint16_t bind_port);
struct uib_context *uib_open_device_protocol(const char *bind_ip,
					     uint16_t bind_port,
					     const char *protocol);
int uib_close_device(struct uib_context *ctx);

uint32_t uib_register_qp(struct uib_context *ctx, struct uib_qp *qp);
void     uib_unregister_qp(struct uib_context *ctx, struct uib_qp *qp);
struct uib_qp *uib_lookup_qp(struct uib_context *ctx, uint32_t qpn);
struct uib_qp *uib_lookup_qp_locked(struct uib_context *ctx, uint32_t qpn);
void uib_qp_retry_schedule(struct uib_qp *qp, uint64_t deadline_ns);
void uib_qp_retry_cancel(struct uib_qp *qp);
struct uib_qp *uib_retry_pop_due(struct uib_context *ctx, uint64_t now_ns);

#endif
