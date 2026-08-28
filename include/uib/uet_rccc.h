/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_UET_RCCC_H
#define UIB_UET_RCCC_H

#include <stddef.h>
#include <stdint.h>

#define UIB_UET_RCCC_COUNTER_MASK 0x00ffffffU

/**
 * struct uib_uet_rccc_config - UET receiver-credit configuration.
 * @sender_linkspeed: Source link rate in bytes per second
 * @receiver_linkspeed: Destination link rate in bytes per second
 * @base_rtt_ns: Configured base round-trip time in nanoseconds
 * @credit_interval_ns: Destination credit pacing interval
 * @mtu: Nominal packet size in bytes
 * @max_wnd: Maximum speculative and outstanding byte window
 * @credit_quantum: Bytes granted by one destination scheduling turn
 */
struct uib_uet_rccc_config {
	uint64_t sender_linkspeed;
	uint64_t receiver_linkspeed;
	uint64_t base_rtt_ns;
	uint64_t credit_interval_ns;
	uint32_t mtu;
	uint32_t max_wnd;
	uint32_t credit_quantum;
};

/**
 * struct uib_uet_rccc - RCCC source state.
 * @config: Applied configuration
 * @credit: Available source credit in bytes
 * @backlog: Posted but unsent data in bytes
 * @retransmit_backlog: Data waiting for retransmission in bytes
 * @inflight: Sent but unacknowledged data in bytes
 * @credit_target: Cumulative requested credit in 256-byte units
 * @cumulative_credit: Last destination grant in 256-byte units
 * @credit_target_sent: Nonzero after the current target reached the peer
 * @speculating: Nonzero while using the initial speculative credit
 * @credit_stalls: Number of admission failures caused by exhausted credit
 * @credit_updates: Number of accepted cumulative credit updates
 */
struct uib_uet_rccc {
	struct uib_uet_rccc_config config;
	int64_t credit;
	uint64_t backlog;
	uint64_t retransmit_backlog;
	uint64_t inflight;
	uint32_t credit_target;
	uint32_t cumulative_credit;
	uint8_t credit_target_sent;
	uint8_t speculating;
	uint64_t credit_stalls;
	uint64_t credit_updates;
};

/**
 * struct uib_uet_rccc_destination - Per-source destination scheduler state.
 * @config: Applied configuration
 * @credit_target: Highest cumulative source demand in 256-byte units
 * @cumulative_credit: Cumulative granted credit in 256-byte units
 * @available_credit: Currently paceable credit in bytes
 * @last_update_ns: Last credit pacing update time
 * @grants: Number of generated credit updates
 */
struct uib_uet_rccc_destination {
	struct uib_uet_rccc_config config;
	uint32_t credit_target;
	uint32_t cumulative_credit;
	uint64_t available_credit;
	uint64_t last_update_ns;
	uint64_t grants;
};

void uib_uet_rccc_config_default(struct uib_uet_rccc_config *config,
				 uint32_t mtu);
int uib_uet_rccc_init(struct uib_uet_rccc *rccc,
		      const struct uib_uet_rccc_config *config);
void uib_uet_rccc_on_new_data(struct uib_uet_rccc *rccc, size_t bytes);
void uib_uet_rccc_on_cancel_data(struct uib_uet_rccc *rccc, size_t bytes);
int uib_uet_rccc_can_send(struct uib_uet_rccc *rccc, size_t bytes);
void uib_uet_rccc_on_send(struct uib_uet_rccc *rccc, size_t bytes);
void uib_uet_rccc_on_ack(struct uib_uet_rccc *rccc, size_t bytes);
void uib_uet_rccc_on_loss(struct uib_uet_rccc *rccc, size_t bytes);
void uib_uet_rccc_on_credit(struct uib_uet_rccc *rccc,
			    uint32_t cumulative_credit);
uint32_t uib_uet_rccc_credit_target(struct uib_uet_rccc *rccc);
void uib_uet_rccc_mark_target_sent(struct uib_uet_rccc *rccc);

int uib_uet_rccc_destination_init(
	struct uib_uet_rccc_destination *destination,
	const struct uib_uet_rccc_config *config, uint64_t now_ns);
uint32_t uib_uet_rccc_destination_update(
	struct uib_uet_rccc_destination *destination, uint32_t credit_target,
	uint64_t now_ns);

#endif
