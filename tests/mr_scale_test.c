// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "uib/context.h"
#include "uib/mr.h"
#include "uib/pd.h"
#include "uib/types.h"

#define BENCH_LOOKUPS	500000U
#define BENCH_MRS	1024U
#define MR_STRIDE	64U

struct lookup_result {
	double nanoseconds;
	double cpu_seconds;
	double million_per_second;
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

static struct uib_mr *linear_key_lookup(struct uib_mr **mrs, uint32_t count,
					uint32_t key)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (mrs[i]->lkey == key)
			return mrs[i];
	}
	return NULL;
}

static struct uib_mr *linear_addr_lookup(struct uib_mr **mrs, uint32_t count,
					 uint64_t va)
{
	uint64_t base;
	uint32_t i;

	for (i = 0; i < count; i++) {
		base = (uint64_t)(uintptr_t)mrs[i]->addr;
		if (va >= base && va - base < mrs[i]->length)
			return mrs[i];
	}
	return NULL;
}

static void set_result(struct lookup_result *result, uint64_t wall_start,
		       uint64_t wall_end, uint64_t cpu_start, uint64_t cpu_end,
		       uint32_t lookups)
{
	double seconds = (double)(wall_end - wall_start) / 1000000000.0;

	result->nanoseconds = (double)(wall_end - wall_start) / lookups;
	result->cpu_seconds = (double)(cpu_end - cpu_start) / 1000000000.0;
	result->million_per_second = (double)lookups / seconds / 1000000.0;
}

static int run_key_case(struct uib_pd *pd, struct uib_mr **mrs,
			uint32_t lookups, int indexed,
			struct lookup_result *result)
{
	struct uib_mr *mr;
	uint64_t wall_start;
	uint64_t wall_end;
	uint64_t cpu_start;
	uint64_t cpu_end;
	uint64_t checksum = 0;
	uint32_t slot;
	uint32_t i;

	cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = clock_ns(CLOCK_MONOTONIC);
	for (i = 0; i < lookups; i++) {
		slot = (i * 2654435761U) & (BENCH_MRS - 1U);
		if (indexed)
			mr = uib_pd_lookup_lkey(pd, mrs[slot]->lkey);
		else
			mr = linear_key_lookup(mrs, BENCH_MRS, mrs[slot]->lkey);
		checksum += mr ? mr->lkey : 0;
	}
	wall_end = clock_ns(CLOCK_MONOTONIC);
	cpu_end = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	if (!checksum)
		return -EIO;
	set_result(result, wall_start, wall_end, cpu_start, cpu_end, lookups);
	return 0;
}

static int run_addr_case(struct uib_pd *pd, struct uib_mr **mrs,
			 uint32_t lookups, int indexed,
			 struct lookup_result *result)
{
	struct uib_mr *mr;
	uint64_t wall_start;
	uint64_t wall_end;
	uint64_t cpu_start;
	uint64_t cpu_end;
	uint64_t checksum = 0;
	uint64_t va;
	uint32_t slot;
	uint32_t i;

	cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = clock_ns(CLOCK_MONOTONIC);
	for (i = 0; i < lookups; i++) {
		slot = (i * 2654435761U) & (BENCH_MRS - 1U);
		va = (uint64_t)(uintptr_t)mrs[slot]->addr + 17U;
		if (indexed)
			mr = uib_pd_lookup_address(pd, va, 1,
						   UIB_ACCESS_REMOTE_READ);
		else
			mr = linear_addr_lookup(mrs, BENCH_MRS, va);
		checksum += mr ? mr->lkey : 0;
	}
	wall_end = clock_ns(CLOCK_MONOTONIC);
	cpu_end = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	if (!checksum)
		return -EIO;
	set_result(result, wall_start, wall_end, cpu_start, cpu_end, lookups);
	return 0;
}

static void print_result(const char *name, const struct lookup_result *result)
{
	printf("  %-13s: %.3f Mlookup/s, %.3f ns/lookup, %.6f CPU s\n",
	       name, result->million_per_second, result->nanoseconds,
	       result->cpu_seconds);
}

int main(void)
{
	struct lookup_result addr_indexed;
	struct lookup_result addr_linear;
	struct lookup_result key_indexed;
	struct lookup_result key_linear;
	struct uib_context ctx = { 0 };
	uint32_t keys[BENCH_MRS];
	struct uib_mr *mrs[BENCH_MRS] = { NULL };
	struct uib_pd *pd;
	uint8_t *memory;
	uint32_t lookups;
	uint32_t i;
	int rc = 0;

	memory = aligned_alloc(MR_STRIDE, BENCH_MRS * MR_STRIDE);
	pd = uib_alloc_pd(&ctx);
	if (!memory || !pd)
		return 2;
	for (i = 0; i < BENCH_MRS; i++) {
		mrs[i] = uib_reg_mr(pd, memory + i * MR_STRIDE, MR_STRIDE,
				    UIB_ACCESS_LOCAL_WRITE |
				    UIB_ACCESS_REMOTE_READ |
				    UIB_ACCESS_REMOTE_WRITE);
		if (!mrs[i]) {
			rc = -errno;
			goto out;
		}
		keys[i] = mrs[i]->lkey;
	}
	for (i = 1; i < BENCH_MRS; i += 2) {
		uib_dereg_mr(mrs[i]);
		mrs[i] = uib_reg_mr(pd, memory + i * MR_STRIDE, MR_STRIDE,
				    UIB_ACCESS_REMOTE_READ);
		if (!mrs[i] || uib_pd_lookup_lkey(pd, keys[i])) {
			rc = -EIO;
			goto out;
		}
	}
	lookups = env_u32("UIB_MR_SCALE_LOOKUPS", BENCH_LOOKUPS);
	if (run_key_case(pd, mrs, lookups, 0, &key_linear) < 0 ||
	    run_key_case(pd, mrs, lookups, 1, &key_indexed) < 0 ||
	    run_addr_case(pd, mrs, lookups, 0, &addr_linear) < 0 ||
	    run_addr_case(pd, mrs, lookups, 1, &addr_indexed) < 0) {
		rc = -EIO;
		goto out;
	}
	printf("MR lookup scaling (%u MRs, %u lookups)\n", BENCH_MRS, lookups);
	print_result("key linear", &key_linear);
	print_result("key indexed", &key_indexed);
	print_result("addr linear", &addr_linear);
	print_result("addr indexed", &addr_indexed);
	printf("  key gain     : %.2f%% throughput, %.2f%% latency\n",
	       100.0 * (key_indexed.million_per_second /
			key_linear.million_per_second - 1.0),
	       100.0 * (1.0 - key_indexed.nanoseconds /
			key_linear.nanoseconds));
	printf("  address gain : %.2f%% throughput, %.2f%% latency\n",
	       100.0 * (addr_indexed.million_per_second /
			addr_linear.million_per_second - 1.0),
	       100.0 * (1.0 - addr_indexed.nanoseconds /
			addr_linear.nanoseconds));
	printf("RESULT PASS\n");
out:
	for (i = 0; i < BENCH_MRS; i++) {
		if (mrs[i])
			uib_dereg_mr(mrs[i]);
	}
	if (pd)
		uib_dealloc_pd(pd);
	free(memory);
	return rc < 0 ? 1 : 0;
}
