// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <endian.h>

#include "uib/uib.h"

#define TEST_BUFFER_LENGTH 128
#define TEST_PAYLOAD_LENGTH 8
#define TEST_RECV_COUNT 8

struct test_endpoint {
	struct uib_context *context;
	struct uib_pd *pd;
	struct uib_cq *cq;
	struct uib_qp *qp;
	struct uib_mr *mr;
	uint8_t buffer[TEST_BUFFER_LENGTH];
};

static int failed;

static void check(const char *name, int condition)
{
	printf("%-36s %s\n", name, condition ? "OK" : "FAIL");
	if (!condition)
		failed = 1;
}

static int endpoint_init(struct test_endpoint *endpoint, const char *protocol,
			 uint16_t port, uint32_t rq_psn)
{
	struct uib_qp_init_attr init;
	struct uib_qp_attr attr;

	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->context = uib_open_device_protocol("127.0.0.1", port,
						     protocol);
	if (!endpoint->context)
		return -1;
	endpoint->pd = uib_alloc_pd(endpoint->context);
	endpoint->cq = uib_create_cq(endpoint->context, 32);
	if (!endpoint->pd || !endpoint->cq)
		return -1;
	memset(&init, 0, sizeof(init));
	init.qp_type = UIB_QPT_RC;
	init.send_cq = endpoint->cq;
	init.recv_cq = endpoint->cq;
	init.max_send_wr = 8;
	init.max_recv_wr = 16;
	init.max_send_sge = 1;
	init.max_recv_sge = 1;
	endpoint->qp = uib_create_qp(endpoint->pd, &init);
	if (!endpoint->qp)
		return -1;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = UIB_QPS_RTS;
	attr.dest_qp_num = endpoint->qp->qp_num;
	attr.sq_psn = rq_psn;
	attr.rq_psn = rq_psn;
	attr.path_mtu = 1024;
	attr.remote_addr.sin_family = AF_INET;
	attr.remote_addr.sin_port = htons(9);
	attr.remote_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (uib_modify_qp(endpoint->qp, &attr,
			  UIB_QP_STATE | UIB_QP_DEST_QPN | UIB_QP_SQ_PSN |
			  UIB_QP_RQ_PSN | UIB_QP_PATH_MTU | UIB_QP_AV) < 0)
		return -1;
	endpoint->mr = uib_reg_mr(endpoint->pd, endpoint->buffer,
				  sizeof(endpoint->buffer), UIB_ACCESS_LOCAL_WRITE);
	return endpoint->mr ? 0 : -1;
}

static void endpoint_cleanup(struct test_endpoint *endpoint)
{
	if (endpoint->mr)
		uib_dereg_mr(endpoint->mr);
	if (endpoint->qp)
		uib_destroy_qp(endpoint->qp);
	if (endpoint->cq)
		uib_destroy_cq(endpoint->cq);
	if (endpoint->pd)
		uib_dealloc_pd(endpoint->pd);
	if (endpoint->context)
		uib_close_device(endpoint->context);
}

static int post_receives(struct test_endpoint *endpoint, unsigned int count,
			 size_t stride)
{
	struct uib_recv_wr request;
	struct uib_sge sge;
	unsigned int i;

	for (i = 0; i < count; i++) {
		memset(&sge, 0, sizeof(sge));
		sge.addr = (uintptr_t)endpoint->buffer + i * stride;
		sge.length = (uint32_t)stride;
		sge.lkey = endpoint->mr->lkey;
		memset(&request, 0, sizeof(request));
		request.wr_id = i + 1;
		request.sg_list = &sge;
		request.num_sge = 1;
		if (uib_post_recv(endpoint->qp, &request, NULL) < 0)
			return -1;
	}
	return 0;
}

static int completion_count(struct test_endpoint *endpoint)
{
	struct uib_wc completions[TEST_RECV_COUNT];

	return uib_poll_cq(endpoint->cq, TEST_RECV_COUNT, completions);
}

