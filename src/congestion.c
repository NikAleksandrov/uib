/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "uib/congestion.h"
#include "uib/dcqcn.h"
#include "uib/env.h"
#include "uib/uet_rccc.h"
#include "uib/uet_nscc.h"
#include "congestion_internal.h"
#include "utils.h"

#define UIB_CONGESTION_ALGORITHMS_MAX 16

static const struct uib_congestion_ops *registered[UIB_CONGESTION_ALGORITHMS_MAX];
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

int uib_dcqcn_config_from_env(struct uib_dcqcn_config *config, uint32_t mtu)
{
	uint64_t alpha;
	int rc;

	uib_dcqcn_config_default(config, mtu);
	rc = uib_env_u64(UIB_ENV_DCQCN_INITIAL_RATE,
			 &config->initial_rate);
	if (rc)
		return rc;
	rc = uib_env_u64(UIB_ENV_DCQCN_MINIMUM_RATE,
			 &config->minimum_rate);
	if (rc)
		return rc;
	rc = uib_env_u64(UIB_ENV_DCQCN_MAXIMUM_RATE,
			 &config->maximum_rate);
	if (rc)
		return rc;
	rc = uib_env_u64(UIB_ENV_DCQCN_ADDITIVE_INCREASE,
			 &config->additive_increase);
	if (rc)
		return rc;
	rc = uib_env_u64(UIB_ENV_DCQCN_RECOVERY_INTERVAL,
			 &config->recovery_interval_ns);
	if (rc)
		return rc;
	alpha = config->alpha_gain_q16;
	rc = uib_env_u64(UIB_ENV_DCQCN_ALPHA_GAIN, &alpha);
	if (rc || alpha > UINT32_MAX)
		return -EINVAL;
	config->alpha_gain_q16 = (uint32_t)alpha;
	return 0;
}

int uib_uet_rccc_config_from_env(struct uib_uet_rccc_config *config,
				 uint32_t mtu)
{
	uint64_t max_wnd;
	uint64_t quantum;
	uint64_t link_rate;
	int rc;

	uib_uet_rccc_config_default(config, mtu);
	max_wnd = config->max_wnd;
	quantum = config->credit_quantum;
	link_rate = config->receiver_linkspeed;
	rc = uib_env_u64(UIB_ENV_UET_RCCC_MAX_WND, &max_wnd);
	if (rc)
		return rc;
	rc = uib_env_u64(UIB_ENV_UET_RCCC_CREDIT_QUANTUM, &quantum);
	if (rc)
		return rc;
	rc = uib_env_u64(UIB_ENV_UET_RCCC_LINK_RATE, &link_rate);
	if (rc)
		return rc;
	rc = uib_env_u64(UIB_ENV_UET_RCCC_BASE_RTT,
			 &config->base_rtt_ns);
	if (rc)
		return rc;
	if (max_wnd > UINT32_MAX || quantum > UINT32_MAX)
		return -EINVAL;
	config->max_wnd = (uint32_t)max_wnd;
	config->credit_quantum = (uint32_t)quantum;
	config->sender_linkspeed = link_rate;
	config->receiver_linkspeed = link_rate;
	config->credit_interval_ns =
		((uint64_t)config->credit_quantum * 1000000000ULL +
		 link_rate - 1) / link_rate;
	if (!config->credit_interval_ns)
		config->credit_interval_ns = 1;
	return 0;
}

static int none_init(void *state, const void *config, uint32_t mtu,
		     uint64_t now_ns)
{
	return 0;
}

static int none_can_send(const void *state, size_t bytes)
{
	return 1;
}

static int none_ack_request(const void *state, uint32_t trigger)
{
	return 0;
}

static void none_on_send(void *state, size_t bytes)
{
}

static void none_on_ack(void *state, const struct uib_congestion_ack *ack)
{
}

static void none_on_loss(void *state, size_t bytes)
{
}

static void none_on_nack(void *state, const struct uib_congestion_nack *nack)
{
}

static const struct uib_congestion_ops none_ops = {
	.abi_version = UIB_CONGESTION_ABI_VERSION,
	.name = "none",
	.init = none_init,
	.can_send = none_can_send,
	.ack_request = none_ack_request,
	.on_send = none_on_send,
	.on_ack = none_on_ack,
	.on_loss = none_on_loss,
	.on_nack = none_on_nack,
};

static int nscc_init(void *state, const void *configuration, uint32_t mtu,
		     uint64_t now_ns)
{
	struct uib_uet_nscc_config config;

	if (configuration) {
		config = *(const struct uib_uet_nscc_config *)configuration;
	} else {
		uib_uet_nscc_config_default(&config, mtu);
	}
	if (!config.mtu || config.max_wnd < config.mtu)
		return -EINVAL;
	uib_uet_nscc_init(state, &config, now_ns);
	return 0;
}

