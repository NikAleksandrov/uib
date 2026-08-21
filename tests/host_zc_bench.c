/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uib/uib.h"

#define BENCH_ITERATIONS 200000U

static uint64_t monotonic_ns(void)
{
	struct timespec timestamp;

	clock_gettime(CLOCK_MONOTONIC, &timestamp);
	return (uint64_t)timestamp.tv_sec * 1000000000ULL +
		(uint64_t)timestamp.tv_nsec;
}

static double bench_regular(struct uib_qp *qp, struct uib_cq *cq,
			    struct uib_mr *mr, void *data)
{
	struct uib_recv_wr request = {0};
	struct uib_rq_slot slot;
	struct uib_sge sge;
	struct uib_wc completion;
	uint64_t started;
	uint64_t ended;
	unsigned int iteration;

	sge.addr = (uint64_t)(uintptr_t)data;
	sge.length = 64;
	sge.lkey = mr->lkey;
	request.sg_list = &sge;
	request.num_sge = 1;
	started = monotonic_ns();
	for (iteration = 0; iteration < BENCH_ITERATIONS; iteration++) {
		request.wr_id = iteration;
		uib_post_recv(qp, &request, NULL);
		uib_qp_pop_recv(qp, &slot);
		uib_qp_emit_recv_slot(qp, &slot, UIB_WC_SUCCESS, UIB_WC_RECV,
				      64, 1, 0);
		uib_qp_release_recv(&slot);
		uib_poll_cq(cq, 1, &completion);
	}
	ended = monotonic_ns();
	return (double)(ended - started) / BENCH_ITERATIONS;
}

static double bench_loan(struct uib_qp *qp, struct uib_cq *cq)
{
	struct uib_rx_loan loan = {0};
	struct uib_rq_slot slot;
	struct uib_wc completion;
	uint64_t started;
	uint64_t ended;
	unsigned int iteration;

	uib_post_recv_loan(qp, 0, 64);
	started = monotonic_ns();
	for (iteration = 0; iteration < BENCH_ITERATIONS; iteration++) {
		uib_qp_pop_recv(qp, &slot);
		uib_qp_emit_recv_slot(qp, &slot, UIB_WC_SUCCESS, UIB_WC_RECV,
				      64, 1, 0);
		uib_qp_release_recv(&slot);
		uib_poll_cq(cq, 1, &completion);
		uib_wc_take_rx_loan(&completion, &loan);
		uib_repost_rx_loan(qp, &loan, iteration + 1);
	}
	ended = monotonic_ns();
	return (double)(ended - started) / BENCH_ITERATIONS;
}

int main(void)
{
	struct uib_qp_init_attr initialization = {
		.qp_type = UIB_QPT_RC,
		.max_send_wr = 8,
		.max_recv_wr = 8,
		.max_send_sge = 1,
		.max_recv_sge = 1,
	};
	struct uib_context *context;
	struct uib_qp *qp;
	struct uib_pd *pd;
	struct uib_cq *cq;
	struct uib_mr *mr;
	void *data;
	double regular_ns;
	double loan_ns;

	data = calloc(1, 64);
	context = uib_open_device_protocol("127.0.0.1", 20310, "dummy");
	pd = context ? uib_alloc_pd(context) : NULL;
	cq = context ? uib_create_cq(context, 16) : NULL;
	initialization.send_cq = cq;
	initialization.recv_cq = cq;
	qp = pd && cq ? uib_create_qp(pd, &initialization) : NULL;
	mr = pd && data ? uib_reg_mr(pd, data, 64, UIB_ACCESS_LOCAL_WRITE) :
		NULL;
	if (!context || !pd || !cq || !qp || !mr)
		return 1;
	regular_ns = bench_regular(qp, cq, mr, data);
	loan_ns = bench_loan(qp, cq);
	printf("regular_post_complete_ns %.2f\n", regular_ns);
	printf("loan_repost_complete_ns  %.2f\n", loan_ns);
	printf("loan_overhead_ns         %.2f\n", loan_ns - regular_ns);
	uib_dereg_mr(mr);
	uib_destroy_qp(qp);
	uib_destroy_cq(cq);
	uib_dealloc_pd(pd);
	uib_close_device(context);
	free(data);
	return 0;
}
