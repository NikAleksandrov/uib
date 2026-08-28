/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Algorithm based on "Congestion Control for Large-Scale RDMA Deployments",
 * Yibo Zhu et al., ACM SIGCOMM 2015, doi:10.1145/2785956.2787484.
 */
#include <errno.h>
#include <limits.h>
#include <string.h>

#include "uib/dcqcn.h"
#include "utils.h"

#define UIB_DCQCN_DEFAULT_RATE 12500000000ULL
#define UIB_DCQCN_MINIMUM_RATE 12500000ULL
#define UIB_DCQCN_ADDITIVE_INCREASE 625000ULL
#define UIB_DCQCN_RECOVERY_INTERVAL_NS 55000ULL
#define UIB_DCQCN_ALPHA_GAIN_Q16 (UIB_DCQCN_SCALE / 256)
#define UIB_DCQCN_FAST_RECOVERY_STEPS 5

/**
 * uib_dcqcn_config_default - Fill default DCQCN settings.
 * @config: Configuration to fill
 * @mtu: Nominal packet size in bytes
 * @uib-api: Congestion control
 */
void uib_dcqcn_config_default(struct uib_dcqcn_config *config, uint32_t mtu)
{
	memset(config, 0, sizeof(*config));
	config->initial_rate = UIB_DCQCN_DEFAULT_RATE;
	config->minimum_rate = UIB_DCQCN_MINIMUM_RATE;
	config->maximum_rate = UIB_DCQCN_DEFAULT_RATE;
	config->additive_increase = UIB_DCQCN_ADDITIVE_INCREASE;
	config->recovery_interval_ns = UIB_DCQCN_RECOVERY_INTERVAL_NS;
	config->alpha_gain_q16 = UIB_DCQCN_ALPHA_GAIN_Q16;
	config->mtu = mtu;
}

/**
 * uib_dcqcn_init - Initialize a DCQCN source.
 * @dcqcn: State to initialize
 * @config: Configuration to apply
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 *
 * Return: 0 on success or `-EINVAL` for invalid configuration.
 */
int uib_dcqcn_init(struct uib_dcqcn *dcqcn,
		   const struct uib_dcqcn_config *config, uint64_t now_ns)
{
	if (!dcqcn || !config || !config->mtu || !config->minimum_rate ||
	    config->minimum_rate > config->initial_rate ||
	    config->initial_rate > config->maximum_rate ||
	    !config->additive_increase || !config->recovery_interval_ns ||
	    !config->alpha_gain_q16 ||
	    config->alpha_gain_q16 > UIB_DCQCN_SCALE)
		return -EINVAL;
	memset(dcqcn, 0, sizeof(*dcqcn));
	dcqcn->config = *config;
	dcqcn->current_rate = config->initial_rate;
	dcqcn->target_rate = config->initial_rate;
	dcqcn->last_alpha_ns = now_ns;
	dcqcn->last_recovery_ns = now_ns;
	dcqcn->alpha_q16 = UIB_DCQCN_SCALE;
	return 0;
}

static void dcqcn_update_alpha(struct uib_dcqcn *dcqcn, uint64_t now_ns)
{
	uint64_t intervals;
	uint64_t complement;

	if (now_ns < dcqcn->last_alpha_ns +
		     dcqcn->config.recovery_interval_ns)
		return;
	intervals = (now_ns - dcqcn->last_alpha_ns) /
		dcqcn->config.recovery_interval_ns;
	complement = UIB_DCQCN_SCALE - dcqcn->config.alpha_gain_q16;
	if (dcqcn->cnp_seen) {
		dcqcn->alpha_q16 = (uint32_t)(
			uib_mul_div_u64(dcqcn->alpha_q16, complement,
					UIB_DCQCN_SCALE) +
			dcqcn->config.alpha_gain_q16);
		dcqcn->cnp_seen = 0;
		intervals--;
	}
	while (intervals--) {
		dcqcn->alpha_q16 = (uint32_t)uib_mul_div_u64(
			dcqcn->alpha_q16, complement, UIB_DCQCN_SCALE);
		if (!dcqcn->alpha_q16)
			break;
	}
	dcqcn->last_alpha_ns = now_ns - now_ns %
		dcqcn->config.recovery_interval_ns;
}

