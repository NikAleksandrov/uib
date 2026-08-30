/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_TRANSPORT_INTERNAL_H
#define UIB_TRANSPORT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

struct uib_context;
struct uib_qp;
struct uib_rx_meta;

void uib_transport_deliver_ecn(struct uib_context *ctx,
			       const struct sockaddr_in *src, const void *buf,
			       size_t len, uint8_t ecn, uint64_t rx_time_ns);
int uib_qp_handle_packet_meta(struct uib_qp *qp,
			      const struct sockaddr_in *src, const void *pkt,
			      size_t len, const struct uib_rx_meta *meta);
int uib_rocev2_handle_packet_meta(struct uib_qp *qp,
				  const struct sockaddr_in *src,
				  const void *packet, size_t len,
				  const struct uib_rx_meta *meta);

#endif
