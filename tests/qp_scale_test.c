/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uib/context.h"
#include "uib/qp.h"

#define TEST_QPS 1024
#define TEST_INITIAL_CAPACITY 4

static int failed;

static void check(const char *name, int condition)
{
	printf("%-30s %s\n", name, condition ? "OK" : "FAIL");
	if (!condition)
		failed = 1;
}

int main(void)
{
	struct uib_context ctx = { 0 };
	struct uib_qp qps[TEST_QPS];
	struct uib_qp *due;
	uint32_t expected;
	uint32_t qpn;
	uint32_t i;

	pthread_mutex_init(&ctx.qp_lock, NULL);
	pthread_rwlock_init(&ctx.qp_io_lock, NULL);
	ctx.qp_table_size = TEST_INITIAL_CAPACITY;
	ctx.qp_next_id = 1;
	ctx.qp_table = calloc(ctx.qp_table_size, sizeof(*ctx.qp_table));
	ctx.qp_free_ids = calloc(ctx.qp_table_size,
				 sizeof(*ctx.qp_free_ids));
	ctx.retry_heap = calloc(ctx.qp_table_size, sizeof(*ctx.retry_heap));
	if (!ctx.qp_table || !ctx.qp_free_ids || !ctx.retry_heap)
		return 2;

	memset(qps, 0, sizeof(qps));
	for (i = 0; i < TEST_QPS; i++) {
		qps[i].ctx = &ctx;
		qpn = uib_register_qp(&ctx, &qps[i]);
		if (!qpn)
			return 2;
		qps[i].qp_num = qpn;
	}
	check("qpn.direct.sequence", qps[0].qp_num == 1 &&
	      qps[TEST_QPS - 1].qp_num == TEST_QPS);
	check("qpn.table.growth", ctx.qp_table_size >= TEST_QPS + 1);
	check("qpn.lookup.constant", uib_lookup_qp(&ctx, TEST_QPS) ==
	      &qps[TEST_QPS - 1]);

	uib_qp_retry_schedule(&qps[0], 300);
	uib_qp_retry_schedule(&qps[1], 100);
	uib_qp_retry_schedule(&qps[2], 200);
	uib_qp_retry_schedule(&qps[0], 50);
	check("retry.heap.not-due", !uib_retry_pop_due(&ctx, 49));
	due = uib_retry_pop_due(&ctx, 50);
	check("retry.heap.decrease-key", due == &qps[0]);
	uib_qp_retry_cancel(&qps[1]);
	check("retry.heap.cancel", !uib_retry_pop_due(&ctx, 199));
	due = uib_retry_pop_due(&ctx, 200);
	check("retry.heap.deadline-order", due == &qps[2]);
	check("retry.heap.empty", !ctx.retry_heap_size);

	for (i = 0; i < TEST_QPS; i += 2)
		uib_unregister_qp(&ctx, &qps[i]);
	check("qpn.free.stack.count", ctx.qp_free_count == TEST_QPS / 2);
	for (i = 0; i < TEST_QPS; i += 2) {
		expected = TEST_QPS - 1 - i;
		qpn = uib_register_qp(&ctx, &qps[i]);
		qps[i].qp_num = qpn;
		if (qpn != expected)
			failed = 1;
	}
	check("qpn.constant-time.reuse", !ctx.qp_free_count);
	check("qpn.no.table.rescan", ctx.qp_next_id == TEST_QPS + 1);

	for (i = 0; i < TEST_QPS; i++)
		uib_unregister_qp(&ctx, &qps[i]);
	free(ctx.retry_heap);
	free(ctx.qp_free_ids);
	free(ctx.qp_table);
	pthread_rwlock_destroy(&ctx.qp_io_lock);
	pthread_mutex_destroy(&ctx.qp_lock);

	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
