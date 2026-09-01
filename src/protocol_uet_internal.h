/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_PROTOCOL_UET_INTERNAL_H
#define UIB_PROTOCOL_UET_INTERNAL_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <sched.h>
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/ip.h>

#include "uib/uet.h"
#include "uib/dcqcn.h"
#include "uib/uet_rccc.h"
#include "uib/uet_nscc.h"
#include "uib/protocol.h"
#include "uib/qp.h"
#include "uib/psn_index.h"
#include "uib/pd.h"
#include "uib/context.h"
#include "uib/congestion.h"
#include "uib/env.h"
#include "uib/transport.h"
#include "uib/mr.h"
#include "congestion_internal.h"
#include "host_zc_internal.h"
#include "qp_internal.h"
#include "ptr_ring.h"
#include "utils.h"

#define UIB_UET_OUT_INDEX_MIN 64U
#define UIB_UET_OUT_POOL_CLASSES 4U
#define UIB_UET_OUT_POOL_LIMIT UIB_UET_PDC_MPR
#define UIB_UET_MPR_ENCODING (UIB_UET_PDC_MPR / 128)
#define UIB_UET_ACK_INTERVAL 16U
#define UIB_UET_ACK_GEN_TRIGGER 16384U
#define UIB_UET_ACK_GEN_MIN_PACKET 1024U
#define UIB_UET_PACKET_MAX UIB_RECV_BUF_LEN
#define UIB_UET_TRACK_POOL_MAX 1024U
#define UIB_UET_READ_FRAGMENTS_INLINE 8U
#define UIB_UET_GATHER_POOL_WARM 8U
#define UIB_UET_PREP_WORKER_QUEUE_SIZE 256U
#define UIB_UET_SEQUENCE_QUEUE_SIZE 4096U
#define UIB_UET_PREP_CHUNK 32U
#define UIB_UET_PREP_WORKER_STACK_SIZE (256U * 1024U)
/* Preparation workers shard QPs; each QP remains ordered on one worker. */
#define UIB_UET_PREP_WORKERS_AUTO_MAX 2U

enum uib_uet_pdc_state {
	UIB_UET_PDC_CLOSED,
	UIB_UET_PDC_SYN_SENT,
	UIB_UET_PDC_ESTABLISHED,
};

struct uib_uet_track_pool;

struct uib_uet_read_fragment {
	uint32_t offset;
	uint32_t length;
};

struct uib_uet_wr_track {
	struct uib_uet_wr_track *next;
	struct uib_uet_wr_track *prev;
	struct uib_uet_wr_track *prep_next;
	struct uib_uet_track_pool *pool;
	struct uib_qp *qp;
	uint64_t wr_id;
	enum uib_wr_opcode opcode;
	int signaled;
	uint32_t total_len;
	uint32_t pending;
	uint32_t packets;
	uint32_t next_packet;
	uint32_t scheduled_packet;
	uint32_t first_psn;
	uint32_t clear_psn;
	uint32_t chunk_max;
	uint64_t nominal_bytes;
	struct sockaddr_in destination;
	const uint8_t *message;
	uint8_t *owned_message;
	struct uib_uet_outstanding *owned_message_buffer;
	struct uib_mr *mr;
	struct uib_send_wr wr;
	uint16_t msg_id;
	uint8_t ses_opcode;
	uint8_t request_header;
	uint8_t fetching_atomic;
	uint8_t send_syn;
	uint8_t rccc_accounted;
	size_t pds_len;
	struct uib_sge *read_sgl;
	int read_num_sge;
	struct uib_uet_read_fragment *read_fragments;
	uint32_t read_fragment_count;
	uint32_t read_fragment_capacity;
	uint32_t read_received;
	uint8_t uet_atomic;
	uint8_t atomic_datatype;
	uint8_t response_code;
	uint8_t prep_ready;
	uint8_t cancelled;
	uint32_t refill_tasks;
	uint32_t refs;
	int refill_error;
	struct uib_uet_read_fragment
		inline_read_fragments[UIB_UET_READ_FRAGMENTS_INLINE];
	struct uib_sge inline_read_sgl[];
};

