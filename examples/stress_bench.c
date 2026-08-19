#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "direct_common.h"

#define BENCH_RECV_ID (1ULL << 63)
#define BENCH_TIMEOUT_NS (10ULL * 1000 * 1000 * 1000)

struct options {
	const char *protocol;
	uint32_t iterations;
	uint32_t message_size;
	uint32_t window;
};

static uint64_t now_ns(clockid_t clock_id)
{
	struct timespec now;

	clock_gettime(clock_id, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static uint64_t timeval_ns(const struct timeval *time)
{
	return (uint64_t)time->tv_sec * 1000000000ULL +
		(uint64_t)time->tv_usec * 1000;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return a > b ? 1 : a < b ? -1 : 0;
}

static uint64_t percentile(const uint64_t *values, uint32_t count,
			   uint32_t numerator)
{
	uint64_t index = ((uint64_t)(count - 1) * numerator + 99) / 100;

	return values[index];
}

static int parse_u32(const char *text, uint32_t *value)
{
	char *end;
	unsigned long parsed = strtoul(text, &end, 0);

	if (!*text || *end || parsed > UINT32_MAX)
		return -EINVAL;
	*value = (uint32_t)parsed;
	return 0;
}

static int parse_options(int argc, char **argv, struct options *options)
{
	static const struct option long_options[] = {
		{ "protocol", required_argument, NULL, 'p' },
		{ "iterations", required_argument, NULL, 'n' },
		{ "size", required_argument, NULL, 's' },
		{ "window", required_argument, NULL, 'w' },
		{ NULL, 0, NULL, 0 },
	};
	int option;

	*options = (struct options) {
		.protocol = "uet",
		.iterations = 10000,
		.message_size = 4096,
		.window = 64,
	};
	while ((option = getopt_long(argc, argv, "p:n:s:w:", long_options,
				     NULL)) != -1) {
		switch (option) {
		case 'p':
			options->protocol = optarg;
			break;
		case 'n':
			if (parse_u32(optarg, &options->iterations) < 0)
				return -EINVAL;
			break;
		case 's':
			if (parse_u32(optarg, &options->message_size) < 0)
				return -EINVAL;
			break;
		case 'w':
			if (parse_u32(optarg, &options->window) < 0)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
	}
	if (!options->iterations || !options->message_size ||
	    !options->window || options->window > UIB_UET_PDC_MPR ||
	    options->message_size > 60000)
		return -EINVAL;
	if (strcmp(options->protocol, "rocev2") &&
	    strcmp(options->protocol, "uet") &&
	    strcmp(options->protocol, "dummy"))
		return -EINVAL;
	return 0;
}

static void fill_message(uint8_t *buffer, uint32_t length, uint64_t sequence)
{
	memset(buffer, (int)sequence, length);
	if (length >= sizeof(sequence))
		memcpy(buffer, &sequence, sizeof(sequence));
}

static int verify_message(const uint8_t *buffer, uint32_t length,
			  uint64_t sequence)
{
	uint64_t found = 0;

	if (length < sizeof(found))
		return buffer[0] == (uint8_t)sequence ? 0 : -EIO;
	memcpy(&found, buffer, sizeof(found));
	return found == sequence ? 0 : -EIO;
}

static int run_batch(struct direct_endpoint *sender,
		     struct direct_endpoint *receiver,
		     const struct options *options, uint32_t first,
		     uint32_t count, uint64_t *posted_ns, uint64_t *latency_ns)
{
	uint32_t send_completions = 0;
	uint32_t receive_completions = 0;
	uint64_t deadline;
	uint32_t slot;
	int i;

	for (slot = 0; slot < count; slot++) {
		uint32_t sequence = first + slot;
		uint8_t *source = sender->buffer +
			(size_t)slot * options->message_size;
		uint8_t *target = receiver->buffer +
			(size_t)slot * options->message_size;

		fill_message(source, options->message_size, sequence);
		memset(target, 0, options->message_size);
		if (direct_post_receive(receiver, BENCH_RECV_ID | (sequence + 1),
					target, options->message_size) < 0)
			return -EIO;
		posted_ns[sequence] = now_ns(CLOCK_MONOTONIC);
		if (direct_post_send(sender, sequence + 1, UIB_WR_SEND, source,
				     options->message_size, 0, 0) < 0)
			return -EIO;
	}

	deadline = now_ns(CLOCK_MONOTONIC) + BENCH_TIMEOUT_NS;
	while (send_completions < count || receive_completions < count) {
		struct uib_wc completions[32];
		int completed;

		completed = uib_poll_cq(sender->cq, 32, completions);
		for (i = 0; i < completed; i++) {
			uint64_t sequence;

			if (completions[i].status != UIB_WC_SUCCESS ||
			    !completions[i].wr_id ||
			    completions[i].wr_id & BENCH_RECV_ID)
				return -EIO;
			sequence = completions[i].wr_id - 1;
			latency_ns[sequence] = now_ns(CLOCK_MONOTONIC) -
				posted_ns[sequence];
			send_completions++;
		}
		completed = uib_poll_cq(receiver->cq, 32, completions);
		for (i = 0; i < completed; i++) {
			uint64_t sequence;
			uint32_t slot;

			if (completions[i].status != UIB_WC_SUCCESS ||
			    !(completions[i].wr_id & BENCH_RECV_ID))
				return -EIO;
			sequence = (completions[i].wr_id & ~BENCH_RECV_ID) - 1;
			slot = (uint32_t)sequence - first;
			if (slot >= count ||
			    verify_message(receiver->buffer +
					   (size_t)slot * options->message_size,
					   options->message_size, sequence) < 0)
				return -EIO;
			receive_completions++;
		}
		if (now_ns(CLOCK_MONOTONIC) >= deadline)
			return -ETIMEDOUT;
	}
	return 0;
}

static void print_stats(const struct options *options, uint64_t wall_ns,
			uint64_t cpu_ns, const struct rusage *before,
			const struct rusage *after, uint64_t *latency_ns,
			const struct direct_endpoint *sender)
{
	long double seconds = (long double)wall_ns / 1000000000.0L;
	long double bytes = (long double)options->iterations *
		options->message_size;
	long double average = 0;
	uint64_t user_ns = timeval_ns(&after->ru_utime) -
		timeval_ns(&before->ru_utime);
	uint64_t system_ns = timeval_ns(&after->ru_stime) -
		timeval_ns(&before->ru_stime);
	uint32_t i;

	for (i = 0; i < options->iterations; i++)
		average += latency_ns[i];
	average /= options->iterations;
	qsort(latency_ns, options->iterations, sizeof(*latency_ns), compare_u64);

	printf("\nUIB traffic stress results\n");
	printf("  protocol / transport : %s / %s\n", options->protocol,
	       sender->context->tops->name);
	printf("  operations            : %u SENDs (%u bytes each, window %u)\n",
	       options->iterations, options->message_size, options->window);
	printf("  payload transferred   : %.3Lf MiB\n", bytes / (1024.0L * 1024.0L));
	printf("  wall time             : %.6Lf seconds\n", seconds);
	printf("  payload bandwidth     : %.3Lf Gbit/s (%.3Lf MiB/s)\n",
	       bytes * 8.0L / wall_ns, bytes / (1024.0L * 1024.0L) / seconds);
	printf("  operation rate        : %.0Lf operations/s\n",
	       options->iterations / seconds);
	printf("  CPU time              : %.6Lf s (user %.6Lf, system %.6Lf)\n",
	       (long double)cpu_ns / 1000000000.0L,
	       (long double)user_ns / 1000000000.0L,
	       (long double)system_ns / 1000000000.0L);
	printf("  CPU utilization       : %.1Lf%%\n",
	       100.0L * cpu_ns / wall_ns);
	printf("  completion latency ns : min %llu, avg %.0Lf, p50 %llu, p95 %llu, p99 %llu, max %llu\n",
	       (unsigned long long)latency_ns[0], average,
	       (unsigned long long)percentile(latency_ns,
					       options->iterations, 50),
	       (unsigned long long)percentile(latency_ns,
					       options->iterations, 95),
	       (unsigned long long)percentile(latency_ns,
					       options->iterations, 99),
	       (unsigned long long)latency_ns[options->iterations - 1]);
	if (!strcmp(options->protocol, "uet")) {
		struct uib_uet_nscc nscc;

		if (!uib_uet_nscc_query_qp(sender->qp, &nscc))
			printf("  NSCC final state      : cwnd %llu bytes, byte-clock residual %lld, base RTT %llu ns\n",
			       (unsigned long long)nscc.cwnd,
			       (long long)nscc.inflight,
			       (unsigned long long)nscc.base_rtt_ns);
	}
}

int main(int argc, char **argv)
{
	struct direct_endpoint sender = {0}, receiver = {0};
	struct options options;
	struct rusage usage_before, usage_after;
	uint64_t *posted_ns = NULL;
	uint64_t *latency_ns = NULL;
	uint64_t wall_start, wall_end, cpu_start, cpu_end;
	size_t buffer_length;
	int rc = 1;
	uint32_t first;

	if (parse_options(argc, argv, &options) < 0) {
		fprintf(stderr, "usage: %s [--protocol rocev2|uet|dummy] [--iterations N] [--size BYTES] [--window N]\n",
			argv[0]);
		return 2;
	}
	buffer_length = (size_t)options.window * options.message_size;
	posted_ns = calloc(options.iterations, sizeof(*posted_ns));
	latency_ns = calloc(options.iterations, sizeof(*latency_ns));
	if (!posted_ns || !latency_ns)
		goto out_free;
	if (direct_endpoint_open(&sender, options.protocol, "127.0.0.1", 19100,
				 buffer_length, options.window) < 0 ||
	    direct_endpoint_open(&receiver, options.protocol, "127.0.0.1", 19101,
				 buffer_length, options.window) < 0)
		goto out_endpoints;
	if (direct_endpoint_connect(&sender, &receiver, 4096) < 0 ||
	    direct_endpoint_connect(&receiver, &sender, 4096) < 0)
		goto out_endpoints;

	getrusage(RUSAGE_SELF, &usage_before);
	wall_start = now_ns(CLOCK_MONOTONIC);
	cpu_start = now_ns(CLOCK_PROCESS_CPUTIME_ID);
	for (first = 0; first < options.iterations;
	     first += options.window) {
		uint32_t count = options.iterations - first;

		if (count > options.window)
			count = options.window;
		if (run_batch(&sender, &receiver, &options, first, count,
			      posted_ns, latency_ns) < 0) {
			fprintf(stderr, "traffic failed in batch beginning at %u\n",
				first);
			goto out_endpoints;
		}
	}
	cpu_end = now_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_end = now_ns(CLOCK_MONOTONIC);
	getrusage(RUSAGE_SELF, &usage_after);
	print_stats(&options, wall_end - wall_start, cpu_end - cpu_start,
		    &usage_before, &usage_after, latency_ns, &sender);
	printf("RESULT PASS\n");
	rc = 0;

out_endpoints:
	direct_endpoint_close(&receiver);
	direct_endpoint_close(&sender);
out_free:
	free(latency_ns);
	free(posted_ns);
	return rc;
}
