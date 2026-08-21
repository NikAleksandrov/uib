/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>

#include "uib/uib.h"

#define TEST_LENGTH 8192U

static int wait_completion(struct uib_cq *cq, struct uib_wc *completion,
			   enum uib_wc_opcode opcode)
{
	struct timespec delay = {
		.tv_nsec = 1000000,
	};
	int iteration;

	for (iteration = 0; iteration < 5000; iteration++) {
		if (uib_poll_cq(cq, 1, completion) == 1 &&
		    completion->opcode == opcode)
			return 0;
		nanosleep(&delay, NULL);
	}
	return -ETIMEDOUT;
}

static int connect_qp(struct uib_qp *qp, struct uib_qp *peer,
		      uint16_t peer_port)
{
	struct uib_qp_attr attributes = {
		.qp_state = UIB_QPS_RTS,
		.dest_qp_num = peer->qp_num,
		.sq_psn = 1,
		.rq_psn = 1,
		.path_mtu = 1024,
		.remote_addr = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		},
	};

	attributes.remote_addr.sin_port = htons(peer_port);
	return uib_modify_qp(qp, &attributes, UIB_QP_STATE |
			     UIB_QP_DEST_QPN | UIB_QP_SQ_PSN |
			     UIB_QP_RQ_PSN | UIB_QP_PATH_MTU | UIB_QP_AV);
}

static int run_protocol(const char *protocol, uint16_t first_port)
{
	struct uib_qp_init_attr initialization = {
		.qp_type = UIB_QPT_RC,
		.max_send_wr = 32,
		.max_recv_wr = 32,
		.max_send_sge = 1,
		.max_recv_sge = 1,
	};
	struct uib_send_wr send = {0};
	struct uib_rx_loan loan = {0};
	struct uib_context *receiver_context;
	struct uib_context *sender_context;
	struct uib_qp *receiver_qp;
	struct uib_qp *sender_qp;
	struct uib_pd *receiver_pd;
	struct uib_pd *sender_pd;
	struct uib_cq *receiver_cq;
	struct uib_cq *sender_cq;
	struct uib_mr *sender_mr;
	struct uib_sge sender_sge;
	struct uib_wc completion;
	uint8_t *sender_data;
	size_t index;
	int rc;

	receiver_context = NULL;
	sender_context = NULL;
	receiver_qp = NULL;
	sender_qp = NULL;
	receiver_pd = NULL;
	sender_pd = NULL;
	receiver_cq = NULL;
	sender_cq = NULL;
	sender_mr = NULL;
	sender_data = malloc(TEST_LENGTH);
	if (!sender_data)
		return -ENOMEM;
	for (index = 0; index < TEST_LENGTH; index++)
		sender_data[index] = (uint8_t)(index ^ 0x5a);
	sender_context = uib_open_device_protocol("127.0.0.1", first_port,
						  protocol);
	receiver_context = uib_open_device_protocol("127.0.0.1",
						    first_port + 1,
						    protocol);
	if (!sender_context || !receiver_context) {
		rc = -errno;
		goto out;
	}
	sender_pd = uib_alloc_pd(sender_context);
	receiver_pd = uib_alloc_pd(receiver_context);
	sender_cq = uib_create_cq(sender_context, 64);
	receiver_cq = uib_create_cq(receiver_context, 64);
	if (!sender_pd || !receiver_pd || !sender_cq || !receiver_cq) {
		rc = -ENOMEM;
		goto out;
	}
	initialization.send_cq = sender_cq;
	initialization.recv_cq = sender_cq;
	sender_qp = uib_create_qp(sender_pd, &initialization);
	initialization.send_cq = receiver_cq;
	initialization.recv_cq = receiver_cq;
	receiver_qp = uib_create_qp(receiver_pd, &initialization);
	if (!sender_qp || !receiver_qp) {
		rc = -ENOMEM;
		goto out;
	}
	rc = connect_qp(sender_qp, receiver_qp, first_port + 1);
	if (rc < 0)
		goto out;
	rc = connect_qp(receiver_qp, sender_qp, first_port);
	if (rc < 0)
		goto out;
	sender_mr = uib_reg_mr(sender_pd, sender_data, TEST_LENGTH, 0);
	if (!sender_mr) {
		rc = -errno;
		goto out;
	}
	rc = uib_post_recv_loan(receiver_qp, 0x22, TEST_LENGTH);
	if (rc < 0)
		goto out;
	sender_sge.addr = (uint64_t)(uintptr_t)sender_data;
	sender_sge.length = TEST_LENGTH;
	sender_sge.lkey = sender_mr->lkey;
	send.wr_id = 0x11;
	send.opcode = UIB_WR_SEND;
	send.send_flags = UIB_SEND_SIGNALED;
	send.sg_list = &sender_sge;
	send.num_sge = 1;
	rc = uib_post_send(sender_qp, &send, NULL);
	if (rc < 0)
		goto out;
	rc = wait_completion(sender_cq, &completion, UIB_WC_SEND);
	if (rc < 0 || completion.status != UIB_WC_SUCCESS)
		goto out;
	rc = wait_completion(receiver_cq, &completion, UIB_WC_RECV);
	if (rc < 0 || completion.status != UIB_WC_SUCCESS ||
	    completion.byte_len != TEST_LENGTH)
		goto out;
	rc = uib_wc_take_rx_loan(&completion, &loan);
	if (rc < 0 || loan.flags != UIB_RX_LOAN_COPIED ||
	    loan.length != TEST_LENGTH ||
	    memcmp(sender_data, loan.data, TEST_LENGTH))
		rc = -EIO;
	else
		rc = 0;

out:
	uib_release_rx_loan(&loan);
	if (sender_mr)
		uib_dereg_mr(sender_mr);
	if (sender_qp)
		uib_destroy_qp(sender_qp);
	if (receiver_qp)
		uib_destroy_qp(receiver_qp);
	if (sender_cq)
		uib_destroy_cq(sender_cq);
	if (receiver_cq)
		uib_destroy_cq(receiver_cq);
	if (sender_pd)
		uib_dealloc_pd(sender_pd);
	if (receiver_pd)
		uib_dealloc_pd(receiver_pd);
	if (sender_context)
		uib_close_device(sender_context);
	if (receiver_context)
		uib_close_device(receiver_context);
	free(sender_data);
	printf("%-28s %s\n", protocol, rc ? "FAIL" : "OK");
	return rc;
}

int main(void)
{
	int failed;

	failed = 0;
	if (run_protocol("rocev2", 20210))
		failed = 1;
	if (run_protocol("uet", 20220))
		failed = 1;
	if (run_protocol("dummy", 20230))
		failed = 1;
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed;
}