struct uib_uet_context;
struct uib_uet_prep_worker;

struct uib_uet_prep_task {
	struct uib_uet_wr_track *track;
	struct uib_uet_prep_worker *worker;
	struct uib_uet_outstanding *result_head;
	struct uib_uet_outstanding *result_tail;
	struct uib_uet_prep_task *refill_next;
	uint32_t packet_index;
	uint32_t packet_count;
	int rc;
	uint8_t ready;
};

struct uib_uet_prep_worker {
	struct uib_uet_context *context;
	struct uib_ptr_ring     ready;
	struct uib_ptr_ring     free_tasks;
	struct uib_uet_prep_task *tasks;
	pthread_cond_t          cond;
	pthread_mutex_t         lock;
	pthread_t               thread;
	int                     initialized;
	int                     ready_initialized;
	int                     free_initialized;
	int                     started;
};

struct uib_uet_context {
	struct uib_uet_prep_worker *workers;
	struct uib_ptr_ring sequence_ready;
	pthread_cond_t sequence_cond;
	pthread_mutex_t sequence_lock;
	pthread_t sequence_thread;
	uint32_t worker_cursor;
	unsigned int worker_count;
	uint8_t trimmable_dscp;
	uint8_t trimmed_dscp;
	uint8_t trimmed_last_hop_dscp;
	uint8_t trimming_enabled;
	int sequence_sync_initialized;
	int sequence_ready_initialized;
	int sequence_started;
	int sequence_run;
	int run;
};

struct uib_uet_track_pool {
	struct uib_uet_wr_track *free;
	pthread_mutex_t          lock;
	uint32_t                 cached;
	uint32_t                 limit;
	uint32_t                 max_sge;
	uint32_t                 refs;
	int                      closing;
};

struct uib_uet_out_pool;

struct uib_uet_outstanding {
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *prev;
	struct uib_uet_outstanding *ack_next;
	struct uib_uet_outstanding *ack_prev;
	struct uib_uet_wr_track *track;
	struct uib_uet_out_pool *pool;
	struct uib_mr *payload_mr;
	struct sockaddr_in dst;
	uint8_t *packet;
	const uint8_t *payload;
	size_t packet_len;
	size_t header_len;
	size_t payload_len;
	uint64_t deadline_ns;
	uint64_t tx_time_ns;
	uint64_t rto_ns;
	uint32_t psn;
	uint32_t nominal_len;
	uint32_t read_offset;
	uint32_t read_len;
	uint16_t msg_id;
	uint8_t request_header;
	uint16_t path_index;
	uint16_t entropy;
	uint8_t retries;
	uint8_t rto_retries;
	uint8_t sent;
	uint8_t tx_pending;
	uint8_t sack_acked;
	uint8_t indexed;
	uint8_t nack_pending;
	uint8_t pool_class;
	uint32_t refs;
	uint8_t storage[];
};

struct uib_uet_out_pool {
	struct uib_uet_outstanding *free[UIB_UET_OUT_POOL_CLASSES];
	pthread_mutex_t              lock;
	uint32_t                     cached[UIB_UET_OUT_POOL_CLASSES];
	uint32_t                     refs;
	int                          closing;
};

struct uib_uet_deferred {
	struct uib_uet_outstanding *packet;
	struct sockaddr_in          source;
	uint32_t                    psn;
	uint16_t                    msg_id;
	uint8_t                     opcode;
	uint8_t                     ecn_marked;
};

