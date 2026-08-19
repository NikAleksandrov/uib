#ifndef UIB_EXAMPLES_DIRECT_COMMON_H
#define UIB_EXAMPLES_DIRECT_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "uib/uib.h"

struct direct_endpoint {
	struct uib_context *context;
	struct uib_pd *pd;
	struct uib_cq *cq;
	struct uib_qp *qp;
	struct uib_mr *mr;
	uint8_t *buffer;
	size_t buffer_length;
};

int direct_endpoint_open(struct direct_endpoint *endpoint, const char *protocol,
			 const char *address, uint16_t port, size_t buffer_length,
			 uint32_t queue_depth);
int direct_endpoint_connect(struct direct_endpoint *local,
			    const struct direct_endpoint *remote,
			    uint32_t path_mtu);
void direct_endpoint_close(struct direct_endpoint *endpoint);
int direct_wait_completion(struct uib_cq *cq, struct uib_wc *completion,
			   uint64_t timeout_ns);
int direct_post_receive(struct direct_endpoint *endpoint, uint64_t wr_id,
			void *buffer, uint32_t length);
int direct_post_send(struct direct_endpoint *endpoint, uint64_t wr_id,
			     enum uib_wr_opcode opcode, void *buffer,
			     uint32_t length, uint64_t remote_address,
			     uint32_t remote_key);

#endif
