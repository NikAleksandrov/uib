/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_UET_NSCC_H
#define UIB_UET_NSCC_H

#include <stddef.h>
#include <stdint.h>

#define UIB_UET_NSCC_SCALE 65536ULL
#define UIB_UET_NSCC_COUNTER_MASK 0x00ffffffU

struct uib_qp;

struct uib_uet_nscc_config {
	uint64_t config_base_rtt_ns;
	uint64_t sender_linkspeed;
	uint64_t receiver_linkspeed;
	uint64_t target_qdelay_ns;
	uint64_t qa_threshold_ns;
	uint64_t adjust_period_ns;
	uint32_t mtu;
	uint32_t bdp;
	uint32_t max_wnd;
	uint32_t adjust_bytes_threshold;
	uint32_t qa_gate;
	uint32_t gamma_q16;
	uint32_t fi_scale_q16;
	uint32_t max_md_jump_q16;
	uint32_t eta;
	uint32_t fi;
	int disable_quick_adapt;
};

struct uib_uet_nscc {
	struct uib_uet_nscc_config config;
	uint64_t cwnd;
	int64_t inflight;
	uint64_t saved_cwnd;
	uint64_t base_rtt_ns;
	uint64_t max_wnd;
	uint64_t achieved_bytes;
	uint64_t received_bytes;
	uint64_t fi_count;
	uint64_t qa_endtime_ns;
	uint64_t bytes_to_ignore;
	uint64_t bytes_ignored;
	uint64_t inc_bytes;
	uint64_t last_adjust_ns;
	uint64_t last_decrease_ns;
	uint64_t delay_sum;
	uint64_t delay_weight;
	int trigger_qa;
	int increase_mode;
};

struct uib_uet_nscc_ack {
	uint64_t ack_time_ns;
	uint64_t tx_time_ns;
	uint32_t newly_received;
	uint32_t service_time_ns;
	uint8_t tx_retries;
	uint8_t retransmit;
	uint8_t ecn_marked;
	uint8_t receiver_cwnd_penalty;
	uint8_t restore_cwnd;
};

void uib_uet_nscc_config_default(struct uib_uet_nscc_config *config,
				 uint32_t mtu);
void uib_uet_nscc_init(struct uib_uet_nscc *nscc,
		       const struct uib_uet_nscc_config *config,
		       uint64_t now_ns);
int uib_uet_nscc_can_send(const struct uib_uet_nscc *nscc, size_t bytes);
int uib_uet_nscc_ack_request(const struct uib_uet_nscc *nscc,
			     uint32_t ack_generation_trigger);
void uib_uet_nscc_on_send(struct uib_uet_nscc *nscc, size_t bytes);
void uib_uet_nscc_on_ack(struct uib_uet_nscc *nscc,
			 const struct uib_uet_nscc_ack *ack);
void uib_uet_nscc_on_inferred_loss(struct uib_uet_nscc *nscc, size_t bytes);
void uib_uet_nscc_on_nack(struct uib_uet_nscc *nscc, size_t bytes,
			  int trimmed, int last_hop, int rccc_enabled,
			  uint64_t now_ns);
int uib_uet_nscc_configure_qp(struct uib_qp *qp,
			      const struct uib_uet_nscc_config *config);
int uib_uet_nscc_query_qp(struct uib_qp *qp, struct uib_uet_nscc *state);

#endif