static int nscc_can_send(const void *state, size_t bytes)
{
	return uib_uet_nscc_can_send(state, bytes);
}

static int nscc_ack_request(const void *state, uint32_t trigger)
{
	return uib_uet_nscc_ack_request(state, trigger);
}

static void nscc_on_send(void *state, size_t bytes)
{
	uib_uet_nscc_on_send(state, bytes);
}

static void nscc_on_ack(void *state, const struct uib_congestion_ack *event)
{
	struct uib_uet_nscc_ack ack = {
		.ack_time_ns = event->ack_time_ns,
		.tx_time_ns = event->tx_time_ns,
		.newly_received = event->newly_received,
		.service_time_ns = event->service_time_ns,
		.tx_retries = event->tx_retries,
		.retransmit = event->retransmit,
		.ecn_marked = event->ecn_marked,
		.receiver_cwnd_penalty = event->receiver_cwnd_penalty,
		.restore_cwnd = event->restore_cwnd,
	};

	uib_uet_nscc_on_ack(state, &ack);
}

static void nscc_on_loss(void *state, size_t bytes)
{
	uib_uet_nscc_on_inferred_loss(state, bytes);
}

static void nscc_on_nack(void *state,
			 const struct uib_congestion_nack *event)
{
	uib_uet_nscc_on_nack(state, event->bytes, event->trimmed,
			     event->last_hop, event->receiver_controlled,
			     event->now_ns);
}

static const struct uib_congestion_ops nscc_ops = {
	.abi_version = UIB_CONGESTION_ABI_VERSION,
	.name = "nscc",
	.state_size = sizeof(struct uib_uet_nscc),
	.init = nscc_init,
	.can_send = nscc_can_send,
	.ack_request = nscc_ack_request,
	.on_send = nscc_on_send,
	.on_ack = nscc_on_ack,
	.on_loss = nscc_on_loss,
	.on_nack = nscc_on_nack,
};

static int dcqcn_init(void *state, const void *configuration, uint32_t mtu,
		       uint64_t now_ns)
{
	struct uib_dcqcn_config config;

	if (configuration)
		config = *(const struct uib_dcqcn_config *)configuration;
	else
		uib_dcqcn_config_default(&config, mtu);
	return uib_dcqcn_init(state, &config, now_ns);
}

static int dcqcn_can_send(const void *state, size_t bytes)
{
	return uib_dcqcn_can_send(state, bytes, uib_now_ns());
}

static int dcqcn_ack_request(const void *state, uint32_t trigger)
{
	const struct uib_dcqcn *dcqcn = state;

	return dcqcn->inflight >= trigger;
}

static void dcqcn_on_send(void *state, size_t bytes)
{
	uib_dcqcn_on_send(state, bytes, uib_now_ns());
}

static void dcqcn_on_ack(void *state,
			 const struct uib_congestion_ack *event)
{
	struct uib_dcqcn *dcqcn = state;

	uib_dcqcn_on_ack(dcqcn, event->newly_received, event->ack_time_ns);
	if (event->ecn_marked)
		uib_dcqcn_on_cnp(dcqcn, event->ack_time_ns);
}

static void dcqcn_on_loss(void *state, size_t bytes)
{
	uib_dcqcn_on_loss(state, bytes, uib_now_ns());
}

static void dcqcn_on_nack(void *state,
			  const struct uib_congestion_nack *event)
{
	uib_dcqcn_on_loss(state, event->bytes, event->now_ns);
}

static const struct uib_congestion_ops dcqcn_ops = {
	.abi_version = UIB_CONGESTION_ABI_VERSION,
	.name = "dcqcn",
	.state_size = sizeof(struct uib_dcqcn),
	.init = dcqcn_init,
	.can_send = dcqcn_can_send,
	.ack_request = dcqcn_ack_request,
	.on_send = dcqcn_on_send,
	.on_ack = dcqcn_on_ack,
	.on_loss = dcqcn_on_loss,
	.on_nack = dcqcn_on_nack,
};

static int rccc_init(void *state, const void *configuration, uint32_t mtu,
		      uint64_t now_ns)
{
	struct uib_uet_rccc_config config;

	if (configuration)
		config = *(const struct uib_uet_rccc_config *)configuration;
	else
		uib_uet_rccc_config_default(&config, mtu);
	return uib_uet_rccc_init(state, &config);
}

static int rccc_can_send(const void *state, size_t bytes)
{
	return uib_uet_rccc_can_send((struct uib_uet_rccc *)state, bytes);
}

