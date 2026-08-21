/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_TRANSPORT_H
#define UIB_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#include "uib/transport_ops.h"

struct uib_context;

extern const struct uib_transport_ops uib_afxdp_ops;
extern const struct uib_transport_ops uib_tcp_devmem_ops;

#define UIB_TIMER_TICK_MS 50

enum uib_uring_rx_mode {
	UIB_URING_RX_MULTISHOT = 0,
	UIB_URING_RX_BATCHED,
};

struct uib_transport_stats {
	uint64_t tx_queued;
	uint64_t tx_submitted;
	uint64_t tx_completed;
	uint64_t tx_errors;
	uint64_t tx_gso_batches;
	uint64_t tx_gso_packets;
	uint64_t rx_packets;
	uint64_t rx_gro_batches;
	uint64_t rx_completion_batches;
	uint32_t rx_largest_batch;
	uint32_t rx_batch;
	uint32_t tx_batch;
	uint32_t sqpoll_requested;
	uint32_t sqpoll_active;
	uint32_t gso_enabled;
	uint32_t gro_enabled;
	enum uib_uring_rx_mode rx_mode;
};

int uib_transport_init(struct uib_context *ctx);
void uib_transport_fini(struct uib_context *ctx);
int uib_transport_send(struct uib_context *ctx, const struct sockaddr_in *dst,
		       const void *buf, size_t len);
int uib_transport_send_entropy(struct uib_context *ctx,
			       const struct sockaddr_in *dst,
			       const void *buf, size_t len, uint16_t entropy);
int uib_transport_send_ref(struct uib_context *ctx,
			   const struct sockaddr_in *dst, const void *buf,
			   size_t len, uib_transport_release_fn release,
			   void *opaque);
int uib_transport_send_ref_batch(struct uib_context *ctx,
				 const struct uib_transport_ref *refs,
				 size_t count);
int uib_transport_get_stats(struct uib_context *ctx,
			    struct uib_transport_stats *stats);

void uib_transport_deliver(struct uib_context *ctx,
			   const struct sockaddr_in *src, const void *buf,
			   size_t len);
void uib_transport_retransmit_walk(struct uib_context *ctx);

enum uib_transport_mode uib_transport_mode_from_env(void);

#endif