struct uib_uet_qp {
	enum uib_uet_pdc_state state;
	pthread_cond_t prep_cond;
	pthread_mutex_t rx_lock;
	uint16_t spdcid;
	uint16_t dpdcid;
	uint16_t next_msg_id;
	uint32_t next_psn;
	uint32_t publish_psn;
	uint32_t rx_base_psn;
	uint32_t tx_clear_psn;
	uint32_t rx_clear_psn;
	uint64_t rx_bitmap[2];
	struct uib_uet_deferred deferred[UIB_UET_PDC_MPR];
	uint32_t deferred_count;
	uint32_t ack_gen_count;
	uint32_t out_count;
	uint32_t sent_count;
	uint32_t materialized_count;
	uint32_t prep_pending;
	int prep_closing;
	int sequence_pending;
	int sequence_queued;
	int rx_initialized;
	int tx_initialized;
	struct uib_uet_outstanding *out_head;
	struct uib_uet_outstanding *out_tail;
	struct uib_uet_outstanding *ack_head;
	struct uib_uet_outstanding *ack_tail;
	struct uib_uet_outstanding *tx_cursor;
	struct uib_uet_wr_track *track_head;
	struct uib_uet_wr_track *track_tail;
	struct uib_uet_wr_track *prep_head;
	struct uib_uet_prep_task *refill_head;
	struct uib_uet_prep_task *refill_tail;
	struct uib_uet_out_pool *out_pool;
	struct uib_uet_track_pool *track_pool;
	struct uib_psn_index out_index;
	uint32_t unindexed_count;
	struct uib_congestion congestion;
	struct uib_uet_rccc_destination rccc_destination;
	uint64_t credit_request_deadline_ns;
	uint32_t receiver_bytes;
	uint32_t previous_received_units;
	int rccc_enabled;
	int debug;
	uint8_t trimmed_dscp;
	uint8_t trimmed_last_hop_dscp;
	uint8_t trimming_enabled;
	struct uib_uet_session_config session;
	enum uib_uet_optimized_mode optimized_mode;
	struct uib_uet_optimized_stats optimized_stats;
	struct uib_uet_multipath_config multipath;
	struct uib_uet_multipath_stats multipath_stats;
	uint16_t path_cursor;
	uint16_t previous_path;
	struct {
		uint64_t blocked_until_ns;
		uint64_t sent;
		uint64_t acknowledged;
		uint32_t ecn_marks;
		uint32_t timeouts;
		int used;
	} paths[UIB_UET_MAX_PATHS];
	struct uib_rq_slot recv_slot;
	struct sockaddr_in recv_src;
	uint16_t recv_msg_id;
	uint32_t recv_total;
	uint32_t recv_last_psn;
	int recv_active;
	int recv_eom;
	uint32_t recv_imm;
	int recv_has_imm;
	uint32_t recv_source_pdc;
	uint16_t write_msg_id;
	uint32_t write_last_psn;
	int write_active;
	int write_eom;
	int write_with_imm;
	struct {
		uint32_t psn;
		uint16_t msg_id;
		uint8_t response_code;
		uint8_t data_len;
		uint32_t modified_len;
		uint8_t data[16];
		int valid;
	} response_cache[UIB_UET_PDC_MPR];
	struct {
		uint32_t psn;
		int valid;
	} guaranteed[UIB_UET_PDC_MPR];
	struct {
		uint32_t first_psn;
		uint16_t msg_id;
		uint8_t response_code;
		int valid;
	} message_error[UIB_UET_PDC_MPR];
};

#define UIB_UET_INTERNAL __attribute__((visibility("hidden")))

UIB_UET_INTERNAL int uet_psn_before(uint32_t a, uint32_t b);
UIB_UET_INTERNAL void uet_debug(struct uib_qp *qp, const char *format, ...);
UIB_UET_INTERNAL int uet_atomic_has_two_operands(uint8_t opcode);
UIB_UET_INTERNAL struct uib_uet_out_pool *uet_out_pool_create(void);
UIB_UET_INTERNAL void uet_out_pool_prewarm_mtu(
	struct uib_uet_out_pool *pool, uint32_t path_mtu);
