/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uib/uib.h"

#define QP_SCALE_RECV_FLAG (1ULL << 63)
#define QP_SCALE_TIMEOUT_NS (30ULL * 1000000000ULL)

struct qp_scale_options {
	const char *protocol;
	uint32_t qps;
	uint32_t iterations;
	uint32_t size;
	uint32_t window;
	uint32_t mtu;
	uint16_t port;
};

struct qp_scale_group {
	struct uib_context *context;
	struct uib_pd *pd;
	struct uib_cq *cq;
	struct uib_mr *mr;
	struct uib_qp **qps;
	uint8_t *buffer;
	size_t buffer_length;
	uint32_t qp_count;
};

static uint64_t clock_ns(clockid_t clock_id)
{
	struct timespec now;

	clock_gettime(clock_id, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static int env_u32(const char *name, uint32_t fallback, uint32_t maximum,
		   uint32_t *result)
{
	const char *value;
	unsigned long parsed;
	char *end;

	value = getenv(name);
	if (!value || !*value) {
		*result = fallback;
		return 0;
	}
	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno || end == value || *end || !parsed || parsed > maximum)
		return -EINVAL;
	*result = (uint32_t)parsed;
	return 0;
}

static int options_init(struct qp_scale_options *options)
{
	const char *protocol;
	uint32_t port;
	int rc;

	protocol = getenv("UIB_BENCH_PROTOCOL");
	options->protocol = protocol && *protocol ? protocol : "uet";
	if (strcmp(options->protocol, "uet") &&
	    strcmp(options->protocol, "rocev2"))
		return -EINVAL;
	rc = env_u32("UIB_QP_SCALE_QPS", 1, 256, &options->qps);
	if (rc < 0)
		return rc;
	rc = env_u32("UIB_BENCH_ITERATIONS", 1000, UINT32_MAX,
		     &options->iterations);
	if (rc < 0)
		return rc;
	rc = env_u32("UIB_BENCH_SIZE", 60000, UINT32_MAX, &options->size);
	if (rc < 0)
		return rc;
	rc = env_u32("UIB_BENCH_WINDOW", 64, UIB_UET_PDC_MPR,
		     &options->window);
	if (rc < 0)
		return rc;
	rc = env_u32("UIB_BENCH_MTU", 8192, UINT16_MAX, &options->mtu);
	if (rc < 0)
		return rc;
	rc = env_u32("UIB_QP_SCALE_PORT", 22550, UINT16_MAX - 1, &port);
	if (rc < 0)
		return rc;
	if (!strcmp(options->protocol, "rocev2") && options->mtu > UIB_PMTU)
		return -EINVAL;
	options->port = (uint16_t)port;
	return 0;
}

static void group_close(struct qp_scale_group *group)
{
	uint32_t i;

	if (!group)
		return;
	for (i = 0; i < group->qp_count; i++) {
		if (group->qps && group->qps[i])
			uib_destroy_qp(group->qps[i]);
	}
	free(group->qps);
	if (group->mr)
		uib_dereg_mr(group->mr);
	free(group->buffer);
	if (group->cq)
		uib_destroy_cq(group->cq);
	if (group->pd)
		uib_dealloc_pd(group->pd);
	if (group->context)
		uib_close_device(group->context);
	memset(group, 0, sizeof(*group));
}

static int group_open(struct qp_scale_group *group,
		      const struct qp_scale_options *options,
		      uint16_t port)
{
	struct uib_qp_init_attr init = {0};
	size_t slots;
	size_t cq_entries;
	uint32_t i;
	int rc;

	memset(group, 0, sizeof(*group));
	if (options->qps > SIZE_MAX / options->window)
		return -EOVERFLOW;
	slots = (size_t)options->qps * options->window;
	if (slots > SIZE_MAX / options->size)
		return -EOVERFLOW;
	group->buffer_length = slots * options->size;
	group->context = uib_open_device_protocol("127.0.0.1", port,
						  options->protocol);
	if (!group->context) {
		rc = -errno;
		goto error;
	}
	group->pd = uib_alloc_pd(group->context);
	if (!group->pd) {
		rc = -errno;
		goto error;
	}
	cq_entries = slots * 2 + options->qps * 16;
	if (cq_entries > INT32_MAX) {
		rc = -EOVERFLOW;
		goto error;
	}
	group->cq = uib_create_cq(group->context, (int)cq_entries);
	if (!group->cq) {
		rc = -errno;
		goto error;
	}
	rc = posix_memalign((void **)&group->buffer, 64,
			    group->buffer_length);
	if (rc) {
		rc = -rc;
		goto error;
	}
	memset(group->buffer, 0, group->buffer_length);
	group->mr = uib_reg_mr(group->pd, group->buffer,
			       group->buffer_length,
			       UIB_ACCESS_LOCAL_WRITE |
			       UIB_ACCESS_REMOTE_WRITE |
			       UIB_ACCESS_REMOTE_READ);
	if (!group->mr) {
		rc = -errno;
		goto error;
	}
	group->qps = calloc(options->qps, sizeof(*group->qps));
	if (!group->qps) {
		rc = -ENOMEM;
		goto error;
	}
	init.send_cq = group->cq;
	init.recv_cq = group->cq;
	init.qp_type = UIB_QPT_RC;
	init.max_send_wr = options->window + 16;
	init.max_recv_wr = options->window + 16;
	init.max_send_sge = 1;
	init.max_recv_sge = 1;
	for (i = 0; i < options->qps; i++) {
		group->qps[i] = uib_create_qp(group->pd, &init);
		if (!group->qps[i]) {
			rc = -errno;
			goto error;
		}
		group->qp_count++;
	}
	return 0;

error:
	group_close(group);
	return rc;
}

static int connect_qp(struct uib_qp *local, const struct uib_qp *remote,
		      uint32_t mtu)
{
	struct uib_qp_attr attr = {0};
	int rc;

	attr.qp_state = UIB_QPS_INIT;
	attr.qp_access_flags = UIB_ACCESS_LOCAL_WRITE |
		UIB_ACCESS_REMOTE_WRITE | UIB_ACCESS_REMOTE_READ;
	rc = uib_modify_qp(local, &attr,
			   UIB_QP_STATE | UIB_QP_ACCESS_FLAGS);
	if (rc < 0)
		return rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = UIB_QPS_RTR;
	attr.dest_qp_num = remote->qp_num;
	attr.remote_addr = remote->ctx->local_addr;
	attr.rq_psn = 0;
	attr.path_mtu = mtu;
	rc = uib_modify_qp(local, &attr, UIB_QP_STATE | UIB_QP_AV |
			   UIB_QP_DEST_QPN | UIB_QP_RQ_PSN |
			   UIB_QP_PATH_MTU);
	if (rc < 0)
		return rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = UIB_QPS_RTS;
	attr.sq_psn = 0;
	return uib_modify_qp(local, &attr,
			     UIB_QP_STATE | UIB_QP_SQ_PSN);
}

static int connect_groups(struct qp_scale_group *sender,
			  struct qp_scale_group *receiver, uint32_t mtu)
{
	uint32_t i;
	int rc;

	for (i = 0; i < sender->qp_count; i++) {
		rc = connect_qp(sender->qps[i], receiver->qps[i], mtu);
		if (rc < 0)
			return rc;
		rc = connect_qp(receiver->qps[i], sender->qps[i], mtu);
		if (rc < 0)
			return rc;
	}
	return 0;
}

static uint8_t *slot_buffer(struct qp_scale_group *group,
			    const struct qp_scale_options *options,
			    uint32_t qp_index, uint32_t slot)
{
	size_t index;

	index = (size_t)qp_index * options->window + slot;
	return group->buffer + index * options->size;
}

static uint64_t work_id(uint32_t qp_index, uint32_t sequence, int receive)
{
	uint64_t id;

	id = ((uint64_t)qp_index << 48) | ((uint64_t)sequence + 1);
	return receive ? id | QP_SCALE_RECV_FLAG : id;
}

static int post_receive(struct qp_scale_group *group,
			const struct qp_scale_options *options,
			uint32_t qp_index, uint32_t slot, uint32_t sequence)
{
	struct uib_recv_wr receive = {0};
	struct uib_recv_wr *bad;
	struct uib_sge sge;
	uint8_t *buffer;

	buffer = slot_buffer(group, options, qp_index, slot);
	sge.addr = (uintptr_t)buffer;
	sge.length = options->size;
	sge.lkey = group->mr->lkey;
	receive.wr_id = work_id(qp_index, sequence, 1);
	receive.sg_list = &sge;
	receive.num_sge = 1;
	return uib_post_recv(group->qps[qp_index], &receive, &bad);
}

static int post_send(struct qp_scale_group *group,
		     const struct qp_scale_options *options,
		     uint32_t qp_index, uint32_t slot, uint32_t sequence)
{
	struct uib_send_wr send = {0};
	struct uib_send_wr *bad;
	struct uib_sge sge;
	uint8_t *buffer;

	buffer = slot_buffer(group, options, qp_index, slot);
	sge.addr = (uintptr_t)buffer;
	sge.length = options->size;
	sge.lkey = group->mr->lkey;
	send.wr_id = work_id(qp_index, sequence, 0);
	send.opcode = UIB_WR_SEND;
	send.send_flags = UIB_SEND_SIGNALED;
	send.sg_list = &sge;
	send.num_sge = 1;
	return uib_post_send(group->qps[qp_index], &send, &bad);
}

static void fill_message(uint8_t *buffer, uint32_t length, uint64_t sequence)
{
	memset(buffer, (int)sequence, length);
	if (length >= sizeof(sequence))
		memcpy(buffer, &sequence, sizeof(sequence));
}

static int verify_message(const uint8_t *buffer, uint32_t length,
			  uint64_t sequence)
{
	uint64_t found;

	if (length < sizeof(found))
		return buffer[0] == (uint8_t)sequence ? 0 : -EIO;
	memcpy(&found, buffer, sizeof(found));
	if (found != sequence)
		return -EIO;
	return buffer[length - 1] == (uint8_t)sequence ? 0 : -EIO;
}

static int poll_batch(struct qp_scale_group *sender,
		      struct qp_scale_group *receiver, uint32_t expected)
{
	struct timespec delay = { .tv_nsec = 1000 };
	struct uib_wc completions[64];
	uint64_t deadline;
	uint32_t send_count;
	uint32_t recv_count;
	int completed;
	int progress;
	int i;

	deadline = clock_ns(CLOCK_MONOTONIC) + QP_SCALE_TIMEOUT_NS;
	send_count = 0;
	recv_count = 0;
	while (send_count < expected || recv_count < expected) {
		progress = 0;
		completed = uib_poll_cq(sender->cq, 64, completions);
		for (i = 0; i < completed; i++) {
			if (completions[i].status != UIB_WC_SUCCESS ||
			    (completions[i].wr_id & QP_SCALE_RECV_FLAG))
				return -EIO;
			send_count++;
		}
		if (completed)
			progress = 1;
		completed = uib_poll_cq(receiver->cq, 64, completions);
		for (i = 0; i < completed; i++) {
			if (completions[i].status != UIB_WC_SUCCESS ||
			    !(completions[i].wr_id & QP_SCALE_RECV_FLAG))
				return -EIO;
			recv_count++;
		}
		if (completed)
			progress = 1;
		if (!progress)
			nanosleep(&delay, NULL);
		if (clock_ns(CLOCK_MONOTONIC) >= deadline)
			return -ETIMEDOUT;
	}
	return 0;
}

static int run_batch(struct qp_scale_group *sender,
		     struct qp_scale_group *receiver,
		     const struct qp_scale_options *options,
		     uint32_t first, uint32_t count)
{
	uint8_t *source;
	uint8_t *target;
	uint64_t sequence;
	uint32_t qp_index;
	uint32_t slot;
	int rc;

	for (slot = 0; slot < count; slot++) {
		for (qp_index = 0; qp_index < options->qps; qp_index++) {
			sequence = (uint64_t)qp_index * options->iterations +
				first + slot;
			source = slot_buffer(sender, options, qp_index, slot);
			target = slot_buffer(receiver, options, qp_index, slot);
			fill_message(source, options->size, sequence);
			memset(target, 0, options->size);
			rc = post_receive(receiver, options, qp_index, slot,
					  first + slot);
			if (rc < 0)
				return rc;
			rc = post_send(sender, options, qp_index, slot,
				       first + slot);
			if (rc < 0)
				return rc;
		}
	}
	rc = poll_batch(sender, receiver, options->qps * count);
	if (rc < 0)
		return rc;
	for (qp_index = 0; qp_index < options->qps; qp_index++) {
		for (slot = 0; slot < count; slot++) {
			sequence = (uint64_t)qp_index * options->iterations +
				first + slot;
			target = slot_buffer(receiver, options, qp_index, slot);
			if (verify_message(target, options->size, sequence) < 0)
				return -EIO;
		}
	}
	return 0;
}

int main(void)
{
	struct qp_scale_group receiver = {0};
	struct qp_scale_group sender = {0};
	struct qp_scale_options options;
	uint64_t wall_start;
	uint64_t wall_end;
	uint64_t cpu_start;
	uint64_t cpu_end;
	uint64_t bytes;
	uint32_t count;
	uint32_t first;
	int rc;

	rc = options_init(&options);
	if (rc < 0) {
		fprintf(stderr, "invalid shared-QP scaling configuration\n");
		return 2;
	}
	rc = group_open(&sender, &options, options.port);
	if (rc < 0)
		goto out;
	rc = group_open(&receiver, &options, (uint16_t)(options.port + 1));
	if (rc < 0)
		goto out;
	rc = connect_groups(&sender, &receiver, options.mtu);
	if (rc < 0)
		goto out;
	cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = clock_ns(CLOCK_MONOTONIC);
	first = 0;
	while (first < options.iterations) {
		count = options.iterations - first;
		if (count > options.window)
			count = options.window;
		rc = run_batch(&sender, &receiver, &options, first, count);
		if (rc < 0)
			goto out;
		first += count;
	}
	wall_end = clock_ns(CLOCK_MONOTONIC);
	cpu_end = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	bytes = (uint64_t)options.qps * options.iterations * options.size;
	printf("CSV,%s,%u,%u,%u,%u,%u,%.9f,%.9f,%.9f\n",
	       options.protocol, options.qps, options.iterations, options.size,
	       options.window, options.mtu,
	       (double)bytes * 8.0 / (double)(wall_end - wall_start),
	       (double)(wall_end - wall_start) / 1000000000.0,
	       (double)(cpu_end - cpu_start) / 1000000000.0);
	puts("RESULT PASS");
	rc = 0;

out:
	if (rc < 0)
		fprintf(stderr, "shared-QP scaling failed: %s\n", strerror(-rc));
	group_close(&receiver);
	group_close(&sender);
	return rc < 0 ? 1 : rc;
}
