/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uib/congestion.h"
#include "uib/dcqcn.h"
#include "uib/uet_rccc.h"

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

static void test_dcqcn(void)
{
	struct uib_dcqcn_config config;
	struct uib_dcqcn dcqcn;
	uint64_t initial_rate;

	uib_dcqcn_config_default(&config, 1000);
	config.initial_rate = 1000000000ULL;
	config.minimum_rate = 100000000ULL;
	config.maximum_rate = 1000000000ULL;
	config.additive_increase = 100000000ULL;
	config.recovery_interval_ns = 1000;
	config.alpha_gain_q16 = UIB_DCQCN_SCALE / 2;
	check("dcqcn.init", !uib_dcqcn_init(&dcqcn, &config, 100));
	initial_rate = dcqcn.current_rate;
	check("dcqcn.initial-admission",
	      uib_dcqcn_can_send(&dcqcn, 1000, 100));
	uib_dcqcn_on_send(&dcqcn, 1000, 100);
	check("dcqcn.pacing", !uib_dcqcn_can_send(&dcqcn, 1000, 1099) &&
	      uib_dcqcn_can_send(&dcqcn, 1000, 1100));
	uib_dcqcn_on_cnp(&dcqcn, 1100);
	check("dcqcn.cnp-decrease",
	      dcqcn.current_rate < initial_rate && dcqcn.cnp_events == 1);
	uib_dcqcn_on_ack(&dcqcn, 1000, 7100);
	check("dcqcn.recovery", dcqcn.inflight == 0 &&
	      dcqcn.current_rate > initial_rate / 2 &&
	      dcqcn.current_rate <= config.maximum_rate);
	uib_dcqcn_on_loss(&dcqcn, 1000, 7200);
	check("dcqcn.loss-accounting",
	      dcqcn.inflight == 0 && dcqcn.cnp_events == 1);
}

static void test_rccc(void)
{
	struct uib_uet_rccc_destination destination;
	struct uib_uet_rccc_config config;
	struct uib_uet_rccc rccc;
	uint32_t credit;
	uint32_t target;
	int i;

	uib_uet_rccc_config_default(&config, 1024);
	config.max_wnd = 4096;
	config.credit_quantum = 2048;
	config.credit_interval_ns = 1000;
	check("rccc.init", !uib_uet_rccc_init(&rccc, &config));
	uib_uet_rccc_on_new_data(&rccc, 8192);
	for (i = 0; i < 4; i++) {
		check("rccc.speculative-admission",
		      uib_uet_rccc_can_send(&rccc, 1024));
		uib_uet_rccc_on_send(&rccc, 1024);
	}
	check("rccc.credit-stall", !uib_uet_rccc_can_send(&rccc, 1024));
	target = uib_uet_rccc_credit_target(&rccc);
	check("rccc.credit-target", target != 0);
	check("rccc.destination-init",
	      !uib_uet_rccc_destination_init(&destination, &config, 100));
	credit = uib_uet_rccc_destination_update(&destination, target, 1100);
	check("rccc.destination-grant", credit != 0 &&
	      destination.grants == 1);
	uib_uet_rccc_on_ack(&rccc, 1024);
	uib_uet_rccc_on_credit(&rccc, credit);
	check("rccc.credit-update", !rccc.speculating &&
	      rccc.credit_updates == 1 &&
	      uib_uet_rccc_can_send(&rccc, 1024));
	uib_uet_rccc_on_send(&rccc, 1024);
	uib_uet_rccc_on_loss(&rccc, 1024);
	check("rccc.loss-backlog", rccc.retransmit_backlog == 1024);
}

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
	check("builtin.dcqcn",
	      !uib_congestion_init(&congestion, "dcqcn", NULL, 1024, 1));
	check("builtin.rccc",
	      !uib_congestion_init(&congestion, "rccc", NULL, 1024, 1));
	test_dcqcn();
	test_rccc();

	return failures ? 1 : 0;
}