UIB_UET_INTERNAL void uet_out_pool_prewarm_gather(
	struct uib_uet_out_pool *pool);
UIB_UET_INTERNAL void uet_out_pool_destroy(struct uib_uet_out_pool *pool);
UIB_UET_INTERNAL void uet_out_pool_push(struct uib_uet_outstanding *out);
UIB_UET_INTERNAL struct uib_uet_outstanding *
uet_alloc_outstanding(struct uib_uet_qp *uet, size_t packet_len);
UIB_UET_INTERNAL struct uib_uet_track_pool *
uet_track_pool_create(uint32_t max_send_wr, uint32_t max_send_sge);
UIB_UET_INTERNAL void uet_track_pool_destroy(
	struct uib_uet_track_pool *pool);
UIB_UET_INTERNAL void uet_track_pool_push(struct uib_uet_wr_track *track);
UIB_UET_INTERNAL void uet_track_get(struct uib_uet_wr_track *track);
UIB_UET_INTERNAL void uet_free_track(struct uib_uet_wr_track *track);
UIB_UET_INTERNAL int uet_refill_locked(struct uib_qp *qp);
UIB_UET_INTERNAL void uet_sequence_progress(struct uib_qp *qp);
UIB_UET_INTERNAL void uet_sequence_notify(struct uib_qp *qp);
UIB_UET_INTERNAL void uet_free_outstanding(void *opaque);
UIB_UET_INTERNAL void uet_process_refill_task(struct uib_uet_prep_task *task);
UIB_UET_INTERNAL void uet_link_outstanding(
	struct uib_uet_qp *uet, struct uib_uet_outstanding *out);
UIB_UET_INTERNAL void uet_link_track(struct uib_uet_qp *uet,
				     struct uib_uet_wr_track *track);
UIB_UET_INTERNAL void uet_remove_track_locked(
	struct uib_qp *qp, struct uib_uet_wr_track *track,
	enum uib_wc_status status);
UIB_UET_INTERNAL int uet_drain_locked(struct uib_qp *qp);
UIB_UET_INTERNAL uint16_t uet_ses_rsp_pack(uint8_t opcode,
					   uint8_t response_code);
UIB_UET_INTERNAL void uet_clear_guaranteed(struct uib_uet_qp *uet,
					   uint32_t clear_psn);
UIB_UET_INTERNAL int uet_trim_code(const struct uib_uet_qp *uet,
				   const struct uib_rx_meta *meta);
UIB_UET_INTERNAL int uet_handle_trimmed(
	struct uib_qp *qp, const struct sockaddr_in *source,
	const void *packet, size_t len, uint8_t type, uint8_t nack_code);
UIB_UET_INTERNAL int uet_handle_request(
	struct uib_qp *qp, const struct sockaddr_in *source,
	const void *packet, size_t len, int ecn_marked);
UIB_UET_INTERNAL int uet_handle_ack(struct uib_qp *qp,
				    const void *packet, size_t len,
				    uint64_t rx_time_ns);
UIB_UET_INTERNAL int uet_handle_nack(struct uib_qp *qp,
				     const void *packet, size_t len);
UIB_UET_INTERNAL int uet_handle_control(
	struct uib_qp *qp, const struct sockaddr_in *source,
	const void *packet, size_t len);
UIB_UET_INTERNAL void uet_qp_fini(struct uib_qp *qp);
UIB_UET_INTERNAL int uet_packet_qpn(const void *packet, size_t len,
				    uint32_t *qpn);
UIB_UET_INTERNAL int uet_post_send(struct uib_qp *qp,
				   struct uib_send_wr *wr,
				   struct uib_send_wr **bad);
UIB_UET_INTERNAL void uet_retransmit_tick(struct uib_qp *qp,
					  uint64_t now_ns);
UIB_UET_INTERNAL uint8_t uet_ipv4_tos(struct uib_context *ctx,
				      const void *packet, size_t len);

#endif
