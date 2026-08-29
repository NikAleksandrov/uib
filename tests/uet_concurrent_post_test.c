/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "uib/uib.h"

#define TEST_PORT 19730
#define TEST_LARGE_LENGTH (512U * 1024U)
#define TEST_SMALL_LENGTH 8U
#define TEST_INLINE_LENGTH 64U
#define TEST_INLINE_OFFSET (TEST_LARGE_LENGTH + TEST_SMALL_LENGTH)
#define TEST_BUFFER_LENGTH (TEST_INLINE_OFFSET + TEST_INLINE_LENGTH)
#define TEST_TIMEOUT_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)

struct test_endpoint {
	struct uib_context *context;
	struct uib_pd *pd;
	struct uib_cq *cq;
	struct uib_qp *qp;
	struct uib_mr *mr;
	uint8_t *buffer;
};

struct post_state {
	struct uib_qp *qp;
	struct uib_mr *local_mr;
	struct uib_mr *remote_mr;
	uint8_t *local_buffer;
	uint8_t *remote_buffer;
	uint64_t wr_id;
	uint32_t length;
	int entered;
	int result;
};

static uint64_t now_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000000ULL + time.tv_nsec;
}

static int endpoint_init(struct test_endpoint *endpoint, const char *address)
{
	struct uib_qp_init_attr initialization;

	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->context = uib_open_device_protocol(address, TEST_PORT, "uet");
	if (!endpoint->context)
		return -1;
	endpoint->pd = uib_alloc_pd(endpoint->context);
	endpoint->cq = uib_create_cq(endpoint->context, 8);
	endpoint->buffer = malloc(TEST_BUFFER_LENGTH);
	if (!endpoint->pd || !endpoint->cq || !endpoint->buffer)
		return -1;
	endpoint->mr = uib_reg_mr(endpoint->pd, endpoint->buffer,
				  TEST_BUFFER_LENGTH,
				  UIB_ACCESS_LOCAL_WRITE |
				  UIB_ACCESS_REMOTE_WRITE);
	if (!endpoint->mr)
		return -1;
	memset(&initialization, 0, sizeof(initialization));
	initialization.qp_type = UIB_QPT_RC;
	initialization.send_cq = endpoint->cq;
	initialization.recv_cq = endpoint->cq;
	initialization.max_send_wr = 8;
	initialization.max_recv_wr = 8;
	initialization.max_send_sge = 1;
	initialization.max_recv_sge = 1;
	endpoint->qp = uib_create_qp(endpoint->pd, &initialization);
	return endpoint->qp ? 0 : -1;
}

static int endpoint_connect(struct test_endpoint *local,
			    const struct test_endpoint *remote,
			    const char *remote_address)
{
	struct uib_qp_attr attributes;

	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = UIB_QPS_RTS;
	attributes.qp_access_flags = UIB_ACCESS_REMOTE_WRITE;
	attributes.dest_qp_num = remote->qp->qp_num;
	attributes.path_mtu = 1024;
	attributes.remote_addr.sin_family = AF_INET;
	attributes.remote_addr.sin_port = htons(TEST_PORT);
	if (inet_pton(AF_INET, remote_address,
		      &attributes.remote_addr.sin_addr) != 1)
		return -1;
	return uib_modify_qp(local->qp, &attributes,
			     UIB_QP_STATE | UIB_QP_ACCESS_FLAGS |
			     UIB_QP_DEST_QPN | UIB_QP_PATH_MTU |
			     UIB_QP_AV);
}

static void endpoint_cleanup(struct test_endpoint *endpoint)
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
}

static void *post_write(void *opaque)
{
	struct post_state *state = opaque;
	struct uib_send_wr request;
	struct uib_send_wr *bad;
	struct uib_sge sge;

	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)state->local_buffer;
	sge.length = state->length;
	sge.lkey = state->local_mr->lkey;
	memset(&request, 0, sizeof(request));
	request.wr_id = state->wr_id;
	request.opcode = UIB_WR_RDMA_WRITE;
	request.send_flags = UIB_SEND_SIGNALED;
	request.sg_list = &sge;
	request.num_sge = 1;
	request.rdma.remote_addr = (uintptr_t)state->remote_buffer;
	request.rdma.rkey = state->remote_mr->rkey;
	__atomic_store_n(&state->entered, 1, __ATOMIC_RELEASE);
	state->result = uib_post_send(state->qp, &request, &bad);
	return NULL;
}

static int wait_completions(struct uib_cq *cq)
{
	struct uib_wc completions[2];
	uint64_t deadline;
	int count;
	int total;
	int i;

	deadline = now_ns() + TEST_TIMEOUT_NS;
	total = 0;
	while (total < 2 && now_ns() < deadline) {
		count = uib_poll_cq(cq, 2 - total, completions + total);
		if (count < 0)
			return -1;
		total += count;
	}
	if (total != 2) {
		fprintf(stderr, "received %d of 2 completions\n", total);
		return -1;
	}
	for (i = 0; i < total; i++) {
		if (completions[i].status != UIB_WC_SUCCESS) {
			fprintf(stderr, "wr %llu completed with status %u\n",
				(unsigned long long)completions[i].wr_id,
				completions[i].status);
			return -1;
		}
		if (completions[i].wr_id != (uint64_t)i + 1) {
			fprintf(stderr, "completion %d has wr id %llu\n", i,
				(unsigned long long)completions[i].wr_id);
			return -1;
		}
	}
	return 0;
}

static int wait_psn_reservation(struct uib_qp *qp)
{
	uint64_t deadline;
	int reserved;

	deadline = now_ns() + TEST_TIMEOUT_NS;
	do {
		pthread_mutex_lock(&qp->lock);
		reserved = qp->sq_psn != 0;
		pthread_mutex_unlock(&qp->lock);
		if (reserved)
			return 0;
	} while (now_ns() < deadline);
	return -1;
}

