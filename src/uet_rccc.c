/* SPDX-License-Identifier: Apache-2.0 */
/*
 * RCCC behavior follows the Ultra Ethernet Specification 1.0.2 published
 * by the Ultra Ethernet Consortium.
 */
#include <errno.h>
#include <limits.h>
#include <string.h>

#include "uib/uet_rccc.h"
#include "utils.h"

#define UIB_RCCC_BASE_RTT_NS 12000ULL
#define UIB_RCCC_LINKSPEED 12500000000ULL
#define UIB_RCCC_CREDIT_PACKETS 4U
#define UIB_RCCC_COUNTER_HALF 0x00800000U

static uint32_t counter_delta(uint32_t newer, uint32_t older)
{
	return (newer - older) & UIB_UET_RCCC_COUNTER_MASK;
}

/**
 * uib_uet_rccc_config_default - Fill default RCCC settings.
 * @config: Configuration to fill
 * @mtu: Nominal packet size in bytes
 * @uib-api: Congestion control
 */
void uib_uet_rccc_config_default(struct uib_uet_rccc_config *config,
				 uint32_t mtu)
{
	uint64_t bdp;

	memset(config, 0, sizeof(*config));
	config->sender_linkspeed = UIB_RCCC_LINKSPEED;
	config->receiver_linkspeed = UIB_RCCC_LINKSPEED;
	config->base_rtt_ns = UIB_RCCC_BASE_RTT_NS;
	config->mtu = mtu;
	bdp = uib_mul_div_u64(UIB_RCCC_LINKSPEED, UIB_RCCC_BASE_RTT_NS,
			      1000000000ULL);
	config->max_wnd = (uint32_t)(bdp + bdp / 2);
	if (config->max_wnd < mtu)
		config->max_wnd = mtu;
	config->credit_quantum = UIB_RCCC_CREDIT_PACKETS * mtu;
	config->credit_interval_ns = uib_mul_div_u64(
		config->credit_quantum, 1000000000ULL,
		config->receiver_linkspeed);
	if (!config->credit_interval_ns)
		config->credit_interval_ns = 1;
}

static int config_valid(const struct uib_uet_rccc_config *config)
{
	return config && config->mtu && config->sender_linkspeed &&
		config->receiver_linkspeed && config->base_rtt_ns &&
		config->credit_interval_ns && config->max_wnd >= config->mtu &&
		config->credit_quantum >= config->mtu;
}

/**
 * uib_uet_rccc_init - Initialize RCCC source state.
 * @rccc: Source state to initialize
 * @config: Configuration to apply
 * @uib-api: Congestion control
 *
 * Return: 0 on success or `-EINVAL` for invalid configuration.
 */
int uib_uet_rccc_init(struct uib_uet_rccc *rccc,
		      const struct uib_uet_rccc_config *config)
{
	if (!rccc || !config_valid(config))
		return -EINVAL;
	memset(rccc, 0, sizeof(*rccc));
	rccc->config = *config;
	rccc->credit = config->max_wnd;
	rccc->speculating = 1;
	return 0;
}

/**
 * uib_uet_rccc_on_new_data - Add newly posted source demand.
 * @rccc: Initialized source state
 * @bytes: Newly posted bytes
 * @uib-api: Congestion control
 */
void uib_uet_rccc_on_new_data(struct uib_uet_rccc *rccc, size_t bytes)
{
	if (!rccc || !bytes)
		return;
	rccc->backlog = rccc->backlog > UINT64_MAX - bytes ? UINT64_MAX :
		rccc->backlog + bytes;
	rccc->credit_target_sent = 0;
}

/**
 * uib_uet_rccc_on_cancel_data - Remove source demand that was not queued.
 * @rccc: Initialized source state
 * @bytes: Cancelled nominal bytes
 * @uib-api: Congestion control
 */
