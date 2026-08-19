#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "direct_common.h"

#define EXAMPLE_BUFFER_SIZE 65536
#define EXAMPLE_MESSAGE_SIZE 10003
#define EXAMPLE_TIMEOUT_NS (3ULL * 1000 * 1000 * 1000)

static int wait_for(struct direct_endpoint *endpoint, uint64_t wr_id,
		    enum uib_wc_opcode opcode)
{
	struct uib_wc completion;
	int rc = direct_wait_completion(endpoint->cq, &completion,
					  EXAMPLE_TIMEOUT_NS);

	if (rc < 0) {
		fprintf(stderr, "completion %llu failed: %s\n",
			(unsigned long long)wr_id, strerror(-rc));
		return rc;
	}
	if (completion.wr_id != wr_id || completion.opcode != opcode) {
		fprintf(stderr, "unexpected completion: id=%llu opcode=%d\n",
			(unsigned long long)completion.wr_id, completion.opcode);
		return -EIO;
	}
	return 0;
}

static int post_atomic(struct direct_endpoint *endpoint, uint64_t wr_id,
		       enum uib_wr_opcode opcode, uint64_t *result,
		       uint64_t remote_address, uint32_t remote_key,
		       uint64_t compare_add, uint64_t swap)
{
	struct uib_sge result_sge = {
		.addr = (uintptr_t)result,
		.length = sizeof(*result),
		.lkey = endpoint->mr->lkey,
	};
	struct uib_send_wr atomic = {
		.wr_id = wr_id,
		.opcode = opcode,
		.send_flags = UIB_SEND_SIGNALED,
		.sg_list = &result_sge,
		.num_sge = 1,
		.atomic = {
			.remote_addr = remote_address,
			.compare_add = compare_add,
			.swap = swap,
			.rkey = remote_key,
		},
	};
	struct uib_send_wr *bad;

	return uib_post_send(endpoint->qp, &atomic, &bad);
}

static int post_uet_atomic(struct direct_endpoint *endpoint, uint64_t wr_id,
			   uint32_t *result, uint32_t *target,
			   uint32_t remote_key, uint32_t operand)
{
	struct uib_uet_atomic_wr atomic = {
		.wr_id = wr_id,
		.remote_addr = (uintptr_t)target,
		.rkey = remote_key,
		.result_lkey = endpoint->mr->lkey,
		.send_flags = UIB_SEND_SIGNALED,
		.opcode = UIB_UET_AMO_BXOR,
		.datatype = UIB_UET_AMO_UINT32,
		.fetching = 1,
		.operand = &operand,
		.result = result,
	};
	struct uib_uet_atomic_wr *bad;

	return uib_uet_post_atomic(endpoint->qp, &atomic, &bad);
}

static int configure_uet(struct direct_endpoint *endpoint, uint32_t initiator)
{
	struct uib_uet_multipath_config multipath = {
		.mode = UIB_UET_MULTIPATH_OBLIVIOUS,
		.entropy_base = UIB_UET_DEFAULT_ENTROPY_BASE,
		.path_count = 8,
		.ecn_hold_ns = UIB_UET_RTX_NS,
		.failure_hold_ns = UIB_UET_DEFAULT_FAILURE_HOLD_NS,
	};
	struct uib_uet_session_config session = {
		.job_id = 0x123456,
		.initiator_id = initiator,
		.pid_on_fep = endpoint->qp->qp_num,
		.resource_index = 7,
		.resource_generation = 3,
		.relative_addressing = 0,
	};
	struct uib_uet_nscc_config congestion;

	if (uib_uet_session_configure(endpoint->qp, &session) < 0)
		return -1;
	if (uib_uet_optimized_configure(endpoint->qp,
					UIB_UET_OPTIMIZED_AUTO) < 0 ||
	    uib_uet_multipath_configure(endpoint->qp, &multipath) < 0)
		return -1;

	/* Applications may override the normative NSCC defaults per QP. */
	uib_uet_nscc_config_default(&congestion, endpoint->qp->path_mtu + 40);
	return uib_uet_nscc_configure_qp(endpoint->qp, &congestion);
}

