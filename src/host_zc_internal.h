/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_HOST_ZC_INTERNAL_H
#define UIB_HOST_ZC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#include "uib/transport_ops.h"

struct uib_context;
struct uib_qp;
struct uib_rq_slot;
struct uib_recv_wr;

typedef void (*uib_direct_consume_fn)(void *opaque);

void uib_transport_deliver_direct(struct uib_context *ctx,
				  const struct sockaddr_in *src,
				  const void *buf, size_t len,
				  uib_transport_release_fn release,
				  void *opaque);
int uib_transport_claim_direct(const void *data, size_t len,
			       uib_transport_release_fn *release, void **opaque);
void uib_transport_direct_scope(const void *data, size_t len,
				uib_transport_release_fn release,
				void *release_opaque,
				uib_direct_consume_fn consume,
				void *consume_opaque);
int uib_host_loan_place(struct uib_rq_slot *slot, uint32_t offset,
			const void *data, size_t length);
int uib_qp_scatter_recv(struct uib_qp *qp, struct uib_rq_slot *slot,
			uint32_t msg_offset, const uint8_t *in,
			size_t in_len, size_t *consumed_out);
int uib_post_recv_owned(struct uib_qp *qp, struct uib_recv_wr *request,
			void *private_data,
			void (*private_release)(void *private_data));

#endif
