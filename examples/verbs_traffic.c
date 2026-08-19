#include <errno.h>
#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VERBS_BUFFER_SIZE 65536
#define VERBS_MESSAGE_SIZE 8197
#define VERBS_TIMEOUT_NS (3ULL * 1000 * 1000 * 1000)

struct verbs_endpoint {
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	struct ibv_mr *mr;
	uint8_t *buffer;
	union ibv_gid gid;
};

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static struct ibv_device *find_uib_device(struct ibv_device **devices,
					 int count)
{
	for (int i = 0; i < count; i++) {
		if (!strcmp(ibv_get_device_name(devices[i]), "uib0"))
			return devices[i];
	}
	return NULL;
}

static int endpoint_open(struct verbs_endpoint *endpoint,
			 struct ibv_device *device, const char *bind_address)
{
	struct ibv_qp_init_attr init = {0};
	int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
		IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;

	memset(endpoint, 0, sizeof(*endpoint));
	setenv("UIB_BIND_IP", bind_address, 1);
	endpoint->context = ibv_open_device(device);
	if (!endpoint->context)
		return -errno;
	endpoint->pd = ibv_alloc_pd(endpoint->context);
	endpoint->cq = ibv_create_cq(endpoint->context, 64, NULL, NULL, 0);
	if (!endpoint->pd || !endpoint->cq)
		return -ENOMEM;
	if (posix_memalign((void **)&endpoint->buffer, 64, VERBS_BUFFER_SIZE))
		return -ENOMEM;
	memset(endpoint->buffer, 0, VERBS_BUFFER_SIZE);
	endpoint->mr = ibv_reg_mr(endpoint->pd, endpoint->buffer,
				  VERBS_BUFFER_SIZE, access);
	if (!endpoint->mr)
		return -ENOMEM;

	init.send_cq = endpoint->cq;
	init.recv_cq = endpoint->cq;
	init.qp_type = IBV_QPT_RC;
	init.cap.max_send_wr = 32;
	init.cap.max_recv_wr = 32;
	init.cap.max_send_sge = 1;
	init.cap.max_recv_sge = 1;
	endpoint->qp = ibv_create_qp(endpoint->pd, &init);
	if (!endpoint->qp || ibv_query_gid(endpoint->context, 1, 0,
					   &endpoint->gid))
		return -EIO;
	return 0;
}

static void endpoint_close(struct verbs_endpoint *endpoint)
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
	if (endpoint->context)
		ibv_close_device(endpoint->context);
	memset(endpoint, 0, sizeof(*endpoint));
}

static int endpoint_connect(struct verbs_endpoint *local,
			    const struct verbs_endpoint *remote)
{
	struct ibv_qp_attr attributes = {0};
	int mask;

	attributes.qp_state = IBV_QPS_INIT;
	attributes.pkey_index = 0;
	attributes.port_num = 1;
	attributes.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
		IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
		IBV_ACCESS_REMOTE_ATOMIC;
	mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
		IBV_QP_ACCESS_FLAGS;
	if (ibv_modify_qp(local->qp, &attributes, mask))
		return -EIO;

	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = IBV_QPS_RTR;
	attributes.path_mtu = IBV_MTU_4096;
	attributes.dest_qp_num = remote->qp->qp_num;
	attributes.rq_psn = 0;
	attributes.max_dest_rd_atomic = 1;
	attributes.min_rnr_timer = 12;
	attributes.ah_attr.is_global = 1;
	attributes.ah_attr.port_num = 1;
	attributes.ah_attr.grh.dgid = remote->gid;
	attributes.ah_attr.grh.sgid_index = 0;
	attributes.ah_attr.grh.hop_limit = 64;
	mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
		IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
		IBV_QP_MIN_RNR_TIMER;
	if (ibv_modify_qp(local->qp, &attributes, mask))
		return -EIO;

	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = IBV_QPS_RTS;
	attributes.sq_psn = 0;
	attributes.timeout = 12;
	attributes.retry_cnt = 7;
	attributes.rnr_retry = 7;
	attributes.max_rd_atomic = 1;
	mask = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
		IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
	return ibv_modify_qp(local->qp, &attributes, mask) ? -EIO : 0;
}

static int wait_completion(struct verbs_endpoint *endpoint, uint64_t wr_id,
			   enum ibv_wc_opcode opcode)
{
	uint64_t deadline = monotonic_ns() + VERBS_TIMEOUT_NS;
	struct timespec delay = { .tv_nsec = 100000 };

	while (monotonic_ns() < deadline) {
		struct ibv_wc completion;
		int count = ibv_poll_cq(endpoint->cq, 1, &completion);

		if (count < 0)
			return -EIO;
		if (count == 1)
			return completion.wr_id == wr_id &&
				completion.opcode == opcode &&
				completion.status == IBV_WC_SUCCESS ? 0 : -EIO;
		nanosleep(&delay, NULL);
	}
	return -ETIMEDOUT;
}

static int post_receive(struct verbs_endpoint *endpoint, uint64_t wr_id,
			void *buffer, uint32_t length)
{
	struct ibv_sge scatter = {
		.addr = (uintptr_t)buffer,
		.length = length,
		.lkey = endpoint->mr->lkey,
	};
	struct ibv_recv_wr receive = {
		.wr_id = wr_id,
		.sg_list = &scatter,
		.num_sge = 1,
	};
	struct ibv_recv_wr *bad;

	return ibv_post_recv(endpoint->qp, &receive, &bad) ? -EIO : 0;
}

