// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

#include "uib/uib.h"

#define CAPTURE_PORT 18524
#define RECEIVER_PORT 18525
#define PAYLOAD_SIZE 32

static int failures;

static void check(const char *name, int condition)
{
	printf("%-32s %s\n", name, condition ? "PASS" : "FAIL");
	if (!condition)
		failures++;
}

static int connect_qp(struct uib_qp *qp, uint32_t remote_qpn,
		      const struct sockaddr_in *remote)
{
	struct uib_qp_attr attr = {0};
	int rc;

	attr.qp_state = UIB_QPS_INIT;
	rc = uib_modify_qp(qp, &attr, UIB_QP_STATE);
	if (rc < 0)
		return rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = UIB_QPS_RTR;
	attr.dest_qp_num = remote_qpn;
	attr.remote_addr = *remote;
	rc = uib_modify_qp(qp, &attr, UIB_QP_STATE | UIB_QP_DEST_QPN |
			   UIB_QP_AV | UIB_QP_RQ_PSN);
	if (rc < 0)
		return rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = UIB_QPS_RTS;
	return uib_modify_qp(qp, &attr, UIB_QP_STATE | UIB_QP_SQ_PSN);
}

static int post_receive(struct uib_qp *qp, struct uib_mr *mr, void *buffer,
			uint64_t wr_id)
{
	struct uib_recv_wr *bad;
	struct uib_recv_wr wr = {0};
	struct uib_sge sge = {
		.addr = (uintptr_t)buffer,
		.length = PAYLOAD_SIZE,
		.lkey = mr->lkey,
	};

	wr.wr_id = wr_id;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	return uib_post_recv(qp, &wr, &bad);
}

static size_t build_send(uint8_t *packet, uint32_t qpn, uint32_t psn,
			 const uint8_t *payload)
{
	uint32_t wire_crc;
	size_t length = UIB_BTH_BYTES + PAYLOAD_SIZE + UIB_ICRC_BYTES;

	uib_bth_pack((struct uib_bth *)packet, UIB_OP_SEND_ONLY,
		     UIB_DEFAULT_PKEY, qpn, psn, 0);
	memcpy(packet + UIB_BTH_BYTES, payload, PAYLOAD_SIZE);
	wire_crc = htonl(uib_icrc(packet, length - UIB_ICRC_BYTES));
	memcpy(packet + length - UIB_ICRC_BYTES, &wire_crc, sizeof(wire_crc));
	return length;
}

static int collect_responses(int fd, int *ack_count, int *cnp_count,
			     int *ect0_count)
{
	uint8_t packet[UIB_RECV_BUF_LEN];
	int received = 0;

	*ack_count = 0;
	*cnp_count = 0;
	*ect0_count = 0;
	for (;;) {
		uint8_t control[CMSG_SPACE(sizeof(uint8_t))];
		struct iovec iov = {
			.iov_base = packet,
			.iov_len = sizeof(packet),
		};
		struct msghdr message = {
			.msg_iov = &iov,
			.msg_iovlen = 1,
			.msg_control = control,
			.msg_controllen = sizeof(control),
		};
		struct cmsghdr *cmsg;
		uint8_t opcode;
		ssize_t length;

		length = recvmsg(fd, &message, 0);
		if (length < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return received;
			return -errno;
		}
		if ((size_t)length < UIB_BTH_BYTES + UIB_ICRC_BYTES)
			continue;
		uib_bth_unpack((const struct uib_bth *)packet, &opcode,
			       NULL, NULL, NULL);
		if (opcode == UIB_OP_ACKNOWLEDGE)
			(*ack_count)++;
		else if (opcode == UIB_OP_CNP)
			(*cnp_count)++;
		for (cmsg = CMSG_FIRSTHDR(&message); cmsg;
		     cmsg = CMSG_NXTHDR(&message, cmsg)) {
			uint8_t received_tos;

			if (cmsg->cmsg_level != IPPROTO_IP ||
			    cmsg->cmsg_type != IP_TOS ||
			    cmsg->cmsg_len < CMSG_LEN(sizeof(received_tos)))
				continue;
			memcpy(&received_tos, CMSG_DATA(cmsg),
			       sizeof(received_tos));
			if (IPTOS_ECN(received_tos) == IPTOS_ECN_ECT0)
				(*ect0_count)++;
		}
		received++;
	}
}

static int wait_completion(struct uib_cq *cq)
{
	struct uib_wc wc;
	struct timespec delay = {
		.tv_nsec = 1000000,
	};
	int i;

	for (i = 0; i < 1000; i++) {
		if (uib_poll_cq(cq, 1, &wc) == 1)
			return wc.status == UIB_WC_SUCCESS;
		nanosleep(&delay, NULL);
	}
	return 0;
}