static int rccc_ack_request(const void *state, uint32_t trigger)
{
	const struct uib_uet_rccc *rccc = state;

	return rccc->credit <= (int64_t)trigger || !rccc->credit_target_sent;
}

static void rccc_on_send(void *state, size_t bytes)
{
	uib_uet_rccc_on_send(state, bytes);
}

static void rccc_on_ack(void *state,
			const struct uib_congestion_ack *event)
{
	uib_uet_rccc_on_ack(state, event->newly_received);
	if (event->credit_valid)
		uib_uet_rccc_on_credit(state, event->cumulative_credit);
}

static void rccc_on_loss(void *state, size_t bytes)
{
	uib_uet_rccc_on_loss(state, bytes);
}

static void rccc_on_nack(void *state,
			 const struct uib_congestion_nack *event)
{
	uib_uet_rccc_on_loss(state, event->bytes);
}

static const struct uib_congestion_ops rccc_ops = {
	.abi_version = UIB_CONGESTION_ABI_VERSION,
	.name = "rccc",
	.state_size = sizeof(struct uib_uet_rccc),
	.init = rccc_init,
	.can_send = rccc_can_send,
	.ack_request = rccc_ack_request,
	.on_send = rccc_on_send,
	.on_ack = rccc_on_ack,
	.on_loss = rccc_on_loss,
	.on_nack = rccc_on_nack,
};

static const struct uib_congestion_ops *congestion_find(const char *name)
{
	const struct uib_congestion_ops *ops = NULL;
	size_t i;

	if (!strcmp(name, none_ops.name))
		return &none_ops;
	if (!strcmp(name, nscc_ops.name))
		return &nscc_ops;
	if (!strcmp(name, dcqcn_ops.name))
		return &dcqcn_ops;
	if (!strcmp(name, rccc_ops.name))
		return &rccc_ops;
	pthread_mutex_lock(&registry_lock);
	for (i = 0; i < UIB_CONGESTION_ALGORITHMS_MAX; i++) {
		if (registered[i] && !strcmp(name, registered[i]->name)) {
			ops = registered[i];
			break;
		}
	}
	pthread_mutex_unlock(&registry_lock);
	return ops;
}

/**
 * uib_congestion_register - Register an experimental congestion algorithm.
 * @ops: Static operations table that remains valid until process exit
 * @uib-api: Congestion control
 *
 * Make an algorithm available by name to protocol-specific selectors such as
 * `UIB_ROCEV2_CC` and `UIB_UET_CC`. Register algorithms before creating UIB
 * queue pairs.
 *
 * Return: 0 on success, `-EEXIST` for a duplicate name, `-ENOSPC` when the
 * registry is full, or `-EINVAL` for an invalid operations table.
 */
int uib_congestion_register(const struct uib_congestion_ops *ops)
{
	size_t free_slot = UIB_CONGESTION_ALGORITHMS_MAX;
	size_t i;
	int rc = 0;

	if (!ops || ops->abi_version != UIB_CONGESTION_ABI_VERSION ||
	    !ops->name || !ops->name[0] ||
	    ops->state_size > UIB_CONGESTION_STATE_SIZE || !ops->init ||
	    !ops->can_send || !ops->ack_request || !ops->on_send ||
	    !ops->on_ack || !ops->on_loss || !ops->on_nack)
		return -EINVAL;
	if (!strcmp(ops->name, "none") || !strcmp(ops->name, "nscc") ||
	    !strcmp(ops->name, "dcqcn") || !strcmp(ops->name, "rccc"))
		return -EEXIST;
	pthread_mutex_lock(&registry_lock);
	for (i = 0; i < UIB_CONGESTION_ALGORITHMS_MAX; i++) {
		if (!registered[i] && free_slot == UIB_CONGESTION_ALGORITHMS_MAX)
			free_slot = i;
		else if (registered[i] && !strcmp(ops->name,
						 registered[i]->name)) {
			rc = -EEXIST;
			goto out;
		}
	}
	if (free_slot == UIB_CONGESTION_ALGORITHMS_MAX) {
		rc = -ENOSPC;
		goto out;
	}
	registered[free_slot] = ops;
out:
	pthread_mutex_unlock(&registry_lock);
	return rc;
}

/**
 * uib_congestion_init - Select and initialize a congestion algorithm.
 * @congestion: Per-flow congestion-control object to initialize
 * @name: Registered algorithm name
 * @config: Optional algorithm-specific configuration
 * @mtu: Protocol packet size used to derive default settings
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Congestion control
 *
 * Return: 0 on success, `-ENOENT` if @name is not registered, or a negative
 * algorithm-specific error code.
 */
