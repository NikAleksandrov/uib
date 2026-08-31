/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_ROCEV2_INTERNAL_H
#define UIB_ROCEV2_INTERNAL_H

#include <stdint.h>
#include <pthread.h>

#include "uib/psn_index.h"
#include "uib/congestion.h"
#include "uib/transport.h"
#include "uib/mr.h"
#include "uib/qp.h"
#include "ptr_ring.h"

struct uib_rx_meta;

#define UIB_ROCEV2_FLOW_LABEL_MASK 0x000fffffU
#define UIB_ROCEV2_UDP_SPORT_MIN 0xc000U
#define UIB_ROCEV2_PKT_MAX (UIB_PMTU + 64)
#define UIB_ROCEV2_OUT_POOL_MAX 1024
#define UIB_ROCEV2_OUT_POOL_CLASSES 3
#define UIB_ROCEV2_TX_BATCH_STACK 64
#define UIB_ROCEV2_ATOMIC_ACK_BYTES 8
#define UIB_ROCEV2_PREP_QUEUE_SIZE 256U
#define UIB_ROCEV2_SEQUENCE_QUEUE_SIZE 4096U
#define UIB_ROCEV2_WORKER_STACK_SIZE (256U * 1024U)
#define UIB_ROCEV2_WORKERS_AUTO_MAX 2U

struct uib_atomic_eth {
	uint64_t va;
	uint32_t rkey;
	uint64_t swap_data;
	uint64_t compare_data;
} __packed;

struct uib_outstanding_pool {
	struct uib_ptr_ring free[UIB_ROCEV2_OUT_POOL_CLASSES];
	uint32_t refs;
	int closing;
};

struct rocev2_send_data {
	const uint8_t *message;
	uint8_t *owned_message;
	struct uib_mr *mr;
	size_t total;
};

struct uib_rocev2_worker;

struct uib_rocev2_task {
	struct uib_rocev2_task *next;
	struct uib_rocev2_worker *worker;
	struct uib_qp *qp;
	struct uib_transport_ref *refs;
	struct uib_send_wr wr;
	struct rocev2_send_data data;
	uint32_t first_psn;
	uint32_t packets;
	uint32_t built;
	uint32_t mtu;
	int rc;
	uint8_t ready;
	struct uib_transport_ref inline_refs[UIB_ROCEV2_TX_BATCH_STACK];
};

struct uib_rocev2_worker {
	struct uib_rocev2_context *context;
	struct uib_ptr_ring ready;
	struct uib_ptr_ring free_tasks;
	struct uib_rocev2_task *tasks;
	pthread_cond_t cond;
	pthread_mutex_t lock;
	pthread_t thread;
	int initialized;
	int ready_initialized;
	int free_initialized;
	int started;
};

struct uib_rocev2_context {
	struct uib_rocev2_worker *workers;
	struct uib_ptr_ring sequence_ready;
	pthread_cond_t sequence_cond;
	pthread_mutex_t sequence_lock;
	pthread_t sequence_thread;
	uint32_t worker_cursor;
	unsigned int worker_count;
	int sequence_sync_initialized;
	int sequence_ready_initialized;
	int sequence_started;
	int sequence_run;
	int flat_sequence;
	int run;
};

static inline uint16_t uib_rocev2_udp_sport(uint32_t local_qpn,
					     uint32_t remote_qpn)
{
	uint64_t value;
	uint32_t flow_label;
	uint32_t high;
	uint32_t low;

	value = (uint64_t)local_qpn * remote_qpn;
	value ^= value >> 20;
	value ^= value >> 40;
	flow_label = (uint32_t)value & UIB_ROCEV2_FLOW_LABEL_MASK;
	low = flow_label & 0x3fffU;
	high = flow_label & 0xfc000U;
	low ^= high >> 14;
	return (uint16_t)(low | UIB_ROCEV2_UDP_SPORT_MIN);
}

static inline int rocev2_psn_cmp(uint32_t a, uint32_t b)
{
	int32_t difference;

	difference = (int32_t)((a - b) << 8);
	if (!difference)
		return 0;
	return difference < 0 ? -1 : 1;
}

struct uib_outstanding {
	struct uib_outstanding *next;
	struct uib_outstanding *prev;
	uint32_t                psn;
	uint8_t                 wire_opcode;
	uint8_t                 retries;
	uint8_t                 signaled;
	uint8_t                 is_read;
	uint8_t                 is_final;
	uint8_t                 indexed;
	uint8_t                 pool_class;
	uint8_t                 congestion_inflight;
	uint8_t                 sent;
	uint32_t                nominal_len;
	uint64_t                wr_id;
	enum uib_wr_opcode      wr_opcode;
	uint64_t                tx_time_ns;
	uint64_t                deadline_ns;
	void                   *pkt;
	size_t                  pkt_len;
	struct sockaddr_in      dst;
	struct uib_sge         *read_sgl;
	int                     read_num_sge;
	uint32_t                read_total_len;
	uint32_t                refs;
	struct uib_outstanding_pool *pool;
	uint8_t                 packet[];
};

