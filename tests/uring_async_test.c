/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "uib/uib.h"

#define TEST_PACKETS 256
#define TEST_GSO_PACKETS 48
#define TEST_PACKET_LENGTH 256

static int failed;
static unsigned int released;

static void release_packet(void *opaque)
{
	unsigned int *counter = opaque;

	__atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
}

static void check(const char *name, int condition)
{
	printf("%-32s %s\n", name, condition ? "OK" : "FAIL");
	if (!condition)
		failed = 1;
}

static int wait_for_traffic(struct uib_context *sender,
			    struct uib_context *receiver,
			    struct uib_transport_stats *tx_stats,
			    struct uib_transport_stats *rx_stats,
			    unsigned int expected)
{
	struct timespec delay;
	unsigned int i;

	delay.tv_sec = 0;
	delay.tv_nsec = 1000000;
	for (i = 0; i < 3000; i++) {
		if (uib_transport_get_stats(sender, tx_stats) < 0 ||
		    uib_transport_get_stats(receiver, rx_stats) < 0)
			return -1;
		if (tx_stats->tx_completed == expected &&
		    rx_stats->rx_packets == expected)
			return 0;
		nanosleep(&delay, NULL);
	}
	return -1;
}

static void run_mode(const char *mode, uint16_t sender_port,
		     uint16_t receiver_port, enum uib_uring_rx_mode expected,
		     int zerocopy)
{
	struct uib_zerocopy_stats zerocopy_stats;
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
	setenv("UIB_ZEROCOPY", zerocopy ? "on" : "off", 1);
	setenv("UIB_ZEROCOPY_THRESHOLD", "1", 1);
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
	rc = wait_for_traffic(sender, receiver, &tx_stats, &rx_stats,
			      TEST_PACKETS);
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
	check("uring.sqpoll_request",
	      tx_stats.sqpoll_requested ==
	      (!strcmp(getenv("UIB_URING_SQPOLL"), "on") ? 1U : 0U));
	memset(&zerocopy_stats, 0, sizeof(zerocopy_stats));
	rc = uib_zerocopy_get_stats(sender, &zerocopy_stats);
	check("uring.zerocopy_accounting", !rc &&
	      zerocopy_stats.copied_calls + zerocopy_stats.zerocopy_calls ==
	      TEST_PACKETS);
	if (zerocopy && zerocopy_stats.zerocopy_calls) {
		check("uring.zerocopy_completions",
		      zerocopy_stats.completions ==
		      zerocopy_stats.zerocopy_calls);
		check("uring.deferred_copy_bound",
		      zerocopy_stats.deferred_copies <=
		      zerocopy_stats.completions);
		check("uring.loopback_deferred",
		      zerocopy_stats.deferred_copies ==
		      zerocopy_stats.completions);
		check("uring.zerocopy_capability",
		      zerocopy_stats.capabilities & UIB_ZEROCOPY_CAP_MSG);
	}

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
	setenv("UIB_URING_SQPOLL", "invalid", 1);
	context = uib_open_device_protocol("127.0.0.1", 19610, "dummy");
	check("uring.invalid_sqpoll", !context);
	if (context)
		uib_close_device(context);
	setenv("UIB_URING_SQPOLL", "off", 1);
	setenv("UIB_UDP_GSO", "invalid", 1);
	context = uib_open_device_protocol("127.0.0.1", 19613, "dummy");
	check("uring.invalid_udp_gso", !context);
	if (context)
		uib_close_device(context);
	setenv("UIB_UDP_GSO", "auto", 1);
}

