#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#include "uib/uib.h"

#define PORT_A 18515
#define PORT_B 18516
#define BIG_LEN 10003
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static int g_fail;

static void nap_ms(int ms)
{
	struct timespec ts;

	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

static int wait_wc(struct uib_cq *cq, struct uib_wc *wc)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if (uib_poll_cq(cq, 1, wc) == 1)
			return 1;
		nap_ms(1);
	}
	return 0;
}

static const char *op_name(enum uib_wc_opcode op)
{
	switch (op) {
	case UIB_WC_SEND:
		return "SEND";
	case UIB_WC_RDMA_WRITE:
		return "RDMA_WRITE";
	case UIB_WC_RDMA_READ:
		return "RDMA_READ";
	case UIB_WC_FETCH_ADD:
		return "FETCH_ADD";
	case UIB_WC_COMP_SWAP:
		return "COMP_SWAP";
	case UIB_WC_UET_ATOMIC:
		return "UET_ATOMIC";
	case UIB_WC_RECV:
		return "RECV";
	case UIB_WC_RECV_RDMA_WITH_IMM:
		return "RECV_IMM";
	}
	return "?";
}

static void report_wc(const char *tag, struct uib_cq *cq)
{
	struct uib_wc wc;

	memset(&wc, 0, sizeof(wc));
	if (!wait_wc(cq, &wc)) {
		printf("%-18s TIMEOUT\n", tag);
		g_fail = 1;
		return;
	}
	printf("%-18s wr_id=%llu status=%d opcode=%s byte_len=%u\n",
	       tag, (unsigned long long)wc.wr_id, (int)wc.status,
	       op_name(wc.opcode), wc.byte_len);
	if (wc.status != UIB_WC_SUCCESS)
		g_fail = 1;
}

static void check_bytes(const char *tag, const void *a, const void *b, size_t n)
{
	int ok = memcmp(a, b, n) == 0;

	printf("%-18s %s (%zu bytes)\n", tag, ok ? "MATCH" : "MISMATCH", n);
	if (!ok)
		g_fail = 1;
}

static int connect_qp(struct uib_qp *qp, uint32_t dest_qpn,
		      struct sockaddr_in remote)
{
	struct uib_qp_attr attr;
	int rc;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = UIB_QPS_INIT;
	attr.qp_access_flags = UIB_ACCESS_LOCAL_WRITE | UIB_ACCESS_REMOTE_WRITE |
			       UIB_ACCESS_REMOTE_READ | UIB_ACCESS_REMOTE_ATOMIC;
	rc = uib_modify_qp(qp, &attr, UIB_QP_STATE | UIB_QP_ACCESS_FLAGS);
	if (rc < 0)
		return rc;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state    = UIB_QPS_RTR;
	attr.dest_qp_num = dest_qpn;
	attr.remote_addr = remote;
	attr.rq_psn      = 0;
	rc = uib_modify_qp(qp, &attr,
			   UIB_QP_STATE | UIB_QP_AV | UIB_QP_DEST_QPN | UIB_QP_RQ_PSN);
	if (rc < 0)
		return rc;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = UIB_QPS_RTS;
	attr.sq_psn   = 0;
	return uib_modify_qp(qp, &attr, UIB_QP_STATE | UIB_QP_SQ_PSN);
}

static void post_recv_buf(struct uib_qp *qp, uint64_t wr_id, void *buf,
			  uint32_t len, uint32_t lkey)
{
	struct uib_sge sge;
	struct uib_recv_wr wr, *bad;

	sge.addr = (uintptr_t)buf;
	sge.length = len;
	sge.lkey = lkey;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = wr_id;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	uib_post_recv(qp, &wr, &bad);
}

static void post_send_buf(struct uib_qp *qp, uint64_t wr_id,
			  enum uib_wr_opcode op, void *buf, uint32_t len,
			  uint32_t lkey, uint64_t raddr, uint32_t rkey)
{
	struct uib_sge sge;
	struct uib_send_wr wr, *bad;

	sge.addr = (uintptr_t)buf;
	sge.length = len;
	sge.lkey = lkey;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = wr_id;
	wr.opcode = op;
	wr.send_flags = UIB_SEND_SIGNALED;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.rdma.remote_addr = raddr;
	wr.rdma.rkey = rkey;
	uib_post_send(qp, &wr, &bad);
}

