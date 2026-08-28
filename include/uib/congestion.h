/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_CONGESTION_H
#define UIB_CONGESTION_H

#include <stddef.h>
#include <stdint.h>

#define UIB_CONGESTION_ABI_VERSION 1
#define UIB_CONGESTION_STATE_SIZE 512

/**
 * struct uib_congestion_ack - Protocol-neutral acknowledgement feedback.
 * @ack_time_ns: Sender monotonic time when the ACK was handled
 * @tx_time_ns: Sender monotonic time when the acknowledged packet was sent
 * @newly_received: Newly acknowledged receiver byte count
 * @service_time_ns: Receiver service or queue-delay sample
 * @tx_retries: Number of sender retransmissions for the acknowledged packet
 * @retransmit: Nonzero when the received packet was marked as retransmitted
 * @ecn_marked: Nonzero when the protocol reported congestion marking
 * @receiver_cwnd_penalty: Receiver-requested window reduction
 * @restore_cwnd: Nonzero when the receiver releases a previous penalty
 * @credit_valid: Nonzero when @cumulative_credit contains receiver credit
 * @cumulative_credit: Wrapping cumulative credit in 256-byte units
 */
struct uib_congestion_ack {
	uint64_t ack_time_ns;
	uint64_t tx_time_ns;
	uint32_t newly_received;
	uint32_t service_time_ns;
	uint8_t tx_retries;
	uint8_t retransmit;
	uint8_t ecn_marked;
	uint8_t receiver_cwnd_penalty;
	uint8_t restore_cwnd;
	uint8_t credit_valid;
	uint32_t cumulative_credit;
};

/**
 * struct uib_congestion_nack - Protocol-neutral negative acknowledgement.
 * @bytes: Nominal bytes covered by the NACK
 * @now_ns: Current monotonic time in nanoseconds
 * @trimmed: Nonzero when a switch trimmed the packet payload
 * @last_hop: Nonzero when trimming occurred on the final hop
 * @receiver_controlled: Nonzero when receiver-controlled CC is active
 */
struct uib_congestion_nack {
	size_t bytes;
	uint64_t now_ns;
	uint8_t trimmed;
	uint8_t last_hop;
	uint8_t receiver_controlled;
};

/**
 * struct uib_congestion_ops - Congestion-control algorithm contract.
 * @abi_version: Must equal UIB_CONGESTION_ABI_VERSION
 * @name: Stable environment and result name for the algorithm
 * @state_size: Bytes required in each flow's bounded state buffer
 * @init: Initialize state from optional algorithm-specific configuration
 * @can_send: Return nonzero when the flow may send the supplied byte count
 * @ack_request: Return nonzero when protocol ACK generation should be forced
 * @on_send: Account newly transmitted bytes
 * @on_ack: Consume translated ACK, timing, and congestion feedback
 * @on_loss: Consume inferred-loss feedback
 * @on_nack: Consume translated explicit NACK feedback
 */
struct uib_congestion_ops {
	uint32_t abi_version;
	const char *name;
	size_t state_size;
	int (*init)(void *state, const void *config, uint32_t mtu,
		    uint64_t now_ns);
	int (*can_send)(const void *state, size_t bytes);
	int (*ack_request)(const void *state, uint32_t trigger);
	void (*on_send)(void *state, size_t bytes);
	void (*on_ack)(void *state, const struct uib_congestion_ack *ack);
	void (*on_loss)(void *state, size_t bytes);
	void (*on_nack)(void *state, const struct uib_congestion_nack *nack);
};

/**
 * struct uib_congestion - Bounded per-flow algorithm instance.
 * @ops: Selected static operations table
 * @state: Aligned algorithm-owned state storage
 */
struct uib_congestion {
	const struct uib_congestion_ops *ops;
	union {
		max_align_t align;
		unsigned char bytes[UIB_CONGESTION_STATE_SIZE];
	} state;
};

int uib_congestion_register(const struct uib_congestion_ops *ops);
int uib_congestion_init(struct uib_congestion *congestion, const char *name,
			const void *config, uint32_t mtu, uint64_t now_ns);
const char *uib_congestion_name(const struct uib_congestion *congestion);
int uib_congestion_enabled(const struct uib_congestion *congestion);
int uib_congestion_can_send(const struct uib_congestion *congestion,
			    size_t bytes);
uint64_t uib_congestion_next_send_ns(
	const struct uib_congestion *congestion);
int uib_congestion_ack_request(const struct uib_congestion *congestion,
			       uint32_t trigger);
void uib_congestion_on_send(struct uib_congestion *congestion, size_t bytes);
void uib_congestion_on_ack(struct uib_congestion *congestion,
			   const struct uib_congestion_ack *ack);
void uib_congestion_on_loss(struct uib_congestion *congestion, size_t bytes);
void uib_congestion_on_nack(struct uib_congestion *congestion,
			    const struct uib_congestion_nack *nack);

#endif
