#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>

#include "uib/uib.h"

#define TEST_LEN 257

static int failed;

static void check(const char *name, int ok)
{
	printf("%-28s %s\n", name, ok ? "OK" : "FAIL");
	if (!ok)
		failed = 1;
}

static int wait_wc(struct uib_cq *cq, struct uib_wc *wc)
{
	struct timespec delay = {
		.tv_nsec = 1000000,
	};

	for (int i = 0; i < 2000; i++) {
		int n = uib_poll_cq(cq, 1, wc);

		if (n > 0)
			return 0;
		nanosleep(&delay, NULL);
	}
	return -ETIMEDOUT;
}

static int connect_qp(struct uib_qp *qp, struct uib_qp *peer,
		      uint16_t peer_port)
{
	struct uib_qp_attr attr = {
		.qp_state = UIB_QPS_RTS,
		.dest_qp_num = peer->qp_num,
		.sq_psn = 1,
		.rq_psn = 1,
		.path_mtu = 1024,
		.remote_addr = {
			.sin_family = AF_INET,
			.sin_port = htons(peer_port),
			.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		},
	};

	return uib_modify_qp(qp, &attr, UIB_QP_STATE | UIB_QP_DEST_QPN |
			     UIB_QP_SQ_PSN | UIB_QP_RQ_PSN |
			     UIB_QP_PATH_MTU | UIB_QP_AV);
}

int main(void)
{
	struct uib_context *a = NULL, *b = NULL;
	struct uib_pd *apd = NULL, *bpd = NULL;
	struct uib_cq *acq = NULL, *bcq = NULL;
	struct uib_qp *aqp = NULL, *bqp = NULL;
	struct uib_mr *smr = NULL, *rmr = NULL;
	uint8_t send_buf[TEST_LEN], recv_buf[TEST_LEN];
	struct uib_qp_init_attr init = {
		.qp_type = UIB_QPT_RC,
		.max_send_wr = 8,
		.max_recv_wr = 8,
		.max_send_sge = 1,
		.max_recv_sge = 1,
	};
	struct uib_recv_wr recv_wr = {0};
	struct uib_send_wr send_wr = {0}, write_wr = {0};
	struct uib_sge send_sge = {0}, recv_sge = {0};
	struct uib_wc wc;
	struct uib_send_wr *bad = NULL;

	check("select.rocev2.default",
	      uib_protocol_find(NULL) == &uib_rocev2_protocol_ops);
	check("select.dummy", uib_protocol_find("dummy") ==
	      &uib_dummy_protocol_ops);
	check("select.unknown", uib_protocol_find("missing") == NULL);
	check("port.rocev2", uib_protocol_default_port("rocev2") == 4791);
	check("port.dummy", uib_protocol_default_port("dummy") == 18516);

	errno = 0;
	check("open.unknown",
	      !uib_open_device_protocol("127.0.0.1", 18518, "missing") &&
	      errno == EPROTONOSUPPORT);

	a = uib_open_device_protocol("127.0.0.1", 18516, "dummy");
	b = uib_open_device_protocol("127.0.0.1", 18517, "dummy");
	check("dummy.open", a && b && a->pops == &uib_dummy_protocol_ops &&
	      b->pops == &uib_dummy_protocol_ops);
	if (!a || !b)
		goto out;

	apd = uib_alloc_pd(a);
	bpd = uib_alloc_pd(b);
	acq = uib_create_cq(a, 8);
	bcq = uib_create_cq(b, 8);
	init.send_cq = init.recv_cq = acq;
	aqp = uib_create_qp(apd, &init);
	init.send_cq = init.recv_cq = bcq;
	bqp = uib_create_qp(bpd, &init);
	check("dummy.resources", apd && bpd && acq && bcq && aqp && bqp);
	if (!apd || !bpd || !acq || !bcq || !aqp || !bqp)
		goto out;
	check("dummy.connect", !connect_qp(aqp, bqp, 18517) &&
	      !connect_qp(bqp, aqp, 18516));

	for (size_t i = 0; i < sizeof(send_buf); i++)
		send_buf[i] = (uint8_t)(i ^ 0xa5);
	memset(recv_buf, 0, sizeof(recv_buf));
	smr = uib_reg_mr(apd, send_buf, sizeof(send_buf), 0);
	rmr = uib_reg_mr(bpd, recv_buf, sizeof(recv_buf),
			 UIB_ACCESS_LOCAL_WRITE | UIB_ACCESS_REMOTE_WRITE);
	check("dummy.mr", smr && rmr);
	if (!smr || !rmr)
		goto out;

	recv_sge.addr = (uintptr_t)recv_buf;
	recv_sge.length = sizeof(recv_buf);
	recv_sge.lkey = rmr->lkey;
	recv_wr.wr_id = 0x22;
	recv_wr.sg_list = &recv_sge;
	recv_wr.num_sge = 1;
	check("dummy.post_recv", !uib_post_recv(bqp, &recv_wr, NULL));

	send_sge.addr = (uintptr_t)send_buf;
	send_sge.length = sizeof(send_buf);
	send_sge.lkey = smr->lkey;
	send_wr.wr_id = 0x11;
	send_wr.opcode = UIB_WR_SEND;
	send_wr.send_flags = UIB_SEND_SIGNALED;
	send_wr.sg_list = &send_sge;
	send_wr.num_sge = 1;
	check("dummy.post_send", !uib_post_send(aqp, &send_wr, NULL));
	check("dummy.send_wc", !wait_wc(acq, &wc) &&
	      wc.status == UIB_WC_SUCCESS && wc.opcode == UIB_WC_SEND);
	check("dummy.recv_wc", !wait_wc(bcq, &wc) &&
	      wc.status == UIB_WC_SUCCESS && wc.opcode == UIB_WC_RECV &&
	      wc.byte_len == TEST_LEN);
	check("dummy.data", !memcmp(send_buf, recv_buf, sizeof(send_buf)));

	write_wr = send_wr;
	write_wr.wr_id = 0x33;
	write_wr.opcode = UIB_WR_RDMA_WRITE;
	check("dummy.unsupported", uib_post_send(aqp, &write_wr, &bad) ==
	      -EOPNOTSUPP && bad == &write_wr);

out:
	if (smr)
		uib_dereg_mr(smr);
	if (rmr)
		uib_dereg_mr(rmr);
	if (aqp)
		uib_destroy_qp(aqp);
	if (bqp)
		uib_destroy_qp(bqp);
	if (acq)
		uib_destroy_cq(acq);
	if (bcq)
		uib_destroy_cq(bcq);
	if (apd)
		uib_dealloc_pd(apd);
	if (bpd)
		uib_dealloc_pd(bpd);
	if (a)
		uib_close_device(a);
	if (b)
		uib_close_device(b);
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