static void run_gso_batch(uint16_t sender_port, uint16_t receiver_port,
			  int zerocopy)
{
	struct uib_zerocopy_stats zerocopy_stats;
	struct uib_transport_ref refs[TEST_GSO_PACKETS];
	struct uib_transport_stats rx_stats;
	struct uib_transport_stats tx_stats;
	struct uib_context *receiver;
	struct uib_context *sender;
	struct sockaddr_in destination;
	uint8_t packets[TEST_GSO_PACKETS][TEST_PACKET_LENGTH];
	uint8_t *zc_storage = NULL;
	void *allocation = NULL;
	size_t page_size = 0;
	size_t stride = 0;
	long system_page_size;
	unsigned int i;
	int rc;

	setenv("UIB_UDP_GSO", "auto", 1);
	setenv("UIB_ZEROCOPY", zerocopy ? "on" : "off", 1);
	setenv("UIB_ZEROCOPY_THRESHOLD", "1", 1);
	if (zerocopy) {
		system_page_size = sysconf(_SC_PAGESIZE);
		page_size = system_page_size > 0 ?
			(size_t)system_page_size : 4096U;
		stride = page_size * 2;
		rc = posix_memalign(&allocation, page_size,
				    TEST_GSO_PACKETS * stride);
		check("gso.zc_storage", !rc);
		if (rc)
			return;
		zc_storage = allocation;
	}
	sender = uib_open_device_protocol("127.0.0.1", sender_port, "dummy");
	receiver = uib_open_device_protocol("127.0.0.1", receiver_port,
					    "dummy");
	check("gso.contexts", sender && receiver);
	if (!sender || !receiver)
		goto out;
	destination = receiver->local_addr;
	memset(packets[0], 0x5a, sizeof(packets[0]));
	rc = uib_transport_send(sender, &destination, packets[0],
				sizeof(packets[0]));
	check("gso.warmup", !rc);
	memset(&tx_stats, 0, sizeof(tx_stats));
	memset(&rx_stats, 0, sizeof(rx_stats));
	rc = wait_for_traffic(sender, receiver, &tx_stats, &rx_stats, 1);
	check("gso.warmup_completion", !rc);
	released = 0;
	for (i = 0; i < TEST_GSO_PACKETS; i++) {
		if (zerocopy) {
			/* Make every test packet consume two skb fragments. */
			refs[i].buf = zc_storage + i * stride + page_size -
				TEST_PACKET_LENGTH / 2;
			memset((void *)refs[i].buf, (int)i,
			       TEST_PACKET_LENGTH);
		} else {
			memset(packets[i], (int)i, sizeof(packets[i]));
			refs[i].buf = packets[i];
		}
		refs[i].dst = &destination;
		refs[i].release = release_packet;
		refs[i].opaque = &released;
		refs[i].len = sizeof(packets[i]);
		refs[i].entropy = (uint16_t)(i + 1);
	}
	rc = uib_transport_send_ref_batch(sender, refs, TEST_GSO_PACKETS);
	check("gso.enqueue_batch", !rc);
	memset(&tx_stats, 0, sizeof(tx_stats));
	memset(&rx_stats, 0, sizeof(rx_stats));
	rc = wait_for_traffic(sender, receiver, &tx_stats, &rx_stats,
			      TEST_GSO_PACKETS + 1);
	if (rc) {
		fprintf(stderr, "gso timeout: tx=%llu errors=%llu rx=%llu\n",
			(unsigned long long)tx_stats.tx_completed,
			(unsigned long long)tx_stats.tx_errors,
			(unsigned long long)rx_stats.rx_packets);
		fprintf(stderr, "gso timeout: batches=%llu packets=%llu\n",
			(unsigned long long)tx_stats.tx_gso_batches,
			(unsigned long long)tx_stats.tx_gso_packets);
	}
	check("gso.async_completion", !rc);
	check("gso.release_callbacks", released == TEST_GSO_PACKETS);
	check("gso.packet_counters",
	      tx_stats.tx_completed == TEST_GSO_PACKETS + 1 &&
	      rx_stats.rx_packets == TEST_GSO_PACKETS + 1);
	if (tx_stats.gso_enabled) {
		check("gso.batch_counters", tx_stats.tx_gso_batches > 0 &&
		      tx_stats.tx_gso_packets == TEST_GSO_PACKETS);
	} else {
		printf("%-32s %s\n", "gso.batch_counters", "SKIP");
	}
	if (zerocopy) {
		memset(&zerocopy_stats, 0, sizeof(zerocopy_stats));
		rc = uib_zerocopy_get_stats(sender, &zerocopy_stats);
		check("gso.zc_stats", !rc);
		if (zerocopy_stats.capabilities & UIB_ZEROCOPY_CAP_MSG) {
			check("gso.zc_calls", zerocopy_stats.zerocopy_calls > 0 &&
			      !zerocopy_stats.copied_calls);
			check("gso.zc_completions",
			      zerocopy_stats.completions ==
			      zerocopy_stats.zerocopy_calls);
		} else {
			printf("%-32s %s\n", "gso.zc_calls", "SKIP");
			printf("%-32s %s\n", "gso.zc_completions", "SKIP");
		}
	}

out:
	if (sender)
		uib_close_device(sender);
	if (receiver)
		uib_close_device(receiver);
	free(allocation);
}

