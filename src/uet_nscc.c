/* SPDX-License-Identifier: Apache-2.0 */
/*
 * NSCC behavior follows the Ultra Ethernet Specification 1.0.2 published
 * by the Ultra Ethernet Consortium.
 */
#include <limits.h>
#include <string.h>

#include "uib/uet_nscc.h"
#include "utils.h"

#define UIB_NSCC_BASE_RTT_NS 12000ULL
#define UIB_NSCC_LINKSPEED 12500000000ULL
#define UIB_NSCC_BASE_BDP 150000U
#define UIB_NSCC_BASE_RTT_WINDOW_NS 10000000ULL

static void nscc_scale_thresholds(struct uib_uet_nscc *nscc, uint64_t rtt);

/**
 * uib_uet_nscc_config_default - Derive default NSCC settings for an MTU.
 * @config: Configuration to apply or fill
 * @mtu: Path maximum transmission unit in bytes
 * @uib-api: Ultra Ethernet NSCC
 */
void uib_uet_nscc_config_default(struct uib_uet_nscc_config *config,
				 uint32_t mtu)
{
	uint64_t linkspeed;

	memset(config, 0, sizeof(*config));
	config->config_base_rtt_ns = UIB_NSCC_BASE_RTT_NS;
	config->sender_linkspeed = UIB_NSCC_LINKSPEED;
	config->receiver_linkspeed = UIB_NSCC_LINKSPEED;
	config->target_qdelay_ns = UIB_NSCC_BASE_RTT_NS;
	config->qa_threshold_ns = 4 * config->target_qdelay_ns;
	config->adjust_period_ns = config->config_base_rtt_ns;
	config->mtu = mtu;
	linkspeed = min_t(uint64_t, config->sender_linkspeed,
			    config->receiver_linkspeed);
	config->bdp = (uint32_t)uib_mul_div_u64(linkspeed,
					    config->config_base_rtt_ns,
					    1000000000ULL);
	config->max_wnd = config->bdp + config->bdp / 2;
	config->adjust_bytes_threshold = 8 * mtu;
	config->qa_gate = 3;
	config->gamma_q16 = (uint32_t)(UIB_UET_NSCC_SCALE * 8 / 10);
	config->fi_scale_q16 = (uint32_t)(UIB_UET_NSCC_SCALE / 4);
	config->max_md_jump_q16 = (uint32_t)(UIB_UET_NSCC_SCALE / 2);
	config->eta = (uint32_t)uib_mul_div_u64(15ULL * mtu, config->bdp,
					    100ULL * UIB_NSCC_BASE_BDP);
	config->fi = (uint32_t)uib_mul_div_u64(5ULL * mtu, config->bdp,
					   UIB_NSCC_BASE_BDP);
	if (!config->eta)
		config->eta = 1;
	if (!config->fi)
		config->fi = 1;
}

/**
 * uib_uet_nscc_init - Initialize an NSCC flow-control state machine.
 * @nscc: NSCC congestion-control state
 * @config: Configuration to apply or fill
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Ultra Ethernet NSCC
 */
void uib_uet_nscc_init(struct uib_uet_nscc *nscc,
		       const struct uib_uet_nscc_config *config,
		       uint64_t now_ns)
{
	memset(nscc, 0, sizeof(*nscc));
	nscc->config = *config;
	nscc->cwnd = config->max_wnd;
	nscc->max_wnd = config->max_wnd;
	nscc->base_rtt_ns = config->config_base_rtt_ns;
	nscc_scale_thresholds(nscc, config->config_base_rtt_ns);
	nscc->last_adjust_ns = now_ns;
	nscc->last_decrease_ns = now_ns;
}

/**
 * uib_uet_nscc_can_send - Check whether the congestion window admits bytes.
 * @nscc: NSCC congestion-control state
 * @bytes: Number of bytes in the prospective transmission
 * @uib-api: Ultra Ethernet NSCC
 *
 * Return: Nonzero if the congestion window admits @bytes, otherwise 0.
 */
int uib_uet_nscc_can_send(const struct uib_uet_nscc *nscc, size_t bytes)
{
	uint64_t inflight = nscc->inflight > 0 ? (uint64_t)nscc->inflight : 0;
	uint64_t nominal = bytes > nscc->config.mtu ? bytes : nscc->config.mtu;

	return inflight + nominal <= nscc->cwnd;
}

/**
 * uib_uet_nscc_ack_request - Decide whether a packet should request an ACK.
 * @nscc: NSCC congestion-control state
 * @ack_generation_trigger: Congestion-window threshold for requesting an ACK
 * @uib-api: Ultra Ethernet NSCC
 *
 * Return: Nonzero if the packet should request an ACK, otherwise 0.
 */