static size_t build_rocev2_packet(uint8_t *packet, size_t capacity,
				  uint32_t qpn, uint32_t psn,
				  const uint8_t *payload, size_t length)
{
	struct uib_bth *bth = (struct uib_bth *)packet;
	uint32_t wire_crc;
	uint32_t crc;
	uint8_t pad;
	size_t packet_length;

	pad = (uint8_t)((-(int)length) & 3);
	packet_length = UIB_BTH_BYTES + length + pad + UIB_ICRC_BYTES;
	if (packet_length > capacity)
		return 0;
	uib_bth_pack(bth, UIB_OP_SEND_ONLY, UIB_DEFAULT_PKEY, qpn, psn, 1);
	uib_bth_set_pad(bth, pad);
	memcpy(packet + UIB_BTH_BYTES, payload, length);
	memset(packet + UIB_BTH_BYTES + length, 0, pad);
	crc = uib_icrc(packet, packet_length - UIB_ICRC_BYTES);
	wire_crc = htonl(crc);
	memcpy(packet + packet_length - UIB_ICRC_BYTES, &wire_crc,
	       sizeof(wire_crc));
	return packet_length;
}

static int inject_rocev2(struct test_endpoint *endpoint, uint32_t psn,
			 const uint8_t *payload)
{
	struct sockaddr_in source;
	uint8_t packet[64];
	size_t length;

	memset(&source, 0, sizeof(source));
	source.sin_family = AF_INET;
	source.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	length = build_rocev2_packet(packet, sizeof(packet),
				     endpoint->qp->qp_num, psn, payload,
				     TEST_PAYLOAD_LENGTH);
	if (!length)
		return -1;
	return uib_qp_handle_packet(endpoint->qp, &source, packet, length);
}

static size_t build_uet_packet(uint8_t *packet, size_t capacity, uint32_t qpn,
			       uint32_t psn, uint16_t message_id,
			       uint8_t flags, uint32_t offset,
			       uint32_t total, const uint8_t *payload)
{
	struct uib_uet_pds_req *request;
	struct uib_uet_ses_req *session;
	size_t packet_length;

	packet_length = sizeof(*request) + sizeof(*session) +
		TEST_PAYLOAD_LENGTH;
	if (packet_length > capacity)
		return 0;
	memset(packet, 0, packet_length);
	request = (struct uib_uet_pds_req *)packet;
	session = (struct uib_uet_ses_req *)(request + 1);
	request->prologue.type_next_flags = uib_uet_prologue_pack(
		UIB_UET_PDS_TYPE_RUD_REQ, UIB_UET_NEXT_REQ_STD, 0);
	request->psn = htonl(psn);
	request->spdcid = htons(7);
	request->dpdcid = htons((uint16_t)qpn);
	session->opcode = UIB_UET_SES_SEND;
	session->flags = flags;
	session->msg_id = htons(message_id);
	session->pid_on_fep = htons((uint16_t)qpn);
	session->request_len = htonl(total);
	if (!(flags & UIB_UET_SES_FLAG_SOM))
		session->header_data = uib_uet_ses_fragment_info(
			TEST_PAYLOAD_LENGTH, offset);
	memcpy(session + 1, payload, TEST_PAYLOAD_LENGTH);
	return packet_length;
}

static int inject_uet(struct test_endpoint *endpoint, uint32_t psn,
		      uint16_t message_id, uint8_t flags, uint32_t offset,
		      uint32_t total, const uint8_t *payload)
{
	struct sockaddr_in source;
	uint8_t packet[128];
	size_t length;

	memset(&source, 0, sizeof(source));
	source.sin_family = AF_INET;
	source.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	length = build_uet_packet(packet, sizeof(packet), endpoint->qp->qp_num,
				  psn, message_id, flags, offset, total,
				  payload);
	if (!length)
		return -1;
	return uib_qp_handle_packet(endpoint->qp, &source, packet, length);
}

