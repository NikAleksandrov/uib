// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uib/uib.h"

#define TEST_PACKETS 256
#define TEST_PACKET_LENGTH 256

static int failed;

static void check(const char *name, int condition)
{
	printf("%-32s %s\n", name, condition ? "OK" : "FAIL");
	if (!condition)
		failed = 1;
}

static int wait_for_traffic(struct uib_context *sender,
			    struct uib_context *receiver,
			    struct uib_transport_stats *tx_stats,
			    struct uib_transport_stats *rx_stats)
{
	struct timespec delay;
	unsigned int i;

	delay.tv_sec = 0;
	delay.tv_nsec = 1000000;
	for (i = 0; i < 3000; i++) {
		if (uib_transport_get_stats(sender, tx_stats) < 0 ||
		    uib_transport_get_stats(receiver, rx_stats) < 0)
			return -1;
		if (tx_stats->tx_completed == TEST_PACKETS &&
		    rx_stats->rx_packets == TEST_PACKETS)
			return 0;
		nanosleep(&delay, NULL);
	}
	return -1;
}

static void run_mode(const char *mode, uint16_t sender_port,
		     uint16_t receiver_port, enum uib_uring_rx_mode expected)
{
	struct uib_transport_stats rx_stats;
	struct uib_transport_stats tx_stats;
	struct uib_context *receiver;
	struct uib_context *sender;
	struct sockaddr_in destination;
	uint8_t packet[TEST_PACKET_LENGTH];
	unsigned int i;
	int rc = 0;

	setenv("UIB_URING_RX_MODE", mode, 1);
	setenv("UIB_URING_RX_BATCH", "17", 1);
	setenv("UIB_URING_TX_BATCH", "7", 1);
	setenv("UIB_ZEROCOPY", "off", 1);
	sender = uib_open_device_protocol("127.0.0.1", sender_port, "dummy");
	receiver = uib_open_device_protocol("127.0.0.1", receiver_port,
					    "dummy");
	check("uring.contexts", sender && receiver);
	if (!sender || !receiver)
		goto out;
	memset(packet, 0xa5, sizeof(packet));
	destination = receiver->local_addr;
	for (i = 0; i < TEST_PACKETS; i++) {
		packet[0] = (uint8_t)i;
		if (uib_transport_send(sender, &destination, packet,
				       sizeof(packet)) < 0) {
			rc = -1;
			break;
		}
	}
	check("uring.enqueue_burst", !rc);
	memset(&tx_stats, 0, sizeof(tx_stats));
	memset(&rx_stats, 0, sizeof(rx_stats));
	rc = wait_for_traffic(sender, receiver, &tx_stats, &rx_stats);
	check("uring.async_completion", !rc);
	check("uring.tx_queued", tx_stats.tx_queued == TEST_PACKETS);
	check("uring.tx_submitted", tx_stats.tx_submitted == TEST_PACKETS);
	check("uring.tx_completed", tx_stats.tx_completed == TEST_PACKETS);
	check("uring.tx_errors", !tx_stats.tx_errors);
	check("uring.rx_packets", rx_stats.rx_packets == TEST_PACKETS);
	check("uring.rx_batches", rx_stats.rx_completion_batches > 0 &&
	      rx_stats.rx_largest_batch > 0);
	check("uring.rx_mode", rx_stats.rx_mode == expected);
	check("uring.runtime_batches", rx_stats.rx_batch == 17 &&
	      tx_stats.tx_batch == 7);

out:
	if (sender)
		uib_close_device(sender);
	if (receiver)
		uib_close_device(receiver);
}

static void run_invalid_batch(void)
{
	struct uib_context *context;

	setenv("UIB_URING_RX_BATCH", "0", 1);
	context = uib_open_device_protocol("127.0.0.1", 19606, "dummy");
	check("uring.invalid_rx_batch", !context);
	if (context)
		uib_close_device(context);
	setenv("UIB_URING_RX_BATCH", "64", 1);
	setenv("UIB_URING_TX_BATCH", "257", 1);
	context = uib_open_device_protocol("127.0.0.1", 19607, "dummy");
	check("uring.invalid_tx_batch", !context);
	if (context)
		uib_close_device(context);
	setenv("UIB_URING_TX_BATCH", "64", 1);
}

static void run_shutdown_drain(void)
{
	struct uib_transport_stats stats;
	struct uib_context *receiver;
	struct uib_context *sender;
	struct sockaddr_in destination;
	struct timespec delay;
	uint8_t packet[TEST_PACKET_LENGTH];
	unsigned int i;
	int received = 0;

	setenv("UIB_URING_RX_MODE", "multishot", 1);
	setenv("UIB_ZEROCOPY", "off", 1);
	sender = uib_open_device_protocol("127.0.0.1", 19604, "dummy");
	receiver = uib_open_device_protocol("127.0.0.1", 19605, "dummy");
	check("drain.contexts", sender && receiver);
	if (!sender || !receiver)
		goto out;
	memset(packet, 0x3c, sizeof(packet));
	destination = receiver->local_addr;
	for (i = 0; i < TEST_PACKETS; i++) {
		if (uib_transport_send(sender, &destination, packet,
				       sizeof(packet)) < 0)
			break;
	}
	check("drain.enqueue_burst", i == TEST_PACKETS);
	uib_close_device(sender);
	sender = NULL;
	delay.tv_sec = 0;
	delay.tv_nsec = 1000000;
	for (i = 0; i < 3000; i++) {
		if (!uib_transport_get_stats(receiver, &stats) &&
		    stats.rx_packets == TEST_PACKETS) {
			received = 1;
			break;
		}
		nanosleep(&delay, NULL);
	}
	check("drain.all_packets", received);

out:
	if (sender)
		uib_close_device(sender);
	if (receiver)
		uib_close_device(receiver);
}

int main(void)
{
	setenv("UIB_TRANSPORT_MODE", "udp-io-uring", 1);
	run_mode("multishot", 19600, 19601, UIB_URING_RX_MULTISHOT);
	run_mode("batch", 19602, 19603, UIB_URING_RX_BATCHED);
	run_shutdown_drain();
	run_invalid_batch();
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
