/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <infiniband/verbs.h>

#define BENCH_CHAIN_LENGTH	128U
#define BENCH_ITERATIONS	100000U
#define BENCH_RECV_DEPTH	64U
#define BENCH_PORT		22100U

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

static struct ibv_context *open_uib_context(void)
{
	struct ibv_device **devices;
	struct ibv_context *context = NULL;
	int count;
	int i;

	devices = ibv_get_device_list(&count);
	if (!devices)
		return NULL;
	for (i = 0; i < count; i++) {
		if (!strcmp(ibv_get_device_name(devices[i]), "uib0")) {
			context = ibv_open_device(devices[i]);
			break;
		}
	}
	ibv_free_device_list(devices);
	return context;
}

int main(void)
{
	struct ibv_qp_init_attr init = { 0 };
	struct ibv_recv_wr recv_wr[BENCH_CHAIN_LENGTH];
	struct ibv_send_wr wr[BENCH_CHAIN_LENGTH];
	struct ibv_recv_wr *recv_bad;
	struct ibv_send_wr *bad;
	struct ibv_context *context;
	struct ibv_sge recv_sge = { 0 };
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;
	uint64_t wall_start;
	uint64_t wall_end;
	uint64_t cpu_start;
	uint64_t cpu_end;
	uint32_t iterations;
	uint32_t i;
	char port[16];
	int rc;

	setenv("UIB_TRANSPORT_MODE", "udp-sync", 1);
	setenv("UIB_BIND_IP", "127.0.0.1", 1);
	snprintf(port, sizeof(port), "%u", BENCH_PORT);
	setenv("UIB_BIND_PORT", port, 1);
	context = open_uib_context();
	if (!context) {
		fprintf(stderr, "failed to open uib0: %s\n", strerror(errno));
		return 2;
	}
	pd = ibv_alloc_pd(context);
	if (!pd) {
		fprintf(stderr, "failed to create PD: %s\n", strerror(errno));
		goto out_context;
	}
	cq = ibv_create_cq(context, 16, NULL, NULL, 0);
	if (!cq) {
		fprintf(stderr, "failed to create CQ: %s\n", strerror(errno));
		goto out_pd;
	}
	init.send_cq = cq;
	init.recv_cq = cq;
	init.cap.max_send_wr = BENCH_CHAIN_LENGTH;
	init.cap.max_recv_wr = BENCH_RECV_DEPTH;
	init.cap.max_send_sge = 1;
	init.cap.max_recv_sge = 1;
	init.qp_type = IBV_QPT_RC;
	qp = ibv_create_qp(pd, &init);
	if (!qp) {
		fprintf(stderr, "failed to create QP: %s\n", strerror(errno));
		goto out_cq;
	}
	memset(wr, 0, sizeof(wr));
	memset(recv_wr, 0, sizeof(recv_wr));
	for (i = 0; i < BENCH_CHAIN_LENGTH; i++) {
		wr[i].wr_id = i;
		wr[i].opcode = IBV_WR_SEND;
		wr[i].next = i + 1U < BENCH_CHAIN_LENGTH ? &wr[i + 1U] : NULL;
		recv_wr[i].wr_id = i;
		recv_wr[i].sg_list = &recv_sge;
		recv_wr[i].num_sge = 1;
		recv_wr[i].next = i + 1U < BENCH_CHAIN_LENGTH ?
			&recv_wr[i + 1U] : NULL;
	}
	recv_wr[BENCH_RECV_DEPTH - 1U].next = NULL;
	recv_bad = NULL;
	rc = ibv_post_recv(qp, recv_wr, &recv_bad);
	if (rc || recv_bad)
		goto out_qp;
	recv_wr[BENCH_RECV_DEPTH - 1U].next =
		&recv_wr[BENCH_RECV_DEPTH];
	iterations = env_u32("UIB_PROVIDER_WR_ITERS", BENCH_ITERATIONS);
	cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = clock_ns(CLOCK_MONOTONIC);
	for (i = 0; i < iterations; i++) {
		bad = NULL;
		rc = ibv_post_send(qp, wr, &bad);
		if (rc != EINVAL || bad != wr)
			goto out_qp;
	}
	wall_end = clock_ns(CLOCK_MONOTONIC);
	cpu_end = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	printf("Provider WR translation (%u WRs, %u calls)\n",
	       BENCH_CHAIN_LENGTH, iterations);
	printf("  throughput : %.3f Mcalls/s\n",
	       (double)iterations * 1000.0 / (wall_end - wall_start));
	printf("  latency    : %.3f ns/call\n",
	       (double)(wall_end - wall_start) / iterations);
	printf("  CPU time   : %.6f s\n",
	       (double)(cpu_end - cpu_start) / 1000000000.0);
	cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = clock_ns(CLOCK_MONOTONIC);
	for (i = 0; i < iterations; i++) {
		recv_bad = NULL;
		rc = ibv_post_recv(qp, recv_wr, &recv_bad);
		if (rc != ENOSPC || recv_bad != recv_wr)
			goto out_qp;
	}
	wall_end = clock_ns(CLOCK_MONOTONIC);
	cpu_end = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	printf("Provider receive WR translation (%u WRs, %u calls)\n",
	       BENCH_CHAIN_LENGTH, iterations);
	printf("  throughput : %.3f Mcalls/s\n",
	       (double)iterations * 1000.0 / (wall_end - wall_start));
	printf("  latency    : %.3f ns/call\n",
	       (double)(wall_end - wall_start) / iterations);
	printf("  CPU time   : %.6f s\n",
	       (double)(cpu_end - cpu_start) / 1000000000.0);
	printf("RESULT PASS\n");
	ibv_destroy_qp(qp);
	ibv_destroy_cq(cq);
	ibv_dealloc_pd(pd);
	ibv_close_device(context);
	return 0;

out_qp:
	ibv_destroy_qp(qp);
out_cq:
	ibv_destroy_cq(cq);
out_pd:
	ibv_dealloc_pd(pd);
out_context:
	ibv_close_device(context);
	return 1;
}