int uib_uet_nscc_ack_request(const struct uib_uet_nscc *nscc,
			     uint32_t ack_generation_trigger)
{
	uint64_t inflight = nscc->inflight > 0 ? (uint64_t)nscc->inflight : 0;

	return nscc->cwnd < inflight + nscc->config.mtu ||
	       nscc->cwnd < ack_generation_trigger;
}

/**
 * uib_uet_nscc_on_send - Account newly transmitted bytes in NSCC.
 * @nscc: NSCC congestion-control state
 * @bytes: Number of newly transmitted bytes
 * @uib-api: Ultra Ethernet NSCC
 */
void uib_uet_nscc_on_send(struct uib_uet_nscc *nscc, size_t bytes)
{
	nscc->inflight += (int64_t)bytes;
}

/*
 * The specification derives the target queueing delay and the quick-adapt
 * delay threshold from the configured base round trip. Keep that ratio
 * against the measured base round trip so a path slower than the configured
 * default is not permanently reported as congested. A path whose base round
 * trip matches the configured value keeps the configured thresholds exactly.
 */
static void nscc_scale_thresholds(struct uib_uet_nscc *nscc, uint64_t rtt)
{
	uint64_t reference = nscc->config.config_base_rtt_ns;

	if (!reference) {
		nscc->target_qdelay_ns = nscc->config.target_qdelay_ns;
		nscc->qa_threshold_ns = nscc->config.qa_threshold_ns;
		return;
	}
	if (rtt < reference)
		rtt = reference;
	nscc->target_qdelay_ns = uib_mul_div_u64(
		nscc->config.target_qdelay_ns, rtt, reference);
	nscc->qa_threshold_ns = uib_mul_div_u64(
		nscc->config.qa_threshold_ns, rtt, reference);
}

static void nscc_apply_base_rtt(struct uib_uet_nscc *nscc, uint64_t rtt)
{
	uint64_t window;

	nscc->base_rtt_ns = rtt;
	nscc_scale_thresholds(nscc, rtt);
	window = uib_mul_div_u64(nscc->config.sender_linkspeed, rtt,
			     1000000000ULL);
	nscc->max_wnd = window + window / 2;
	if (nscc->max_wnd < nscc->config.mtu)
		nscc->max_wnd = nscc->config.mtu;
	if (nscc->cwnd > nscc->max_wnd)
		nscc->cwnd = nscc->max_wnd;
}

/*
 * Track the base round trip as a minimum over a bounded observation window
 * rather than as an all-time minimum. A monotonically decreasing estimate
 * cannot follow a path whose round trip is longer than the configured
 * default, and every later sample is then reported as queueing delay.
 */
static void nscc_update_base_rtt(struct uib_uet_nscc *nscc, uint64_t rtt,
				 uint64_t now)
{
	if (!rtt)
		return;
	if (!nscc->base_rtt_sample_ns || rtt < nscc->base_rtt_sample_ns)
		nscc->base_rtt_sample_ns = rtt;
	if (!nscc->base_rtt_window_ns)
		nscc->base_rtt_window_ns = now + UIB_NSCC_BASE_RTT_WINDOW_NS;
	if (rtt < nscc->base_rtt_ns) {
		nscc_apply_base_rtt(nscc, rtt);
		return;
	}
	if (now < nscc->base_rtt_window_ns)
		return;
	if (nscc->base_rtt_sample_ns > nscc->base_rtt_ns)
		nscc_apply_base_rtt(nscc, nscc->base_rtt_sample_ns);
	nscc->base_rtt_sample_ns = 0;
	nscc->base_rtt_window_ns = now + UIB_NSCC_BASE_RTT_WINDOW_NS;
}

static uint64_t nscc_average_delay(const struct uib_uet_nscc *nscc)
{
	return nscc->delay_weight ? nscc->delay_sum / nscc->delay_weight : 0;
}

static void nscc_update_delay(struct uib_uet_nscc *nscc, uint64_t delay,
			      uint32_t weight)
{
	uint64_t old_weight = nscc->delay_weight;

	if (!weight)
		weight = 1;
	if (old_weight > nscc->cwnd) {
		nscc->delay_sum >>= 1;
		nscc->delay_weight >>= 1;
	}
	nscc->delay_sum += delay * weight;
	nscc->delay_weight += weight;
}

