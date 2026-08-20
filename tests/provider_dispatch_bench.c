// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <infiniband/verbs.h>

#define BENCH_CONTEXTS	64U
#define BENCH_LOOKUPS	1000000U
#define BENCH_PORT_BASE	22000U

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

static struct ibv_device *find_uib_device(void)
{
	struct ibv_device **devices;
	struct ibv_device *device = NULL;
	int count;
	int i;

	devices = ibv_get_device_list(&count);
	if (!devices)
		return NULL;
	for (i = 0; i < count; i++) {
		if (!strcmp(ibv_get_device_name(devices[i]), "uib0")) {
			device = devices[i];
			break;
		}
	}
	ibv_free_device_list(devices);
	return device;
}

int main(void)
{
	struct ibv_context **contexts;
	struct ibv_device *device;
	uint64_t wall_start;
	uint64_t wall_end;
	uint64_t cpu_start;
	uint64_t cpu_end;
	uint64_t checksum = 0;
	uint32_t context_count;
	uint32_t lookups;
	uint32_t created = 0;
	uint32_t i;
	char port[16];
	__be16 pkey;
	int rc;

	context_count = env_u32("UIB_PROVIDER_CONTEXTS", BENCH_CONTEXTS);
	lookups = env_u32("UIB_PROVIDER_LOOKUPS", BENCH_LOOKUPS);
	if (context_count > UINT16_MAX - BENCH_PORT_BASE)
		return 2;
	contexts = calloc(context_count, sizeof(*contexts));
	if (!contexts)
		return 2;
	setenv("UIB_TRANSPORT_MODE", "udp-sync", 1);
	setenv("UIB_BIND_IP", "127.0.0.1", 1);
	device = find_uib_device();
	if (!device)
		goto fail;
	for (i = 0; i < context_count; i++) {
		snprintf(port, sizeof(port), "%u", BENCH_PORT_BASE + i);
		setenv("UIB_BIND_PORT", port, 1);
		contexts[i] = ibv_open_device(device);
		if (!contexts[i])
			goto fail;
		created++;
	}
	cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = clock_ns(CLOCK_MONOTONIC);
	for (i = 0; i < lookups; i++) {
		rc = ibv_query_pkey(contexts[0], 1, 0, &pkey);
		if (rc)
			goto fail;
		checksum += pkey;
	}
	wall_end = clock_ns(CLOCK_MONOTONIC);
	cpu_end = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	if (!checksum)
		goto fail;
	printf("Provider dispatch scaling (%u contexts, %u calls)\n",
	       context_count, lookups);
	printf("  throughput : %.3f Mcalls/s\n",
	       (double)lookups * 1000.0 / (wall_end - wall_start));
	printf("  latency    : %.3f ns/call\n",
	       (double)(wall_end - wall_start) / lookups);
	printf("  CPU time   : %.6f s\n",
	       (double)(cpu_end - cpu_start) / 1000000000.0);
	printf("RESULT PASS\n");
	while (created)
		ibv_close_device(contexts[--created]);
	free(contexts);
	return 0;

fail:
	while (created)
		ibv_close_device(contexts[--created]);
	free(contexts);
	return 1;
}