static void run_rocev2_ordering(void)
{
	static const uint8_t payloads[4][TEST_PAYLOAD_LENGTH] = {
		{ 0x10, 1, 2, 3, 4, 5, 6, 7 },
		{ 0x20, 1, 2, 3, 4, 5, 6, 7 },
		{ 0x30, 1, 2, 3, 4, 5, 6, 7 },
		{ 0x40, 1, 2, 3, 4, 5, 6, 7 },
	};
	static const uint8_t corrupt[TEST_PAYLOAD_LENGTH] = {
		0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
	};
	struct test_endpoint endpoint;
	unsigned int i;

	check("rocev2.setup", !endpoint_init(&endpoint, "rocev2", 19700, 100));
	if (!endpoint.context || !endpoint.mr)
		goto out;
	check("rocev2.post-receives",
	      !post_receives(&endpoint, 4, TEST_PAYLOAD_LENGTH));
	check("rocev2.reversed-future", !inject_rocev2(&endpoint, 101,
						       payloads[1]) &&
	      completion_count(&endpoint) == 0);
	check("rocev2.first-arrival", !inject_rocev2(&endpoint, 100,
						     payloads[0]) &&
	      completion_count(&endpoint) == 1);
	check("rocev2.duplicate-stale", !inject_rocev2(&endpoint, 100,
						       corrupt) &&
	      completion_count(&endpoint) == 0);
	check("rocev2.ahead-of-window", !inject_rocev2(&endpoint, 164,
						       payloads[3]) &&
	      completion_count(&endpoint) == 0);
	for (i = 1; i < 4; i++) {
		check("rocev2.delayed-retry",
		      !inject_rocev2(&endpoint, 100 + i, payloads[i]) &&
		      completion_count(&endpoint) == 1);
	}
	check("rocev2.delivery-order",
	      !memcmp(endpoint.buffer, payloads, sizeof(payloads)));

out:
	endpoint_cleanup(&endpoint);
}

static void run_rocev2_wrap(void)
{
	static const uint8_t payloads[3][TEST_PAYLOAD_LENGTH] = {
		{ 0xa1, 1, 2, 3, 4, 5, 6, 7 },
		{ 0xa2, 1, 2, 3, 4, 5, 6, 7 },
		{ 0xa3, 1, 2, 3, 4, 5, 6, 7 },
	};
	struct test_endpoint endpoint;

	check("rocev2.wrap.setup",
	      !endpoint_init(&endpoint, "rocev2", 19701, 0x00fffffe));
	if (!endpoint.context || !endpoint.mr)
		goto out;
	check("rocev2.wrap.post", !post_receives(&endpoint, 3,
						 TEST_PAYLOAD_LENGTH));
	check("rocev2.wrap.future", !inject_rocev2(&endpoint, 0,
						   payloads[2]) &&
	      completion_count(&endpoint) == 0);
	check("rocev2.wrap.base", !inject_rocev2(&endpoint, 0x00fffffe,
						 payloads[0]) &&
	      completion_count(&endpoint) == 1);
	check("rocev2.wrap.maximum", !inject_rocev2(&endpoint, 0x00ffffff,
						    payloads[1]) &&
	      completion_count(&endpoint) == 1);
	check("rocev2.wrap.zero", !inject_rocev2(&endpoint, 0, payloads[2]) &&
	      completion_count(&endpoint) == 1);
	check("rocev2.wrap.data",
	      !memcmp(endpoint.buffer, payloads, sizeof(payloads)));

out:
	endpoint_cleanup(&endpoint);
}