void uib_uet_rccc_on_cancel_data(struct uib_uet_rccc *rccc, size_t bytes)
{
	if (!rccc || !bytes)
		return;
	rccc->backlog = bytes < rccc->backlog ? rccc->backlog - bytes : 0;
	rccc->credit_target_sent = 0;
	uib_uet_rccc_credit_target(rccc);
}

/**
 * uib_uet_rccc_credit_target - Calculate cumulative requested credit.
 * @rccc: Initialized source state
 * @uib-api: Congestion control
 *
 * Return: Cumulative credit target in wrapping 256-byte units.
 */
uint32_t uib_uet_rccc_credit_target(struct uib_uet_rccc *rccc)
{
	uint64_t demand;
	int64_t delta;
	uint64_t units;

	if (!rccc)
		return 0;
	demand = min_t(uint64_t, rccc->backlog + rccc->retransmit_backlog,
		       rccc->config.max_wnd);
	delta = (int64_t)demand - rccc->credit;
	if ((rccc->speculating || delta > 0) && delta < rccc->config.mtu)
		delta = rccc->config.mtu;
	if (delta < 0)
		delta = 0;
	units = ((uint64_t)delta + 255) >> 8;
	rccc->credit_target = (rccc->cumulative_credit + units) &
		UIB_UET_RCCC_COUNTER_MASK;
	return rccc->credit_target;
}

/**
 * uib_uet_rccc_can_send - Test whether source credit admits a packet.
 * @rccc: Initialized source state
 * @bytes: Prospective nominal packet size
 * @uib-api: Congestion control
 *
 * Return: Nonzero when sufficient credit is available, otherwise 0.
 */
int uib_uet_rccc_can_send(struct uib_uet_rccc *rccc, size_t bytes)
{
	if (!rccc || !bytes)
		return 0;
	if (rccc->credit >= 0 && (uint64_t)rccc->credit >= bytes)
		return 1;
	uib_uet_rccc_credit_target(rccc);
	rccc->credit_stalls++;
	return 0;
}

/**
 * uib_uet_rccc_mark_target_sent - Record target delivery to the peer.
 * @rccc: Initialized source state
 * @uib-api: Congestion control
 */
void uib_uet_rccc_mark_target_sent(struct uib_uet_rccc *rccc)
{
	if (rccc)
		rccc->credit_target_sent = 1;
}

/**
 * uib_uet_rccc_on_send - Consume source credit.
 * @rccc: Initialized source state
 * @bytes: Nominal transmitted bytes
 * @uib-api: Congestion control
 */
void uib_uet_rccc_on_send(struct uib_uet_rccc *rccc, size_t bytes)
{
	if (!rccc || !bytes)
		return;
	rccc->credit -= (int64_t)bytes;
	rccc->inflight += bytes;
	if (rccc->retransmit_backlog >= bytes)
		rccc->retransmit_backlog -= bytes;
	else if (rccc->backlog >= bytes)
		rccc->backlog -= bytes;
	else
		rccc->backlog = 0;
	rccc->credit_target_sent = 1;
}

static void stop_speculating(struct uib_uet_rccc *rccc)
{
	if (!rccc->speculating)
		return;
	rccc->speculating = 0;
	if (rccc->credit > 0)
		rccc->credit = 0;
}

/**
 * uib_uet_rccc_on_ack - Apply acknowledgement feedback.
 * @rccc: Initialized source state
 * @bytes: Newly acknowledged bytes
 * @uib-api: Congestion control
 */
void uib_uet_rccc_on_ack(struct uib_uet_rccc *rccc, size_t bytes)
{
	if (!rccc)
		return;
	stop_speculating(rccc);
	rccc->inflight = bytes < rccc->inflight ? rccc->inflight - bytes : 0;
}

/**
 * uib_uet_rccc_on_loss - Return lost data to the retransmit backlog.
 * @rccc: Initialized source state
 * @bytes: Nominal lost bytes
 * @uib-api: Congestion control
 */