static void run_iov_batch(uint16_t sender_port, uint16_t receiver_port,
			  int check_gso)
{
	struct uib_transport_iov_ref refs[TEST_GSO_PACKETS];
	struct uib_transport_stats rx_stats;
	struct uib_transport_stats tx_stats;
	struct uib_context *receiver;
	struct uib_context *sender;
	struct sockaddr_in destination;
	struct iovec vectors[TEST_GSO_PACKETS][2];
	uint8_t packets[TEST_GSO_PACKETS][TEST_PACKET_LENGTH];
	uint8_t warmup[TEST_PACKET_LENGTH];
	unsigned int i;
	int rc;

	setenv("UIB_UDP_GSO", "auto", 1);
	setenv("UIB_ZEROCOPY", "off", 1);
	sender = uib_open_device_protocol("127.0.0.1", sender_port, "dummy");
	receiver = uib_open_device_protocol("127.0.0.1", receiver_port,
					    "dummy");
	check("iov.contexts", sender && receiver);
	if (!sender || !receiver)
		goto out;
	destination = receiver->local_addr;
	memset(warmup, 0xa5, sizeof(warmup));
	rc = uib_transport_send(sender, &destination, warmup, sizeof(warmup));
	check("iov.warmup", !rc);
	memset(&tx_stats, 0, sizeof(tx_stats));
	memset(&rx_stats, 0, sizeof(rx_stats));
	rc = wait_for_traffic(sender, receiver, &tx_stats, &rx_stats, 1);
	check("iov.warmup_completion", !rc);
	released = 0;
	for (i = 0; i < TEST_GSO_PACKETS; i++) {
		memset(packets[i], (int)i, sizeof(packets[i]));
		vectors[i][0].iov_base = packets[i];
		vectors[i][0].iov_len = 37;
		vectors[i][1].iov_base = packets[i] + 37;
		vectors[i][1].iov_len = sizeof(packets[i]) - 37;
		refs[i].dst = &destination;
		refs[i].iov = vectors[i];
		refs[i].release = release_packet;
		refs[i].opaque = &released;
		refs[i].len = sizeof(packets[i]);
		refs[i].iov_count = 2;
		refs[i].entropy = (uint16_t)(i + 1);
	}
	rc = uib_transport_send_iov_ref_batch(sender, refs,
					       TEST_GSO_PACKETS);
	check("iov.enqueue_batch", !rc);
	memset(&tx_stats, 0, sizeof(tx_stats));
	memset(&rx_stats, 0, sizeof(rx_stats));
	rc = wait_for_traffic(sender, receiver, &tx_stats, &rx_stats,
			      TEST_GSO_PACKETS + 1);
	check("iov.async_completion", !rc);
	check("iov.release_callbacks", released == TEST_GSO_PACKETS);
	check("iov.packet_counters",
	      tx_stats.tx_completed == TEST_GSO_PACKETS + 1 &&
	      rx_stats.rx_packets == TEST_GSO_PACKETS + 1);
	if (check_gso && tx_stats.gso_enabled) {
		check("iov.gso_counters", tx_stats.tx_gso_batches > 0 &&
		      tx_stats.tx_gso_packets == TEST_GSO_PACKETS);
	} else {
		printf("%-32s %s\n", "iov.gso_counters", "SKIP");
	}

out:
	if (sender)
		uib_close_device(sender);
	if (receiver)
		uib_close_device(receiver);
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
	setenv("UIB_URING_SQPOLL", "off", 1);
	run_mode("multishot", 19600, 19601, UIB_URING_RX_MULTISHOT, 0);
	run_mode("batch", 19602, 19603, UIB_URING_RX_BATCHED, 0);
	run_mode("batch", 19608, 19609, UIB_URING_RX_BATCHED, 1);
	setenv("UIB_URING_SQPOLL", "on", 1);
	run_mode("batch", 19611, 19612, UIB_URING_RX_BATCHED, 0);
	setenv("UIB_URING_SQPOLL", "off", 1);
	run_gso_batch(19614, 19615, 0);
	run_gso_batch(19616, 19617, 1);
	run_iov_batch(19618, 19619, 1);
	setenv("UIB_TRANSPORT_MODE", "udp-sync", 1);
	run_iov_batch(19620, 19621, 1);
	setenv("UIB_TRANSPORT_MODE", "udp-io-uring", 1);
	run_shutdown_drain();
	run_invalid_batch();
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
