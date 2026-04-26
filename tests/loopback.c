#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "uib/uib.h"

#define PORT_A 18515
#define PORT_B 18516
#define BIG_LEN 10003

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
			       UIB_ACCESS_REMOTE_READ;
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
	unsigned int i;

	ctxA = uib_open_device("127.0.0.1", PORT_A);
	ctxB = uib_open_device("127.0.0.1", PORT_B);
	if (!ctxA || !ctxB) {
		printf("open_device failed\n");
		return 2;
	}

	pdA = uib_alloc_pd(ctxA);
	pdB = uib_alloc_pd(ctxB);
	cqA = uib_create_cq(ctxA, 64);
	cqB = uib_create_cq(ctxB, 64);

	mrA = uib_reg_mr(pdA, bufA, sizeof(bufA),
			 UIB_ACCESS_LOCAL_WRITE | UIB_ACCESS_REMOTE_WRITE |
			 UIB_ACCESS_REMOTE_READ);
	mrB = uib_reg_mr(pdB, bufB, sizeof(bufB),
			 UIB_ACCESS_LOCAL_WRITE | UIB_ACCESS_REMOTE_WRITE |
			 UIB_ACCESS_REMOTE_READ);

	sendA   = bufA + 0;
	writeA  = bufA + 16384;
	readA   = bufA + 32768;
	recvB   = bufB + 0;
	targetB = bufB + 16384;

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
