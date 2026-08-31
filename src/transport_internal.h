/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_TRANSPORT_INTERNAL_H
#define UIB_TRANSPORT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

struct uib_context;

void uib_transport_deliver_ecn(struct uib_context *ctx,
			       const struct sockaddr_in *src, const void *buf,
			       size_t len, uint8_t tos, uint64_t rx_time_ns);
void uib_transport_deliver_validated(struct uib_context *ctx,
				     const struct sockaddr_in *src,
				     const void *buf, size_t len, uint8_t tos,
				     uint64_t rx_time_ns);
#endif
