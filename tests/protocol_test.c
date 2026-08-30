/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>

#include "uib/uib.h"
#include "../src/host_zc_internal.h"

#define TEST_LEN 257
#define TEST_DUMMY_MAGIC 0x55494244u
#define TEST_DUMMY_VERSION 1
#define TEST_DUMMY_SEND 1

struct test_dummy_hdr {
	uint32_t magic;
	uint8_t  version;
	uint8_t  opcode;
	uint16_t header_len;
	uint32_t dqpn;
	uint32_t sqpn;
	uint32_t psn;
	uint32_t payload_len;
} __packed;

struct direct_place {
	struct uib_rq_slot *slot;
	const void         *data;
	size_t              length;
	uint32_t            offset;
	int                 rc;
};

static int failed;

static void mark_released(void *opaque)
{
	int *released = opaque;

	*released = 1;
}

static void place_direct(void *opaque)
{
	struct direct_place *place = opaque;

	place->rc = uib_host_loan_place(place->slot, place->offset,
					place->data, place->length);
}

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
	int i;

	for (i = 0; i < 2000; i++) {
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
	struct uib_context *config_context = NULL;
	struct uib_pd *apd = NULL, *bpd = NULL;
	struct uib_cq *acq = NULL, *bcq = NULL;
	struct uib_qp *aqp = NULL, *bqp = NULL;
	struct uib_qp *aqp2 = NULL, *bqp2 = NULL;
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
	struct uib_rx_loan loan = {0};
	struct uib_rq_slot direct_slot;
	struct uib_rq_slot repost_slot;
	struct direct_place direct_place;
	struct sockaddr_in source = {0};
	uint8_t direct_packet[sizeof(struct test_dummy_hdr) + TEST_LEN];
	struct test_dummy_hdr *direct_header;
	int direct_released = 0;
	int first_released = 0;
	int second_released = 0;
	struct uib_send_wr *bad = NULL;
	size_t i;

	setvbuf(stdout, NULL, _IONBF, 0);
	setenv(UIB_ENV_UDP_WORKERS, "2", 1);
	setenv(UIB_ENV_UDP_WORKER_AFFINITY, "off", 1);
	setenv(UIB_ENV_TX_WORKERS, "2", 1);
	setenv(UIB_ENV_TX_WORKER_AFFINITY, "off", 1);

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

	setenv(UIB_ENV_TRANSPORT_MODE, "udp-sync", 1);
	config_context = uib_open_device_protocol("127.0.0.1", 18520,
						  "uet");
	check("tx-workers.open", config_context != NULL);
	if (config_context)
		uib_close_device(config_context);
	config_context = NULL;
	setenv(UIB_ENV_TX_WORKERS, "0", 1);
	errno = 0;
	config_context = uib_open_device_protocol("127.0.0.1", 18520,
						  "uet");
	check("tx-workers.invalid", !config_context && errno == EINVAL);
	setenv(UIB_ENV_TX_WORKERS, "2", 1);
	setenv(UIB_ENV_UDP_FLOWS, "2", 1);
	config_context = uib_open_device_protocol("127.0.0.1", 18520,
						  "uet");
	check("udp-flows.open", config_context != NULL);
	if (config_context)
		uib_close_device(config_context);
	config_context = NULL;
	setenv(UIB_ENV_ZEROCOPY, "required", 1);
	errno = 0;
	config_context = uib_open_device_protocol("127.0.0.1", 18520,
						  "uet");
	check("udp-flows.required-zc", !config_context && errno == ENOTSUP);
	unsetenv(UIB_ENV_ZEROCOPY);
	unsetenv(UIB_ENV_TRANSPORT_MODE);
	errno = 0;
	config_context = uib_open_device_protocol("127.0.0.1", 18520,
						  "uet");
	check("udp-flows.async", !config_context && errno == ENOTSUP);
	setenv(UIB_ENV_TRANSPORT_MODE, "udp-sync", 1);
	errno = 0;
	config_context = uib_open_device_protocol("127.0.0.1", 65500,
						  "uet");
	check("udp-flows.port-range", !config_context && errno == ERANGE);
	errno = 0;
	config_context = uib_open_device_protocol("127.0.0.1", 18520,
						  "dummy");
	check("udp-flows.protocol", !config_context &&
	      errno == EPROTONOSUPPORT);
	setenv(UIB_ENV_UDP_FLOWS, "65", 1);
	errno = 0;
	config_context = uib_open_device_protocol("127.0.0.1", 18520,
						  "uet");
	check("udp-flows.invalid", !config_context && errno == EINVAL);
	unsetenv(UIB_ENV_UDP_FLOWS);
	unsetenv(UIB_ENV_TRANSPORT_MODE);

	setenv(UIB_ENV_RETRY_TIMEOUT_US, "invalid", 1);
	errno = 0;
	config_context = uib_open_device_protocol("127.0.0.1", 18519,
						  "dummy");
	check("retry-timeout.invalid", !config_context && errno == EINVAL);
	setenv(UIB_ENV_RETRY_TIMEOUT_US, "1000", 1);
	config_context = uib_open_device_protocol("127.0.0.1", 18519,
						  "dummy");
	check("retry-timeout.minimum", config_context &&
	      config_context->retry_timeout_ns == 1000000ULL);
	if (config_context)
		uib_close_device(config_context);
	config_context = NULL;
	unsetenv(UIB_ENV_RETRY_TIMEOUT_US);

	a = uib_open_device_protocol("127.0.0.1", 18516, "dummy");
	b = uib_open_device_protocol("127.0.0.1", 18517, "dummy");
	check("dummy.open", a && b && a->pops == &uib_dummy_protocol_ops &&
	      b->pops == &uib_dummy_protocol_ops);
	check("retry-timeout.default", a && b &&
	      a->retry_timeout_ns == 20000000ULL &&
	      b->retry_timeout_ns == 20000000ULL);
	if (!a || !b)
		goto out;
	check("loan.capabilities",
	      uib_host_zc_capabilities(b) == UIB_HOST_ZC_PROVIDER_LOANS);

	apd = uib_alloc_pd(a);
	bpd = uib_alloc_pd(b);
	acq = uib_create_cq(a, 8);
	bcq = uib_create_cq(b, 8);
	init.send_cq = init.recv_cq = acq;
	aqp = uib_create_qp(apd, &init);
	init.send_cq = init.recv_cq = bcq;
	bqp = uib_create_qp(bpd, &init);
	init.send_cq = init.recv_cq = acq;
	aqp2 = uib_create_qp(apd, &init);
	init.send_cq = init.recv_cq = bcq;
	bqp2 = uib_create_qp(bpd, &init);
	check("dummy.resources", apd && bpd && acq && bcq && aqp && bqp &&
	      aqp2 && bqp2);
	if (!apd || !bpd || !acq || !bcq || !aqp || !bqp || !aqp2 || !bqp2)
		goto out;
	check("dummy.connect", !connect_qp(aqp, bqp, 18517) &&
	      !connect_qp(bqp, aqp, 18516) &&
	      !connect_qp(aqp2, bqp2, 18517) &&
	      !connect_qp(bqp2, aqp2, 18516));

	for (i = 0; i < sizeof(send_buf); i++)
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

	recv_wr.wr_id = 0x23;
	check("workers.second.post_recv", !uib_post_recv(bqp2, &recv_wr, NULL));
	send_wr.wr_id = 0x12;
	check("workers.second.post_send", !uib_post_send(aqp2, &send_wr, NULL));
	check("workers.second.send_wc", !wait_wc(acq, &wc) &&
	      wc.status == UIB_WC_SUCCESS && wc.opcode == UIB_WC_SEND);
	check("workers.second.recv_wc", !wait_wc(bcq, &wc) &&
	      wc.wr_id == 0x23 && wc.status == UIB_WC_SUCCESS &&
	      wc.opcode == UIB_WC_RECV && wc.byte_len == TEST_LEN);

	check("loan.post", !uib_post_recv_loan(bqp, 0x44,
					     sizeof(send_buf)));
	send_wr.wr_id = 0x55;
	check("loan.send", !uib_post_send(aqp, &send_wr, NULL));
	check("loan.send_wc", !wait_wc(acq, &wc) &&
	      wc.status == UIB_WC_SUCCESS && wc.opcode == UIB_WC_SEND);
	check("loan.recv_wc", !wait_wc(bcq, &wc) && wc.wr_id == 0x44 &&
	      wc.status == UIB_WC_SUCCESS && wc.byte_len == TEST_LEN);
	check("loan.take", !uib_wc_take_rx_loan(&wc, &loan));
	check("loan.copied", loan.flags == UIB_RX_LOAN_COPIED &&
	      loan.length == TEST_LEN && loan.capacity == TEST_LEN);
	check("loan.data", !memcmp(send_buf, loan.data, sizeof(send_buf)));
	uib_release_rx_loan(&loan);

	direct_header = (struct test_dummy_hdr *)direct_packet;
	direct_header->magic = htonl(TEST_DUMMY_MAGIC);
	direct_header->version = TEST_DUMMY_VERSION;
	direct_header->opcode = TEST_DUMMY_SEND;
	direct_header->header_len = htons(sizeof(*direct_header));
	direct_header->dqpn = htonl(bqp->qp_num);
	direct_header->sqpn = htonl(aqp->qp_num);
	direct_header->psn = htonl(3);
	direct_header->payload_len = htonl(TEST_LEN);
	memcpy(direct_packet + sizeof(*direct_header), send_buf, TEST_LEN);
	check("loan.direct_post", !uib_post_recv_loan(bqp, 0x66, TEST_LEN));
	uib_transport_deliver_direct(b, &source, direct_packet,
				     sizeof(direct_packet), mark_released,
				     &direct_released);
	check("loan.direct_held", !direct_released);
	check("loan.direct_wc", !wait_wc(bcq, &wc) && wc.wr_id == 0x66);
	check("loan.direct_take", !uib_wc_take_rx_loan(&wc, &loan));
	check("loan.direct", loan.flags == UIB_RX_LOAN_DIRECT &&
	      loan.data == direct_packet + sizeof(*direct_header) &&
	      !memcmp(loan.data, send_buf, TEST_LEN));
	check("loan.repost", !uib_repost_rx_loan(bqp, &loan, 0x77));
	check("loan.direct_release", direct_released && !loan.private_data);
	check("loan.repost_pop", !uib_qp_pop_recv(bqp, &repost_slot));
	uib_qp_emit_recv_slot(bqp, &repost_slot, UIB_WC_SUCCESS, UIB_WC_RECV,
			      0, aqp->qp_num, 0);
	uib_qp_release_recv(&repost_slot);
	check("loan.repost_wc", !wait_wc(bcq, &wc) && wc.wr_id == 0x77);
	check("loan.repost_take", !uib_wc_take_rx_loan(&wc, &loan));
	uib_release_rx_loan(&loan);

	check("loan.segments_post", !uib_post_recv_loan(bqp, 0x88, 64));
	check("loan.segments_pop", !uib_qp_pop_recv(bqp, &direct_slot));
	direct_place.slot = &direct_slot;
	direct_place.data = send_buf;
	direct_place.length = 32;
	direct_place.offset = 0;
	direct_place.rc = -1;
	uib_transport_direct_scope(send_buf, 32, mark_released,
				   &first_released, place_direct, &direct_place);
	check("loan.segment_first", !direct_place.rc && !first_released);
	direct_place.data = send_buf + 32;
	direct_place.offset = 32;
	uib_transport_direct_scope(send_buf + 32, 32, mark_released,
				   &second_released, place_direct, &direct_place);
	check("loan.segment_second", !direct_place.rc && !second_released);
	uib_qp_emit_recv_slot(bqp, &direct_slot, UIB_WC_SUCCESS, UIB_WC_RECV,
			      64, aqp->qp_num, 0);
	uib_qp_release_recv(&direct_slot);
	check("loan.segments_wc", !wait_wc(bcq, &wc) && wc.wr_id == 0x88);
	check("loan.segments_take", !uib_wc_take_rx_loan(&wc, &loan));
	check("loan.segments", loan.flags == UIB_RX_LOAN_DIRECT &&
	      !loan.data && loan.segment_count == 2 &&
	      loan.segments[0].iov_base == send_buf &&
	      loan.segments[0].iov_len == 32 &&
	      loan.segments[1].iov_base == send_buf + 32 &&
	      loan.segments[1].iov_len == 32);
	uib_release_rx_loan(&loan);
	check("loan.segments_release", first_released && second_released);

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
	if (aqp2)
		uib_destroy_qp(aqp2);
	if (bqp2)
		uib_destroy_qp(bqp2);
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
	unsetenv(UIB_ENV_UDP_WORKERS);
	unsetenv(UIB_ENV_UDP_WORKER_AFFINITY);
	unsetenv(UIB_ENV_TX_WORKERS);
	unsetenv(UIB_ENV_TX_WORKER_AFFINITY);
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