static int nscc_quick_adapt(struct uib_uet_nscc *nscc, int loss,
			    int ecn, uint64_t delay, uint64_t now)
{
	int acted = 0;

	if (nscc->config.disable_quick_adapt)
		return 0;
	if (nscc->bytes_ignored < nscc->bytes_to_ignore && ecn) {
		acted = 1;
	} else if (now >= nscc->qa_endtime_ns) {
		uint64_t gate = nscc->max_wnd >> nscc->config.qa_gate;

		if (nscc->qa_endtime_ns &&
		    (nscc->trigger_qa || loss ||
		     delay > nscc->qa_threshold_ns) &&
		    nscc->achieved_bytes < gate) {
			nscc->cwnd = max_t(uint64_t, nscc->achieved_bytes,
					     nscc->config.mtu);
			nscc->bytes_to_ignore = nscc->inflight > 0 ?
				(uint64_t)nscc->inflight : 0;
			nscc->bytes_ignored = 0;
			nscc->trigger_qa = 0;
			acted = 1;
		}
		nscc->achieved_bytes = 0;
		nscc->qa_endtime_ns = now + nscc->base_rtt_ns +
				      nscc->target_qdelay_ns;
	}
	if (acted) {
		nscc->inc_bytes = 0;
		nscc->received_bytes = 0;
	}
	return acted;
}

static int nscc_apply_receiver_penalty(struct uib_uet_nscc *nscc,
				       const struct uib_uet_nscc_ack *ack)
{
	uint64_t decrease;

	if (ack->receiver_cwnd_penalty) {
		if (!nscc->saved_cwnd)
			nscc->saved_cwnd = nscc->cwnd;
		decrease = ((uint64_t)ack->receiver_cwnd_penalty *
			    ack->newly_received) >> 7;
		if (nscc->inflight > 0)
			nscc->cwnd = min_t(uint64_t, nscc->cwnd,
					     (uint64_t)nscc->inflight);
		nscc->cwnd = decrease < nscc->cwnd ?
			nscc->cwnd - decrease : nscc->config.mtu;
		nscc->cwnd = max_t(uint64_t, nscc->cwnd, nscc->config.mtu);
		return 1;
	}
	if (ack->restore_cwnd && nscc->saved_cwnd) {
		nscc->cwnd = min_t(uint64_t, nscc->saved_cwnd, nscc->max_wnd);
		nscc->saved_cwnd = 0;
	}
	return 0;
}

static void nscc_fast_increase(struct uib_uet_nscc *nscc, uint32_t bytes,
			       uint64_t delay)
{
	if (delay <= nscc->base_rtt_ns / 64) {
		nscc->fi_count += bytes;
		if (nscc->fi_count > nscc->cwnd || nscc->increase_mode) {
			nscc->cwnd += uib_mul_div_u64(bytes,
						 nscc->config.fi_scale_q16,
						 UIB_UET_NSCC_SCALE);
			nscc->cwnd = min_t(uint64_t, nscc->cwnd, nscc->max_wnd);
			nscc->increase_mode = 1;
			return;
		}
	} else {
		nscc->fi_count = 0;
	}
	nscc->increase_mode = 0;
}

static void nscc_proportional_increase(struct uib_uet_nscc *nscc,
				       uint32_t bytes, uint64_t delay)
{
	uint64_t gap;
	uint64_t scale_a;
	uint64_t increment;

	nscc_fast_increase(nscc, bytes, delay);
	if (nscc->increase_mode)
		return;
	gap = nscc->target_qdelay_ns - delay;
	scale_a = uib_mul_div_u64(nscc->config.bdp, UIB_UET_NSCC_SCALE,
			      UIB_NSCC_BASE_BDP);
	increment = uib_mul_div_u64(4ULL * nscc->config.mtu, scale_a,
				    UIB_UET_NSCC_SCALE);
	increment = uib_mul_div_u64(increment, bytes, 1);
	increment = uib_mul_div_u64(increment, gap,
				    nscc->target_qdelay_ns);
	nscc->inc_bytes += increment;
}

static void nscc_multiplicative_decrease(struct uib_uet_nscc *nscc,
					 uint64_t now)
{
	uint64_t delay = nscc_average_delay(nscc);
	uint64_t fraction;

	nscc->increase_mode = 0;
	nscc->fi_count = 0;
	if (delay <= nscc->target_qdelay_ns ||
	    now - nscc->last_decrease_ns <= nscc->base_rtt_ns)
		return;
	fraction = uib_mul_div_u64(nscc->config.gamma_q16,
				   delay - nscc->target_qdelay_ns, delay);
	if (fraction > UIB_UET_NSCC_SCALE - nscc->config.max_md_jump_q16)
		fraction = UIB_UET_NSCC_SCALE -
			nscc->config.max_md_jump_q16;
	nscc->cwnd = uib_mul_div_u64(nscc->cwnd,
				 UIB_UET_NSCC_SCALE - fraction,
				 UIB_UET_NSCC_SCALE);
	nscc->cwnd = max_t(uint64_t, nscc->cwnd, nscc->config.mtu);
	nscc->last_decrease_ns = now;
}

static void nscc_fulfill_adjustment(struct uib_uet_nscc *nscc, uint64_t now)
{
	if (nscc->cwnd)
		nscc->cwnd += nscc->inc_bytes / nscc->cwnd;
	if (now - nscc->last_adjust_ns >= nscc->config.adjust_period_ns) {
		nscc->last_adjust_ns = now;
		nscc->cwnd += nscc->config.eta;
	}
	nscc->cwnd = min_t(uint64_t, nscc->cwnd, nscc->max_wnd);
	nscc->inc_bytes = 0;
	nscc->received_bytes = 0;
}