static int test_inline_write(struct test_endpoint *sender,
			     struct test_endpoint *receiver)
{
	uint8_t expected[TEST_INLINE_LENGTH];
	struct uib_send_wr request;
	struct uib_send_wr *bad;
	struct uib_wc completion;
	struct uib_sge sge;
	uint64_t deadline;
	int count;
	int rc;

	memset(expected, 0x3c, sizeof(expected));
	memcpy(sender->buffer + TEST_INLINE_OFFSET, expected, sizeof(expected));
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)(sender->buffer + TEST_INLINE_OFFSET);
	sge.length = TEST_INLINE_LENGTH;
	sge.lkey = sender->mr->lkey;
	memset(&request, 0, sizeof(request));
	request.wr_id = 3;
	request.opcode = UIB_WR_RDMA_WRITE;
	request.send_flags = UIB_SEND_SIGNALED | UIB_SEND_INLINE;
	request.sg_list = &sge;
	request.num_sge = 1;
	request.rdma.remote_addr =
		(uintptr_t)(receiver->buffer + TEST_INLINE_OFFSET);
	request.rdma.rkey = receiver->mr->rkey;
	rc = uib_post_send(sender->qp, &request, &bad);
	memset(sender->buffer + TEST_INLINE_OFFSET, 0, TEST_INLINE_LENGTH);
	if (rc)
		return -1;
	deadline = now_ns() + TEST_TIMEOUT_NS;
	do {
		count = uib_poll_cq(sender->cq, 1, &completion);
		if (count < 0)
			return -1;
	} while (!count && now_ns() < deadline);
	if (count != 1 || completion.status != UIB_WC_SUCCESS ||
	    completion.wr_id != 3)
		return -1;
	return memcmp(receiver->buffer + TEST_INLINE_OFFSET, expected,
		      sizeof(expected)) ? -1 : 0;
}

int main(void)
{
	struct test_endpoint receiver;
	struct test_endpoint sender;
	struct post_state large;
	struct post_state small;
	pthread_t large_thread;
	pthread_t small_thread;
	int rc;

	if (!getenv("UIB_TRANSPORT_MODE"))
		setenv("UIB_TRANSPORT_MODE", "udp-sync", 1);
	if (!getenv("UIB_UET_CC"))
		setenv("UIB_UET_CC", "none", 1);
	if (!getenv("UIB_UDP_WORKERS"))
		setenv("UIB_UDP_WORKERS", "1", 1);
	if (!getenv("UIB_UDP_WORKER_AFFINITY"))
		setenv("UIB_UDP_WORKER_AFFINITY", "off", 1);
	memset(&sender, 0, sizeof(sender));
	memset(&receiver, 0, sizeof(receiver));
	memset(&large, 0, sizeof(large));
	memset(&small, 0, sizeof(small));
	rc = 1;
	if (endpoint_init(&sender, "127.0.0.1")) {
		perror("sender endpoint");
		goto out;
	}
	if (endpoint_init(&receiver, "127.0.0.2")) {
		perror("receiver endpoint");
		goto out;
	}
	if (endpoint_connect(&sender, &receiver, "127.0.0.2") ||
	    endpoint_connect(&receiver, &sender, "127.0.0.1")) {
		fprintf(stderr, "endpoint connection failed\n");
		goto out;
	}
	memset(sender.buffer, 0x5a, TEST_LARGE_LENGTH);
	memset(sender.buffer + TEST_LARGE_LENGTH, 0xa5, TEST_SMALL_LENGTH);
	memset(receiver.buffer, 0, TEST_BUFFER_LENGTH);
	large.qp = sender.qp;
	large.local_mr = sender.mr;
	large.remote_mr = receiver.mr;
	large.local_buffer = sender.buffer;
	large.remote_buffer = receiver.buffer;
	large.wr_id = 1;
	large.length = TEST_LARGE_LENGTH;
	small.qp = sender.qp;
	small.local_mr = sender.mr;
	small.remote_mr = receiver.mr;
	small.local_buffer = sender.buffer + TEST_LARGE_LENGTH;
	small.remote_buffer = receiver.buffer + TEST_LARGE_LENGTH;
	small.wr_id = 2;
	small.length = TEST_SMALL_LENGTH;
	if (pthread_create(&large_thread, NULL, post_write, &large)) {
		fprintf(stderr, "large poster creation failed\n");
		goto out;
	}
	while (!__atomic_load_n(&large.entered, __ATOMIC_ACQUIRE))
		;
	if (wait_psn_reservation(sender.qp)) {
		fprintf(stderr, "large poster did not reserve PSNs\n");
		pthread_join(large_thread, NULL);
		goto out;
	}
	if (pthread_create(&small_thread, NULL, post_write, &small)) {
		fprintf(stderr, "small poster creation failed\n");
		pthread_join(large_thread, NULL);
		goto out;
	}
	pthread_join(large_thread, NULL);
	pthread_join(small_thread, NULL);
	if (large.result || small.result) {
		fprintf(stderr, "post results: large=%d small=%d\n",
			large.result, small.result);
		goto out;
	}
	if (wait_completions(sender.cq))
		goto out;
	if (memcmp(sender.buffer, receiver.buffer, TEST_INLINE_OFFSET)) {
		fprintf(stderr, "remote data does not match\n");
		goto out;
	}
	if (test_inline_write(&sender, &receiver)) {
		fprintf(stderr, "inline write did not preserve its payload\n");
		goto out;
	}
	rc = 0;

out:
	endpoint_cleanup(&receiver);
	endpoint_cleanup(&sender);
	printf("concurrent UET post ordering: %s\n", rc ? "FAIL" : "PASS");
	return rc;
}