int uib_congestion_init(struct uib_congestion *congestion, const char *name,
			const void *config, uint32_t mtu, uint64_t now_ns)
{
	const struct uib_congestion_ops *ops;
	int rc;

	if (!congestion || !name || !mtu)
		return -EINVAL;
	ops = congestion_find(name);
	if (!ops)
		return -ENOENT;
	memset(congestion, 0, sizeof(*congestion));
	rc = ops->init(congestion->state.bytes, config, mtu, now_ns);
	if (rc)
		return rc;
	congestion->ops = ops;
	return 0;
}

/**
 * uib_congestion_name - Read the selected congestion algorithm name.
 * @congestion: Initialized congestion-control object
 * @uib-api: Congestion control
 *
 * Return: The registered algorithm name, or `NULL` if uninitialized.
 */
const char *uib_congestion_name(const struct uib_congestion *congestion)
{
	return congestion && congestion->ops ? congestion->ops->name : NULL;
}

/**
 * uib_congestion_enabled - Test whether congestion control is active.
 * @congestion: Initialized congestion-control object
 * @uib-api: Congestion control
 *
 * Return: Nonzero for an active algorithm, otherwise 0.
 */
int uib_congestion_enabled(const struct uib_congestion *congestion)
{
	const char *name = uib_congestion_name(congestion);

	return name && strcmp(name, "none");
}

/**
 * uib_congestion_can_send - Test whether a flow may transmit bytes.
 * @congestion: Initialized congestion-control object
 * @bytes: Nominal bytes in the prospective transmission
 * @uib-api: Congestion control
 *
 * Return: Nonzero when the algorithm admits the transmission, otherwise 0.
 */
int uib_congestion_can_send(const struct uib_congestion *congestion,
			    size_t bytes)
{
	return congestion && congestion->ops &&
		congestion->ops->can_send(congestion->state.bytes, bytes);
}

/**
 * uib_congestion_next_send_ns - Read a time-based admission deadline.
 * @congestion: Initialized congestion-control object
 * @uib-api: Congestion control
 *
 * Return: The next monotonic send time, or 0 when the algorithm is not paced.
 */
uint64_t uib_congestion_next_send_ns(
	const struct uib_congestion *congestion)
{
	if (!congestion || congestion->ops != &dcqcn_ops)
		return 0;
	return ((const struct uib_dcqcn *)congestion->state.bytes)->next_send_ns;
}

/**
 * uib_congestion_ack_request - Test whether a packet should request an ACK.
 * @congestion: Initialized congestion-control object
 * @trigger: Protocol-specific ACK generation threshold
 * @uib-api: Congestion control
 *
 * Return: Nonzero when the packet should request an ACK, otherwise 0.
 */
int uib_congestion_ack_request(const struct uib_congestion *congestion,
			       uint32_t trigger)
{
	return congestion && congestion->ops &&
		congestion->ops->ack_request(congestion->state.bytes, trigger);
}

/**
 * uib_congestion_on_send - Report newly transmitted bytes.
 * @congestion: Initialized congestion-control object
 * @bytes: Number of newly transmitted nominal bytes
 * @uib-api: Congestion control
 */
void uib_congestion_on_send(struct uib_congestion *congestion, size_t bytes)
{
	if (congestion && congestion->ops)
		congestion->ops->on_send(congestion->state.bytes, bytes);
}

/**
 * uib_congestion_on_ack - Report sender and receiver ACK feedback.
 * @congestion: Initialized congestion-control object
 * @ack: Protocol-neutral ACK event
 * @uib-api: Congestion control
 */
void uib_congestion_on_ack(struct uib_congestion *congestion,
			   const struct uib_congestion_ack *ack)
{
	if (congestion && congestion->ops && ack)
		congestion->ops->on_ack(congestion->state.bytes, ack);
}

/**
 * uib_congestion_on_loss - Report inferred packet loss.
 * @congestion: Initialized congestion-control object
 * @bytes: Nominal bytes removed from flight
 * @uib-api: Congestion control
 */
void uib_congestion_on_loss(struct uib_congestion *congestion, size_t bytes)
{
	if (congestion && congestion->ops)
		congestion->ops->on_loss(congestion->state.bytes, bytes);
}

/**
 * uib_congestion_on_nack - Report explicit negative acknowledgement feedback.
 * @congestion: Initialized congestion-control object
 * @nack: Protocol-neutral NACK event
 * @uib-api: Congestion control
 */
void uib_congestion_on_nack(struct uib_congestion *congestion,
			    const struct uib_congestion_nack *nack)
{
	if (congestion && congestion->ops && nack)
		congestion->ops->on_nack(congestion->state.bytes, nack);
}