int main(void)
{
	uint8_t received[PAYLOAD_SIZE] = {0};
	uint8_t payload[PAYLOAD_SIZE];
	uint8_t packet[UIB_BTH_BYTES + PAYLOAD_SIZE + UIB_ICRC_BYTES];
	struct sockaddr_in capture = {0};
	struct sockaddr_in receiver = {0};
	struct uib_qp_init_attr init = {0};
	struct timeval timeout = {
		.tv_usec = 100000,
	};
	struct uib_context *context;
	struct uib_pd *pd;
	struct uib_cq *cq;
	struct uib_mr *mr;
	struct uib_qp *qp;
	size_t packet_len;
	int ack_count;
	int cnp_count;
	int ect0_count;
	int capture_fd;
	int tos;
	int one = 1;
	int i;

	setenv("UIB_PROTOCOL", "rocev2", 1);
	setenv("UIB_TRANSPORT_MODE", "udp-sync", 1);
	setenv("UIB_ZEROCOPY", "off", 1);
	capture_fd = socket(AF_INET, SOCK_DGRAM, 0);
	check("capture.socket", capture_fd >= 0);
	if (capture_fd < 0)
		return 1;
	capture.sin_family = AF_INET;
	capture.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	capture.sin_port = htons(CAPTURE_PORT);
	check("capture.bind",
	      !bind(capture_fd, (struct sockaddr *)&capture, sizeof(capture)));
	check("capture.recvtos",
	      !setsockopt(capture_fd, IPPROTO_IP, IP_RECVTOS, &one,
			  sizeof(one)));
	setsockopt(capture_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		   sizeof(timeout));
	context = uib_open_device("127.0.0.1", RECEIVER_PORT);
	check("receiver.open", context != NULL);
	if (!context)
		goto out_socket;
	pd = uib_alloc_pd(context);
	cq = uib_create_cq(context, 16);
	mr = uib_reg_mr(pd, received, sizeof(received),
			UIB_ACCESS_LOCAL_WRITE);
	init.send_cq = cq;
	init.recv_cq = cq;
	init.qp_type = UIB_QPT_RC;
	init.max_send_wr = 4;
	init.max_recv_wr = 4;
	init.max_send_sge = 1;
	init.max_recv_sge = 1;
	qp = uib_create_qp(pd, &init);
	check("receiver.resources", pd && cq && mr && qp);
	if (!pd || !cq || !mr || !qp)
		goto out_context;
	check("receiver.connect", !connect_qp(qp, 0x1234, &capture));
	receiver.sin_family = AF_INET;
	receiver.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	receiver.sin_port = htons(RECEIVER_PORT);
	for (i = 0; i < PAYLOAD_SIZE; i++)
		payload[i] = (uint8_t)(i + 7);

	check("ce.post_receive", !post_receive(qp, mr, received, 1));
	tos = IPTOS_ECN_CE;
	check("ce.set_tos", !setsockopt(capture_fd, IPPROTO_IP, IP_TOS,
					 &tos, sizeof(tos)));
	packet_len = build_send(packet, qp->qp_num, 0, payload);
	check("ce.send", sendto(capture_fd, packet, packet_len, 0,
				(struct sockaddr *)&receiver, sizeof(receiver)) ==
			 (ssize_t)packet_len);
	check("ce.receive_completion", wait_completion(cq));
	check("ce.payload", !memcmp(received, payload, sizeof(received)));
	check("ce.responses",
	      collect_responses(capture_fd, &ack_count, &cnp_count,
				&ect0_count) >= 2);
	check("ce.ack", ack_count == 1);
	check("ce.cnp", cnp_count == 1);
	check("ce.responses_ect0", ect0_count == 2);

	memset(received, 0, sizeof(received));
	check("ect.post_receive", !post_receive(qp, mr, received, 2));
	tos = IPTOS_ECN_ECT0;
	check("ect.set_tos", !setsockopt(capture_fd, IPPROTO_IP, IP_TOS,
					  &tos, sizeof(tos)));
	packet_len = build_send(packet, qp->qp_num, 1, payload);
	check("ect.send", sendto(capture_fd, packet, packet_len, 0,
				 (struct sockaddr *)&receiver, sizeof(receiver)) ==
			  (ssize_t)packet_len);
	check("ect.receive_completion", wait_completion(cq));
	check("ect.responses",
	      collect_responses(capture_fd, &ack_count, &cnp_count,
				&ect0_count) >= 1);
	check("ect.ack", ack_count == 1);
	check("ect.no_cnp", cnp_count == 0);
	check("ect.response_ect0", ect0_count == 1);

	uib_destroy_qp(qp);
	uib_dereg_mr(mr);
	uib_destroy_cq(cq);
	uib_dealloc_pd(pd);
out_context:
	uib_close_device(context);
out_socket:
	close(capture_fd);
	printf("RESULT %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