static void post_atomic(struct uib_qp *qp, uint64_t wr_id,
			enum uib_wr_opcode opcode, uint64_t *result,
			uint32_t lkey, uint64_t remote_addr, uint32_t rkey,
			uint64_t compare_add, uint64_t swap)
{
	struct uib_send_wr wr, *bad;
	struct uib_sge sge;

	memset(&wr, 0, sizeof(wr));
	sge.addr = (uintptr_t)result;
	sge.length = sizeof(*result);
	sge.lkey = lkey;
	wr.wr_id = wr_id;
	wr.opcode = opcode;
	wr.send_flags = UIB_SEND_SIGNALED;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.atomic.remote_addr = remote_addr;
	wr.atomic.rkey = rkey;
	wr.atomic.compare_add = compare_add;
	wr.atomic.swap = swap;
	uib_post_send(qp, &wr, &bad);
}

static void run_uet_atomic(struct uib_qp *qp, struct uib_cq *cq,
			   struct uib_mr *local_mr, struct uib_mr *remote_mr,
			   void *result, void *target, uint8_t datatype,
			   uint8_t opcode, uint64_t initial, uint64_t operand,
			   uint64_t swap, uint64_t expected)
{
	struct uib_uet_atomic_wr wr = {0};
	struct uib_uet_atomic_wr *bad;
	struct uib_wc wc;
	size_t width = uib_uet_atomic_datatype_size(datatype);
	uint64_t returned = 0;
	uint64_t actual = 0;
	int ok;

	memcpy(target, &initial, width);
	memset(result, 0, width);
	wr.wr_id = 0xC000 + opcode + ((uint64_t)datatype << 8);
	wr.remote_addr = (uintptr_t)target;
	wr.rkey = remote_mr->rkey;
	wr.result_lkey = local_mr->lkey;
	wr.send_flags = UIB_SEND_SIGNALED;
	wr.opcode = opcode;
	wr.datatype = datatype;
	wr.fetching = 1;
	wr.operand = &operand;
	wr.compare = &operand;
	wr.swap = &swap;
	wr.result = result;
	if (uib_uet_post_atomic(qp, &wr, &bad) < 0 ||
	    !wait_wc(cq, &wc) || wc.status != UIB_WC_SUCCESS) {
		printf("uet.atomic.%u.%u       COMPLETION FAIL\n", datatype,
		       opcode);
		g_fail = 1;
		return;
	}
	memcpy(&returned, result, width);
	memcpy(&actual, target, width);
	ok = returned == (initial & (width == 8 ? UINT64_MAX :
				       (1ULL << (width * 8)) - 1)) &&
		actual == (expected & (width == 8 ? UINT64_MAX :
				       (1ULL << (width * 8)) - 1));
	printf("uet.atomic.%u.%u       %s\n", datatype, opcode,
	       ok ? "PASS" : "FAIL");
	if (!ok)
		g_fail = 1;
}

static void test_uet_atomic_matrix(struct uib_qp *qp, struct uib_cq *cq,
				   struct uib_mr *local_mr,
				   struct uib_mr *remote_mr,
				   void *result, void *target)
{
	static const struct {
		uint8_t opcode;
		uint64_t operand;
		uint64_t swap;
		uint64_t expected;
	} operations[] = {
		{ UIB_UET_AMO_MIN, 3, 0, 3 },
		{ UIB_UET_AMO_MAX, 3, 0, 6 },
		{ UIB_UET_AMO_SUM, 3, 0, 9 },
		{ UIB_UET_AMO_DIFF, 3, 0, 3 },
		{ UIB_UET_AMO_PROD, 3, 0, 18 },
		{ UIB_UET_AMO_LOR, 3, 0, 1 },
		{ UIB_UET_AMO_LAND, 3, 0, 1 },
		{ UIB_UET_AMO_BOR, 3, 0, 7 },
		{ UIB_UET_AMO_BAND, 3, 0, 2 },
		{ UIB_UET_AMO_LXOR, 3, 0, 0 },
		{ UIB_UET_AMO_BXOR, 3, 0, 5 },
		{ UIB_UET_AMO_READ, 0, 0, 6 },
		{ UIB_UET_AMO_WRITE, 3, 0, 3 },
		{ UIB_UET_AMO_CSWAP, 6, 9, 9 },
		{ UIB_UET_AMO_CSWAP_NE, 3, 9, 9 },
		{ UIB_UET_AMO_CSWAP_LE, 3, 9, 6 },
		{ UIB_UET_AMO_CSWAP_LT, 3, 9, 6 },
		{ UIB_UET_AMO_CSWAP_GE, 3, 9, 9 },
		{ UIB_UET_AMO_CSWAP_GT, 3, 9, 9 },
		{ UIB_UET_AMO_MSWAP, 3, 9, 5 },
	};
	uint8_t datatype;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(operations); i++)
		run_uet_atomic(qp, cq, local_mr, remote_mr, result, target,
			       UIB_UET_AMO_UINT64, operations[i].opcode, 6,
			       operations[i].operand, operations[i].swap,
			       operations[i].expected);
	for (datatype = UIB_UET_AMO_INT8;
	     datatype <= UIB_UET_AMO_UINT64; datatype++)
		run_uet_atomic(qp, cq, local_mr, remote_mr, result, target,
			       datatype, UIB_UET_AMO_SUM, 6, 3, 0, 9);
	for (datatype = UIB_UET_AMO_INT8;
	     datatype <= UIB_UET_AMO_INT64; datatype += 2)
		run_uet_atomic(qp, cq, local_mr, remote_mr, result, target,
			       datatype, UIB_UET_AMO_MIN, UINT64_MAX - 1,
			       1, 0, UINT64_MAX - 1);
}

