#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

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
	return direct_endpoint_open_memory(endpoint, protocol, address, port,
					   buffer_length, queue_depth, "host");
}

int direct_endpoint_open_memory(struct direct_endpoint *endpoint,
				const char *protocol, const char *address,
				uint16_t port, size_t buffer_length,
				uint32_t queue_depth, const char *memory)
{
	unsigned long long parsed_offset = 0;
	struct uib_qp_init_attr init = {0};
	const char *offset_value;
	const char *fd_value;
	long parsed_fd;
	char *end;
	int dmabuf_fd = -1;
	int owned_fd = 0;
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
	endpoint->buffer_length = buffer_length;
	access = UIB_ACCESS_LOCAL_WRITE | UIB_ACCESS_REMOTE_WRITE |
		 UIB_ACCESS_REMOTE_READ | UIB_ACCESS_REMOTE_ATOMIC;
	if (!strcmp(memory, "dmabuf") || !strcmp(memory, "gpu-dmabuf")) {
		if (!strcmp(memory, "gpu-dmabuf")) {
			fd_value = getenv("UIB_BENCH_DMABUF_FD");
			offset_value = getenv("UIB_BENCH_DMABUF_OFFSET");
			if (!fd_value || !*fd_value)
				goto err;
			errno = 0;
			parsed_fd = strtol(fd_value, &end, 0);
			if (errno || !end || *end || parsed_fd < 0 ||
			    parsed_fd > INT_MAX)
				goto err;
			if (offset_value && *offset_value) {
				errno = 0;
				parsed_offset = strtoull(offset_value, &end, 0);
				if (errno || !end || *end || parsed_offset > SIZE_MAX)
					goto err;
			}
			dmabuf_fd = (int)parsed_fd;
		} else {
			dmabuf_fd = memfd_create("uib-example", MFD_CLOEXEC);
			owned_fd = 1;
			if (dmabuf_fd < 0 ||
			    ftruncate(dmabuf_fd, (off_t)buffer_length) < 0)
				goto err;
		}
		endpoint->mr = uib_reg_mr_dmabuf(endpoint->pd, dmabuf_fd,
						 (size_t)parsed_offset,
						 buffer_length, access);
		if (owned_fd) {
			close(dmabuf_fd);
			dmabuf_fd = -1;
		}
		if (!endpoint->mr)
			goto err;
		endpoint->buffer = endpoint->mr->addr;
		endpoint->dmabuf_memory = 1;
	} else {
		if (strcmp(memory, "host") ||
		    posix_memalign((void **)&endpoint->buffer, 64, buffer_length))
			goto err;
		memset(endpoint->buffer, 0, buffer_length);
		endpoint->mr = uib_reg_mr(endpoint->pd, endpoint->buffer,
					  buffer_length, access);
	}
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
	if (owned_fd && dmabuf_fd >= 0)
		close(dmabuf_fd);
	direct_endpoint_close(endpoint);
	return rc;
}

int direct_endpoint_connect(struct direct_endpoint *local,
			    const struct direct_endpoint *remote,
			    uint32_t path_mtu)
{
	return direct_endpoint_connect_address(local, &remote->context->local_addr,
					       remote->qp->qp_num, path_mtu);
}

int direct_endpoint_connect_address(struct direct_endpoint *local,
				    const struct sockaddr_in *remote_address,
				    uint32_t remote_qpn,
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

	/* RTR identifies the peer QP and packet destination. */
	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = UIB_QPS_RTR;
	attributes.dest_qp_num = remote_qpn;
	attributes.remote_addr = *remote_address;
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
	if (!endpoint->dmabuf_memory)
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
	struct uib_recv_wr *bad;
	struct uib_recv_wr receive;
	struct uib_sge scatter;
	uint64_t address = direct_buffer_address(endpoint, buffer);

	scatter = (struct uib_sge) {
		.addr = address,
		.length = length,
		.lkey = endpoint->mr->lkey,
	};
	receive = (struct uib_recv_wr) {
		.wr_id = wr_id,
		.sg_list = &scatter,
		.num_sge = 1,
	};

	return uib_post_recv(endpoint->qp, &receive, &bad);
}

int direct_post_send(struct direct_endpoint *endpoint, uint64_t wr_id,
			     enum uib_wr_opcode opcode, void *buffer,
			     uint32_t length, uint64_t remote_address,
			     uint32_t remote_key)
{
	struct uib_send_wr *bad;
	struct uib_send_wr send;
	struct uib_sge gather;
	uint64_t address = direct_buffer_address(endpoint, buffer);

	gather = (struct uib_sge) {
		.addr = address,
		.length = length,
		.lkey = endpoint->mr->lkey,
	};
	send = (struct uib_send_wr) {
		.wr_id = wr_id,
		.opcode = opcode,
		.send_flags = UIB_SEND_SIGNALED,
		.sg_list = &gather,
		.num_sge = 1,
	};
	send.rdma.remote_addr = remote_address;
	send.rdma.rkey = remote_key;
	return uib_post_send(endpoint->qp, &send, &bad);
}

uint64_t direct_buffer_address(const struct direct_endpoint *endpoint,
			       const void *buffer)
{
	if (!endpoint->dmabuf_memory)
		return (uintptr_t)buffer;
	return endpoint->mr->iova +
		(size_t)((const uint8_t *)buffer - endpoint->buffer);
}
