/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
#include <stdint.h>

#include "uib/congestion.h"
#include "congestion_plugin.h"

struct pass_state {
	uint64_t transmitted;
};

static struct uib_test_congestion_stats plugin_stats;

static int pass_init(void *state, const void *config, uint32_t mtu,
		     uint64_t now_ns)
{
	__atomic_add_fetch(&plugin_stats.initializations, 1, __ATOMIC_RELAXED);
	return 0;
}

static int pass_can_send(const void *state, size_t bytes)
{
	__atomic_add_fetch(&plugin_stats.admissions, 1, __ATOMIC_RELAXED);
	return 1;
}

static int pass_ack_request(const void *state, uint32_t trigger)
{
	return 0;
}

static void pass_on_send(void *state, size_t bytes)
{
	struct pass_state *pass = state;

	pass->transmitted += bytes;
	__atomic_add_fetch(&plugin_stats.sends, 1, __ATOMIC_RELAXED);
}

static void pass_on_ack(void *state, const struct uib_congestion_ack *ack)
{
	__atomic_add_fetch(&plugin_stats.acknowledgements, 1,
			   __ATOMIC_RELAXED);
}

static void pass_on_loss(void *state, size_t bytes)
{
	__atomic_add_fetch(&plugin_stats.losses, 1, __ATOMIC_RELAXED);
}

static void pass_on_nack(void *state,
			 const struct uib_congestion_nack *nack)
{
	__atomic_add_fetch(&plugin_stats.nacks, 1, __ATOMIC_RELAXED);
}

void uib_test_congestion_get_stats(struct uib_test_congestion_stats *stats)
{
	stats->initializations = __atomic_load_n(&plugin_stats.initializations,
						 __ATOMIC_RELAXED);
	stats->admissions = __atomic_load_n(&plugin_stats.admissions,
					    __ATOMIC_RELAXED);
	stats->sends = __atomic_load_n(&plugin_stats.sends, __ATOMIC_RELAXED);
	stats->acknowledgements =
		__atomic_load_n(&plugin_stats.acknowledgements,
				__ATOMIC_RELAXED);
	stats->losses = __atomic_load_n(&plugin_stats.losses, __ATOMIC_RELAXED);
	stats->nacks = __atomic_load_n(&plugin_stats.nacks, __ATOMIC_RELAXED);
}

static const struct uib_congestion_ops pass_ops = {
	.abi_version = UIB_CONGESTION_ABI_VERSION,
	.name = "test-pass",
	.state_size = sizeof(struct pass_state),
	.init = pass_init,
	.can_send = pass_can_send,
	.ack_request = pass_ack_request,
	.on_send = pass_on_send,
	.on_ack = pass_on_ack,
	.on_loss = pass_on_loss,
	.on_nack = pass_on_nack,
};

static void __attribute__((constructor)) register_pass_algorithm(void)
{
	uib_congestion_register(&pass_ops);
}
