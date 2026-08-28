/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_DCQCN_H
#define UIB_DCQCN_H

#include <stddef.h>
#include <stdint.h>

#define UIB_DCQCN_SCALE 65536U

/**
 * struct uib_dcqcn_config - DCQCN source configuration.
 * @initial_rate: Initial transmit rate in bytes per second
 * @minimum_rate: Lowest permitted transmit rate in bytes per second
 * @maximum_rate: Highest permitted transmit rate in bytes per second
 * @additive_increase: Target-rate increment in bytes per second
 * @recovery_interval_ns: Rate-recovery and alpha-update interval
 * @alpha_gain_q16: ECN alpha EWMA gain in Q16 fixed-point units
 * @mtu: Nominal packet size in bytes
 */
struct uib_dcqcn_config {
	uint64_t initial_rate;
	uint64_t minimum_rate;
	uint64_t maximum_rate;
	uint64_t additive_increase;
	uint64_t recovery_interval_ns;
	uint32_t alpha_gain_q16;
	uint32_t mtu;
};

/**
 * struct uib_dcqcn - Per-flow DCQCN source state.
 * @config: Applied configuration
 * @current_rate: Current transmit rate in bytes per second
 * @target_rate: Recovery target in bytes per second
 * @next_send_ns: Earliest monotonic time for the next packet
 * @last_alpha_ns: Last alpha update time
 * @last_recovery_ns: Last rate recovery time
 * @inflight: Bytes transmitted but not acknowledged
 * @alpha_q16: Congestion estimate in Q16 fixed-point units
 * @recovery_steps: Completed fast-recovery intervals
 * @cnp_seen: Nonzero when the current alpha interval saw a CNP
 * @cnp_events: Number of applied CNP events
 */
struct uib_dcqcn {
	struct uib_dcqcn_config config;
	uint64_t current_rate;
	uint64_t target_rate;
	uint64_t next_send_ns;
	uint64_t last_alpha_ns;
	uint64_t last_recovery_ns;
	uint64_t inflight;
	uint32_t alpha_q16;
	uint32_t recovery_steps;
	uint8_t cnp_seen;
	uint64_t cnp_events;
};

void uib_dcqcn_config_default(struct uib_dcqcn_config *config, uint32_t mtu);
int uib_dcqcn_init(struct uib_dcqcn *dcqcn,
		   const struct uib_dcqcn_config *config, uint64_t now_ns);
int uib_dcqcn_can_send(const struct uib_dcqcn *dcqcn, size_t bytes,
		       uint64_t now_ns);
void uib_dcqcn_on_send(struct uib_dcqcn *dcqcn, size_t bytes,
		       uint64_t now_ns);
void uib_dcqcn_on_ack(struct uib_dcqcn *dcqcn, size_t bytes,
		      uint64_t now_ns);
void uib_dcqcn_on_cnp(struct uib_dcqcn *dcqcn, uint64_t now_ns);
void uib_dcqcn_on_loss(struct uib_dcqcn *dcqcn, size_t bytes,
		       uint64_t now_ns);

#endif