/**
 * uib_uet_nscc_on_ack - Apply receiver feedback to NSCC sender state.
 * @nscc: NSCC congestion-control state
 * @ack: Acknowledgment feedback to apply
 * @uib-api: Ultra Ethernet NSCC
 */
void uib_uet_nscc_on_ack(struct uib_uet_nscc *nscc,
			 const struct uib_uet_nscc_ack *ack)
{
	uint64_t rtt;
	uint64_t delay;
	int receiver_limited;

	/*
	 * Rcvd_Bytes is rounded up to 256-byte units. The specification makes
	 * inflight signed because this subtraction may leave a small negative
	 * residual until later traffic advances the byte clock.
	 */
	nscc->inflight -= ack->newly_received;
	nscc->bytes_ignored += ack->newly_received;
	nscc->received_bytes += ack->newly_received;
	nscc->achieved_bytes += ack->newly_received;
	receiver_limited = nscc_apply_receiver_penalty(nscc, ack);
	if ((ack->tx_retries == 0 && !ack->retransmit) ||
	    (ack->tx_retries == 1 && ack->retransmit)) {
		if (ack->ack_time_ns <= ack->tx_time_ns + ack->service_time_ns)
			return;
		rtt = ack->ack_time_ns - ack->tx_time_ns - ack->service_time_ns;
	} else {
		return;
	}
	nscc_update_base_rtt(nscc, rtt, ack->ack_time_ns);
	delay = rtt > nscc->base_rtt_ns ? rtt - nscc->base_rtt_ns : 0;
	nscc_update_delay(nscc, delay, ack->newly_received);
	if (nscc_quick_adapt(nscc, 0, ack->ecn_marked, delay,
			     ack->ack_time_ns))
		return;
	if (!ack->ecn_marked && !receiver_limited) {
		if (delay >= nscc->target_qdelay_ns)
			nscc->inc_bytes += (uint64_t)nscc->config.fi *
					   ack->newly_received;
		else
			nscc_proportional_increase(nscc,
					   ack->newly_received, delay);
	} else if (ack->ecn_marked &&
		   delay >= nscc->target_qdelay_ns) {
		nscc_multiplicative_decrease(nscc, ack->ack_time_ns);
	}
	if (ack->ack_time_ns - nscc->last_adjust_ns >=
		    nscc->config.adjust_period_ns ||
	    nscc->received_bytes > nscc->config.adjust_bytes_threshold)
		nscc_fulfill_adjustment(nscc, ack->ack_time_ns);
}

/**
 * uib_uet_nscc_on_inferred_loss - Reduce the window after inferred loss.
 * @nscc: NSCC congestion-control state
 * @bytes: Number of bytes inferred lost
 * @uib-api: Ultra Ethernet NSCC
 */
void uib_uet_nscc_on_inferred_loss(struct uib_uet_nscc *nscc, size_t bytes)
{
	nscc->cwnd = nscc->cwnd > bytes ? nscc->cwnd - bytes :
		nscc->config.mtu;
	nscc->cwnd = max_t(uint64_t, nscc->cwnd, nscc->config.mtu);
	nscc->bytes_ignored += bytes;
	nscc->inflight -= (int64_t)bytes;
}

/**
 * uib_uet_nscc_on_nack - Reduce the window after an explicit NACK.
 * @nscc: NSCC congestion-control state
 * @bytes: Number of bytes reported by the NACK
 * @trimmed: Nonzero if the packet was trimmed
 * @last_hop: Nonzero if trimming occurred at the last hop
 * @rccc_enabled: Nonzero when receiver-driven congestion control is enabled
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Ultra Ethernet NSCC
 */
void uib_uet_nscc_on_nack(struct uib_uet_nscc *nscc, size_t bytes,
			  int trimmed, int last_hop, int rccc_enabled,
			  uint64_t now_ns)
{
	int adjust = trimmed && (!last_hop || !rccc_enabled);

	nscc->inflight -= (int64_t)bytes;
	if (trimmed) {
		nscc_update_delay(nscc, nscc->config.config_base_rtt_ns,
				  (uint32_t)bytes);
		nscc->bytes_ignored += bytes;
	}
	if (!adjust)
		return;
	nscc->trigger_qa = 1;
	if (nscc_quick_adapt(nscc, 1, 1, 0, now_ns))
		return;
	nscc->cwnd = nscc->cwnd > bytes ? nscc->cwnd - bytes :
		nscc->config.mtu;
	nscc->cwnd = max_t(uint64_t, nscc->cwnd, nscc->config.mtu);
}
