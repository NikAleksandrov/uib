#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uib/uet_nscc.h"

static int failures;

static void check(const char *name, int condition)
{
	printf("%-36s %s\n", name, condition ? "PASS" : "FAIL");
	if (!condition)
		failures++;
}

int main(void)
{
	struct uib_uet_nscc_config config;
	struct uib_uet_nscc_ack ack;
	struct uib_uet_nscc nscc;
	uint64_t initial;

	uib_uet_nscc_config_default(&config, 4096);
	check("config.bdp", config.bdp == 150000);
	check("config.max_wnd", config.max_wnd == 225000);
	check("config.target_qdelay", config.target_qdelay_ns == 12000);
	uib_uet_nscc_init(&nscc, &config, 1000000);
	initial = nscc.cwnd;
	check("init.cwnd", initial == config.max_wnd);
	check("gate.empty", uib_uet_nscc_can_send(&nscc, 4096));

	uib_uet_nscc_on_send(&nscc, 4096);
	check("send.inflight", nscc.inflight == 4096);
	memset(&ack, 0, sizeof(ack));
	ack.tx_time_ns = 1000000;
	ack.ack_time_ns = 1010000;
	ack.newly_received = 4096;
	uib_uet_nscc_on_ack(&nscc, &ack);
	check("ack.inflight", nscc.inflight == 0);
	check("ack.base_rtt", nscc.base_rtt_ns == 10000);
	check("ack.max_wnd_scaled", nscc.max_wnd == 187500);
	uib_uet_nscc_on_send(&nscc, 73);
	memset(&ack, 0, sizeof(ack));
	ack.tx_retries = 2;
	ack.newly_received = 256;
	uib_uet_nscc_on_ack(&nscc, &ack);
	check("ack.signed-byte-clock", nscc.inflight == -183);
	nscc.inflight = 0;

	nscc.cwnd = 32768;
	nscc.inc_bytes = 0;
	nscc.received_bytes = 0;
	nscc.last_adjust_ns = 1030000;
	uib_uet_nscc_on_send(&nscc, 8192);
	memset(&ack, 0, sizeof(ack));
	ack.tx_time_ns = 1020000;
	ack.ack_time_ns = 1030000;
	ack.newly_received = 8192;
	ack.receiver_cwnd_penalty = 64;
	uib_uet_nscc_on_ack(&nscc, &ack);
	check("receiver.penalty", nscc.cwnd == 28672);
	check("receiver.saved", nscc.saved_cwnd == 32768);

	memset(&ack, 0, sizeof(ack));
	ack.tx_time_ns = 1040000;
	ack.ack_time_ns = 1050000;
	ack.tx_retries = 2;
	ack.restore_cwnd = 1;
	uib_uet_nscc_on_ack(&nscc, &ack);
	check("receiver.restore", nscc.cwnd == 32768);

	nscc.cwnd = 16384;
	nscc.inflight = 8192;
	uib_uet_nscc_on_inferred_loss(&nscc, 4096);
	check("loss.cwnd", nscc.cwnd == 12288);
	check("loss.inflight", nscc.inflight == 4096);
	check("gate.full", !uib_uet_nscc_can_send(&nscc, 12288));
	check("ack.request", uib_uet_nscc_ack_request(&nscc, 16384));

	return failures ? 1 : 0;
}
