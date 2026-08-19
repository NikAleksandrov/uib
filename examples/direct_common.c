#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "direct_common.h"

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

int direct_endpoint_open(struct direct_endpoint *endpoint, const char *protocol,
			 const char *address, uint16_t port, size_t buffer_length,
			 uint32_t queue_depth)
{
	struct uib_qp_init_attr init = {0};
	int access;
	int rc = -ENOMEM;

	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->context = uib_open_device_protocol(address, port, protocol);
	if (!endpoint->context)
		return -errno;
	endpoint->pd = uib_alloc_pd(endpoint->context);
	if (!endpoint->pd)
		goto err;
	endpoint->cq = uib_create_cq(endpoint->context, (int)queue_depth * 2 + 16);
	if (!endpoint->cq)
		goto err;
	if (posix_memalign((void **)&endpoint->buffer, 64, buffer_length))
		goto err;
	memset(endpoint->buffer, 0, buffer_length);
	endpoint->buffer_length = buffer_length;
	access = UIB_ACCESS_LOCAL_WRITE | UIB_ACCESS_REMOTE_WRITE |
		 UIB_ACCESS_REMOTE_READ | UIB_ACCESS_REMOTE_ATOMIC;
	endpoint->mr = uib_reg_mr(endpoint->pd, endpoint->buffer,
				  buffer_length, access);
	if (!endpoint->mr)
		goto err;

	init.send_cq = endpoint->cq;
	init.recv_cq = endpoint->cq;
	init.qp_type = UIB_QPT_RC;
	init.max_send_wr = queue_depth + 16;
	init.max_recv_wr = queue_depth + 16;
	init.max_send_sge = 1;
	init.max_recv_sge = 1;
	endpoint->qp = uib_create_qp(endpoint->pd, &init);
	if (!endpoint->qp)
		goto err;
	return 0;

err:
	direct_endpoint_close(endpoint);
	return rc;
}

int direct_endpoint_connect(struct direct_endpoint *local,
			    const struct direct_endpoint *remote,
			    uint32_t path_mtu)
{
	struct uib_qp_attr attributes = {0};
	int rc;

	/* INIT grants the target-side permissions used by RMA and atomics. */
	attributes.qp_state = UIB_QPS_INIT;
	attributes.qp_access_flags = UIB_ACCESS_LOCAL_WRITE |
		UIB_ACCESS_REMOTE_WRITE | UIB_ACCESS_REMOTE_READ |
		UIB_ACCESS_REMOTE_ATOMIC;
	rc = uib_modify_qp(local->qp, &attributes,
			   UIB_QP_STATE | UIB_QP_ACCESS_FLAGS);
	if (rc < 0)
		return rc;

	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = UIB_QPS_RTR;
	attributes.dest_qp_num = remote->qp->qp_num;
	attributes.remote_addr = remote->context->local_addr;
	attributes.rq_psn = 0;
	attributes.path_mtu = path_mtu;
	rc = uib_modify_qp(local->qp, &attributes, UIB_QP_STATE | UIB_QP_AV |
			   UIB_QP_DEST_QPN | UIB_QP_RQ_PSN | UIB_QP_PATH_MTU);
	if (rc < 0)
		return rc;

	/* RTS establishes the initial transmit sequence number. */
	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = UIB_QPS_RTS;
	attributes.sq_psn = 0;
	return uib_modify_qp(local->qp, &attributes,
			     UIB_QP_STATE | UIB_QP_SQ_PSN);
}

void direct_endpoint_close(struct direct_endpoint *endpoint)
{
	if (endpoint->qp)
		uib_destroy_qp(endpoint->qp);
	if (endpoint->mr)
		uib_dereg_mr(endpoint->mr);
	free(endpoint->buffer);
	if (endpoint->cq)
		uib_destroy_cq(endpoint->cq);
	if (endpoint->pd)
		uib_dealloc_pd(endpoint->pd);
	if (endpoint->context)
		uib_close_device(endpoint->context);
	memset(endpoint, 0, sizeof(*endpoint));
}

int direct_wait_completion(struct uib_cq *cq, struct uib_wc *completion,
			   uint64_t timeout_ns)
{
	struct timespec delay = { .tv_nsec = 100000 };
	uint64_t deadline = monotonic_ns() + timeout_ns;

	while (monotonic_ns() < deadline) {
		if (uib_poll_cq(cq, 1, completion) == 1)
			return completion->status == UIB_WC_SUCCESS ? 0 : -EIO;
		nanosleep(&delay, NULL);
	}
	return -ETIMEDOUT;
}

int direct_post_receive(struct direct_endpoint *endpoint, uint64_t wr_id,
			void *buffer, uint32_t length)
{
	struct uib_sge scatter = {
		.addr = (uintptr_t)buffer,
		.length = length,
		.lkey = endpoint->mr->lkey,
	};
	struct uib_recv_wr receive = {
		.wr_id = wr_id,
		.sg_list = &scatter,
		.num_sge = 1,
	};
	struct uib_recv_wr *bad;

	return uib_post_recv(endpoint->qp, &receive, &bad);
}

int direct_post_send(struct direct_endpoint *endpoint, uint64_t wr_id,
			     enum uib_wr_opcode opcode, void *buffer,
			     uint32_t length, uint64_t remote_address,
			     uint32_t remote_key)
{
	struct uib_sge gather = {
		.addr = (uintptr_t)buffer,
		.length = length,
		.lkey = endpoint->mr->lkey,
	};
	struct uib_send_wr send = {
		.wr_id = wr_id,
		.opcode = opcode,
		.send_flags = UIB_SEND_SIGNALED,
		.sg_list = &gather,
		.num_sge = 1,
	};
	struct uib_send_wr *bad;

	send.rdma.remote_addr = remote_address;
	send.rdma.rkey = remote_key;
	return uib_post_send(endpoint->qp, &send, &bad);
}
