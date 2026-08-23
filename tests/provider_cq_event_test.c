/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>

#include <infiniband/verbs.h>

#define TEST_LENGTH 256
#define TEST_IMMEDIATE 0x12345678U

struct endpoint {
	struct ibv_comp_channel *channel;
	struct ibv_context      *context;
	struct ibv_device       *device;
	struct ibv_pd           *pd;
	struct ibv_cq           *cq;
	struct ibv_qp           *qp;
	struct ibv_mr           *mr;
	union ibv_gid             gid;
	uint8_t                  *buffer;
};

static int open_endpoint(struct endpoint *endpoint, struct ibv_device *device,
			 const char *address)
{
	struct ibv_qp_init_attr init;

	memset(endpoint, 0, sizeof(*endpoint));
	setenv("UIB_BIND_IP", address, 1);
	endpoint->context = ibv_open_device(device);
	if (!endpoint->context)
		return -errno;
	endpoint->channel = ibv_create_comp_channel(endpoint->context);
	endpoint->pd = ibv_alloc_pd(endpoint->context);
	endpoint->cq = ibv_create_cq(endpoint->context, 8, endpoint,
				     endpoint->channel, 0);
	endpoint->buffer = calloc(1, TEST_LENGTH);
	if (!endpoint->channel || !endpoint->pd || !endpoint->cq ||
	    !endpoint->buffer)
		return -ENOMEM;
	endpoint->mr = ibv_reg_mr_iova2(endpoint->pd, endpoint->buffer,
					TEST_LENGTH,
					(uint64_t)(uintptr_t)endpoint->buffer,
					IBV_ACCESS_LOCAL_WRITE |
					IBV_ACCESS_REMOTE_WRITE);
	if (!endpoint->mr)
		return -errno;
	memset(&init, 0, sizeof(init));
	init.send_cq = endpoint->cq;
	init.recv_cq = endpoint->cq;
	init.qp_type = IBV_QPT_RC;
	init.cap.max_send_wr = 8;
	init.cap.max_recv_wr = 8;
	init.cap.max_send_sge = 1;
	init.cap.max_recv_sge = 1;
	endpoint->qp = ibv_create_qp(endpoint->pd, &init);
	if (!endpoint->qp || ibv_query_gid(endpoint->context, 1, 0,
					   &endpoint->gid))
		return -EIO;
	return 0;
}

static void close_endpoint(struct endpoint *endpoint)
{
	if (endpoint->qp)
		ibv_destroy_qp(endpoint->qp);
	if (endpoint->mr)
		ibv_dereg_mr(endpoint->mr);
	free(endpoint->buffer);
	if (endpoint->cq)
		ibv_destroy_cq(endpoint->cq);
	if (endpoint->pd)
		ibv_dealloc_pd(endpoint->pd);
	if (endpoint->channel)
		ibv_destroy_comp_channel(endpoint->channel);
	if (endpoint->context)
		ibv_close_device(endpoint->context);
}

static int connect_endpoint(struct endpoint *local,
			    const struct endpoint *remote)
{
	struct ibv_qp_attr attributes;
	int mask;

	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = IBV_QPS_INIT;
	attributes.port_num = 1;
	attributes.pkey_index = 0;
	mask = IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX;
	if (ibv_modify_qp(local->qp, &attributes, mask))
		return -1;
	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = IBV_QPS_RTR;
	attributes.path_mtu = IBV_MTU_1024;
	attributes.dest_qp_num = remote->qp->qp_num;
	attributes.ah_attr.is_global = 1;
	attributes.ah_attr.port_num = 1;
	attributes.ah_attr.grh.dgid = remote->gid;
	attributes.ah_attr.grh.hop_limit = 64;
	mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
		IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
	if (ibv_modify_qp(local->qp, &attributes, mask))
		return -1;
	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = IBV_QPS_RTS;
	mask = IBV_QP_STATE | IBV_QP_SQ_PSN;
	return ibv_modify_qp(local->qp, &attributes, mask);
}

static int wait_event(struct endpoint *endpoint, uint64_t wr_id,
		      enum ibv_wc_opcode opcode)
{
	struct ibv_cq *event_cq;
	struct ibv_wc completion;
	struct pollfd poll_fd;
	void *context;

	poll_fd.fd = endpoint->channel->fd;
	poll_fd.events = POLLIN;
	poll_fd.revents = 0;
	if (poll(&poll_fd, 1, 3000) != 1)
		return -1;
	if (ibv_get_cq_event(endpoint->channel, &event_cq, &context))
		return -1;
	if (event_cq != endpoint->cq || context != endpoint) {
		ibv_ack_cq_events(event_cq, 1);
		return -1;
	}
	ibv_ack_cq_events(event_cq, 1);
	if (ibv_poll_cq(endpoint->cq, 1, &completion) != 1)
		return -1;
	if (completion.wr_id != wr_id || completion.status != IBV_WC_SUCCESS ||
	    completion.opcode != opcode)
		return -1;
	if (opcode == IBV_WC_RECV_RDMA_WITH_IMM &&
	    (!(completion.wc_flags & IBV_WC_WITH_IMM) ||
	     ntohl(completion.imm_data) != TEST_IMMEDIATE ||
	     completion.byte_len != TEST_LENGTH))
		return -1;
	return 0;
}

