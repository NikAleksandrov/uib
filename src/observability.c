/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uib/context.h"
#include "uib/env.h"
#include "uib/transport.h"
#include "uib/zerocopy.h"
#include "observability.h"

#define UIB_STATS_PREFIX "UIB_STATS "

static uint64_t context_sequence;

static uint64_t process_token(void)
{
	char hostname[256];
	uint64_t hash = 1469598103934665603ULL;
	uint64_t pid = getpid();
	size_t i;

	if (gethostname(hostname, sizeof(hostname)))
		hostname[0] = '\0';
	hostname[sizeof(hostname) - 1] = '\0';
	for (i = 0; hostname[i]; i++) {
		hash ^= (unsigned char)hostname[i];
		hash *= 1099511628211ULL;
	}
	for (i = 0; i < sizeof(pid); i++) {
		hash ^= pid & 0xff;
		hash *= 1099511628211ULL;
		pid >>= 8;
	}
	return hash;
}

static void write_stderr(const char *buffer, size_t length)
{
	ssize_t written;

	while (length) {
		written = write(STDERR_FILENO, buffer, length);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (!written)
			return;
		buffer += written;
		length -= written;
	}
}

static int append_transport(char *buffer, size_t length,
			    const struct uib_transport_stats *stats)
{
	return snprintf(buffer, length,
		"true,\"transportCounters\":{"
		"\"txQueued\":%" PRIu64 ","
		"\"txSubmitted\":%" PRIu64 ","
		"\"txCompleted\":%" PRIu64 ","
		"\"txErrors\":%" PRIu64 ","
		"\"txGsoBatches\":%" PRIu64 ","
		"\"txGsoPackets\":%" PRIu64 ","
		"\"rxPackets\":%" PRIu64 ","
		"\"rxGroBatches\":%" PRIu64 ","
		"\"rxCompletionBatches\":%" PRIu64 ","
		"\"rxDispatchDrops\":%" PRIu64 ","
		"\"rxLargestBatch\":%u,"
		"\"rxBatch\":%u,\"rxWorkers\":%u,"
		"\"rxWorkersPinned\":%u,\"txBatch\":%u,"
		"\"sqpollRequested\":%u,\"sqpollActive\":%u,"
		"\"gsoEnabled\":%u,\"groEnabled\":%u,"
		"\"rxMode\":%u}",
		stats->tx_queued, stats->tx_submitted, stats->tx_completed,
		stats->tx_errors, stats->tx_gso_batches,
		stats->tx_gso_packets, stats->rx_packets,
		stats->rx_gro_batches, stats->rx_completion_batches,
		stats->rx_dispatch_drops, stats->rx_largest_batch,
		stats->rx_batch, stats->rx_workers, stats->rx_workers_pinned,
		stats->tx_batch,
		stats->sqpoll_requested, stats->sqpoll_active,
		stats->gso_enabled, stats->gro_enabled, stats->rx_mode);
}

void uib_observability_emit(struct uib_context *ctx)
{
	struct uib_zerocopy_stats zerocopy;
	struct uib_transport_stats transport;
	char output[2048];
	const char *setting;
	uint64_t sequence;
	size_t offset;
	int available;
	int saved_errno;
	int length;

	setting = getenv(UIB_ENV_STATS_OUTPUT);
	if (!setting || strcmp(setting, "stderr"))
		return;

	saved_errno = errno;
	sequence = __atomic_add_fetch(&context_sequence, 1, __ATOMIC_RELAXED);
	available = !uib_transport_get_stats(ctx, &transport);
	uib_zerocopy_get_stats(ctx, &zerocopy);
	length = snprintf(output, sizeof(output),
		UIB_STATS_PREFIX "{\"schemaVersion\":1,\"pid\":%ld,"
		"\"processToken\":%" PRIu64 ","
		"\"contextSequence\":%" PRIu64 ","
		"\"protocol\":\"%s\",\"transport\":\"%s\","
		"\"transportCountersAvailable\":",
		(long)getpid(), process_token(), sequence, ctx->pops->name,
		ctx->tops->name);
	if (length < 0 || (size_t)length >= sizeof(output))
		goto out;
	offset = length;
	if (available)
		length = append_transport(output + offset,
					  sizeof(output) - offset, &transport);
	else
		length = snprintf(output + offset, sizeof(output) - offset,
				  "false,\"transportCounters\":null");
	if (length < 0 || (size_t)length >= sizeof(output) - offset)
		goto out;
	offset += length;
	length = snprintf(output + offset, sizeof(output) - offset,
		",\"zeroCopyCounters\":{"
		"\"copiedCalls\":%" PRIu64 ","
		"\"copiedBytes\":%" PRIu64 ","
		"\"zeroCopyCalls\":%" PRIu64 ","
		"\"zeroCopyBytes\":%" PRIu64 ","
		"\"completions\":%" PRIu64 ","
		"\"deferredCopies\":%" PRIu64 ","
		"\"errors\":%" PRIu64 ","
		"\"capabilities\":%u,\"mode\":%u,"
		"\"threshold\":%zu}}\n",
		zerocopy.copied_calls, zerocopy.copied_bytes,
		zerocopy.zerocopy_calls, zerocopy.zerocopy_bytes,
		zerocopy.completions, zerocopy.deferred_copies,
		zerocopy.errors, zerocopy.capabilities, zerocopy.mode,
		zerocopy.threshold);
	if (length < 0 || (size_t)length >= sizeof(output) - offset)
		goto out;
	offset += length;
	write_stderr(output, offset);
out:
	errno = saved_errno;
}