static void dcqcn_recover(struct uib_dcqcn *dcqcn, uint64_t now_ns)
{
	uint64_t intervals;

	if (now_ns < dcqcn->last_recovery_ns +
		     dcqcn->config.recovery_interval_ns)
		return;
	intervals = (now_ns - dcqcn->last_recovery_ns) /
		dcqcn->config.recovery_interval_ns;
	while (intervals--) {
		if (dcqcn->recovery_steps >= UIB_DCQCN_FAST_RECOVERY_STEPS)
			dcqcn->target_rate = min_t(uint64_t,
				dcqcn->target_rate + dcqcn->config.additive_increase,
				dcqcn->config.maximum_rate);
		dcqcn->current_rate = dcqcn->current_rate / 2 +
			dcqcn->target_rate / 2;
		dcqcn->current_rate = min_t(uint64_t, dcqcn->current_rate,
					      dcqcn->config.maximum_rate);
		dcqcn->recovery_steps++;
		if (dcqcn->current_rate == dcqcn->config.maximum_rate)
			break;
	}
	dcqcn->last_recovery_ns = now_ns - now_ns %
		dcqcn->config.recovery_interval_ns;
}

static void dcqcn_advance(struct uib_dcqcn *dcqcn, uint64_t now_ns)
{
	dcqcn_update_alpha(dcqcn, now_ns);
	dcqcn_recover(dcqcn, now_ns);
}

/**
 * uib_dcqcn_can_send - Test whether DCQCN pacing admits a packet.
 * @dcqcn: Initialized source state
 * @bytes: Prospective nominal packet size
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 *
 * Return: Nonzero when the packet may be sent, otherwise 0.
 */
int uib_dcqcn_can_send(const struct uib_dcqcn *dcqcn, size_t bytes,
		       uint64_t now_ns)
{
	return dcqcn && bytes && now_ns >= dcqcn->next_send_ns;
}

/**
 * uib_dcqcn_on_send - Account one paced transmission.
 * @dcqcn: Initialized source state
 * @bytes: Nominal transmitted bytes
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 */
void uib_dcqcn_on_send(struct uib_dcqcn *dcqcn, size_t bytes,
		       uint64_t now_ns)
{
	uint64_t interval;
	uint64_t base;

	if (!dcqcn || !bytes)
		return;
	dcqcn_advance(dcqcn, now_ns);
	base = max_t(uint64_t, now_ns, dcqcn->next_send_ns);
	interval = uib_mul_div_u64(bytes, 1000000000ULL,
				   dcqcn->current_rate);
	if (!interval)
		interval = 1;
	dcqcn->next_send_ns = base > UINT64_MAX - interval ? UINT64_MAX :
		base + interval;
	dcqcn->inflight = dcqcn->inflight > UINT64_MAX - bytes ? UINT64_MAX :
		dcqcn->inflight + bytes;
}

/**
 * uib_dcqcn_on_ack - Apply non-congested acknowledgement feedback.
 * @dcqcn: Initialized source state
 * @bytes: Newly acknowledged bytes
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 */
void uib_dcqcn_on_ack(struct uib_dcqcn *dcqcn, size_t bytes,
		      uint64_t now_ns)
{
	if (!dcqcn)
		return;
	dcqcn->inflight = bytes < dcqcn->inflight ? dcqcn->inflight - bytes : 0;
	dcqcn_advance(dcqcn, now_ns);
}

/**
 * uib_dcqcn_on_cnp - Apply one congestion notification packet.
 * @dcqcn: Initialized source state
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 */
void uib_dcqcn_on_cnp(struct uib_dcqcn *dcqcn, uint64_t now_ns)
{
	uint64_t factor;

	if (!dcqcn)
		return;
	dcqcn_update_alpha(dcqcn, now_ns);
	dcqcn->cnp_seen = 1;
	dcqcn->cnp_events++;
	dcqcn->target_rate = dcqcn->current_rate;
	factor = UIB_DCQCN_SCALE - dcqcn->alpha_q16 / 2;
	dcqcn->current_rate = uib_mul_div_u64(dcqcn->current_rate, factor,
					     UIB_DCQCN_SCALE);
	dcqcn->current_rate = max_t(uint64_t, dcqcn->current_rate,
					      dcqcn->config.minimum_rate);
	dcqcn->recovery_steps = 0;
	dcqcn->last_recovery_ns = now_ns;
}

/**
 * uib_dcqcn_on_loss - Apply loss and remove bytes from flight.
 * @dcqcn: Initialized source state
 * @bytes: Lost nominal bytes
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 */
void uib_dcqcn_on_loss(struct uib_dcqcn *dcqcn, size_t bytes,
		       uint64_t now_ns)
{
	if (!dcqcn)
		return;
	dcqcn->inflight = bytes < dcqcn->inflight ? dcqcn->inflight - bytes : 0;
	uib_dcqcn_on_cnp(dcqcn, now_ns);
}
