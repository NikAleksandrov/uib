/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
#include <stdint.h>

#include "uib/congestion.h"

struct pass_state {
	uint64_t transmitted;
};

static int pass_init(void *state, const void *config, uint32_t mtu,
		     uint64_t now_ns)
{
	return 0;
}

static int pass_can_send(const void *state, size_t bytes)
{
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
}

static void pass_on_ack(void *state, const struct uib_congestion_ack *ack)
{
}

static void pass_on_loss(void *state, size_t bytes)
{
}

static void pass_on_nack(void *state,
			 const struct uib_congestion_nack *nack)
{
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