int main(int argc, char **argv)
{
	const char *protocol = argc > 1 ? argv[1] : "uet";
	struct direct_endpoint sender = {0}, receiver = {0};
	uint8_t *send_buffer, *receive_buffer, *read_buffer, *remote_buffer;
	uint64_t *atomic_result, *atomic_target;
	struct uib_uet_nscc congestion;
	struct uib_uet_multipath_stats multipath_stats;
	struct uib_uet_optimized_stats optimized_stats;
	enum uib_uet_optimized_mode optimized_mode;
	int full_rdma = strcmp(protocol, "dummy");
	int is_uet = !strcmp(protocol, "uet");
	int rc = 1;
	size_t i;

	if (direct_endpoint_open(&sender, protocol, "127.0.0.1", 19000,
				 EXAMPLE_BUFFER_SIZE, 32) < 0 ||
	    direct_endpoint_open(&receiver, protocol, "127.0.0.1", 19001,
				 EXAMPLE_BUFFER_SIZE, 32) < 0) {
		fprintf(stderr, "open failed: %s\n", strerror(errno));
		goto out;
	}
	if (direct_endpoint_connect(&sender, &receiver, 4096) < 0 ||
	    direct_endpoint_connect(&receiver, &sender, 4096) < 0) {
		fprintf(stderr, "QP connection failed\n");
		goto out;
	}
	if (is_uet && (configure_uet(&sender, 0x101) < 0 ||
		       configure_uet(&receiver, 0x202) < 0)) {
		fprintf(stderr, "UET session configuration failed\n");
		goto out;
	}

	send_buffer = sender.buffer;
	read_buffer = sender.buffer + 16384;
	atomic_result = (uint64_t *)(sender.buffer + 32768);
	receive_buffer = receiver.buffer;
	remote_buffer = receiver.buffer + 16384;
	atomic_target = (uint64_t *)(receiver.buffer + 32768);
	for (i = 0; i < EXAMPLE_MESSAGE_SIZE; i++)
		send_buffer[i] = (uint8_t)(i * 17 + 3);

	if (direct_post_receive(&receiver, 0x2001, receive_buffer,
				EXAMPLE_MESSAGE_SIZE) < 0 ||
	    direct_post_send(&sender, 0x1001, UIB_WR_SEND, send_buffer,
			     EXAMPLE_MESSAGE_SIZE, 0, 0) < 0 ||
	    wait_for(&sender, 0x1001, UIB_WC_SEND) < 0 ||
	    wait_for(&receiver, 0x2001, UIB_WC_RECV) < 0 ||
	    memcmp(send_buffer, receive_buffer, EXAMPLE_MESSAGE_SIZE)) {
		fprintf(stderr, "SEND transfer failed\n");
		goto out;
	}
	printf("SEND  %u bytes transferred and verified\n", EXAMPLE_MESSAGE_SIZE);

	if (full_rdma) {
		/* WRITE and READ name a registered remote address and its rkey. */
		if (direct_post_send(&sender, 0x1002, UIB_WR_RDMA_WRITE,
				     send_buffer, EXAMPLE_MESSAGE_SIZE,
				     (uintptr_t)remote_buffer,
				     receiver.mr->rkey) < 0 ||
		    wait_for(&sender, 0x1002, UIB_WC_RDMA_WRITE) < 0 ||
		    memcmp(send_buffer, remote_buffer, EXAMPLE_MESSAGE_SIZE)) {
			fprintf(stderr, "RDMA WRITE failed\n");
			goto out;
		}
		memset(read_buffer, 0, EXAMPLE_MESSAGE_SIZE);
		if (direct_post_send(&sender, 0x1003, UIB_WR_RDMA_READ,
				     read_buffer, EXAMPLE_MESSAGE_SIZE,
				     (uintptr_t)remote_buffer,
				     receiver.mr->rkey) < 0 ||
		    wait_for(&sender, 0x1003, UIB_WC_RDMA_READ) < 0 ||
		    memcmp(read_buffer, remote_buffer, EXAMPLE_MESSAGE_SIZE)) {
			fprintf(stderr, "RDMA READ failed\n");
			goto out;
		}
		printf("WRITE and READ transferred and verified %u bytes\n",
		       EXAMPLE_MESSAGE_SIZE);
	}

	if (is_uet) {
		uint32_t *generic_result = (uint32_t *)(sender.buffer + 49152);
		uint32_t *generic_target = (uint32_t *)(receiver.buffer + 49152);

		/* Fetching atomics return the old value in the local result SGE. */
		*atomic_target = 41;
		*atomic_result = 0;
		if (post_atomic(&sender, 0x1004, UIB_WR_ATOMIC_FETCH_ADD,
				atomic_result, (uintptr_t)atomic_target,
				receiver.mr->rkey, 1, 0) < 0 ||
		    wait_for(&sender, 0x1004, UIB_WC_FETCH_ADD) < 0 ||
		    *atomic_result != 41 || *atomic_target != 42) {
			fprintf(stderr, "fetch-add failed\n");
			goto out;
		}
		if (post_atomic(&sender, 0x1005, UIB_WR_ATOMIC_CMP_SWAP,
				atomic_result, (uintptr_t)atomic_target,
				receiver.mr->rkey, 42, 99) < 0 ||
		    wait_for(&sender, 0x1005, UIB_WC_COMP_SWAP) < 0 ||
		    *atomic_result != 42 || *atomic_target != 99) {
			fprintf(stderr, "compare-swap failed\n");
			goto out;
		}
		*generic_target = 0xa5a5U;
		*generic_result = 0;
		if (post_uet_atomic(&sender, 0x1006, generic_result,
				    generic_target, receiver.mr->rkey,
				    0x00ffU) < 0 ||
		    wait_for(&sender, 0x1006, UIB_WC_UET_ATOMIC) < 0 ||
		    *generic_result != 0xa5a5U || *generic_target != 0xa55aU) {
			fprintf(stderr, "generic UET atomic failed\n");
			goto out;
		}
		if (uib_uet_nscc_query_qp(sender.qp, &congestion) < 0)
			goto out;
		if (uib_uet_optimized_query(sender.qp, &optimized_mode,
					    &optimized_stats) < 0 ||
		    uib_uet_multipath_query(sender.qp, NULL,
					    &multipath_stats) < 0)
			goto out;
		printf("UET NSCC cwnd=%llu bytes byte-clock-residual=%lld base-rtt=%llu ns\n",
		       (unsigned long long)congestion.cwnd,
		       (long long)congestion.inflight,
		       (unsigned long long)congestion.base_rtt_ns);
		printf("UET optimized mode=%u medium=%llu standard=%llu; multipath paths=%u switches=%llu\n",
		       optimized_mode,
		       (unsigned long long)optimized_stats.medium_requests,
		       (unsigned long long)optimized_stats.standard_requests,
		       multipath_stats.paths_used,
		       (unsigned long long)multipath_stats.path_switches);
	}

	printf("direct UIB %s example: PASS\n", protocol);
	rc = 0;
out:
	direct_endpoint_close(&receiver);
	direct_endpoint_close(&sender);
	return rc;
}