int main(void)
{
	struct uib_context *ctxA, *ctxB;
	struct uib_pd *pdA, *pdB;
	struct uib_cq *cqA, *cqB;
	struct uib_qp *qpA, *qpB;
	struct uib_qp_init_attr ia;
	struct uib_mr *mrA, *mrB;
	static uint8_t bufA[65536], bufB[65536];
	uint8_t *sendA, *recvB, *writeA, *targetB, *readA;
	uint64_t *atomicA, *atomicB;
	int is_uet = getenv("UIB_PROTOCOL") &&
		!strcmp(getenv("UIB_PROTOCOL"), "uet");
	unsigned int i;

	ctxA = uib_open_device("127.0.0.1", PORT_A);
	ctxB = uib_open_device("127.0.0.1", PORT_B);
	if (!ctxA || !ctxB) {
		printf("open_device failed: %s\n", strerror(errno));
		return 2;
	}

	pdA = uib_alloc_pd(ctxA);
	pdB = uib_alloc_pd(ctxB);
	cqA = uib_create_cq(ctxA, 64);
	cqB = uib_create_cq(ctxB, 64);

	mrA = uib_reg_mr(pdA, bufA, sizeof(bufA),
			 UIB_ACCESS_LOCAL_WRITE | UIB_ACCESS_REMOTE_WRITE |
			 UIB_ACCESS_REMOTE_READ | UIB_ACCESS_REMOTE_ATOMIC);
	mrB = uib_reg_mr(pdB, bufB, sizeof(bufB),
			 UIB_ACCESS_LOCAL_WRITE | UIB_ACCESS_REMOTE_WRITE |
			 UIB_ACCESS_REMOTE_READ | UIB_ACCESS_REMOTE_ATOMIC);

	sendA   = bufA + 0;
	writeA  = bufA + 16384;
	readA   = bufA + 32768;
	recvB   = bufB + 0;
	targetB = bufB + 16384;
	atomicA = (uint64_t *)(bufA + 49152);
	atomicB = (uint64_t *)(bufB + 49152);

	memset(&ia, 0, sizeof(ia));
	ia.send_cq = cqA; ia.recv_cq = cqA; ia.qp_type = UIB_QPT_RC;
	ia.max_send_wr = 64; ia.max_recv_wr = 64;
	ia.max_send_sge = 1; ia.max_recv_sge = 1;
	qpA = uib_create_qp(pdA, &ia);

	memset(&ia, 0, sizeof(ia));
	ia.send_cq = cqB; ia.recv_cq = cqB; ia.qp_type = UIB_QPT_RC;
	ia.max_send_wr = 64; ia.max_recv_wr = 64;
	ia.max_send_sge = 1; ia.max_recv_sge = 1;
	qpB = uib_create_qp(pdB, &ia);

	if (connect_qp(qpA, qpB->qp_num, ctxB->local_addr) < 0 ||
	    connect_qp(qpB, qpA->qp_num, ctxA->local_addr) < 0) {
		printf("connect failed\n");
		return 2;
	}

	for (i = 0; i < 64; i++)
		sendA[i] = (uint8_t)(i + 1);
	memset(recvB, 0, 64);
	post_recv_buf(qpB, 0xB001, recvB, 64, mrB->lkey);
	nap_ms(20);
	post_send_buf(qpA, 0xA001, UIB_WR_SEND, sendA, 64, mrA->lkey, 0, 0);
	report_wc("send.small.recv", cqB);
	report_wc("send.small.send", cqA);
	check_bytes("send.small.data", recvB, sendA, 64);

	for (i = 0; i < BIG_LEN; i++)
		sendA[i] = (uint8_t)(i * 7 + 3);
	memset(recvB, 0, BIG_LEN);
	post_recv_buf(qpB, 0xB002, recvB, BIG_LEN, mrB->lkey);
	nap_ms(20);
	post_send_buf(qpA, 0xA002, UIB_WR_SEND, sendA, BIG_LEN, mrA->lkey, 0, 0);
	report_wc("send.big.recv", cqB);
	report_wc("send.big.send", cqA);
	check_bytes("send.big.data", recvB, sendA, BIG_LEN);

	for (i = 0; i < BIG_LEN; i++)
		writeA[i] = (uint8_t)(i ^ 0x5a);
	memset(targetB, 0, BIG_LEN);
	post_send_buf(qpA, 0xA003, UIB_WR_RDMA_WRITE, writeA, BIG_LEN, mrA->lkey,
		      (uintptr_t)targetB, mrB->rkey);
	report_wc("write.send", cqA);
	nap_ms(20);
	check_bytes("write.data", targetB, writeA, BIG_LEN);

	memset(readA, 0, BIG_LEN);
	post_send_buf(qpA, 0xA004, UIB_WR_RDMA_READ, readA, BIG_LEN, mrA->lkey,
		      (uintptr_t)targetB, mrB->rkey);
	report_wc("read.send", cqA);
	check_bytes("read.data", readA, targetB, BIG_LEN);

	if (is_uet) {
		*atomicA = 0;
		*atomicB = 41;
		post_atomic(qpA, 0xA005, UIB_WR_ATOMIC_FETCH_ADD, atomicA,
			    mrA->lkey, (uintptr_t)atomicB, mrB->rkey, 1, 0);
		report_wc("atomic.add", cqA);
		check_bytes("atomic.add.old", atomicA, &(uint64_t){41}, 8);
		check_bytes("atomic.add.new", atomicB, &(uint64_t){42}, 8);

		*atomicA = 0;
		post_atomic(qpA, 0xA006, UIB_WR_ATOMIC_CMP_SWAP, atomicA,
			    mrA->lkey, (uintptr_t)atomicB, mrB->rkey, 42, 99);
		report_wc("atomic.cswap", cqA);
		check_bytes("atomic.cswap.old", atomicA, &(uint64_t){42}, 8);
		check_bytes("atomic.cswap.new", atomicB, &(uint64_t){99}, 8);

		test_uet_atomic_matrix(qpA, cqA, mrA, mrB, atomicA, atomicB);
		if (getenv("UIB_UET_MULTIPATH")) {
			struct uib_uet_multipath_stats stats;
			struct uib_uet_multipath_config config;
			int ok;

			memset(&stats, 0, sizeof(stats));
			memset(&config, 0, sizeof(config));
			ok = !uib_uet_multipath_query(qpA, &config, &stats) &&
				stats.packets && stats.paths_used > 1 &&
				stats.active_entropy >= config.entropy_base &&
				stats.active_entropy <
				config.entropy_base + config.path_count;
			if (getenv("UIB_TEST_DROP_TX"))
				ok = ok && stats.timeout_failovers;
			printf("uet.multipath         %s (paths=%u switches=%llu failovers=%llu)\n",
			       ok ? "PASS" : "FAIL", stats.paths_used,
			       (unsigned long long)stats.path_switches,
			       (unsigned long long)stats.timeout_failovers);
			if (!ok)
				g_fail = 1;
		}
	}

	uib_destroy_qp(qpA);
	uib_destroy_qp(qpB);
	uib_destroy_cq(cqA);
	uib_destroy_cq(cqB);
	uib_dereg_mr(mrA);
	uib_dereg_mr(mrB);
	uib_dealloc_pd(pdA);
	uib_dealloc_pd(pdB);
	uib_close_device(ctxA);
	uib_close_device(ctxB);

	printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
