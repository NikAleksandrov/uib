/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uib/congestion.h"

struct test_state {
	uint32_t mtu;
	uint32_t inflight;
	uint32_t acknowledgements;
};

static int failures;

static void check(const char *name, int condition)
{
	printf("%-36s %s\n", name, condition ? "PASS" : "FAIL");
	if (!condition)
		failures++;
}

static int test_init(void *state, const void *config, uint32_t mtu,
		     uint64_t now_ns)
{
	struct test_state *test = state;

	test->mtu = mtu;
	return 0;
}

static int test_can_send(const void *state, size_t bytes)
{
	const struct test_state *test = state;

	return test->inflight + bytes <= 2 * test->mtu;
}

static int test_ack_request(const void *state, uint32_t trigger)
{
	const struct test_state *test = state;

	return test->inflight >= trigger;
}

static void test_on_send(void *state, size_t bytes)
{
	struct test_state *test = state;

	test->inflight += bytes;
}

static void test_on_ack(void *state, const struct uib_congestion_ack *ack)
{
	struct test_state *test = state;

	test->inflight -= ack->newly_received;
	test->acknowledgements++;
}

static void test_on_loss(void *state, size_t bytes)
{
	struct test_state *test = state;

	test->inflight -= bytes;
}

static void test_on_nack(void *state,
			 const struct uib_congestion_nack *nack)
{
	test_on_loss(state, nack->bytes);
}

static const struct uib_congestion_ops test_ops = {
	.abi_version = UIB_CONGESTION_ABI_VERSION,
	.name = "test-window",
	.state_size = sizeof(struct test_state),
	.init = test_init,
	.can_send = test_can_send,
	.ack_request = test_ack_request,
	.on_send = test_on_send,
	.on_ack = test_on_ack,
	.on_loss = test_on_loss,
	.on_nack = test_on_nack,
};

int main(void)
{
	struct uib_congestion_ack ack = {
		.newly_received = 1024,
	};
	struct uib_congestion congestion;
	struct test_state *state;

	check("registry.register", !uib_congestion_register(&test_ops));
	check("registry.duplicate",
	      uib_congestion_register(&test_ops) == -EEXIST);
	check("registry.unknown",
	      uib_congestion_init(&congestion, "missing", NULL, 1024, 1) ==
	      -ENOENT);
	check("custom.init",
	      !uib_congestion_init(&congestion, "test-window", NULL, 1024, 1));
	check("custom.name",
	      !strcmp(uib_congestion_name(&congestion), "test-window"));
	check("custom.enabled", uib_congestion_enabled(&congestion));
	check("custom.empty", uib_congestion_can_send(&congestion, 1024));
	uib_congestion_on_send(&congestion, 1024);
	uib_congestion_on_send(&congestion, 1024);
	check("custom.full", !uib_congestion_can_send(&congestion, 1));
	check("custom.ack-request",
	      uib_congestion_ack_request(&congestion, 2048));
	uib_congestion_on_ack(&congestion, &ack);
	state = (struct test_state *)congestion.state.bytes;
	check("custom.ack", state->inflight == 1024 &&
	      state->acknowledgements == 1);
	uib_congestion_on_loss(&congestion, 1024);
	check("custom.loss", state->inflight == 0);

	check("none.init",
	      !uib_congestion_init(&congestion, "none", NULL, 1024, 1));
	check("none.disabled", !uib_congestion_enabled(&congestion));
	check("none.unlimited", uib_congestion_can_send(&congestion, SIZE_MAX));

	return failures ? 1 : 0;
}