int main(void)
{
	struct ibv_recv_wr *bad_receive;
	struct ibv_send_wr *bad_send;
	struct ibv_device **devices;
	struct endpoint receiver = {0};
	struct endpoint sender = {0};
	struct ibv_recv_wr receive;
	struct ibv_send_wr send;
	struct ibv_device *device = NULL;
	struct ibv_sge receive_sge;
	struct ibv_sge send_sge;
	int device_count;
	int i;
	int rc = 1;

	setenv("UIB_PROTOCOL", "rocev2", 1);
	setenv("UIB_URING_RX_MODE", "multishot", 1);
	devices = ibv_get_device_list(&device_count);
	for (i = 0; devices && i < device_count; i++) {
		if (!strcmp(ibv_get_device_name(devices[i]), "uib0")) {
			device = devices[i];
			break;
		}
	}
	if (!device || open_endpoint(&sender, device, "127.0.0.1") < 0 ||
	    open_endpoint(&receiver, device, "127.0.0.2") < 0)
		goto out;
	if (connect_endpoint(&sender, &receiver) ||
	    connect_endpoint(&receiver, &sender))
		goto out;
	memset(sender.buffer, 0x5a, TEST_LENGTH);
	memset(&receive_sge, 0, sizeof(receive_sge));
	receive_sge.addr = (uintptr_t)receiver.buffer;
	receive_sge.length = TEST_LENGTH;
	receive_sge.lkey = receiver.mr->lkey;
	memset(&receive, 0, sizeof(receive));
	receive.wr_id = 0x2001;
	receive.sg_list = &receive_sge;
	receive.num_sge = 1;
	if (ibv_post_recv(receiver.qp, &receive, &bad_receive))
		goto out;
	if (ibv_req_notify_cq(sender.cq, 0) ||
	    ibv_req_notify_cq(receiver.cq, 0))
		goto out;
	memset(&send_sge, 0, sizeof(send_sge));
	send_sge.addr = (uintptr_t)sender.buffer;
	send_sge.length = TEST_LENGTH;
	send_sge.lkey = sender.mr->lkey;
	memset(&send, 0, sizeof(send));
	send.wr_id = 0x1001;
	send.opcode = IBV_WR_SEND;
	send.send_flags = IBV_SEND_SIGNALED;
	send.sg_list = &send_sge;
	send.num_sge = 1;
	if (ibv_post_send(sender.qp, &send, &bad_send))
		goto out;
	if (wait_event(&sender, 0x1001, IBV_WC_SEND) ||
	    wait_event(&receiver, 0x2001, IBV_WC_RECV))
		goto out;
	if (memcmp(sender.buffer, receiver.buffer, TEST_LENGTH))
		goto out;

	memset(receiver.buffer, 0, TEST_LENGTH);
	memset(sender.buffer, 0xa5, TEST_LENGTH);
	memset(&receive, 0, sizeof(receive));
	receive.wr_id = 0x2002;
	if (ibv_post_recv(receiver.qp, &receive, &bad_receive))
		goto out;
	if (ibv_req_notify_cq(sender.cq, 0) ||
	    ibv_req_notify_cq(receiver.cq, 0))
		goto out;
	memset(&send, 0, sizeof(send));
	send.wr_id = 0x1002;
	send.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	send.send_flags = IBV_SEND_SIGNALED;
	send.imm_data = htonl(TEST_IMMEDIATE);
	send.sg_list = &send_sge;
	send.num_sge = 1;
	send.wr.rdma.remote_addr = (uintptr_t)receiver.buffer;
	send.wr.rdma.rkey = receiver.mr->rkey;
	if (ibv_post_send(sender.qp, &send, &bad_send))
		goto out;
	if (wait_event(&sender, 0x1002, IBV_WC_RDMA_WRITE) ||
	    wait_event(&receiver, 0x2002, IBV_WC_RECV_RDMA_WITH_IMM))
		goto out;
	if (memcmp(sender.buffer, receiver.buffer, TEST_LENGTH))
		goto out;
	rc = 0;

out:
	if (device) {
		close_endpoint(&receiver);
		close_endpoint(&sender);
	}
	if (devices)
		ibv_free_device_list(devices);
	printf("RESULT %s\n", rc ? "FAIL" : "PASS");
	return rc;
}