void uib_uet_rccc_on_loss(struct uib_uet_rccc *rccc, size_t bytes)
{
	if (!rccc)
		return;
	stop_speculating(rccc);
	rccc->inflight = bytes < rccc->inflight ? rccc->inflight - bytes : 0;
	rccc->retransmit_backlog += bytes;
	rccc->credit_target_sent = 0;
	uib_uet_rccc_credit_target(rccc);
}

/**
 * uib_uet_rccc_on_credit - Apply a cumulative destination grant.
 * @rccc: Initialized source state
 * @cumulative_credit: Wrapping cumulative credit in 256-byte units
 * @uib-api: Congestion control
 */
void uib_uet_rccc_on_credit(struct uib_uet_rccc *rccc,
			    uint32_t cumulative_credit)
{
	uint32_t delta;

	if (!rccc)
		return;
	stop_speculating(rccc);
	cumulative_credit &= UIB_UET_RCCC_COUNTER_MASK;
	delta = counter_delta(cumulative_credit, rccc->cumulative_credit);
	if (!delta || delta >= UIB_RCCC_COUNTER_HALF)
		return;
	rccc->credit += (int64_t)delta << 8;
	rccc->cumulative_credit = cumulative_credit;
	rccc->credit_updates++;
}

/**
 * uib_uet_rccc_destination_init - Initialize a destination scheduler.
 * @destination: Destination state to initialize
 * @config: Configuration to apply
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 *
 * Return: 0 on success or `-EINVAL` for invalid configuration.
 */
int uib_uet_rccc_destination_init(
	struct uib_uet_rccc_destination *destination,
	const struct uib_uet_rccc_config *config, uint64_t now_ns)
{
	if (!destination || !config_valid(config))
		return -EINVAL;
	memset(destination, 0, sizeof(*destination));
	destination->config = *config;
	destination->available_credit = config->credit_quantum;
	destination->last_update_ns = now_ns;
	return 0;
}

/**
 * uib_uet_rccc_destination_update - Pace credit for observed demand.
 * @destination: Initialized destination state
 * @credit_target: Source cumulative target in 256-byte units
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 *
 * Return: Cumulative destination credit in 256-byte units.
 */
uint32_t uib_uet_rccc_destination_update(
	struct uib_uet_rccc_destination *destination, uint32_t credit_target,
	uint64_t now_ns)
{
	uint64_t elapsed;
	uint64_t replenished;
	uint64_t grant;
	uint32_t demand;

	if (!destination)
		return 0;
	credit_target &= UIB_UET_RCCC_COUNTER_MASK;
	demand = counter_delta(credit_target, destination->credit_target);
	if (demand && demand < UIB_RCCC_COUNTER_HALF)
		destination->credit_target = credit_target;
	if (now_ns > destination->last_update_ns) {
		elapsed = now_ns - destination->last_update_ns;
		replenished = uib_mul_div_u64(
			destination->config.receiver_linkspeed, elapsed,
			1000000000ULL);
		destination->available_credit = min_t(uint64_t,
			destination->available_credit + replenished,
			destination->config.max_wnd);
		destination->last_update_ns = now_ns;
	}
	demand = counter_delta(destination->credit_target,
			       destination->cumulative_credit);
	if (!demand || demand >= UIB_RCCC_COUNTER_HALF)
		return destination->cumulative_credit;
	grant = min_t(uint64_t, (uint64_t)demand << 8,
		      destination->config.credit_quantum);
	grant = min_t(uint64_t, grant, destination->available_credit);
	grant &= ~255ULL;
	if (!grant)
		return destination->cumulative_credit;
	destination->available_credit -= grant;
	destination->cumulative_credit =
		(destination->cumulative_credit + (grant >> 8)) &
		UIB_UET_RCCC_COUNTER_MASK;
	destination->grants++;
	return destination->cumulative_credit;
}