static int post_operation(struct verbs_endpoint *endpoint, uint64_t wr_id,
			  enum ibv_wr_opcode opcode, void *buffer,
			  uint32_t length, uint64_t remote_address,
			  uint32_t remote_key, uint64_t compare_add,
			  uint64_t swap)
{
	struct ibv_sge gather = {
		.addr = (uintptr_t)buffer,
		.length = length,
		.lkey = endpoint->mr->lkey,
	};
	struct ibv_send_wr operation = {
		.wr_id = wr_id,
		.opcode = opcode,
		.send_flags = IBV_SEND_SIGNALED,
		.sg_list = &gather,
		.num_sge = 1,
	};
	struct ibv_send_wr *bad;

	if (opcode == IBV_WR_RDMA_WRITE || opcode == IBV_WR_RDMA_READ) {
		operation.wr.rdma.remote_addr = remote_address;
		operation.wr.rdma.rkey = remote_key;
	} else if (opcode == IBV_WR_ATOMIC_FETCH_AND_ADD ||
		   opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
		operation.wr.atomic.remote_addr = remote_address;
		operation.wr.atomic.rkey = remote_key;
		operation.wr.atomic.compare_add = compare_add;
		operation.wr.atomic.swap = swap;
	}
	return ibv_post_send(endpoint->qp, &operation, &bad) ? -EIO : 0;
}

int main(int argc, char **argv)
{
	const char *protocol = argc > 1 ? argv[1] : "uet";
	struct verbs_endpoint sender = {0}, receiver = {0};
	struct ibv_device **devices = NULL;
	struct ibv_device *device;
	uint8_t *source, *receive, *read_result, *remote;
	uint64_t *atomic_result, *atomic_target;
	int device_count;
	int full_rdma = strcmp(protocol, "dummy");
	int is_uet = !strcmp(protocol, "uet");
	int rc = 1;

	setenv("UIB_PROTOCOL", protocol, 1);
	devices = ibv_get_device_list(&device_count);
	device = devices ? find_uib_device(devices, device_count) : NULL;
	if (!device || endpoint_open(&sender, device, "127.0.0.1") < 0 ||
	    endpoint_open(&receiver, device, "127.0.0.2") < 0 ||
	    endpoint_connect(&sender, &receiver) < 0 ||
	    endpoint_connect(&receiver, &sender) < 0) {
		fprintf(stderr, "libibverbs endpoint setup failed\n");
		goto out;
	}

	source = sender.buffer;
	read_result = sender.buffer + 16384;
	atomic_result = (uint64_t *)(sender.buffer + 32768);
	receive = receiver.buffer;
	remote = receiver.buffer + 16384;
	atomic_target = (uint64_t *)(receiver.buffer + 32768);
	for (uint32_t i = 0; i < VERBS_MESSAGE_SIZE; i++)
		source[i] = (uint8_t)(i * 29 + 7);

	if (post_receive(&receiver, 0x2001, receive, VERBS_MESSAGE_SIZE) < 0 ||
	    post_operation(&sender, 0x1001, IBV_WR_SEND, source,
			   VERBS_MESSAGE_SIZE, 0, 0, 0, 0) < 0 ||
	    wait_completion(&sender, 0x1001, IBV_WC_SEND) < 0 ||
	    wait_completion(&receiver, 0x2001, IBV_WC_RECV) < 0 ||
	    memcmp(source, receive, VERBS_MESSAGE_SIZE))
		goto out;
	printf("libibverbs SEND transferred and verified %u bytes\n",
	       VERBS_MESSAGE_SIZE);

	if (full_rdma) {
		if (post_operation(&sender, 0x1002, IBV_WR_RDMA_WRITE, source,
				   VERBS_MESSAGE_SIZE, (uintptr_t)remote,
				   receiver.mr->rkey, 0, 0) < 0 ||
		    wait_completion(&sender, 0x1002, IBV_WC_RDMA_WRITE) < 0 ||
		    memcmp(source, remote, VERBS_MESSAGE_SIZE))
			goto out;
		memset(read_result, 0, VERBS_MESSAGE_SIZE);
		if (post_operation(&sender, 0x1003, IBV_WR_RDMA_READ,
				   read_result, VERBS_MESSAGE_SIZE,
				   (uintptr_t)remote, receiver.mr->rkey,
				   0, 0) < 0 ||
		    wait_completion(&sender, 0x1003, IBV_WC_RDMA_READ) < 0 ||
		    memcmp(read_result, remote, VERBS_MESSAGE_SIZE))
			goto out;
		printf("libibverbs WRITE and READ transferred and verified data\n");
	}

	if (is_uet) {
		*atomic_target = 8;
		if (post_operation(&sender, 0x1004,
				   IBV_WR_ATOMIC_FETCH_AND_ADD, atomic_result,
				   sizeof(*atomic_result),
				   (uintptr_t)atomic_target, receiver.mr->rkey,
				   5, 0) < 0 ||
		    wait_completion(&sender, 0x1004, IBV_WC_FETCH_ADD) < 0 ||
		    *atomic_result != 8 || *atomic_target != 13)
			goto out;
		if (post_operation(&sender, 0x1005, IBV_WR_ATOMIC_CMP_AND_SWP,
				   atomic_result, sizeof(*atomic_result),
				   (uintptr_t)atomic_target, receiver.mr->rkey,
				   13, 21) < 0 ||
		    wait_completion(&sender, 0x1005, IBV_WC_COMP_SWAP) < 0 ||
		    *atomic_result != 13 || *atomic_target != 21)
			goto out;
		printf("libibverbs UET atomics returned and updated correct values\n");
	}

	printf("libibverbs %s example: PASS\n", protocol);
	rc = 0;
out:
	endpoint_close(&receiver);
	endpoint_close(&sender);
	if (devices)
		ibv_free_device_list(devices);
	return rc;
}