struct uib_rocev2_qp {
	pthread_cond_t                prep_cond;
	struct uib_congestion         congestion;
	struct uib_rocev2_task       *prep_head;
	struct uib_rocev2_task       *prep_tail;
	struct uib_outstanding      *out_head;
	struct uib_outstanding      *out_tail;
	struct uib_outstanding      *read_head;
	struct uib_outstanding      *read_tail;
	struct uib_outstanding_pool *out_pool;
	struct uib_psn_index         out_index;
	uint32_t                     out_count;
	uint32_t                     prep_pending;
	uint8_t                      prep_closing;
	uint8_t                      sequence_pending;
	uint8_t                      sequence_queued;
	uint32_t                     last_acked_psn;
	uint32_t                     last_rx_psn;
	uint32_t                     atomic_ack_psn;
	uint64_t                     atomic_ack_value;
	uint8_t                      last_rx_valid;
	uint8_t                      atomic_ack_valid;
	uint32_t                     sequence_retry_psn;
	uint8_t                      sequence_retry_active;

	struct uib_rq_slot           frag_recv;
	struct uib_mr               *frag_recv_mr;
	uint8_t                      frag_recv_active;
	uint32_t                     frag_recv_offset;

	uint64_t                     frag_write_va;
	uint32_t                     frag_write_total;
	uint32_t                     frag_write_offset;
	uint8_t                      frag_write_active;
	struct uib_mr               *frag_write_mr;

	struct uib_outstanding      *frag_read_out;
	uint32_t                     frag_read_offset;
};

static inline struct uib_rocev2_qp *rocev2_qp(const struct uib_qp *qp)
{
	return (struct uib_rocev2_qp *)qp->protocol_private;
}

int uib_rocev2_qp_init(struct uib_qp *qp);
void uib_rocev2_qp_fini(struct uib_qp *qp);
void uib_rocev2_qp_modified(struct uib_qp *qp, int attr_mask);
int uib_rocev2_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			 struct uib_send_wr **bad);
int uib_rocev2_handle_packet_meta(struct uib_qp *qp,
				  const struct sockaddr_in *src,
				  const void *packet, size_t len,
				  const struct uib_rx_meta *meta);
void uib_rocev2_retransmit_tick(struct uib_qp *qp, uint64_t now_ns);
struct uib_outstanding_pool *rocev2_outstanding_pool_create(
						uint32_t max_send_wr);
void rocev2_outstanding_pool_destroy(struct uib_outstanding_pool *pool);
void rocev2_free_outstanding(void *opaque);
int rocev2_congestion_init(struct uib_qp *qp);
void rocev2_process_task(struct uib_rocev2_task *task);
void rocev2_task_release(struct uib_rocev2_task *task);
void rocev2_sequence_notify(struct uib_qp *qp);
void rocev2_sequence_progress(struct uib_qp *qp);
int rocev2_scatter_recv_fragment(struct uib_qp *qp,
				  struct uib_rq_slot *slot,
				  uint32_t offset, const uint8_t *data,
				  size_t length, size_t *consumed);
void rocev2_outstanding_unlink(struct uib_qp *qp,
			       struct uib_outstanding *out);
void rocev2_outstanding_retry_reschedule(struct uib_qp *qp);
int rocev2_drain_locked(struct uib_qp *qp);
void rocev2_send_ack(struct uib_qp *qp, const struct sockaddr_in *dst,
			 uint32_t psn, uint8_t syndrome);
void rocev2_send_cnp(struct uib_qp *qp, const struct sockaddr_in *dst);
int rocev2_send_outstanding(struct uib_qp *qp,
			    struct uib_outstanding *out);
int rocev2_transport_send(struct uib_qp *qp,
			  const struct sockaddr_in *dst,
			  const void *packet, size_t length);
void rocev2_request_ack(struct uib_outstanding *out);
void rocev2_fail_outstanding(struct uib_qp *qp,
			     enum uib_wc_status status);
int rocev2_packet_qpn(const void *packet, size_t len, uint32_t *qpn);
int rocev2_path_mtu_valid(uint32_t path_mtu);
int rocev2_wire_encode(const struct uib_wire_meta *meta,
			void *packet, size_t len);
int rocev2_wire_decode(const struct uib_wire_meta *meta,
			void *packet, size_t len);

#endif