static void run_uet_ordering(void)
{
	static const uint8_t payloads[4][TEST_PAYLOAD_LENGTH] = {
		{ 0x51, 1, 2, 3, 4, 5, 6, 7 },
		{ 0x52, 1, 2, 3, 4, 5, 6, 7 },
		{ 0x53, 1, 2, 3, 4, 5, 6, 7 },
		{ 0x54, 1, 2, 3, 4, 5, 6, 7 },
	};
	static const uint8_t corrupt[TEST_PAYLOAD_LENGTH] = {
		0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
	};
	struct test_endpoint endpoint;
	uint32_t total = sizeof(payloads);

	check("uet.setup", !endpoint_init(&endpoint, "uet", 19702, 100));
	if (!endpoint.context || !endpoint.mr)
		goto out;
	check("uet.post-receive", !post_receives(&endpoint, 1, total));
	check("uet.ahead-of-window",
	      inject_uet(&endpoint, 100 + UIB_UET_PDC_MPR, 1,
			 UIB_UET_SES_FLAG_EOM, 3 * TEST_PAYLOAD_LENGTH,
			 total, payloads[3]) == -ERANGE &&
	      completion_count(&endpoint) == 0);
	check("uet.reverse.third", !inject_uet(&endpoint, 102, 1, 0,
					       2 * TEST_PAYLOAD_LENGTH, total,
					       payloads[2]));
	check("uet.duplicate-in-window", !inject_uet(&endpoint, 102, 1, 0,
						     2 * TEST_PAYLOAD_LENGTH,
						     total, corrupt));
	check("uet.reverse.eom", !inject_uet(&endpoint, 103, 1,
					     UIB_UET_SES_FLAG_EOM,
					     3 * TEST_PAYLOAD_LENGTH, total,
					     payloads[3]));
	check("uet.loss-hole-open", !inject_uet(&endpoint, 100, 1,
						UIB_UET_SES_FLAG_SOM, 0,
						total, payloads[0]) &&
	      completion_count(&endpoint) == 0);
	check("uet.delayed-hole-close", !inject_uet(&endpoint, 101, 1, 0,
						     TEST_PAYLOAD_LENGTH,
						     total, payloads[1]) &&
	      completion_count(&endpoint) == 1);
	check("uet.reordered-payload",
	      !memcmp(endpoint.buffer, payloads, sizeof(payloads)));
	check("uet.stale-after-window", !inject_uet(&endpoint, 100, 1,
						     UIB_UET_SES_FLAG_SOM, 0,
						     total, corrupt) &&
	      completion_count(&endpoint) == 0 &&
	      !memcmp(endpoint.buffer, payloads, sizeof(payloads)));

out:
	endpoint_cleanup(&endpoint);
}

static void run_uet_wrap(void)
{
	static const uint8_t payloads[3][TEST_PAYLOAD_LENGTH] = {
		{ 0xb1, 1, 2, 3, 4, 5, 6, 7 },
		{ 0xb2, 1, 2, 3, 4, 5, 6, 7 },
		{ 0xb3, 1, 2, 3, 4, 5, 6, 7 },
	};
	struct test_endpoint endpoint;
	uint32_t total = sizeof(payloads);

	check("uet.wrap.setup",
	      !endpoint_init(&endpoint, "uet", 19703, UINT32_MAX - 1));
	if (!endpoint.context || !endpoint.mr)
		goto out;
	check("uet.wrap.post", !post_receives(&endpoint, 1, total));
	check("uet.wrap.zero-first", !inject_uet(&endpoint, 0, 2,
						 UIB_UET_SES_FLAG_EOM,
						 2 * TEST_PAYLOAD_LENGTH, total,
						 payloads[2]));
	check("uet.wrap.maximum", !inject_uet(&endpoint, UINT32_MAX, 2, 0,
					      TEST_PAYLOAD_LENGTH, total,
					      payloads[1]));
	check("uet.wrap.base", !inject_uet(&endpoint, UINT32_MAX - 1, 2,
					   UIB_UET_SES_FLAG_SOM, 0, total,
					   payloads[0]) &&
	      completion_count(&endpoint) == 1);
	check("uet.wrap.data",
	      !memcmp(endpoint.buffer, payloads, sizeof(payloads)));

out:
	endpoint_cleanup(&endpoint);
}

int main(void)
{
	setenv("UIB_ZEROCOPY", "off", 1);
	run_rocev2_ordering();
	run_rocev2_wrap();
	run_uet_ordering();
	run_uet_wrap();
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
