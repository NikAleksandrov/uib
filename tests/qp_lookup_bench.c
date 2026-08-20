// SPDX-License-Identifier: MIT
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "uib/context.h"
#include "uib/qp.h"

#define BENCH_QPS 4096U
#define BENCH_THREADS 8U
#define BENCH_LOOKUPS 1000000U

struct bench_worker {
	struct uib_context *ctx;
	pthread_mutex_t *start_lock;
	uint64_t checksum;
	uint32_t lookups;
	uint32_t thread_id;
	int serialized;
};

struct bench_result {
	double wall_seconds;
	double cpu_seconds;
	double million_lookups_per_second;
	double nanoseconds_per_lookup;
};

static uint64_t clock_ns(clockid_t clock_id)
{
	struct timespec now;

	clock_gettime(clock_id, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static uint32_t env_u32(const char *name, uint32_t fallback)
{
	unsigned long parsed;
	const char *value;
	char *end;

	value = getenv(name);
	if (!value || !*value)
		return fallback;
	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno || !end || *end || !parsed || parsed > UINT32_MAX)
		return fallback;
	return (uint32_t)parsed;
}

static void *lookup_worker(void *opaque)
{
	struct bench_worker *worker = opaque;
	struct uib_context *ctx = worker->ctx;
	struct uib_qp *qp;
	uint64_t checksum = 0;
	uint32_t qpn;
	uint32_t i;

	pthread_mutex_lock(worker->start_lock);
	pthread_mutex_unlock(worker->start_lock);
	for (i = 0; i < worker->lookups; i++) {
		qpn = 1 + ((i + worker->thread_id) % BENCH_QPS);
		pthread_rwlock_rdlock(&ctx->qp_io_lock);
		if (worker->serialized) {
			pthread_mutex_lock(&ctx->qp_lock);
			qp = ctx->qp_table[qpn];
			pthread_mutex_unlock(&ctx->qp_lock);
		} else {
			qp = uib_lookup_qp_locked(ctx, qpn);
		}
		pthread_rwlock_unlock(&ctx->qp_io_lock);
		checksum += qp ? qp->qp_num : 0;
	}
	worker->checksum = checksum;
	return NULL;
}

static int run_lookup_case(struct uib_context *ctx, uint32_t thread_count,
			   uint32_t lookups, int serialized,
			   struct bench_result *result)
{
	struct bench_worker *workers;
	pthread_mutex_t start_lock;
	pthread_t *threads;
	uint64_t expected = 0;
	uint64_t checksum = 0;
	uint64_t wall_start;
	uint64_t wall_end;
	uint64_t cpu_start;
	uint64_t cpu_end;
	uint64_t total;
	uint32_t qpn;
	uint32_t created = 0;
	uint32_t i;
	uint32_t j;
	int error;
	int rc = 0;

	threads = calloc(thread_count, sizeof(*threads));
	workers = calloc(thread_count, sizeof(*workers));
	if (!threads || !workers) {
		rc = -ENOMEM;
		goto out;
	}
	error = pthread_mutex_init(&start_lock, NULL);
	if (error) {
		rc = -error;
		goto out;
	}
	pthread_mutex_lock(&start_lock);
	for (i = 0; i < thread_count; i++) {
		workers[i].ctx = ctx;
		workers[i].start_lock = &start_lock;
		workers[i].lookups = lookups;
		workers[i].thread_id = i;
		workers[i].serialized = serialized;
		error = pthread_create(&threads[i], NULL, lookup_worker,
				       &workers[i]);
		if (error) {
			rc = -error;
			goto out_threads;
		}
		created++;
	}
	cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = clock_ns(CLOCK_MONOTONIC);
	pthread_mutex_unlock(&start_lock);
	for (i = 0; i < thread_count; i++)
		pthread_join(threads[i], NULL);
	created = 0;
	wall_end = clock_ns(CLOCK_MONOTONIC);
	cpu_end = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	for (i = 0; i < thread_count; i++) {
		checksum += workers[i].checksum;
		for (j = 0; j < lookups; j++) {
			qpn = 1 + ((j + i) % BENCH_QPS);
			expected += qpn;
		}
	}
	if (checksum != expected) {
		rc = -EIO;
		goto out_lock;
	}
	total = (uint64_t)thread_count * lookups;
	result->wall_seconds = (double)(wall_end - wall_start) / 1000000000.0;
	result->cpu_seconds = (double)(cpu_end - cpu_start) / 1000000000.0;
	result->million_lookups_per_second =
		(double)total / result->wall_seconds / 1000000.0;
	result->nanoseconds_per_lookup =
		(double)(wall_end - wall_start) / (double)total;
	goto out_lock;

out_threads:
	pthread_mutex_unlock(&start_lock);
	for (i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
out_lock:
	pthread_mutex_destroy(&start_lock);
out:
	free(workers);
	free(threads);
	return rc;
}

int main(void)
{
	struct bench_result serialized;
	struct bench_result direct;
	struct uib_context ctx = { 0 };
	struct uib_qp *qps;
	uint32_t thread_count;
	uint32_t lookups;
	uint32_t i;
	int rc;

	thread_count = env_u32("UIB_SCALE_THREADS", BENCH_THREADS);
	lookups = env_u32("UIB_SCALE_LOOKUPS", BENCH_LOOKUPS);
	if (thread_count > 256)
		thread_count = 256;
	ctx.qp_table_size = BENCH_QPS + 1;
	ctx.qp_table = calloc(ctx.qp_table_size, sizeof(*ctx.qp_table));
	qps = calloc(BENCH_QPS, sizeof(*qps));
	if (!ctx.qp_table || !qps)
		return 2;
	pthread_mutex_init(&ctx.qp_lock, NULL);
	pthread_rwlock_init(&ctx.qp_io_lock, NULL);
	for (i = 0; i < BENCH_QPS; i++) {
		qps[i].qp_num = i + 1;
		ctx.qp_table[i + 1] = &qps[i];
	}
	rc = run_lookup_case(&ctx, thread_count, lookups, 1, &serialized);
	if (!rc)
		rc = run_lookup_case(&ctx, thread_count, lookups, 0, &direct);
	if (rc < 0) {
		fprintf(stderr, "QP lookup benchmark failed: %d\n", rc);
		return 1;
	}
	printf("QP lookup scaling (%u threads, %u lookups/thread)\n",
	       thread_count, lookups);
	printf("  serialized : %.3f Mlookup/s, %.3f ns/lookup, %.6f CPU s\n",
	       serialized.million_lookups_per_second,
	       serialized.nanoseconds_per_lookup, serialized.cpu_seconds);
	printf("  direct     : %.3f Mlookup/s, %.3f ns/lookup, %.6f CPU s\n",
	       direct.million_lookups_per_second,
	       direct.nanoseconds_per_lookup, direct.cpu_seconds);
	printf("  improvement: %.2f%% throughput, %.2f%% wall latency\n",
	       100.0 * (direct.million_lookups_per_second /
			serialized.million_lookups_per_second - 1.0),
	       100.0 * (1.0 - direct.nanoseconds_per_lookup /
			serialized.nanoseconds_per_lookup));
	printf("RESULT PASS\n");
	pthread_rwlock_destroy(&ctx.qp_io_lock);
	pthread_mutex_destroy(&ctx.qp_lock);
	free(qps);
	free(ctx.qp_table);
	return 0;
}
