/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Wire formats and transport behavior follow the Ultra Ethernet
 * Specification 1.0.2 published by the Ultra Ethernet Consortium.
 */
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
#define UIB_UET_PREP_CHUNK 32U
#define UIB_UET_PREP_WORKER_STACK_SIZE (256U * 1024U)

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
	uint32_t worker_cursor;
	unsigned int worker_count;
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
	uint8_t sack_acked;
	uint8_t indexed;
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

static int uet_refill_locked(struct uib_qp *qp);
static int uet_schedule_refill_locked(struct uib_qp *qp);
static void uet_free_outstanding(void *opaque);
static void uet_process_refill_task(struct uib_uet_prep_task *task);

static size_t uet_out_pool_capacity(uint8_t pool_class)
{
	static const size_t capacities[UIB_UET_OUT_POOL_CLASSES] = {
		256,
		2048,
		8192,
		UIB_UET_PACKET_MAX,
	};

	return capacities[pool_class];
}

static uint8_t uet_out_pool_class(size_t packet_len)
{
	uint8_t pool_class;

	for (pool_class = 0; pool_class < UIB_UET_OUT_POOL_CLASSES;
	     pool_class++) {
		if (packet_len <= uet_out_pool_capacity(pool_class))
			return pool_class;
	}
	return UIB_UET_OUT_POOL_CLASSES;
}

static void uet_out_pool_put(struct uib_uet_out_pool *pool)
{
	if (!__atomic_sub_fetch(&pool->refs, 1, __ATOMIC_ACQ_REL)) {
		pthread_mutex_destroy(&pool->lock);
		free(pool);
	}
}

static void uet_out_pool_push(struct uib_uet_outstanding *out)
{
	struct uib_uet_out_pool *pool = out->pool;
	uint8_t pool_class = out->pool_class;

	pthread_mutex_lock(&pool->lock);
	if (!pool->closing &&
	    pool->cached[pool_class] < UIB_UET_OUT_POOL_LIMIT) {
		out->next = pool->free[pool_class];
		pool->free[pool_class] = out;
		pool->cached[pool_class]++;
		pthread_mutex_unlock(&pool->lock);
		return;
	}
	pthread_mutex_unlock(&pool->lock);
	free(out);
	uet_out_pool_put(pool);
}

static struct uib_uet_out_pool *uet_out_pool_create(void)
{
	struct uib_uet_out_pool *pool;

	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return NULL;
	if (pthread_mutex_init(&pool->lock, NULL)) {
		free(pool);
		return NULL;
	}
	pool->refs = 1;
	return pool;
}

static void uet_out_pool_prewarm(struct uib_uet_out_pool *pool,
				 uint8_t pool_class, uint32_t target)
{
	struct uib_uet_outstanding *out;
	uint32_t cached;

	if (pool_class >= UIB_UET_OUT_POOL_CLASSES)
		return;
	for (;;) {
		pthread_mutex_lock(&pool->lock);
		cached = pool->cached[pool_class];
		pthread_mutex_unlock(&pool->lock);
		if (cached >= target)
			return;
		out = malloc(sizeof(*out) +
			     uet_out_pool_capacity(pool_class));
		if (!out)
			return;
		__atomic_add_fetch(&pool->refs, 1, __ATOMIC_RELAXED);
		out->pool = pool;
		out->pool_class = pool_class;
		uet_out_pool_push(out);
	}
}

static void uet_out_pool_prewarm_mtu(struct uib_uet_out_pool *pool,
				     uint32_t path_mtu)
{
	uint8_t header_class;
	uint8_t packet_class;

	header_class = uet_out_pool_class(256);
	packet_class = uet_out_pool_class(path_mtu);
	uet_out_pool_prewarm(pool, header_class, UIB_UET_PDC_MPR);
	if (packet_class != header_class)
		uet_out_pool_prewarm(pool, packet_class, UIB_UET_PDC_MPR);
}

static void uet_out_pool_prewarm_gather(struct uib_uet_out_pool *pool)
{
	uint8_t pool_class;

	for (pool_class = 0; pool_class < UIB_UET_OUT_POOL_CLASSES;
	     pool_class++)
		uet_out_pool_prewarm(pool, pool_class,
				     UIB_UET_GATHER_POOL_WARM);
}

static void uet_out_pool_destroy(struct uib_uet_out_pool *pool)
{
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;
	uint8_t pool_class;

	if (!pool)
		return;
	pthread_mutex_lock(&pool->lock);
	pool->closing = 1;
	for (pool_class = 0; pool_class < UIB_UET_OUT_POOL_CLASSES;
	     pool_class++) {
		out = pool->free[pool_class];
		pool->free[pool_class] = NULL;
		pool->cached[pool_class] = 0;
		while (out) {
			next = out->next;
			free(out);
			uet_out_pool_put(pool);
			out = next;
		}
	}
	pthread_mutex_unlock(&pool->lock);
	uet_out_pool_put(pool);
}

static struct uib_uet_outstanding *
uet_alloc_outstanding(struct uib_uet_qp *uet, size_t packet_len)
{
	struct uib_uet_out_pool *pool = uet->out_pool;
	struct uib_uet_outstanding *out;
	uint8_t pool_class;

	pool_class = uet_out_pool_class(packet_len);
	if (pool_class == UIB_UET_OUT_POOL_CLASSES)
		return NULL;
	pthread_mutex_lock(&pool->lock);
	out = pool->free[pool_class];
	if (out) {
		pool->free[pool_class] = out->next;
		pool->cached[pool_class]--;
	}
	pthread_mutex_unlock(&pool->lock);
	if (!out) {
		out = malloc(sizeof(*out) +
			     uet_out_pool_capacity(pool_class));
		if (!out)
			return NULL;
		__atomic_add_fetch(&pool->refs, 1, __ATOMIC_RELAXED);
		out->pool = pool;
	}
	out->next = NULL;
	out->prev = NULL;
	out->ack_next = NULL;
	out->ack_prev = NULL;
	out->track = NULL;
	out->pool = pool;
	out->payload_mr = NULL;
	out->pool_class = pool_class;
	out->packet = out->storage;
	out->payload = NULL;
	out->packet_len = packet_len;
	out->header_len = packet_len;
	out->payload_len = 0;
	out->deadline_ns = 0;
	out->tx_time_ns = 0;
	out->psn = 0;
	out->nominal_len = 0;
	out->read_offset = 0;
	out->read_len = 0;
	out->msg_id = 0;
	out->request_header = 0;
	out->path_index = 0;
	out->entropy = 0;
	out->retries = 0;
	out->rto_retries = 0;
	out->sent = 0;
	out->sack_acked = 0;
	out->indexed = 0;
	out->refs = 1;
	return out;
}

static void uet_track_pool_put(struct uib_uet_track_pool *pool)
{
	if (!__atomic_sub_fetch(&pool->refs, 1, __ATOMIC_ACQ_REL)) {
		pthread_mutex_destroy(&pool->lock);
		free(pool);
	}
}

static struct uib_uet_wr_track *
uet_track_pool_new(struct uib_uet_track_pool *pool)
{
	struct uib_uet_wr_track *track;
	size_t size;

	size = sizeof(*track) + pool->max_sge * sizeof(*track->inline_read_sgl);
	track = malloc(size);
	if (!track)
		return NULL;
	__atomic_add_fetch(&pool->refs, 1, __ATOMIC_RELAXED);
	track->pool = pool;
	return track;
}

static void uet_track_pool_push(struct uib_uet_wr_track *track)
{
	struct uib_uet_track_pool *pool;

	pool = track->pool;
	pthread_mutex_lock(&pool->lock);
	if (!pool->closing && pool->cached < pool->limit) {
		track->next = pool->free;
		pool->free = track;
		pool->cached++;
		pthread_mutex_unlock(&pool->lock);
		return;
	}
	pthread_mutex_unlock(&pool->lock);
	free(track);
	uet_track_pool_put(pool);
}

static struct uib_uet_wr_track *
uet_track_pool_pop(struct uib_uet_track_pool *pool)
{
	struct uib_uet_wr_track *track;

	pthread_mutex_lock(&pool->lock);
	track = pool->free;
	if (track) {
		pool->free = track->next;
		pool->cached--;
	}
	pthread_mutex_unlock(&pool->lock);
	if (!track)
		track = uet_track_pool_new(pool);
	if (!track)
		return NULL;
	memset(track, 0, sizeof(*track));
	track->pool = pool;
	track->refs = 1;
	return track;
}

static struct uib_uet_track_pool *
uet_track_pool_create(uint32_t max_send_wr, uint32_t max_send_sge)
{
	struct uib_uet_track_pool *pool;
	struct uib_uet_wr_track *track;
	uint32_t i;

	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return NULL;
	if (pthread_mutex_init(&pool->lock, NULL)) {
		free(pool);
		return NULL;
	}
	pool->limit = max_send_wr < UIB_UET_TRACK_POOL_MAX ? max_send_wr :
		UIB_UET_TRACK_POOL_MAX;
	if (!pool->limit)
		pool->limit = 64;
	pool->max_sge = max_send_sge ? max_send_sge : 1;
	pool->refs = 1;
	for (i = 0; i < pool->limit; i++) {
		track = uet_track_pool_new(pool);
		if (!track)
			break;
		track->next = pool->free;
		pool->free = track;
		pool->cached++;
	}
	return pool;
}

static void uet_track_pool_destroy(struct uib_uet_track_pool *pool)
{
	struct uib_uet_wr_track *next;
	struct uib_uet_wr_track *track;

	if (!pool)
		return;
	pthread_mutex_lock(&pool->lock);
	pool->closing = 1;
	track = pool->free;
	pool->free = NULL;
	pool->cached = 0;
	while (track) {
		next = track->next;
		free(track);
		uet_track_pool_put(pool);
		track = next;
	}
	pthread_mutex_unlock(&pool->lock);
	uet_track_pool_put(pool);
}

static void uet_debug(struct uib_qp *qp, const char *format, ...)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	va_list args;

	if (!uet || !uet->debug)
		return;
	fprintf(stderr, "uib: uet qpn %u: ", qp->qp_num);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static int uet_psn_before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

/**
 * uib_uet_prologue_pack - Encode an Ultra Ethernet packet prologue.
 * @type: Ultra Ethernet packet type
 * @next: Next-header type
 * @flags: Prologue flags
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The packed prologue value in network byte order.
 */
uint16_t uib_uet_prologue_pack(uint8_t type, uint8_t next, uint8_t flags)
{
	uint16_t value;

	value = ((uint16_t)type & UIB_UET_PROLOGUE_TYPE_MASK) <<
		UIB_UET_PROLOGUE_TYPE_SHIFT;
	value |= ((uint16_t)next & UIB_UET_PROLOGUE_NEXT_MASK) <<
		 UIB_UET_PROLOGUE_NEXT_SHIFT;
	value |= flags & UIB_UET_PROLOGUE_FLAGS_MASK;
	return htons(value);
}

/**
 * uib_uet_prologue_type - Decode the packet type from a UET prologue.
 * @prologue: Ultra Ethernet packet prologue
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded prologue type.
 */
uint8_t uib_uet_prologue_type(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)((ntohs(prologue->type_next_flags) >>
			  UIB_UET_PROLOGUE_TYPE_SHIFT) &
			 UIB_UET_PROLOGUE_TYPE_MASK);
}

/**
 * uib_uet_prologue_next - Decode the next-header type from a UET prologue.
 * @prologue: Ultra Ethernet packet prologue
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded next-header value.
 */
uint8_t uib_uet_prologue_next(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)((ntohs(prologue->type_next_flags) >>
			  UIB_UET_PROLOGUE_NEXT_SHIFT) &
			 UIB_UET_PROLOGUE_NEXT_MASK);
}

/**
 * uib_uet_prologue_flags - Decode flags from a UET prologue.
 * @prologue: Ultra Ethernet packet prologue
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded prologue flags.
 */
uint8_t uib_uet_prologue_flags(const struct uib_uet_prologue *prologue)
{
	return (uint8_t)(ntohs(prologue->type_next_flags) &
			 UIB_UET_PROLOGUE_FLAGS_MASK);
}

/**
 * uib_uet_nscc_wire_pack - Encode NSCC receiver feedback for the wire.
 * @state: Wire or congestion-control state to encode or fill
 * @service_time_ns: Service time in nanoseconds to encode or fill
 * @restore: Restore-window flag to encode or fill
 * @penalty: Congestion-window penalty to encode or fill
 * @received_bytes: Newly received byte count to encode
 * @out_of_order: Out-of-order packet count to encode or fill
 * @uib-api: Ultra Ethernet extensions
 */
void uib_uet_nscc_wire_pack(struct uib_uet_nscc_wire_state *state,
			    uint32_t service_time_ns, uint8_t restore,
			    uint8_t penalty, uint32_t received_bytes,
			    uint16_t out_of_order)
{
	uint32_t received_units = (received_bytes + 255U) >> 8;
	uint32_t receiver_state;
	uint32_t service_units = (service_time_ns + 127U) >> 7;

	if (service_units > UINT16_MAX)
		service_units = UINT16_MAX;
	receiver_state = (restore ? UIB_UET_NSCC_RESTORE_MASK : 0) |
		((uint32_t)(penalty & UIB_UET_NSCC_PENALTY_MASK) <<
		 UIB_UET_NSCC_PENALTY_SHIFT) |
		(received_units & UIB_UET_NSCC_RCVD_MASK);
	state->service_time = htons((uint16_t)service_units);
	state->receiver_state = htonl(receiver_state);
	state->out_of_order_count = htons(out_of_order);
}

/**
 * uib_uet_nscc_wire_unpack - Decode NSCC receiver feedback from the wire.
 * @state: Wire or congestion-control state to encode or fill
 * @service_time_ns: Output for the decoded service time in nanoseconds
 * @restore: Output for the restore-window flag
 * @penalty: Output for the congestion-window penalty
 * @received_units: Output for the decoded receive counter units
 * @out_of_order: Output for the out-of-order packet count
 * @uib-api: Ultra Ethernet extensions
 */
void uib_uet_nscc_wire_unpack(const struct uib_uet_nscc_wire_state *state,
			      uint32_t *service_time_ns, uint8_t *restore,
			      uint8_t *penalty, uint32_t *received_units,
			      uint16_t *out_of_order)
{
	uint32_t receiver_state = ntohl(state->receiver_state);

	if (service_time_ns)
		*service_time_ns = (uint32_t)ntohs(state->service_time) << 7;
	if (restore)
		*restore = !!(receiver_state & UIB_UET_NSCC_RESTORE_MASK);
	if (penalty)
		*penalty = (receiver_state >> UIB_UET_NSCC_PENALTY_SHIFT) &
			UIB_UET_NSCC_PENALTY_MASK;
	if (received_units)
		*received_units = receiver_state & UIB_UET_NSCC_RCVD_MASK;
	if (out_of_order)
		*out_of_order = ntohs(state->out_of_order_count);
}

/**
 * uib_uet_rccc_request_pack - Encode RCCC request state.
 * @ccc_id: Source congestion-control context identifier
 * @credit_target: Cumulative credit target in 256-byte units
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The packed request state in network byte order.
 */
uint32_t uib_uet_rccc_request_pack(uint8_t ccc_id, uint32_t credit_target)
{
	return htonl((uint32_t)ccc_id << 24 |
		     (credit_target & UIB_UET_RCCC_COUNTER_MASK));
}

/**
 * uib_uet_rccc_request_unpack - Decode RCCC request state.
 * @state: Packed request state in network byte order
 * @ccc_id: Output for the source congestion-control context identifier
 * @credit_target: Output for the cumulative 256-byte credit target
 * @uib-api: Ultra Ethernet extensions
 */
void uib_uet_rccc_request_unpack(uint32_t state, uint8_t *ccc_id,
				 uint32_t *credit_target)
{
	uint32_t value = ntohl(state);

	if (ccc_id)
		*ccc_id = value >> 24;
	if (credit_target)
		*credit_target = value & UIB_UET_RCCC_COUNTER_MASK;
}

/**
 * uib_uet_rccc_wire_pack - Encode an RCCC cumulative credit grant.
 * @state: Credit acknowledgement state to fill
 * @cumulative_credit: Cumulative credit in 256-byte units
 * @out_of_order: Out-of-order packet count
 * @uib-api: Ultra Ethernet extensions
 */
void uib_uet_rccc_wire_pack(struct uib_uet_rccc_wire_state *state,
			    uint32_t cumulative_credit,
			    uint16_t out_of_order)
{
	cumulative_credit &= UIB_UET_RCCC_COUNTER_MASK;
	state->credit[0] = cumulative_credit >> 16;
	state->credit[1] = cumulative_credit >> 8;
	state->credit[2] = cumulative_credit;
	memset(state->reserved, 0, sizeof(state->reserved));
	state->out_of_order_count = htons(out_of_order);
}

/**
 * uib_uet_rccc_wire_unpack - Decode an RCCC cumulative credit grant.
 * @state: Credit acknowledgement state to decode
 * @cumulative_credit: Output for cumulative credit in 256-byte units
 * @out_of_order: Output for the out-of-order packet count
 * @uib-api: Ultra Ethernet extensions
 */
void uib_uet_rccc_wire_unpack(const struct uib_uet_rccc_wire_state *state,
			      uint32_t *cumulative_credit,
			      uint16_t *out_of_order)
{
	if (cumulative_credit)
		*cumulative_credit = (uint32_t)state->credit[0] << 16 |
			(uint32_t)state->credit[1] << 8 | state->credit[2];
	if (out_of_order)
		*out_of_order = ntohs(state->out_of_order_count);
}

/**
 * uib_uet_ses_fragment_info - Encode SES fragment length and message offset.
 * @payload_len: Fragment payload length in bytes
 * @message_offset: Offset of the fragment in the complete message
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The packed SES fragment-information field in network byte order.
 */
uint64_t uib_uet_ses_fragment_info(uint32_t payload_len,
				   uint32_t message_offset)
{
	return htobe64(((uint64_t)(payload_len & UIB_UET_SES_LENGTH_MASK) << 32) |
			message_offset);
}

/**
 * uib_uet_ses_payload_len - Decode an SES request's fragment payload length.
 * @request: Ultra Ethernet session request
 * @packet_payload_len: Payload bytes available in the packet
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded fragment payload length in bytes.
 */
uint32_t uib_uet_ses_payload_len(const struct uib_uet_ses_req *request,
				 size_t packet_payload_len)
{
	uint64_t fragment;

	if (request->flags & UIB_UET_SES_FLAG_SOM)
		return (uint32_t)packet_payload_len;
	fragment = be64toh(request->header_data);
	return (uint32_t)(fragment >> 32) & UIB_UET_SES_LENGTH_MASK;
}

/**
 * uib_uet_ses_message_offset - Decode an SES request's message offset.
 * @request: Ultra Ethernet session request
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The decoded message offset in bytes.
 */
uint32_t uib_uet_ses_message_offset(const struct uib_uet_ses_req *request)
{
	if (request->flags & UIB_UET_SES_FLAG_SOM)
		return 0;
	return (uint32_t)be64toh(request->header_data);
}

/**
 * uib_uet_build_ack - Build a UET cumulative or exact ACK packet.
 * @packet: Packet buffer
 * @capacity: Capacity of the output buffer in bytes
 * @cack_psn: Highest cumulatively acknowledged packet sequence number
 * @ack_psn: Packet sequence number acknowledged exactly
 * @spdcid: Source packet delivery context identifier
 * @dpdcid: Destination packet delivery context identifier
 * @next_header: Next-header type to encode
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The encoded ACK length on success, or -EINVAL for invalid input.
 */
int uib_uet_build_ack(void *packet, size_t capacity, uint32_t cack_psn,
		      uint32_t ack_psn, uint16_t spdcid, uint16_t dpdcid,
		      uint8_t next_header)
{
	struct uib_uet_pds_ack *ack = packet;
	int32_t offset = (int32_t)(ack_psn - cack_psn);

	if (!packet || capacity < sizeof(*ack) ||
	    offset < INT16_MIN || offset > INT16_MAX)
		return -EINVAL;

	memset(ack, 0, sizeof(*ack));
	ack->prologue.type_next_flags =
		uib_uet_prologue_pack(UIB_UET_PDS_TYPE_ACK, next_header, 0);
	ack->ack_psn_offset = htons((uint16_t)(int16_t)offset);
	ack->cack_psn = htonl(cack_psn);
	ack->spdcid = htons(spdcid);
	ack->dpdcid = htons(dpdcid);
	return sizeof(*ack);
}

static uint16_t uet_ses_rsp_pack(uint8_t opcode, uint8_t response_code)
{
	uint16_t value;

	value = ((uint16_t)opcode & UIB_UET_SES_RSP_OPCODE_MASK) <<
		UIB_UET_SES_RSP_OPCODE_SHIFT;
	value |= response_code & UIB_UET_SES_RSP_RC_MASK;
	return htons(value);
}

static int uet_packet_qpn(const void *packet, size_t len, uint32_t *qpn)
{
	const struct uib_uet_prologue *prologue = packet;
	uint8_t type;

	if (!packet || !qpn || len < sizeof(*prologue))
		return -EINVAL;
	type = uib_uet_prologue_type(prologue);
	switch (type) {
	case UIB_UET_PDS_TYPE_RUD_REQ:
	case UIB_UET_PDS_TYPE_RUD_CC_REQ: {
		const struct uib_uet_pds_req *req = packet;
		size_t pds_len;
		uint8_t next;

		if (len < sizeof(*req))
			return -EBADMSG;
		pds_len = type == UIB_UET_PDS_TYPE_RUD_CC_REQ ?
			sizeof(struct uib_uet_pds_cc_req) : sizeof(*req);
		if (len < pds_len)
			return -EBADMSG;
		*qpn = ntohs(req->dpdcid);
		if (!*qpn) {
			const struct uib_uet_ses_req_small *ses;
			size_t header_len;

			next = uib_uet_prologue_next(&req->prologue);
			header_len = next == UIB_UET_NEXT_REQ_SMALL ?
				sizeof(struct uib_uet_ses_req_small) :
				next == UIB_UET_NEXT_REQ_MEDIUM ?
				sizeof(struct uib_uet_ses_req_medium) :
				next == UIB_UET_NEXT_REQ_STD ?
				sizeof(struct uib_uet_ses_req) : 0;
			if (!header_len || len < pds_len + header_len)
				return -EBADMSG;
			ses = (const struct uib_uet_ses_req_small *)
				((const uint8_t *)req + pds_len);
			*qpn = ntohs(ses->pid_on_fep) & UIB_UET_SES_PID_MASK;
		}
		return *qpn ? 0 : -ENOENT;
	}
	case UIB_UET_PDS_TYPE_ACK:
	case UIB_UET_PDS_TYPE_ACK_CC: {
		const struct uib_uet_pds_ack *ack = packet;

		if (len < sizeof(*ack))
			return -EBADMSG;
		*qpn = ntohs(ack->dpdcid);
		return *qpn ? 0 : -ENOENT;
	}
	case UIB_UET_PDS_TYPE_NACK: {
		const struct uib_uet_pds_nack *nack = packet;

		if (len < sizeof(*nack))
			return -EBADMSG;
		*qpn = ntohs(nack->dpdcid);
		return *qpn ? 0 : -ENOENT;
	}
	case UIB_UET_PDS_TYPE_CTRL_MSG: {
		const struct uib_uet_pds_control *control = packet;

		if (len < sizeof(*control))
			return -EBADMSG;
		*qpn = ntohs(control->dpdcid);
		return *qpn ? 0 : -ENOENT;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static int uet_congestion_init(struct uib_qp *qp, struct uib_uet_qp *uet)
{
	struct uib_uet_nscc_config nscc_config;
	struct uib_uet_rccc_config rccc_config;
	struct uib_dcqcn_config dcqcn_config;
	const char *cc;
	const void *configuration = NULL;
	uint32_t mtu = qp->path_mtu + 40;
	int rc;

	cc = getenv(UIB_ENV_UET_CC);
	if (!cc)
		cc = "nscc";
	if (!strcmp(cc, "nscc")) {
		const char *max_wnd;

		uib_uet_nscc_config_default(&nscc_config, mtu);
		max_wnd = getenv(UIB_ENV_UET_NSCC_MAX_WND);
		if (max_wnd && strtoull(max_wnd, NULL, 0) >= nscc_config.mtu) {
			nscc_config.max_wnd = (uint32_t)strtoull(max_wnd, NULL, 0);
			nscc_config.bdp = nscc_config.max_wnd;
		}
		configuration = &nscc_config;
	} else if (!strcmp(cc, "dcqcn")) {
		const char *experimental = getenv(UIB_ENV_UET_CC_EXPERIMENTAL);

		if (!experimental || strcmp(experimental, "on"))
			return -EPERM;
		if (uib_dcqcn_config_from_env(&dcqcn_config, mtu))
			return -EINVAL;
		configuration = &dcqcn_config;
	} else if (!strcmp(cc, "rccc")) {
		if (uib_uet_rccc_config_from_env(&rccc_config, mtu))
			return -EINVAL;
		configuration = &rccc_config;
	}
	rc = uib_congestion_init(&uet->congestion, cc, configuration, mtu,
				 uib_now_ns());
	if (rc)
		return rc;
	uet->rccc_enabled = !strcmp(cc, "rccc");
	if (uet->rccc_enabled)
		return uib_uet_rccc_destination_init(&uet->rccc_destination,
						     &rccc_config,
						     uib_now_ns());
	return 0;
}

static void *uet_prep_worker(void *argument)
{
	struct uib_uet_prep_worker *worker;
	struct uib_uet_context *context;
	struct uib_uet_prep_task *task;

	worker = argument;
	context = worker->context;
	for (;;) {
		task = uib_ptr_ring_pop(&worker->ready);
		if (task) {
			uet_process_refill_task(task);
			continue;
		}
		pthread_mutex_lock(&worker->lock);
		while (__atomic_load_n(&context->run, __ATOMIC_ACQUIRE) &&
		       !uib_ptr_ring_count(&worker->ready))
			pthread_cond_wait(&worker->cond, &worker->lock);
		if (!__atomic_load_n(&context->run, __ATOMIC_ACQUIRE) &&
		    !uib_ptr_ring_count(&worker->ready)) {
			pthread_mutex_unlock(&worker->lock);
			break;
		}
		pthread_mutex_unlock(&worker->lock);
	}
	return NULL;
}

static void uet_context_cleanup(struct uib_uet_context *context)
{
	struct uib_uet_prep_worker *worker;
	unsigned int i;

	if (!context)
		return;
	__atomic_store_n(&context->run, 0, __ATOMIC_RELEASE);
	for (i = 0; i < context->worker_count; i++) {
		worker = &context->workers[i];
		if (!worker->initialized)
			continue;
		pthread_mutex_lock(&worker->lock);
		pthread_cond_broadcast(&worker->cond);
		pthread_mutex_unlock(&worker->lock);
	}
	for (i = 0; i < context->worker_count; i++) {
		worker = &context->workers[i];
		if (worker->started)
			pthread_join(worker->thread, NULL);
		if (worker->free_initialized)
			uib_ptr_ring_destroy(&worker->free_tasks);
		if (worker->ready_initialized)
			uib_ptr_ring_destroy(&worker->ready);
		if (worker->initialized) {
			pthread_cond_destroy(&worker->cond);
			pthread_mutex_destroy(&worker->lock);
		}
		free(worker->tasks);
	}
	free(context->workers);
	free(context);
}

static int uet_prep_worker_init(struct uib_uet_context *context,
				struct uib_uet_prep_worker *worker)
{
	unsigned int i;
	int rc;

	worker->context = context;
	rc = pthread_mutex_init(&worker->lock, NULL);
	if (rc)
		return -rc;
	rc = pthread_cond_init(&worker->cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&worker->lock);
		return -rc;
	}
	worker->initialized = 1;
	rc = uib_ptr_ring_init(&worker->ready,
			       UIB_UET_PREP_WORKER_QUEUE_SIZE);
	if (rc < 0)
		return rc;
	worker->ready_initialized = 1;
	rc = uib_ptr_ring_init(&worker->free_tasks,
			       UIB_UET_PREP_WORKER_QUEUE_SIZE);
	if (rc < 0)
		return rc;
	worker->free_initialized = 1;
	worker->tasks = calloc(UIB_UET_PREP_WORKER_QUEUE_SIZE,
			       sizeof(*worker->tasks));
	if (!worker->tasks)
		return -ENOMEM;
	for (i = 0; i < UIB_UET_PREP_WORKER_QUEUE_SIZE; i++) {
		worker->tasks[i].worker = worker;
		uib_ptr_ring_push(&worker->free_tasks, &worker->tasks[i]);
	}
	return 0;
}

static int uet_context_init(struct uib_context *ctx)
{
	struct uib_uet_context *context;
	struct uib_uet_prep_worker *worker;
	pthread_attr_t attr;
	cpu_set_t allowed;
	cpu_set_t worker_set;
	unsigned int count = 0;
	unsigned int i;
	int attr_ready;
	int cpu;
	int rc;

	rc = uib_worker_configuration(&count, &allowed, &attr_ready,
				      UIB_ENV_TX_WORKERS,
				      UIB_ENV_TX_WORKER_AFFINITY, 0);
	if (rc < 0)
		return rc;
	context = calloc(1, sizeof(*context));
	if (!context)
		return -ENOMEM;
	context->worker_count = count;
	context->run = 1;
	context->workers = calloc(count, sizeof(*context->workers));
	if (!context->workers) {
		rc = -ENOMEM;
		goto error;
	}
	for (i = 0; i < count; i++) {
		worker = &context->workers[i];
		rc = uet_prep_worker_init(context, worker);
		if (rc < 0)
			goto error;
		rc = pthread_attr_init(&attr);
		if (rc) {
			rc = -rc;
			goto error;
		}
		rc = pthread_attr_setstacksize(&attr,
					       UIB_UET_PREP_WORKER_STACK_SIZE);
		if (rc) {
			pthread_attr_destroy(&attr);
			rc = -rc;
			goto error;
		}
		if (attr_ready) {
			cpu = uib_worker_cpu(&allowed, i);
			CPU_ZERO(&worker_set);
			CPU_SET(cpu, &worker_set);
			rc = pthread_attr_setaffinity_np(&attr,
						 sizeof(worker_set), &worker_set);
			if (rc) {
				pthread_attr_destroy(&attr);
				rc = -rc;
				goto error;
			}
		}
		rc = pthread_create(&worker->thread, &attr, uet_prep_worker,
				    worker);
		pthread_attr_destroy(&attr);
		if (rc) {
			rc = -rc;
			goto error;
		}
		worker->started = 1;
	}
	ctx->protocol_private = context;
	return 0;

error:
	uet_context_cleanup(context);
	return rc;
}

static void uet_context_fini(struct uib_context *ctx)
{
	struct uib_uet_context *context;

	context = ctx->protocol_private;
	ctx->protocol_private = NULL;
	uet_context_cleanup(context);
}

static int uet_qp_init(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = calloc(1, sizeof(*uet));
	const char *optimized;
	const char *multipath;
	int rc;

	if (!uet)
		return -ENOMEM;
	uet->out_pool = uet_out_pool_create();
	if (!uet->out_pool) {
		free(uet);
		return -ENOMEM;
	}
	uet_out_pool_prewarm_gather(uet->out_pool);
	uet_out_pool_prewarm_mtu(uet->out_pool, qp->path_mtu);
	uet->track_pool = uet_track_pool_create(qp->max_send_wr,
					       qp->max_send_sge);
	if (!uet->track_pool) {
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return -ENOMEM;
	}
	if (uib_psn_index_init(&uet->out_index, UIB_UET_OUT_INDEX_MIN)) {
		uet_track_pool_destroy(uet->track_pool);
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return -ENOMEM;
	}
	rc = pthread_mutex_init(&uet->rx_lock, NULL);
	if (rc) {
		uib_psn_index_destroy(&uet->out_index);
		uet_track_pool_destroy(uet->track_pool);
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return -rc;
	}
	rc = pthread_cond_init(&uet->prep_cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&uet->rx_lock);
		uib_psn_index_destroy(&uet->out_index);
		uet_track_pool_destroy(uet->track_pool);
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return -rc;
	}
	uet->spdcid = (uint16_t)qp->qp_num;
	uet->next_msg_id = 1;
	uet->session.pid_on_fep = (uint16_t)qp->qp_num;
	uet->optimized_mode = UIB_UET_OPTIMIZED_AUTO;
	optimized = getenv(UIB_ENV_UET_OPTIMIZED);
	if (optimized && !strcmp(optimized, "off"))
		uet->optimized_mode = UIB_UET_OPTIMIZED_OFF;
	else if (optimized && !strcmp(optimized, "small"))
		uet->optimized_mode = UIB_UET_OPTIMIZED_SMALL;
	else if (optimized && !strcmp(optimized, "medium"))
		uet->optimized_mode = UIB_UET_OPTIMIZED_MEDIUM;
	uet->multipath.entropy_base = UIB_UET_DEFAULT_ENTROPY_BASE;
	uet->multipath.path_count = UIB_UET_MAX_PATHS;
	uet->multipath.ecn_hold_ns = qp->ctx->retry_timeout_ns;
	uet->multipath.failure_hold_ns = UIB_UET_DEFAULT_FAILURE_HOLD_NS;
	multipath = getenv(UIB_ENV_UET_MULTIPATH);
	if (multipath && !strcmp(multipath, "oblivious"))
		uet->multipath.mode = UIB_UET_MULTIPATH_OBLIVIOUS;
	else if (multipath && !strcmp(multipath, "path-aware"))
		uet->multipath.mode = UIB_UET_MULTIPATH_PATH_AWARE;
	uet->path_cursor = (uint16_t)(qp->qp_num % UIB_UET_MAX_PATHS);
	uet->previous_path = UINT16_MAX;
	uet->debug = !!getenv(UIB_ENV_UET_DEBUG);
	rc = uet_congestion_init(qp, uet);
	if (rc) {
		pthread_cond_destroy(&uet->prep_cond);
		pthread_mutex_destroy(&uet->rx_lock);
		uib_psn_index_destroy(&uet->out_index);
		uet_track_pool_destroy(uet->track_pool);
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return rc;
	}
	qp->protocol_private = uet;
	return 0;
}

static void uet_qp_modified(struct uib_qp *qp, int attr_mask)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if ((attr_mask & UIB_QP_PATH_MTU) && uet) {
		struct uib_congestion previous = uet->congestion;

		uet_out_pool_prewarm_mtu(uet->out_pool, qp->path_mtu);
		if (uet_congestion_init(qp, uet))
			uet->congestion = previous;
	}
}

/**
 * uib_uet_session_configure - Configure a QP's UET session parameters.
 * @qp: Queue pair
 * @config: Configuration to apply or fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_session_configure(struct uib_qp *qp,
			      const struct uib_uet_session_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops ||
	    config->job_id > UIB_UET_SES_JOB_ID_MASK ||
	    config->pid_on_fep > UIB_UET_SES_PID_MASK ||
	    config->resource_index > UIB_UET_SES_PID_MASK)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	uet->session = *config;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_session_query - Read a QP's UET session parameters.
 * @qp: Queue pair
 * @config: Session configuration structure to fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_session_query(struct uib_qp *qp,
			  struct uib_uet_session_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	*config = uet->session;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_optimized_configure - Select a QP's optimized-header policy.
 * @qp: Queue pair
 * @mode: Mode to apply or fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_optimized_configure(struct uib_qp *qp,
				enum uib_uet_optimized_mode mode)
{
	struct uib_uet_qp *uet;

	if (!qp || qp->ctx->pops != &uib_uet_protocol_ops ||
	    mode > UIB_UET_OPTIMIZED_MEDIUM)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (uet->out_count) {
		pthread_mutex_unlock(&qp->lock);
		return -EBUSY;
	}
	uet->optimized_mode = mode;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_optimized_query - Read optimized-header counters and policy.
 * @qp: Queue pair
 * @mode: Output for the optimized-header policy
 * @stats: Statistics structure to fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_optimized_query(struct uib_qp *qp,
			    enum uib_uet_optimized_mode *mode,
			    struct uib_uet_optimized_stats *stats)
{
	struct uib_uet_qp *uet;

	if (!qp || (!mode && !stats) ||
	    qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (mode)
		*mode = uet->optimized_mode;
	if (stats)
		*stats = uet->optimized_stats;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_multipath_configure - Configure a QP's entropy paths and policy.
 * @qp: Queue pair
 * @config: Configuration to apply or fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_multipath_configure(struct uib_qp *qp,
				const struct uib_uet_multipath_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops ||
	    config->mode > UIB_UET_MULTIPATH_PATH_AWARE ||
	    !config->path_count || config->path_count > UIB_UET_MAX_PATHS ||
	    (uint32_t)config->entropy_base + config->path_count > UINT16_MAX)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (uet->out_count) {
		pthread_mutex_unlock(&qp->lock);
		return -EBUSY;
	}
	uet->multipath = *config;
	memset(&uet->multipath_stats, 0, sizeof(uet->multipath_stats));
	memset(uet->paths, 0, sizeof(uet->paths));
	uet->path_cursor %= config->path_count;
	uet->previous_path = UINT16_MAX;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_multipath_query - Read a QP's multipath state and counters.
 * @qp: Queue pair
 * @config: Multipath configuration structure to fill
 * @stats: Statistics structure to fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_multipath_query(struct uib_qp *qp,
			    struct uib_uet_multipath_config *config,
			    struct uib_uet_multipath_stats *stats)
{
	struct uib_uet_qp *uet;

	if (!qp || (!config && !stats) ||
	    qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (config)
		*config = uet->multipath;
	if (stats)
		*stats = uet->multipath_stats;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_atomic_datatype_size - Return a UET atomic datatype's byte size.
 * @datatype: Ultra Ethernet atomic datatype code
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The datatype width in bytes, or 0 if @datatype is unsupported.
 */
size_t uib_uet_atomic_datatype_size(uint8_t datatype)
{
	switch (datatype) {
	case UIB_UET_AMO_INT8:
	case UIB_UET_AMO_UINT8:
		return 1;
	case UIB_UET_AMO_INT16:
	case UIB_UET_AMO_UINT16:
		return 2;
	case UIB_UET_AMO_INT32:
	case UIB_UET_AMO_UINT32:
		return 4;
	case UIB_UET_AMO_INT64:
	case UIB_UET_AMO_UINT64:
		return 8;
	default:
		return 0;
	}
}

static int uet_atomic_has_two_operands(uint8_t opcode)
{
	return opcode >= UIB_UET_AMO_CSWAP && opcode <= UIB_UET_AMO_MSWAP;
}

/**
 * uib_uet_post_atomic - Post an extended Ultra Ethernet atomic operation.
 * @qp: Queue pair
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_post_atomic(struct uib_qp *qp, struct uib_uet_atomic_wr *wr,
			struct uib_uet_atomic_wr **bad)
{
	struct uib_uet_atomic_wr *current;

	for (current = wr; current; current = current->next) {
		struct uib_send_wr send = {0};
		struct uib_send_wr *send_bad;
		struct uib_sge result_sge;
		size_t width;
		int rc;

		width = uib_uet_atomic_datatype_size(current->datatype);
		if (!qp || qp->ctx->pops != &uib_uet_protocol_ops || !width ||
		    current->opcode >= UIB_UET_AMO_INVAL ||
		    (current->opcode != UIB_UET_AMO_READ &&
		     !current->operand &&
		     !uet_atomic_has_two_operands(current->opcode)) ||
		    (uet_atomic_has_two_operands(current->opcode) &&
		     (!current->compare || !current->swap)) ||
		    (current->fetching && !current->result)) {
			if (bad)
				*bad = current;
			return -EINVAL;
		}
		send.wr_id = current->wr_id;
		send.opcode = UIB_WR_UET_ATOMIC;
		send.send_flags = current->send_flags;
		send.atomic.remote_addr = current->remote_addr;
		send.atomic.rkey = current->rkey;
		send.atomic.uet_opcode = current->opcode;
		send.atomic.uet_datatype = current->datatype;
		send.atomic.uet_semantic_control = current->semantic_control;
		send.atomic.uet_fetching = !!current->fetching;
		if (current->operand)
			memcpy(send.atomic.uet_operand, current->operand, width);
		if (current->compare)
			memcpy(send.atomic.uet_compare, current->compare, width);
		if (current->swap)
			memcpy(send.atomic.uet_swap, current->swap, width);
		if (current->fetching) {
			result_sge.addr = (uintptr_t)current->result;
			result_sge.length = (uint32_t)width;
			result_sge.lkey = current->result_lkey;
			send.sg_list = &result_sge;
			send.num_sge = 1;
		}
		rc = uib_post_send(qp, &send, &send_bad);
		if (rc < 0) {
			if (bad)
				*bad = current;
			return rc;
		}
	}
	return 0;
}

/**
 * uib_uet_nscc_configure_qp - Configure NSCC for one UET QP.
 * @qp: Queue pair
 * @config: Configuration to apply or fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_nscc_configure_qp(struct uib_qp *qp,
			      const struct uib_uet_nscc_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops ||
	    !config->mtu || config->max_wnd < config->mtu)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (uet->out_count) {
		pthread_mutex_unlock(&qp->lock);
		return -EBUSY;
	}
	if (uib_congestion_init(&uet->congestion, "nscc", config,
				qp->path_mtu + 40, uib_now_ns())) {
		pthread_mutex_unlock(&qp->lock);
		return -EINVAL;
	}
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_nscc_query_qp - Read NSCC state from one UET QP.
 * @qp: Queue pair
 * @state: NSCC state structure to fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_nscc_query_qp(struct uib_qp *qp, struct uib_uet_nscc *state)
{
	struct uib_uet_qp *uet;

	if (!qp || !state || qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (strcmp(uib_congestion_name(&uet->congestion), "nscc")) {
		pthread_mutex_unlock(&qp->lock);
		return -EOPNOTSUPP;
	}
	*state = *(struct uib_uet_nscc *)uet->congestion.state.bytes;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static void uet_track_get(struct uib_uet_wr_track *track)
{
	__atomic_add_fetch(&track->refs, 1, __ATOMIC_RELAXED);
}

static void uet_free_track(struct uib_uet_wr_track *track)
{
	if (!track)
		return;
	if (__atomic_sub_fetch(&track->refs, 1, __ATOMIC_ACQ_REL))
		return;
	uib_mr_put(track->mr);
	if (track->owned_message_buffer)
		uet_free_outstanding(track->owned_message_buffer);
	else
		free(track->owned_message);
	if (track->read_sgl != track->inline_read_sgl)
		free(track->read_sgl);
	if (track->read_fragments != track->inline_read_fragments)
		free(track->read_fragments);
	uet_track_pool_push(track);
}

static void uet_free_outstanding(void *opaque)
{
	struct uib_uet_outstanding *out = opaque;
	struct uib_mr *mr;

	if (!out)
		return;
	if (__atomic_sub_fetch(&out->refs, 1, __ATOMIC_ACQ_REL))
		return;
	mr = out->payload_mr;
	out->payload_mr = NULL;
	uib_mr_put(mr);
	uet_out_pool_push(out);
}

static void uet_outstanding_index_add(struct uib_uet_qp *uet,
				      struct uib_uet_outstanding *out)
{
	if (uib_psn_index_insert(&uet->out_index, out->psn, out)) {
		uet->unindexed_count++;
		return;
	}
	out->indexed = 1;
}

static void uet_outstanding_index_remove(struct uib_uet_qp *uet,
					 struct uib_uet_outstanding *out)
{
	if (!out->indexed) {
		if (uet->unindexed_count)
			uet->unindexed_count--;
		return;
	}
	uib_psn_index_remove(&uet->out_index, out->psn, out);
	out->indexed = 0;
}

static struct uib_uet_outstanding *
uet_first_unsent(struct uib_uet_qp *uet)
{
	struct uib_uet_outstanding *out;

	for (out = uet->out_head; out; out = out->next) {
		if (!out->sent && !out->sack_acked)
			return out;
	}
	return NULL;
}

static int uet_out_requires_response(const struct uib_uet_outstanding *out)
{
	return out->track->opcode == UIB_WR_RDMA_READ ||
		out->track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		out->track->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
		(out->track->opcode == UIB_WR_UET_ATOMIC &&
		 out->track->read_num_sge);
}

static void uet_link_outstanding(struct uib_uet_qp *uet,
				 struct uib_uet_outstanding *out)
{
	out->next = NULL;
	out->prev = uet->out_tail;
	if (uet->out_tail)
		uet->out_tail->next = out;
	else
		uet->out_head = out;
	uet->out_tail = out;
	if (!uet_out_requires_response(out)) {
		out->ack_next = NULL;
		out->ack_prev = uet->ack_tail;
		if (uet->ack_tail)
			uet->ack_tail->ack_next = out;
		else
			uet->ack_head = out;
		uet->ack_tail = out;
	}
	if (!uet->tx_cursor)
		uet->tx_cursor = out;
	uet_outstanding_index_add(uet, out);
	uet->out_count++;
}

static void uet_link_track(struct uib_uet_qp *uet,
			   struct uib_uet_wr_track *track)
{
	track->next = NULL;
	track->prev = uet->track_tail;
	if (uet->track_tail)
		uet->track_tail->next = track;
	else
		uet->track_head = track;
	uet->track_tail = track;
}

static void uet_unlink_track(struct uib_uet_qp *uet,
			     struct uib_uet_wr_track *track)
{
	if (track->prev)
		track->prev->next = track->next;
	else
		uet->track_head = track->next;
	if (track->next)
		track->next->prev = track->prev;
	else
		uet->track_tail = track->prev;
	track->next = NULL;
	track->prev = NULL;
}

static void uet_unlink_outstanding(struct uib_uet_qp *uet,
				   struct uib_uet_outstanding *out)
{
	struct uib_uet_outstanding *cursor;

	if (out->prev)
		out->prev->next = out->next;
	else
		uet->out_head = out->next;
	if (out->next)
		out->next->prev = out->prev;
	else
		uet->out_tail = out->prev;
	if (uet->tx_cursor == out) {
		cursor = out->next;
		while (cursor && (cursor->sent || cursor->sack_acked))
			cursor = cursor->next;
		uet->tx_cursor = cursor;
	}
	if (out->ack_prev)
		out->ack_prev->ack_next = out->ack_next;
	else if (uet->ack_head == out)
		uet->ack_head = out->ack_next;
	if (out->ack_next)
		out->ack_next->ack_prev = out->ack_prev;
	else if (uet->ack_tail == out)
		uet->ack_tail = out->ack_prev;
	uet_outstanding_index_remove(uet, out);
	if (out->sent)
		uet->sent_count--;
	uet->out_count--;
	uet->materialized_count--;
	out->next = NULL;
	out->prev = NULL;
	out->ack_next = NULL;
	out->ack_prev = NULL;
}

static void uet_qp_fini(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *next_track;
	struct uib_uet_wr_track *track;
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;
	uint32_t i;

	if (!uet)
		return;
	pthread_mutex_lock(&qp->lock);
	uet->prep_closing = 1;
	while (uet->prep_pending)
		pthread_cond_wait(&uet->prep_cond, &qp->lock);
	pthread_mutex_unlock(&qp->lock);
	for (out = uet->out_head; out; out = next) {
		next = out->next;
		uet_unlink_outstanding(uet, out);
		uet_free_outstanding(out);
	}
	for (track = uet->track_head; track; track = next_track) {
		next_track = track->next;
		uet_free_track(track);
	}
	for (i = 0; i < UIB_UET_PDC_MPR; i++)
		uet_free_outstanding(uet->deferred[i].packet);
	if (uet->recv_active)
		uib_qp_release_recv(&uet->recv_slot);
	pthread_cond_destroy(&uet->prep_cond);
	pthread_mutex_destroy(&uet->rx_lock);
	uib_psn_index_destroy(&uet->out_index);
	uet_track_pool_destroy(uet->track_pool);
	uet_out_pool_destroy(uet->out_pool);
	free(uet);
	qp->protocol_private = NULL;
}

static void uet_track_done(struct uib_qp *qp, struct uib_uet_wr_track *track,
			   enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if (!track->pending)
		return;
	if (status != UIB_WC_SUCCESS)
		track->pending = 1;
	track->pending--;
	if (track->pending)
		return;

	qp_emit_send_vendor(qp, track->wr_id, track->opcode,
			    track->signaled, status,
			    track->opcode == UIB_WR_RDMA_READ ||
			    track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
			    track->opcode == UIB_WR_ATOMIC_CMP_SWAP ?
			    track->total_len : 0, track->response_code);
	uet_unlink_track(uet, track);
	uet_free_track(track);
}

static void uet_remove_track_locked(struct uib_qp *qp,
				    struct uib_uet_wr_track *track,
				    enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;

	__atomic_store_n(&track->cancelled, 1, __ATOMIC_RELEASE);
	for (out = uet->out_head; out; out = next) {
		next = out->next;
		if (out->track != track)
			continue;
		uet_unlink_outstanding(uet, out);
		uet_free_outstanding(out);
	}
	track->pending = 1;
	uet_track_done(qp, track, status);
	if (!uet->sent_count)
		uib_qp_retry_cancel(qp);
}

static struct uib_uet_outstanding *
uet_find_outstanding(struct uib_uet_qp *uet, uint32_t psn, uint16_t msg_id)
{
	struct uib_uet_outstanding *out;

	out = uib_psn_index_lookup(&uet->out_index, psn);
	if (out && (!msg_id || out->msg_id == msg_id))
		return out;
	if (!uet->unindexed_count)
		return NULL;
	for (out = uet->out_head; out; out = out->next) {
		if (!out->indexed && out->psn == psn &&
		    (!msg_id || out->msg_id == msg_id))
			return out;
	}
	return NULL;
}

static void uet_select_path(struct uib_uet_qp *uet,
			    struct uib_uet_outstanding *out, uint64_t now_ns)
{
	uint16_t count = uet->multipath.path_count;
	uint16_t selected = uet->path_cursor;
	uint16_t candidate;
	uint16_t i;

	if (uet->multipath.mode == UIB_UET_MULTIPATH_OFF)
		return;
	for (i = 0; i < count; i++) {
		candidate = (uint16_t)((uet->path_cursor + i) % count);
		if (uet->multipath.mode != UIB_UET_MULTIPATH_PATH_AWARE ||
		    uet->paths[candidate].blocked_until_ns <= now_ns) {
			selected = candidate;
			if (i)
				uet->multipath_stats.ecn_avoids++;
			break;
		}
	}
	uet->path_cursor = (uint16_t)((selected + 1) % count);
	out->path_index = selected;
	out->entropy = uet->multipath.entropy_base + selected;
	if (uet->previous_path != UINT16_MAX &&
	    uet->previous_path != selected)
		uet->multipath_stats.path_switches++;
	uet->previous_path = selected;
	uet->multipath_stats.active_entropy = out->entropy;
	if (!uet->paths[selected].used) {
		uet->paths[selected].used = 1;
		uet->multipath_stats.paths_used++;
	}
}

static uint32_t uet_rccc_credit_pack(uint32_t credit)
{
	return htonl((credit & UIB_UET_RCCC_COUNTER_MASK) << 8);
}

static uint32_t uet_rccc_credit_unpack(uint32_t payload)
{
	return ntohl(payload) >> 8;
}

static int uet_send_control(struct uib_qp *qp,
			    const struct sockaddr_in *destination,
			    uint8_t control_type, uint32_t payload)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_pds_control control = {0};
	struct sockaddr_in dst = *destination;

	control.prologue.type_next_flags = uib_uet_prologue_pack(
		UIB_UET_PDS_TYPE_CTRL_MSG, control_type, 0);
	control.spdcid = htons(uet->spdcid);
	control.dpdcid = htons((uint16_t)qp->dest_qp_num);
	control.payload = payload;
	if (!dst.sin_port)
		dst.sin_port = htons(UIB_UET_DEFAULT_PORT);
	return uib_transport_send_entropy(qp->ctx, &dst, &control,
					  sizeof(control), 0);
}

static int uet_send_credit_request_locked(struct uib_qp *qp,
					  uint64_t now_ns)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_rccc *rccc;
	uint32_t payload;
	uint32_t target;
	int rc;

	if (!uet->rccc_enabled)
		return 0;
	rccc = (struct uib_uet_rccc *)uet->congestion.state.bytes;
	if (rccc->credit_target_sent &&
	    now_ns < uet->credit_request_deadline_ns)
		return 0;
	target = uib_uet_rccc_credit_target(rccc);
	payload = uib_uet_rccc_request_pack((uint8_t)uet->spdcid, target);
	rc = uet_send_control(qp, &qp->remote_addr,
			      UIB_UET_CONTROL_CREDIT_REQUEST, payload);
	if (rc < 0)
		return rc;
	uib_uet_rccc_mark_target_sent(rccc);
	uet->credit_request_deadline_ns = now_ns +
					 qp->ctx->retry_timeout_ns;
	uib_qp_retry_schedule(qp, uet->credit_request_deadline_ns);
	return 0;
}

static uint64_t uet_rto_interval(const struct uib_qp *qp,
				 const struct uib_uet_outstanding *out)
{
	uint64_t interval;

	interval = qp->ctx->retry_timeout_ns;
	if (out->rto_retries >= 63 || interval > (UINT64_MAX >>
						      out->rto_retries))
		return UINT64_MAX;
	return interval << out->rto_retries;
}

static int uet_drain_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *outstanding[UIB_UET_PDC_MPR];
	struct uib_uet_outstanding *candidate;
	struct uib_uet_outstanding *out;
	struct uib_transport_iov_ref refs[UIB_UET_PDC_MPR];
	struct iovec vectors[UIB_UET_PDC_MPR][2];
	struct uib_congestion congestion_before;
	uint64_t deadline;
	uint64_t now_ns;
	uint64_t rto;
	uint32_t capacity;
	uint32_t count;
	uint32_t i;
	int rc;

	capacity = UIB_UET_PDC_MPR - uet->sent_count;
	candidate = uet->tx_cursor;
	count = 0;
	congestion_before = uet->congestion;
	now_ns = uib_now_ns();
	while (candidate && count < capacity) {
		if (!uib_congestion_can_send(&uet->congestion,
					     candidate->nominal_len))
			break;
		candidate->tx_time_ns = now_ns;
		if (!candidate->entropy)
			uet_select_path(uet, candidate, now_ns);
		vectors[count][0].iov_base = candidate->packet;
		vectors[count][0].iov_len = candidate->header_len;
		refs[count].dst = &candidate->dst;
		refs[count].iov = vectors[count];
		refs[count].release = uet_free_outstanding;
		refs[count].opaque = candidate;
		refs[count].len = candidate->packet_len;
		refs[count].iov_count = 1;
		refs[count].flow = qp->qp_num + candidate->msg_id;
		refs[count].entropy = candidate->entropy;
		if (candidate->payload_len) {
			vectors[count][1].iov_base = (void *)candidate->payload;
			vectors[count][1].iov_len = candidate->payload_len;
			refs[count].iov_count++;
		}
		outstanding[count] = candidate;
		__atomic_add_fetch(&candidate->refs, 1, __ATOMIC_RELAXED);
		uib_congestion_on_send(&uet->congestion,
				       candidate->nominal_len);
		count++;
		candidate = candidate->next;
		while (candidate && (candidate->sent || candidate->sack_acked))
			candidate = candidate->next;
	}
	if (!count) {
		if (candidate && uet->rccc_enabled) {
			rc = uet_send_credit_request_locked(qp, now_ns);
			if (rc < 0)
				return rc;
		}
		deadline = uib_congestion_next_send_ns(&uet->congestion);
		if (candidate && deadline) {
			if (deadline <= now_ns)
				deadline = now_ns + 1;
			uib_qp_retry_schedule(qp, deadline);
		}
		return 0;
	}
	rc = uib_transport_send_iov_ref_batch(qp->ctx, refs, count);
	if (rc < 0) {
		uet->congestion = congestion_before;
		return rc;
	}
	deadline = UINT64_MAX;
	for (i = 0; i < count; i++) {
		out = outstanding[i];
		out->sent = 1;
		uet->sent_count++;
		rto = uet_rto_interval(qp, out);
		out->deadline_ns = rto > UINT64_MAX - out->tx_time_ns ?
			UINT64_MAX : out->tx_time_ns + rto;
		if (out->deadline_ns < deadline)
			deadline = out->deadline_ns;
		if (out->entropy) {
			uet->paths[out->path_index].sent++;
			uet->multipath_stats.packets++;
		}
	}
	uet->tx_cursor = candidate;
	if (candidate) {
		uint64_t next = uib_congestion_next_send_ns(&uet->congestion);

		if (next && next < deadline)
			deadline = next;
	}
	if (!qp->retry_scheduled || deadline < qp->retry_deadline_ns)
		uib_qp_retry_schedule(qp, deadline);
	return 0;
}

static uint8_t uet_response_next_header(uint32_t data_len,
					uint8_t request_header)
{
	if (!data_len)
		return UIB_UET_NEXT_RSP;
	return request_header == UIB_UET_NEXT_REQ_STD ?
		UIB_UET_NEXT_RSP_DATA : UIB_UET_NEXT_RSP_DATA_SMALL;
}

static size_t uet_response_header_len(uint8_t next)
{
	if (next == UIB_UET_NEXT_RSP_DATA_SMALL)
		return sizeof(struct uib_uet_ses_rsp_data_small);
	if (next == UIB_UET_NEXT_RSP_DATA)
		return sizeof(struct uib_uet_ses_rsp_data);
	return sizeof(struct uib_uet_ses_rsp);
}

struct uet_response_encoding {
	uint8_t opcode;
	uint8_t response_code;
	uint8_t guaranteed_delivery;
};

static struct uet_response_encoding
uet_response_construct(uint8_t response_code, uint32_t data_len,
			uint32_t modified_len, uint32_t requested_len,
			int guarantee_error)
{
	struct uet_response_encoding response;

	response.response_code = response_code;
	if (response_code != UIB_UET_SES_RC_OK)
		response.guaranteed_delivery = guarantee_error;
	else
		response.guaranteed_delivery = data_len ||
			modified_len != requested_len;
	if (data_len)
		response.opcode = UIB_UET_SES_RSP_WITH_DATA;
	else if (response.guaranteed_delivery)
		response.opcode = UIB_UET_SES_RSP_RESPONSE;
	else
		response.opcode = UIB_UET_SES_RSP_DEFAULT;
	return response;
}

static void uet_fill_response_cc(struct uib_qp *qp,
				 struct uib_uet_pds_ack_ext *extension,
				 uint64_t receive_time_ns)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_rccc_wire_state *credit_state;
	struct uib_uet_nscc_wire_state *cc_state;

	if (!uet->rccc_enabled) {
		extension->cc_type_flags_mpr = htons(
			(UIB_UET_CC_TYPE_NSCC << UIB_UET_CC_TYPE_SHIFT) |
			UIB_UET_MPR_ENCODING);
		cc_state = (struct uib_uet_nscc_wire_state *)
			&extension->ack_cc_state;
		uib_uet_nscc_wire_pack(
			cc_state, (uint32_t)(uib_now_ns() - receive_time_ns),
			0, 0, uet->receiver_bytes, UINT16_MAX);
		return;
	}
	extension->cc_type_flags_mpr = htons(
		(UIB_UET_CC_TYPE_CREDIT << UIB_UET_CC_TYPE_SHIFT) |
		UIB_UET_MPR_ENCODING);
	credit_state = (struct uib_uet_rccc_wire_state *)
		&extension->ack_cc_state;
	pthread_mutex_lock(&qp->lock);
	uib_uet_rccc_wire_pack(credit_state,
			       uet->rccc_destination.cumulative_credit,
			       UINT16_MAX);
	pthread_mutex_unlock(&qp->lock);
}

static int uet_rx_psn_received(const struct uib_uet_qp *uet, uint32_t psn)
{
	uint32_t offset;
	uint64_t mask;
	unsigned int word;

	if (uet_psn_before(psn, uet->rx_base_psn))
		return 1;
	offset = psn - uet->rx_base_psn;
	if (offset >= UIB_UET_PDC_MPR)
		return 0;
	word = offset / 64;
	mask = 1ULL << (offset % 64);
	return !!(uet->rx_bitmap[word] & mask);
}

static void uet_fill_response_sack(struct uib_qp *qp,
				   struct uib_uet_pds_ack_ext *extension,
				   uint32_t cack_psn)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint64_t bitmap;
	uint32_t sack_psn;
	uint32_t psn;
	uint32_t i;

	sack_psn = (cack_psn + 1) & ~7U;
	bitmap = 0;
	pthread_mutex_lock(&qp->lock);
	for (i = 0; i < 64; i++) {
		psn = sack_psn + i;
		if (!uet_psn_before(cack_psn, psn) ||
		    uet_rx_psn_received(uet, psn))
			bitmap |= 1ULL << i;
	}
	pthread_mutex_unlock(&qp->lock);
	extension->sack_psn_offset = htons((uint16_t)(int16_t)
					      (sack_psn - cack_psn));
	extension->sack_bitmap = htobe64(bitmap);
}

static void uet_fill_small_response(struct uib_qp *qp, void *wire,
				    const struct uib_uet_pds_req *request,
				    const struct uib_uet_ses_req *ses_request,
				    uint8_t response_code, const void *data,
				    uint32_t data_len)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_ses_rsp_data_small *response = wire;
	uint32_t identity;

	identity = ntohl(ses_request->index_gen_job_id);
	response->list_opcode_version_rc = uet_ses_rsp_pack(
		UIB_UET_SES_RSP_WITH_DATA, response_code);
	response->payload_len = htons(data_len & UIB_UET_SES_LENGTH_MASK);
	response->index_gen_job_id = htonl(identity);
	response->request_psn = request->psn;
	memcpy(response + 1, data, data_len);
	pthread_mutex_lock(&qp->lock);
	uet->optimized_stats.small_responses++;
	pthread_mutex_unlock(&qp->lock);
}

static void uet_fill_data_response(struct uib_qp *qp, void *wire,
				   const struct uib_uet_ses_req *request,
				   uint8_t response_code, const void *data,
				   uint32_t data_len, uint32_t modified_len,
				   uint32_t message_offset)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_ses_rsp_data *response = wire;
	uint32_t identity;

	identity = ntohl(request->index_gen_job_id);
	response->list_opcode_version_rc = uet_ses_rsp_pack(
		UIB_UET_SES_RSP_WITH_DATA, response_code);
	pthread_mutex_lock(&qp->lock);
	response->response_msg_id = htons(uet->next_msg_id++);
	pthread_mutex_unlock(&qp->lock);
	response->index_gen_job_id = htonl(identity);
	response->request_msg_id = request->msg_id;
	response->payload_len = htons(data_len & UIB_UET_SES_LENGTH_MASK);
	response->modified_len = htonl(modified_len);
	response->message_offset = htonl(message_offset);
	memcpy(response + 1, data, data_len);
}

static void uet_fill_plain_response(struct uib_qp *qp, void *wire,
				    const struct uib_uet_ses_req *request,
				    const struct uet_response_encoding *encoding,
				    uint32_t modified_len)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_ses_rsp *response = wire;
	uint32_t identity;

	response->list_opcode_version_rc = uet_ses_rsp_pack(
		encoding->opcode, encoding->response_code);
	response->msg_id = request->msg_id;
	if (encoding->response_code == UIB_UET_SES_RC_BAD_GENERATION) {
		identity = ntohl(request->index_gen_job_id);
		identity &= UIB_UET_SES_JOB_ID_MASK;
		identity |= (uint32_t)uet->session.resource_generation << 24;
		response->index_gen_job_id = htonl(identity);
	} else {
		response->index_gen_job_id = request->index_gen_job_id;
	}
	response->modified_len = htonl(modified_len);
}

static int uet_send_one_response(struct uib_qp *qp,
				 const struct sockaddr_in *destination,
				 const struct uib_uet_pds_req *request,
				 const struct uib_uet_ses_req *ses_request,
				 uint32_t cack_psn,
				 const struct uet_response_encoding *encoding,
				 const void *data, uint32_t data_len,
				 uint32_t modified_len, uint32_t message_offset,
				 uint64_t receive_time_ns, int ecn_marked,
				 uint8_t request_header)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_pds_ack_ext *extension;
	struct uib_uet_pds_ack *ack;
	struct sockaddr_in dst = *destination;
	uint8_t packet[UIB_RECV_BUF_LEN];
	size_t response_len;
	size_t packet_len;
	uint8_t next;
	void *wire;
	int rc;

	next = uet_response_next_header(data_len, request_header);
	response_len = uet_response_header_len(next);
	packet_len = sizeof(*ack) + sizeof(*extension) + response_len + data_len;
	if (packet_len > sizeof(packet))
		return -EMSGSIZE;
	memset(packet, 0, sizeof(*ack) + sizeof(*extension) + response_len);
	rc = uib_uet_build_ack(packet, packet_len, cack_psn,
			       ntohl(request->psn), uet->spdcid,
			       ntohs(request->spdcid), next);
	if (rc < 0)
		return rc;
	ack = (struct uib_uet_pds_ack *)packet;
	ack->prologue.type_next_flags = uib_uet_prologue_pack(
		UIB_UET_PDS_TYPE_ACK_CC, next,
		(ecn_marked ? UIB_UET_ACK_FLAG_M : 0) |
		(encoding->guaranteed_delivery ?
		 UIB_UET_ACK_FLAG_REQ_CLEAR : 0));
	extension = (struct uib_uet_pds_ack_ext *)(packet + rc);
	uet_fill_response_cc(qp, extension, receive_time_ns);
	uet_fill_response_sack(qp, extension, cack_psn);
	wire = extension + 1;
	if (next == UIB_UET_NEXT_RSP_DATA_SMALL)
		uet_fill_small_response(qp, wire, request, ses_request,
					encoding->response_code, data, data_len);
	else if (next == UIB_UET_NEXT_RSP_DATA)
		uet_fill_data_response(qp, wire, ses_request,
				       encoding->response_code,
				       data, data_len, modified_len,
				       message_offset);
	else
		uet_fill_plain_response(qp, wire, ses_request, encoding,
					modified_len);
	dst.sin_port = qp->remote_addr.sin_port;
	if (!dst.sin_port)
		dst.sin_port = htons(UIB_UET_DEFAULT_PORT);
	return uib_transport_send_entropy(qp->ctx, &dst, packet, packet_len,
					  ntohs(destination->sin_port));
}

static int uet_send_response(struct uib_qp *qp,
			     const struct sockaddr_in *destination,
			     const struct uib_uet_pds_req *request,
			     const struct uib_uet_ses_req *ses_request,
			     uint32_t cack_psn,
			     const struct uet_response_encoding *encoding,
			     const void *data, size_t data_len,
				     uint32_t modified_len,
				     uint64_t receive_time_ns, int ecn_marked,
				     uint8_t request_header)
{
	uint32_t response_mtu;
	uint32_t offset = 0;
	int rc;

	if (!data_len)
		return uet_send_one_response(qp, destination, request,
					     ses_request, cack_psn,
					     encoding, NULL, 0,
					     modified_len, 0,
					     receive_time_ns, ecn_marked,
					     request_header);
	if (data_len > UINT32_MAX)
		return -EMSGSIZE;
	if (request_header != UIB_UET_NEXT_REQ_STD) {
		if (data_len > UIB_UET_SES_LENGTH_MASK ||
		    data_len + sizeof(struct uib_uet_pds_ack) +
		    sizeof(struct uib_uet_pds_ack_ext) +
		    sizeof(struct uib_uet_ses_rsp_data_small) > qp->path_mtu)
			return -EMSGSIZE;
		return uet_send_one_response(qp, destination, request,
					     ses_request, cack_psn,
					     encoding, data,
					     (uint32_t)data_len, modified_len,
					     0, receive_time_ns, ecn_marked,
					     request_header);
	}
	if (qp->path_mtu <= sizeof(struct uib_uet_pds_ack) +
			    sizeof(struct uib_uet_pds_ack_ext) +
			    sizeof(struct uib_uet_ses_rsp_data))
		return -EMSGSIZE;
	response_mtu = qp->path_mtu - sizeof(struct uib_uet_pds_ack) -
		sizeof(struct uib_uet_pds_ack_ext) -
		sizeof(struct uib_uet_ses_rsp_data);
	while (offset < data_len) {
		uint32_t chunk = (uint32_t)data_len - offset;

		if (chunk > response_mtu)
			chunk = response_mtu;
		rc = uet_send_one_response(qp, destination, request,
					   ses_request, cack_psn,
					   encoding,
					   (const uint8_t *)data + offset,
					   chunk, modified_len, offset,
					   receive_time_ns, ecn_marked,
					   request_header);
		if (rc < 0)
			return rc;
		offset += chunk;
	}
	return 0;
}

static void uet_rx_advance(struct uib_uet_qp *uet)
{
	while (uet->rx_bitmap[0] & 1) {
		uet->rx_bitmap[0] >>= 1;
		uet->rx_bitmap[0] |= uet->rx_bitmap[1] << 63;
		uet->rx_bitmap[1] >>= 1;
		uet->rx_base_psn++;
	}
}

static int uet_rx_mark(struct uib_uet_qp *uet, uint32_t psn, int *duplicate)
{
	uint32_t offset;
	uint64_t mask;
	unsigned int word;

	*duplicate = 0;
	if (!uet->rx_initialized)
		return -EINVAL;
	if (uet_psn_before(psn, uet->rx_base_psn)) {
		*duplicate = 1;
		return 0;
	}
	offset = psn - uet->rx_base_psn;
	if (offset >= UIB_UET_PDC_MPR)
		return -ERANGE;
	word = offset / 64;
	mask = 1ULL << (offset % 64);
	if (uet->rx_bitmap[word] & mask) {
		*duplicate = 1;
		return 0;
	}
	uet->rx_bitmap[word] |= mask;
	uet_rx_advance(uet);
	return 0;
}

static int uet_mr_contains(const struct uib_mr *mr, uint64_t address,
			   size_t length)
{
	uint64_t base;

	base = mr->kind == UIB_MR_HOST ?
		(uint64_t)(uintptr_t)mr->addr : mr->iova;
	if (address < base || length > mr->length)
		return 0;
	return address - base <= mr->length - length;
}

static struct uib_mr *uet_lookup_remote_mr(struct uib_qp *qp,
					   uint32_t rkey,
					   uint64_t address,
					   size_t length, int access,
					   int address_only,
					   uint8_t *response_code)
{
	struct uib_mr *mr;

	if (address_only) {
		mr = uib_pd_lookup_address(qp->pd, address, length, 0);
		if (!mr) {
			*response_code = UIB_UET_SES_RC_BAD_ADDR;
			return NULL;
		}
	} else {
		mr = uib_pd_lookup_rkey(qp->pd, rkey);
		if (!mr || mr->rkey != rkey) {
			if (mr)
				uib_mr_put(mr);
			*response_code = UIB_UET_SES_RC_BAD_MKEY;
			return NULL;
		}
		if (!uet_mr_contains(mr, address, length)) {
			uib_mr_put(mr);
			*response_code = UIB_UET_SES_RC_BAD_ADDR;
			return NULL;
		}
	}
	if ((mr->access & access) != access) {
		uib_mr_put(mr);
		*response_code = UIB_UET_SES_RC_OP_VIOLATION;
		return NULL;
	}
	return mr;
}

static void uet_reset_receive(struct uib_uet_qp *uet)
{
	memset(&uet->recv_slot, 0, sizeof(uet->recv_slot));
	uet->recv_active = 0;
	uet->recv_eom = 0;
	uet->recv_has_imm = 0;
}

static void uet_abort_receive(struct uib_qp *qp,
			      enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if (!uet->recv_active)
		return;
	uib_qp_emit_recv_slot(qp, &uet->recv_slot, status, UIB_WC_RECV, 0,
			      0, 0);
	uib_qp_release_recv(&uet->recv_slot);
	uet_reset_receive(uet);
}

static void uet_reset_write(struct uib_uet_qp *uet)
{
	uet->write_active = 0;
	uet->write_eom = 0;
	uet->write_with_imm = 0;
}

static void uet_abort_write(struct uib_qp *qp, enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if (uet->write_with_imm)
		uet_abort_receive(qp, status);
	uet_reset_write(uet);
}

static int uet_handle_send(struct uib_qp *qp,
			   const struct uib_uet_ses_req *ses,
			   const uint8_t *data, size_t data_len,
			   uint32_t psn, uint32_t source_pdc,
			   uint8_t *response_code)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint8_t flags = ses->flags & UIB_UET_SES_FLAGS_MASK;
	uint32_t offset = uib_uet_ses_message_offset(ses);
	size_t consumed = 0;
	int rc;

	if (!uet->recv_active) {
		rc = uib_qp_pop_recv(qp, &uet->recv_slot);
		if (rc < 0) {
			*response_code = rc == -ENOENT ? UIB_UET_SES_RC_NO_MATCH :
				UIB_UET_SES_RC_DROPPED;
			return rc;
		}
		uet->recv_active = 1;
		uet->recv_msg_id = ntohs(ses->msg_id);
		uet->recv_total = ntohl(ses->request_len);
		uet->recv_has_imm = !!(flags & UIB_UET_SES_FLAG_HD);
		if (uet->recv_has_imm)
			uet->recv_imm = (uint32_t)be64toh(ses->header_data);
	}
	if (!uet->recv_active || uet->recv_msg_id != ntohs(ses->msg_id)) {
		*response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
		uet_abort_receive(qp, UIB_WC_GENERAL_ERR);
		return -EBADMSG;
	}

	rc = uib_qp_scatter_recv(qp, &uet->recv_slot, offset, data, data_len,
				 &consumed);
	if (rc < 0) {
		*response_code = rc == -EMSGSIZE ? UIB_UET_SES_RC_TOO_LONG :
			UIB_UET_SES_RC_PERM_VIOLATION;
		uet_abort_receive(qp, rc == -EMSGSIZE ? UIB_WC_LOC_LEN_ERR :
				  UIB_WC_LOC_PROT_ERR);
		return rc;
	}
	if (consumed != data_len) {
		*response_code = UIB_UET_SES_RC_TOO_LONG;
		uet_abort_receive(qp, UIB_WC_LOC_LEN_ERR);
		return -EMSGSIZE;
	}
	if (flags & UIB_UET_SES_FLAG_EOM) {
		uet->recv_last_psn = psn;
		uet->recv_eom = 1;
	}
	if (uet->recv_eom && uet_psn_before(uet->recv_last_psn,
					     uet->rx_base_psn)) {
		uib_qp_emit_recv_slot(qp, &uet->recv_slot, UIB_WC_SUCCESS,
				      UIB_WC_RECV, uet->recv_total,
				      source_pdc, uet->recv_imm);
		uib_qp_release_recv(&uet->recv_slot);
		uet_reset_receive(uet);
	}
	return 0;
}

static int uet_handle_write(struct uib_qp *qp,
			    const struct uib_uet_ses_req *ses,
			    const uint8_t *data, size_t data_len,
			    int address_only, uint32_t psn,
			    uint32_t source_pdc, uint8_t *response_code)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint8_t flags = ses->flags & UIB_UET_SES_FLAGS_MASK;
	uint32_t offset = uib_uet_ses_message_offset(ses);
	uint64_t address = be64toh(ses->buffer_offset) + offset;
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	uint16_t msg_id = ntohs(ses->msg_id);
	struct uib_mr *mr;
	int with_imm = flags & UIB_UET_SES_FLAG_HD;
	int rc;

	if (flags & UIB_UET_SES_FLAG_SOM) {
		if (uet->write_active && uet->write_msg_id != msg_id) {
			*response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
			return -EBADMSG;
		}
		if (!uet->write_active) {
			if (with_imm) {
				if (uet->recv_active) {
					*response_code =
						UIB_UET_SES_RC_INITIATOR_ERROR;
					return -EBADMSG;
				}
				rc = uib_qp_pop_recv(qp, &uet->recv_slot);
				if (rc < 0) {
					*response_code = rc == -ENOENT ?
						UIB_UET_SES_RC_NO_MATCH :
						UIB_UET_SES_RC_DROPPED;
					return rc;
				}
				uet->recv_active = 1;
				uet->recv_msg_id = msg_id;
				uet->recv_total = ntohl(ses->request_len);
				uet->recv_imm =
					(uint32_t)be64toh(ses->header_data);
				uet->recv_source_pdc = source_pdc;
			}
			uet->write_active = 1;
			uet->write_msg_id = msg_id;
			uet->write_eom = 0;
			uet->write_with_imm = with_imm;
		}
	}
	if (!uet->write_active || uet->write_msg_id != msg_id) {
		*response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
		return -EBADMSG;
	}
	if (uib_uet_ses_payload_len(ses, data_len) != data_len ||
	    offset > ntohl(ses->request_len) ||
	    data_len > ntohl(ses->request_len) - offset) {
		*response_code = UIB_UET_SES_RC_UNSUPP_SIZE;
		uet_abort_write(qp, UIB_WC_LOC_LEN_ERR);
		return -EMSGSIZE;
	}
	mr = uet_lookup_remote_mr(qp, rkey, address, data_len,
				  UIB_ACCESS_REMOTE_WRITE, address_only,
				  response_code);
	if (!mr) {
		uet_abort_write(qp, UIB_WC_LOC_PROT_ERR);
		return -EACCES;
	}
	rc = uib_mr_place(mr, address, data, data_len);
	uib_mr_put(mr);
	if (rc == -ERANGE)
		*response_code = UIB_UET_SES_RC_BAD_ADDR;
	else if (rc < 0)
		*response_code = UIB_UET_SES_RC_HOST_UNSUCCESS_CMPL;
	if (rc < 0) {
		uet_abort_write(qp, rc == -ERANGE ? UIB_WC_LOC_LEN_ERR :
				UIB_WC_LOC_PROT_ERR);
		return rc;
	}
	if (flags & UIB_UET_SES_FLAG_EOM) {
		uet->write_last_psn = psn;
		uet->write_eom = 1;
	}
	if (!uet->write_eom ||
	    !uet_psn_before(uet->write_last_psn, uet->rx_base_psn))
		return 0;
	if (uet->write_with_imm) {
		uib_qp_emit_recv_slot(qp, &uet->recv_slot, UIB_WC_SUCCESS,
				      UIB_WC_RECV_RDMA_WITH_IMM,
				      uet->recv_total,
				      uet->recv_source_pdc, uet->recv_imm);
		uib_qp_release_recv(&uet->recv_slot);
		uet_reset_receive(uet);
	}
	uet_reset_write(uet);
	return 0;
}

static uint8_t uet_validate_session(struct uib_qp *qp,
				    const struct uib_uet_ses_req *ses)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint32_t identity = ntohl(ses->index_gen_job_id);
	uint32_t job_id = identity & UIB_UET_SES_JOB_ID_MASK;
	uint16_t pid = ntohs(ses->pid_on_fep) & UIB_UET_SES_PID_MASK;
	uint16_t index = ntohs(ses->index) & UIB_UET_SES_PID_MASK;
	uint8_t generation = identity >> 24;

	if (ses->flags >> UIB_UET_SES_VERSION_SHIFT)
		return UIB_UET_SES_RC_UNSUPP_OP;
	if (job_id != uet->session.job_id)
		return UIB_UET_SES_RC_BAD_JOB_ID;
	if (pid != uet->session.pid_on_fep)
		return UIB_UET_SES_RC_BAD_PID;
	if (index != uet->session.resource_index)
		return UIB_UET_SES_RC_BAD_INDEX;
	if (generation != uet->session.resource_generation)
		return UIB_UET_SES_RC_BAD_GENERATION;
	if (!!(ses->flags & UIB_UET_SES_FLAG_REL) !=
	    !!uet->session.relative_addressing)
		return UIB_UET_SES_RC_PERM_VIOLATION;
	return UIB_UET_SES_RC_OK;
}

static int uet_handle_read(struct uib_qp *qp,
			   const struct uib_uet_ses_req *ses,
			   const void **data, void **owned_data,
			   struct uib_uet_outstanding **owned_buffer,
			   size_t *data_len,
			   int address_only, uint8_t *response_code)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint64_t address = be64toh(ses->buffer_offset);
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	uint32_t length = ntohl(ses->request_len);
	struct uib_mr *mr;
	int rc;

	mr = uet_lookup_remote_mr(qp, rkey, address, length,
				  UIB_ACCESS_REMOTE_READ, address_only,
				  response_code);
	if (!mr)
		return -EACCES;
	*owned_buffer = uet_alloc_outstanding(uet, length ? length : 1);
	if (*owned_buffer)
		*owned_data = (*owned_buffer)->packet;
	else
		*owned_data = malloc(length ? length : 1);
	if (!*owned_data) {
		uib_mr_put(mr);
		*response_code = UIB_UET_SES_RC_UNCOR_TRANSIENT;
		return -ENOMEM;
	}
	rc = uib_mr_read(mr, address, *owned_data, length);
	uib_mr_put(mr);
	if (rc < 0) {
		if (*owned_buffer)
			uet_free_outstanding(*owned_buffer);
		else
			free(*owned_data);
		*owned_buffer = NULL;
		*owned_data = NULL;
		*response_code = rc == -ERANGE ? UIB_UET_SES_RC_BAD_ADDR :
			UIB_UET_SES_RC_HOST_UNSUCCESS_CMPL;
		return rc;
	}
	*data = *owned_data;
	*data_len = length;
	return 0;
}

static uint64_t uet_atomic_decode(const uint8_t *value, size_t width)
{
	uint64_t result = 0;
	size_t i;

	for (i = 0; i < width; i++)
		result = (result << 8) | value[i];
	return result;
}

static void uet_atomic_encode(uint8_t *value, size_t width, uint64_t input)
{
	size_t i;

	for (i = 0; i < width; i++) {
		value[width - i - 1] = input & UINT8_MAX;
		input >>= 8;
	}
}

static int64_t uet_atomic_signed(uint64_t value, size_t width)
{
	unsigned int shift = 64 - (unsigned int)width * 8;

	return (int64_t)(value << shift) >> shift;
}

static int uet_atomic_apply(uint8_t opcode, int is_signed, size_t width,
			    uint64_t old, uint64_t operand, uint64_t swap,
			    uint64_t *updated)
{
	uint64_t mask = width == sizeof(uint64_t) ? UINT64_MAX :
		(1ULL << (width * 8)) - 1;
	int64_t signed_old = uet_atomic_signed(old, width);
	int64_t signed_operand = uet_atomic_signed(operand, width);
	int condition;

	old &= mask;
	operand &= mask;
	swap &= mask;
	switch (opcode) {
	case UIB_UET_AMO_MIN:
		*updated = is_signed ?
			(signed_old < signed_operand ? old : operand) :
			(old < operand ? old : operand);
		break;
	case UIB_UET_AMO_MAX:
		*updated = is_signed ?
			(signed_old > signed_operand ? old : operand) :
			(old > operand ? old : operand);
		break;
	case UIB_UET_AMO_SUM:
		*updated = old + operand;
		break;
	case UIB_UET_AMO_DIFF:
		*updated = old - operand;
		break;
	case UIB_UET_AMO_PROD:
		*updated = old * operand;
		break;
	case UIB_UET_AMO_LOR:
		*updated = !!old || !!operand;
		break;
	case UIB_UET_AMO_LAND:
		*updated = !!old && !!operand;
		break;
	case UIB_UET_AMO_BOR:
		*updated = old | operand;
		break;
	case UIB_UET_AMO_BAND:
		*updated = old & operand;
		break;
	case UIB_UET_AMO_LXOR:
		*updated = !!old != !!operand;
		break;
	case UIB_UET_AMO_BXOR:
		*updated = old ^ operand;
		break;
	case UIB_UET_AMO_READ:
		*updated = old;
		break;
	case UIB_UET_AMO_WRITE:
		*updated = operand;
		break;
	case UIB_UET_AMO_CSWAP:
		*updated = old == operand ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_NE:
		*updated = old != operand ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_LE:
		condition = is_signed ? signed_old <= signed_operand :
			old <= operand;
		*updated = condition ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_LT:
		condition = is_signed ? signed_old < signed_operand :
			old < operand;
		*updated = condition ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_GE:
		condition = is_signed ? signed_old >= signed_operand :
			old >= operand;
		*updated = condition ? swap : old;
		break;
	case UIB_UET_AMO_CSWAP_GT:
		condition = is_signed ? signed_old > signed_operand :
			old > operand;
		*updated = condition ? swap : old;
		break;
	case UIB_UET_AMO_MSWAP:
		*updated = (old & ~operand) | (swap & operand);
		break;
	default:
		return -EOPNOTSUPP;
	}
	*updated &= mask;
	return 0;
}

static int uet_handle_atomic(struct uib_qp *qp,
			     const struct uib_uet_ses_req *ses,
			     const uint8_t *data, size_t data_len,
			     const void **response, size_t *response_len,
			     uint8_t *result, int address_only,
			     uint8_t *response_code)
{
	const struct uib_uet_ses_atomic *atomic;
	uint64_t address = be64toh(ses->buffer_offset);
	uint32_t rkey = (uint32_t)be64toh(ses->match_bits);
	uint64_t operand = 0, swap = 0, old = 0, updated;
	int is_signed;
	struct uib_mr *mr;
	size_t width;
	int fetching;
	int rc;

	if (data_len < sizeof(*atomic)) {
		*response_code = UIB_UET_SES_RC_AMO_UNSUPP_SIZE;
		return -EMSGSIZE;
	}
	atomic = (const struct uib_uet_ses_atomic *)data;
	data += sizeof(*atomic);
	data_len -= sizeof(*atomic);
	width = uib_uet_atomic_datatype_size(atomic->datatype);
	if (!width) {
		*response_code = UIB_UET_SES_RC_AMO_UNSUPP_DT;
		return -EAFNOSUPPORT;
	}
	if (address & (width - 1)) {
		*response_code = UIB_UET_SES_RC_AMO_UNALIGNED;
		return -EINVAL;
	}
	if (uet_atomic_has_two_operands(atomic->opcode)) {
		if (data_len == 32 && width == sizeof(uint64_t)) {
			operand = uet_atomic_decode(data + 8, width);
			swap = uet_atomic_decode(data + 24, width);
		} else if (data_len == 2 * width) {
			operand = uet_atomic_decode(data, width);
			swap = uet_atomic_decode(data + width, width);
		} else {
			*response_code = UIB_UET_SES_RC_AMO_UNSUPP_SIZE;
			return -EMSGSIZE;
		}
	} else if (atomic->opcode != UIB_UET_AMO_READ) {
		if (data_len != width) {
			*response_code = UIB_UET_SES_RC_AMO_UNSUPP_SIZE;
			return -EMSGSIZE;
		}
		operand = uet_atomic_decode(data, width);
	} else if (data_len) {
		*response_code = UIB_UET_SES_RC_AMO_UNSUPP_SIZE;
		return -EMSGSIZE;
	}
	mr = uet_lookup_remote_mr(qp, rkey, address, width,
				  UIB_ACCESS_REMOTE_ATOMIC |
				  UIB_ACCESS_REMOTE_READ |
				  UIB_ACCESS_REMOTE_WRITE,
				  address_only, response_code);
	if (!mr)
		return -EACCES;
	is_signed = !(atomic->datatype & 1);
	rc = uet_atomic_apply(atomic->opcode, is_signed, width, 0,
			      operand, swap, &updated);
	if (rc < 0) {
		*response_code = UIB_UET_SES_RC_AMO_UNSUPP_OP;
		uib_mr_put(mr);
		return rc;
	}
	pthread_mutex_lock(&qp->lock);
	rc = uib_mr_read(mr, address, &old, width);
	if (rc < 0) {
		pthread_mutex_unlock(&qp->lock);
		uib_mr_put(mr);
		return rc;
	}
	rc = uet_atomic_apply(atomic->opcode, is_signed, width, old,
			      operand, swap, &updated);
	if (!rc && atomic->opcode != UIB_UET_AMO_READ)
		rc = uib_mr_place(mr, address, &updated, width);
	pthread_mutex_unlock(&qp->lock);
	uib_mr_put(mr);
	if (rc < 0) {
		*response_code = rc == -ERANGE ? UIB_UET_SES_RC_BAD_ADDR :
			UIB_UET_SES_RC_HOST_UNSUCCESS_CMPL;
		return rc;
	}
	fetching = (ses->opcode & UIB_UET_SES_OPCODE_MASK) ==
		UIB_UET_SES_FETCHING_ATOMIC;
	if (fetching) {
		uet_atomic_encode(result, width, old);
		*response = result;
		*response_len = width;
	}
	return 0;
}

static int uet_response_cache_lookup(struct uib_uet_qp *uet, uint32_t psn,
				     uint16_t msg_id, uint8_t *response_code,
				     uint32_t *modified_len, uint8_t *data,
				     size_t *data_len)
{
	uint32_t slot;

	slot = psn % UIB_UET_PDC_MPR;
	if (!uet->response_cache[slot].valid ||
	    uet->response_cache[slot].psn != psn ||
	    uet->response_cache[slot].msg_id != msg_id)
		return -ENOENT;
	*response_code = uet->response_cache[slot].response_code;
	*modified_len = uet->response_cache[slot].modified_len;
	*data_len = uet->response_cache[slot].data_len;
	memcpy(data, uet->response_cache[slot].data, *data_len);
	return 0;
}

static void uet_response_cache_store(struct uib_uet_qp *uet, uint32_t psn,
				     uint16_t msg_id, uint8_t response_code,
				     uint32_t modified_len, const void *data,
				     size_t data_len)
{
	uint32_t slot;

	if (data_len > sizeof(uet->response_cache[0].data))
		return;
	slot = psn % UIB_UET_PDC_MPR;
	uet->response_cache[slot].psn = psn;
	uet->response_cache[slot].msg_id = msg_id;
	uet->response_cache[slot].response_code = response_code;
	uet->response_cache[slot].modified_len = modified_len;
	uet->response_cache[slot].data_len = (uint8_t)data_len;
	if (data_len)
		memcpy(uet->response_cache[slot].data, data, data_len);
	uet->response_cache[slot].valid = 1;
}

static void uet_mark_guaranteed(struct uib_uet_qp *uet, uint32_t psn)
{
	uint32_t slot;

	slot = psn % UIB_UET_PDC_MPR;
	uet->guaranteed[slot].psn = psn;
	uet->guaranteed[slot].valid = 1;
}

static void uet_clear_guaranteed(struct uib_uet_qp *uet, uint32_t clear_psn)
{
	uint32_t i;

	if (clear_psn == uet->rx_clear_psn ||
	    uet_psn_before(clear_psn, uet->rx_clear_psn))
		return;
	uet->rx_clear_psn = clear_psn;
	for (i = 0; i < UIB_UET_PDC_MPR; i++) {
		if (uet->guaranteed[i].valid &&
		    !uet_psn_before(clear_psn, uet->guaranteed[i].psn))
			uet->guaranteed[i].valid = 0;
		if (uet->response_cache[i].valid &&
		    !uet_psn_before(clear_psn, uet->response_cache[i].psn))
			uet->response_cache[i].valid = 0;
	}
}

static uint32_t uet_cack_psn(const struct uib_uet_qp *uet)
{
	uint32_t cack = uet->rx_base_psn - 1;
	uint32_t i;

	for (i = 0; i < UIB_UET_PDC_MPR; i++) {
		if (!uet->guaranteed[i].valid)
			continue;
		if (!uet_psn_before(cack, uet->guaranteed[i].psn))
			cack = uet->guaranteed[i].psn - 1;
	}
	return cack;
}

static void uet_apply_request_clear(struct uib_uet_qp *uet,
				    const struct uib_uet_pds_req *request)
{
	int32_t offset;
	uint32_t clear_psn;

	offset = (int16_t)ntohs(request->clear_psn_offset);
	clear_psn = ntohl(request->psn) + offset;
	uet_clear_guaranteed(uet, clear_psn);
}

static int uet_parse_request(const struct uib_uet_pds_req *request, size_t len,
			     size_t pds_len,
			     struct uib_uet_ses_req *canonical,
			     const uint8_t **data, size_t *data_len,
			     uint8_t *request_header)
{
	const uint8_t *ses = (const uint8_t *)request + pds_len;
	size_t header_len;

	*request_header = uib_uet_prologue_next(&request->prologue);
	switch (*request_header) {
	case UIB_UET_NEXT_REQ_STD:
		header_len = sizeof(*canonical);
		if (len < pds_len + header_len)
			return -EBADMSG;
		memcpy(canonical, ses, header_len);
		break;
	case UIB_UET_NEXT_REQ_MEDIUM: {
		const struct uib_uet_ses_req_medium *medium;

		header_len = sizeof(*medium);
		if (len < pds_len + header_len)
			return -EBADMSG;
		medium = (const struct uib_uet_ses_req_medium *)ses;
		memset(canonical, 0, sizeof(*canonical));
		canonical->opcode = medium->opcode;
		canonical->flags = medium->flags;
		canonical->msg_id = htons((uint16_t)ntohl(request->psn));
		canonical->index_gen_job_id = medium->index_gen_job_id;
		canonical->pid_on_fep = medium->pid_on_fep;
		canonical->index = medium->index;
		canonical->buffer_offset = medium->buffer_offset;
		canonical->header_data = medium->buffer_offset;
		canonical->initiator = medium->initiator;
		canonical->match_bits = medium->match_bits;
		canonical->request_len =
			htonl(ntohs(medium->request_len) &
			      UIB_UET_SES_LENGTH_MASK);
		break;
	}
	case UIB_UET_NEXT_REQ_SMALL: {
		const struct uib_uet_ses_req_small *small;

		header_len = sizeof(*small);
		if (len < pds_len + header_len)
			return -EBADMSG;
		small = (const struct uib_uet_ses_req_small *)ses;
		memset(canonical, 0, sizeof(*canonical));
		canonical->opcode = small->opcode;
		canonical->flags = small->flags;
		canonical->msg_id = htons((uint16_t)ntohl(request->psn));
		canonical->index_gen_job_id = small->index_gen_job_id;
		canonical->pid_on_fep = small->pid_on_fep;
		canonical->index = small->index;
		canonical->buffer_offset = small->buffer_offset;
		canonical->request_len =
			htonl(ntohs(small->request_len) &
			      UIB_UET_SES_LENGTH_MASK);
		break;
	}
	default:
		return -EBADMSG;
	}
	*data = ses + header_len;
	*data_len = len - pds_len - header_len;
	return 0;
}

struct uet_request_result {
	const void *data;
	void *owned_data;
	struct uib_uet_outstanding *owned_buffer;
	size_t data_len;
	uint32_t modified_len;
	uint32_t response_identity;
	uint8_t response_code;
	uint8_t guarantee_error;
	uint8_t storage[16];
};

static void uet_request_result_release(struct uet_request_result *result)
{
	if (result->owned_buffer)
		uet_free_outstanding(result->owned_buffer);
	else
		free(result->owned_data);
	result->owned_buffer = NULL;
	result->owned_data = NULL;
	result->data = NULL;
	result->data_len = 0;
}

static int uet_defer_request(struct uib_qp *qp,
			     const struct sockaddr_in *source,
			     const void *packet, size_t packet_len,
			     uint32_t psn, uint16_t msg_id, uint8_t opcode,
			     int ecn_marked)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *old;
	struct uib_uet_outstanding *out;
	struct uib_uet_deferred *deferred;
	uint32_t slot;

	out = uet_alloc_outstanding(uet, packet_len);
	if (!out)
		return -ENOMEM;
	memcpy(out->packet, packet, packet_len);
	out->psn = psn;
	slot = psn % UIB_UET_PDC_MPR;
	pthread_mutex_lock(&qp->lock);
	deferred = &uet->deferred[slot];
	if (deferred->packet && deferred->psn == psn) {
		pthread_mutex_unlock(&qp->lock);
		uet_free_outstanding(out);
		return 0;
	}
	old = deferred->packet;
	if (!old)
		__atomic_add_fetch(&uet->deferred_count, 1, __ATOMIC_RELAXED);
	deferred->packet = out;
	deferred->source = *source;
	deferred->psn = psn;
	deferred->msg_id = msg_id;
	deferred->opcode = opcode;
	deferred->ecn_marked = !!ecn_marked;
	pthread_mutex_unlock(&qp->lock);
	uet_free_outstanding(old);
	return 0;
}

static int uet_deferred_ready(const struct uib_uet_qp *uet,
			      const struct uib_uet_deferred *deferred)
{
	if (deferred->psn == uet->rx_base_psn &&
	    ((deferred->opcode == UIB_UET_SES_SEND && !uet->recv_active) ||
	     (deferred->opcode == UIB_UET_SES_WRITE && !uet->write_active)))
		return 1;
	if (uet->recv_active && deferred->opcode == UIB_UET_SES_SEND &&
	    deferred->msg_id == uet->recv_msg_id)
		return 1;
	if (uet->write_active && deferred->opcode == UIB_UET_SES_WRITE &&
	    deferred->msg_id == uet->write_msg_id)
		return 1;
	return 0;
}

static void uet_drop_deferred_copy(struct uib_qp *qp, uint32_t psn)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *packet;
	struct uib_uet_deferred *deferred;
	uint32_t slot;

	packet = NULL;
	if (!__atomic_load_n(&uet->deferred_count, __ATOMIC_RELAXED))
		return;
	slot = psn % UIB_UET_PDC_MPR;
	pthread_mutex_lock(&qp->lock);
	deferred = &uet->deferred[slot];
	if (deferred->packet && deferred->psn == psn) {
		packet = deferred->packet;
		memset(deferred, 0, sizeof(*deferred));
		__atomic_sub_fetch(&uet->deferred_count, 1,
				   __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&qp->lock);
	uet_free_outstanding(packet);
}

static struct uib_uet_outstanding *
uet_take_deferred(struct uib_qp *qp, struct sockaddr_in *source,
		  int *ecn_marked)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *packet;
	struct uib_uet_deferred *deferred;
	uint32_t best_offset;
	uint32_t offset;
	uint32_t best;
	uint32_t i;

	packet = NULL;
	if (!__atomic_load_n(&uet->deferred_count, __ATOMIC_RELAXED))
		return NULL;
	best_offset = UINT32_MAX;
	best = 0;
	pthread_mutex_lock(&qp->lock);
	for (i = 0; i < UIB_UET_PDC_MPR; i++) {
		deferred = &uet->deferred[i];
		if (!deferred->packet || !uet_deferred_ready(uet, deferred))
			continue;
		offset = deferred->psn - uet->rx_base_psn;
		if (offset >= best_offset)
			continue;
		best = i;
		best_offset = offset;
	}
	if (best_offset != UINT32_MAX) {
		deferred = &uet->deferred[best];
		packet = deferred->packet;
		*source = deferred->source;
		*ecn_marked = deferred->ecn_marked;
		memset(deferred, 0, sizeof(*deferred));
		__atomic_sub_fetch(&uet->deferred_count, 1,
				   __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&qp->lock);
	return packet;
}

static int uet_accept_request(struct uib_qp *qp,
			      const struct uib_uet_pds_req *request,
			      size_t packet_len, size_t payload_len,
			      uint8_t packet_type,
			      uint8_t opcode, uint8_t ses_flags,
			      uint16_t msg_id, uint32_t psn,
			      uint64_t receive_time_ns, int *duplicate,
			      uint32_t *cack, int *ack_due, int *new_pdc)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	const struct uib_uet_pds_cc_req *cc_request;
	uint32_t credit_target;
	uint32_t ack_bytes;
	uint32_t offset;
	int stale;
	int rc;

	pthread_mutex_lock(&qp->lock);
	*ack_due = 0;
	*new_pdc = !uet->rx_initialized;
	if (packet_type == UIB_UET_PDS_TYPE_RUD_CC_REQ && uet->rccc_enabled) {
		cc_request = (const struct uib_uet_pds_cc_req *)request;
		uib_uet_rccc_request_unpack(cc_request->req_cc_state, NULL,
					    &credit_target);
		uib_uet_rccc_destination_update(&uet->rccc_destination,
						credit_target,
						receive_time_ns);
	}
	uet->dpdcid = ntohs(request->spdcid);
	if (uib_uet_prologue_flags(&request->prologue) & UIB_UET_REQ_FLAG_SYN)
		uet->state = UIB_UET_PDC_ESTABLISHED;
	if (!uet->rx_initialized) {
		uet->rx_base_psn = qp->rq_psn;
		uet->rx_clear_psn = qp->rq_psn - 1;
		uet->rx_initialized = 1;
	}
	uet_apply_request_clear(uet, request);
	stale = uet_psn_before(psn, uet->rx_base_psn);
	if (opcode == UIB_UET_SES_SEND && !uet->recv_active &&
	    uet_psn_before(uet->rx_base_psn, psn)) {
		offset = psn - uet->rx_base_psn;
		if (offset >= UIB_UET_PDC_MPR) {
			pthread_mutex_unlock(&qp->lock);
			return -ERANGE;
		}
		uet_debug(qp, "defer request psn=%u msg=%u expected_psn=%u",
			  psn, msg_id, uet->rx_base_psn);
		pthread_mutex_unlock(&qp->lock);
		return 1;
	}
	if (!stale && uet->recv_active &&
	    uet->recv_msg_id != msg_id) {
		uet_debug(qp, "defer request psn=%u msg=%u active_msg=%u",
			  psn, msg_id, uet->recv_msg_id);
		pthread_mutex_unlock(&qp->lock);
		return 1;
	}
	if (!stale && opcode == UIB_UET_SES_WRITE &&
	    ((!uet->write_active &&
	      (!(ses_flags & UIB_UET_SES_FLAG_SOM) ||
	       psn != uet->rx_base_psn)) ||
	     (uet->write_active && uet->write_msg_id != msg_id))) {
		uet_debug(qp, "defer write psn=%u msg=%u active_msg=%u", psn,
			  msg_id, uet->write_msg_id);
		pthread_mutex_unlock(&qp->lock);
		return 1;
	}
	rc = uet_rx_mark(uet, psn, duplicate);
	if (!rc && !*duplicate) {
		if (uib_congestion_enabled(&uet->congestion))
			uet->receiver_bytes += (uint32_t)packet_len + 40;
		ack_bytes = payload_len < UIB_UET_ACK_GEN_MIN_PACKET ?
			UIB_UET_ACK_GEN_MIN_PACKET : (uint32_t)payload_len;
		if (ack_bytes >= UIB_UET_ACK_GEN_TRIGGER -
				 uet->ack_gen_count) {
			uet->ack_gen_count = UIB_UET_ACK_GEN_TRIGGER;
			*ack_due = 1;
		} else {
			uet->ack_gen_count += ack_bytes;
		}
	}
	*cack = uet_cack_psn(uet);
	pthread_mutex_unlock(&qp->lock);
	uet_debug(qp, "rx request psn=%u cack=%u msg=%u duplicate=%d rc=%d",
		  psn, *cack, msg_id, *duplicate, rc);
	return rc;
}

static int uet_execute_request(struct uib_qp *qp,
			       const struct uib_uet_pds_req *request,
			       const struct uib_uet_ses_req *ses,
			       const uint8_t *data, size_t data_len,
			       uint8_t request_header, uint32_t psn,
			       struct uet_request_result *result)
{
	uint8_t opcode;
	int rc;

	opcode = ses->opcode & UIB_UET_SES_OPCODE_MASK;
	result->response_code = UIB_UET_SES_RC_OK;
	if ((ses->flags & UIB_UET_SES_FLAG_IE) ||
	    opcode == UIB_UET_SES_MSG_ERROR) {
		result->response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
		uet_abort_receive(qp, UIB_WC_GENERAL_ERR);
		return -ECANCELED;
	}
	switch (opcode) {
	case UIB_UET_SES_NOOP:
		if (data_len || ntohl(ses->request_len) ||
		    (ses->flags & (UIB_UET_SES_FLAG_SOM |
				   UIB_UET_SES_FLAG_EOM)) !=
		    (UIB_UET_SES_FLAG_SOM | UIB_UET_SES_FLAG_EOM)) {
			result->response_code = UIB_UET_SES_RC_UNSUPP_SIZE;
			rc = -EMSGSIZE;
		} else {
			result->modified_len = 0;
			rc = 0;
		}
		break;
	case UIB_UET_SES_SEND:
		rc = uet_handle_send(qp, ses, data, data_len, psn,
				     ntohs(request->spdcid),
				     &result->response_code);
		result->modified_len = ntohl(ses->request_len);
		break;
	case UIB_UET_SES_WRITE:
		rc = uet_handle_write(qp, ses, data, data_len,
				      request_header == UIB_UET_NEXT_REQ_SMALL,
				      psn, ntohs(request->spdcid),
				      &result->response_code);
		result->modified_len = ntohl(ses->request_len);
		break;
	case UIB_UET_SES_READ:
		rc = uet_handle_read(qp, ses, &result->data, &result->owned_data,
				     &result->owned_buffer, &result->data_len,
				     request_header == UIB_UET_NEXT_REQ_SMALL,
				     &result->response_code);
		result->modified_len = result->data_len;
		break;
	case UIB_UET_SES_FETCHING_ATOMIC:
	case UIB_UET_SES_ATOMIC:
		rc = uet_handle_atomic(qp, ses, data, data_len, &result->data,
				       &result->data_len, result->storage,
				       request_header == UIB_UET_NEXT_REQ_SMALL,
				       &result->response_code);
		result->modified_len = result->data_len;
		break;
	default:
		result->response_code = UIB_UET_SES_RC_UNSUPP_OP;
		rc = -EOPNOTSUPP;
		break;
	}
	if (rc < 0)
		result->modified_len = 0;
	return rc;
}

static void uet_duplicate_response(struct uib_qp *qp, uint32_t psn,
				   uint16_t msg_id,
				   struct uet_request_result *result)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	int rc;

	pthread_mutex_lock(&qp->lock);
	rc = uet_response_cache_lookup(uet, psn, msg_id,
				       &result->response_code,
				       &result->modified_len, result->storage,
				       &result->data_len);
	pthread_mutex_unlock(&qp->lock);
	if (!rc && result->data_len) {
		result->data = result->storage;
	} else if (rc < 0) {
		result->response_code = UIB_UET_SES_RC_INITIATOR_ERROR;
		result->modified_len = 0;
		result->data_len = 0;
	}
}

static int uet_record_message_error(struct uib_uet_qp *uet, uint16_t msg_id,
				    uint32_t psn, uint8_t *response_code)
{
	uint32_t slot;

	slot = msg_id % UIB_UET_PDC_MPR;
	if (uet->message_error[slot].valid &&
	    uet->message_error[slot].msg_id == msg_id) {
		*response_code = uet->message_error[slot].response_code;
		return uet->message_error[slot].first_psn == psn;
	}
	uet->message_error[slot].first_psn = psn;
	uet->message_error[slot].msg_id = msg_id;
	uet->message_error[slot].response_code = *response_code;
	uet->message_error[slot].valid = 1;
	return 1;
}

static int uet_handle_request_one(struct uib_qp *qp,
				  const struct sockaddr_in *src,
				  const void *packet, size_t len,
				  int ecn_marked)
{
	const struct uib_uet_pds_req *request = packet;
	struct uib_uet_ses_req ses;
	struct uet_request_result result = {
		.response_code = UIB_UET_SES_RC_OK,
	};
	struct uet_response_encoding encoding;
	const uint8_t *data;
	struct uib_uet_qp *uet = qp->protocol_private;
	uint64_t receive_time_ns = uib_now_ns();
	size_t data_len;
	size_t pds_len;
	uint32_t psn;
	uint32_t cack;
	uint16_t msg_id;
	uint8_t request_header;
	uint8_t request_flags;
	uint8_t packet_type;
	uint8_t opcode;
	int duplicate;
	int ack_requested;
	int ack_due;
	int new_pdc;
	int pdc_idle;
	int send_ack;
	int rc;

	if (len < sizeof(*request))
		return -EBADMSG;
	packet_type = uib_uet_prologue_type(&request->prologue);
	pds_len = packet_type == UIB_UET_PDS_TYPE_RUD_CC_REQ ?
		sizeof(struct uib_uet_pds_cc_req) : sizeof(*request);
	if (len < pds_len)
		return -EBADMSG;
	rc = uet_parse_request(request, len, pds_len, &ses, &data, &data_len,
			       &request_header);
	if (rc < 0)
		return rc;
	psn = ntohl(request->psn);
	msg_id = ntohs(ses.msg_id);
	opcode = ses.opcode & UIB_UET_SES_OPCODE_MASK;
	request_flags = uib_uet_prologue_flags(&request->prologue);
	ack_requested = !!(request_flags & UIB_UET_REQ_FLAG_AR);
	rc = uet_accept_request(qp, request, len, data_len, packet_type, opcode,
				ses.flags & UIB_UET_SES_FLAGS_MASK, msg_id, psn,
				receive_time_ns, &duplicate, &cack, &ack_due,
				&new_pdc);
	if (rc < 0)
		return rc;
	if (rc > 0)
		return uet_defer_request(qp, src, packet, len, psn, msg_id,
					 opcode,
					 ecn_marked);
	uet_drop_deferred_copy(qp, psn);
	result.response_code = uet_validate_session(qp, &ses);
	if (duplicate && opcode != UIB_UET_SES_READ) {
		uet_duplicate_response(qp, psn, msg_id, &result);
	} else if (result.response_code == UIB_UET_SES_RC_OK) {
		uet_execute_request(qp, request, &ses, data, data_len,
				    request_header, psn, &result);
	}
	pthread_mutex_lock(&qp->lock);
	if (result.response_code != UIB_UET_SES_RC_OK)
		result.guarantee_error = uet_record_message_error(
			uet, msg_id, psn, &result.response_code);
	if (!duplicate && opcode != UIB_UET_SES_READ)
		uet_response_cache_store(uet, psn, msg_id, result.response_code,
					 result.modified_len, result.data,
					 result.data_len);
	pthread_mutex_unlock(&qp->lock);
	if (result.response_code != UIB_UET_SES_RC_OK) {
		uet_request_result_release(&result);
		result.modified_len = 0;
	}
	encoding = uet_response_construct(result.response_code,
					  (uint32_t)result.data_len,
					  result.modified_len,
					  ntohl(ses.request_len),
					  result.guarantee_error);
	pthread_mutex_lock(&qp->lock);
	if (encoding.guaranteed_delivery)
		uet_mark_guaranteed(uet, psn);
	cack = uet_cack_psn(uet);
	pthread_mutex_unlock(&qp->lock);
	pdc_idle = result.response_code == UIB_UET_SES_RC_OK &&
		((opcode == UIB_UET_SES_SEND && !uet->recv_active) ||
		 (opcode == UIB_UET_SES_WRITE && !uet->write_active));
	send_ack = ack_requested || ack_due || new_pdc || duplicate ||
		ecn_marked || pdc_idle ||
		(request_flags & UIB_UET_REQ_FLAG_SYN) ||
		encoding.guaranteed_delivery ||
		result.response_code != UIB_UET_SES_RC_OK ||
		(opcode != UIB_UET_SES_SEND && opcode != UIB_UET_SES_WRITE);
	if (!send_ack) {
		uet_request_result_release(&result);
		return 0;
	}
	rc = uet_send_response(qp, src, request, &ses, cack, &encoding,
			       result.data, result.data_len,
			       result.modified_len,
			       receive_time_ns, ecn_marked, request_header);
	if (rc >= 0) {
		pthread_mutex_lock(&qp->lock);
		uet->ack_gen_count = 0;
		pthread_mutex_unlock(&qp->lock);
	}
	uet_request_result_release(&result);
	return rc;
}

static int uet_handle_request(struct uib_qp *qp,
			      const struct sockaddr_in *source,
			      const void *packet, size_t len, int ecn_marked)
{
	struct uib_uet_qp *uet;
	struct uib_uet_outstanding *deferred;
	struct sockaddr_in deferred_source;
	int deferred_ecn;
	int rc;

	uet = qp->protocol_private;
	pthread_mutex_lock(&uet->rx_lock);
	rc = uet_handle_request_one(qp, source, packet, len, ecn_marked);
	if (rc < 0)
		goto out;
	for (;;) {
		deferred = uet_take_deferred(qp, &deferred_source,
					       &deferred_ecn);
		if (!deferred) {
			rc = 0;
			break;
		}
		rc = uet_handle_request_one(qp, &deferred_source,
					    deferred->packet,
					    deferred->packet_len,
					    deferred_ecn);
		uet_free_outstanding(deferred);
		if (rc < 0)
			break;
	}

out:
	pthread_mutex_unlock(&uet->rx_lock);
	return rc;
}

static void uet_complete_out_locked(struct uib_qp *qp,
				    struct uib_uet_outstanding *out,
				    enum uib_wc_status status)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *track = out->track;

	uet_unlink_outstanding(uet, out);
	uet_free_outstanding(out);
	uet_track_done(qp, track, status);
	if (!uet->sent_count)
		uib_qp_retry_cancel(qp);
}

static int uet_track_read_fragment(struct uib_uet_wr_track *track,
				   uint32_t offset, uint32_t length)
{
	struct uib_uet_read_fragment *fragments;
	uint32_t capacity;
	uint32_t i;

	for (i = 0; i < track->read_fragment_count; i++) {
		uint32_t old_offset = track->read_fragments[i].offset;
		uint32_t old_length = track->read_fragments[i].length;

		if (old_offset == offset && old_length == length)
			return 1;
		if (offset < old_offset + old_length && old_offset < offset + length)
			return -EBADMSG;
	}
	if (track->read_fragment_count == track->read_fragment_capacity) {
		capacity = track->read_fragment_capacity * 2;
		fragments = malloc(capacity * sizeof(*fragments));
		if (!fragments)
			return -ENOMEM;
		memcpy(fragments, track->read_fragments,
		       track->read_fragment_count * sizeof(*fragments));
		if (track->read_fragments != track->inline_read_fragments)
			free(track->read_fragments);
		track->read_fragments = fragments;
		track->read_fragment_capacity = capacity;
	}
	track->read_fragments[track->read_fragment_count].offset = offset;
	track->read_fragments[track->read_fragment_count].length = length;
	track->read_fragment_count++;
	track->read_received += length;
	return 0;
}

struct uet_ack_view {
	const struct uib_uet_pds_ack *ack;
	const struct uib_uet_pds_ack_ext *extension;
	const uint8_t *data;
	size_t data_len;
	uint32_t cack;
	uint32_t ack_psn;
	uint32_t sack_psn;
	uint32_t message_offset;
	uint32_t modified_len;
	uint32_t response_identity;
	uint16_t msg_id;
	uint64_t sack_bitmap;
	uint8_t response_code;
	uint8_t has_data_response;
};

static int uet_parse_ack(const void *packet, size_t len,
			 struct uet_ack_view *view)
{
	const struct uib_uet_ses_rsp_data_small *small;
	const struct uib_uet_ses_rsp_data *response_data;
	const struct uib_uet_ses_rsp *response;
	size_t header_len;
	uint32_t small_request_psn;
	uint16_t packed;
	uint8_t next;
	int32_t offset;

	memset(view, 0, sizeof(*view));
	small_request_psn = 0;
	view->ack = packet;
	header_len = sizeof(*view->ack);
	if (uib_uet_prologue_type(&view->ack->prologue) ==
	    UIB_UET_PDS_TYPE_ACK_CC) {
		if (len < header_len + sizeof(*view->extension))
			return -EBADMSG;
		view->extension = (const struct uib_uet_pds_ack_ext *)
			((const uint8_t *)packet + header_len);
		header_len += sizeof(*view->extension);
	}
	next = uib_uet_prologue_next(&view->ack->prologue);
	if (next == UIB_UET_NEXT_RSP_DATA) {
		if (len < header_len + sizeof(*response_data))
			return -EBADMSG;
		response_data = (const struct uib_uet_ses_rsp_data *)
			((const uint8_t *)packet + header_len);
		view->data = (const uint8_t *)(response_data + 1);
		view->data_len = len - header_len - sizeof(*response_data);
		if ((ntohs(response_data->payload_len) &
		     UIB_UET_SES_LENGTH_MASK) != view->data_len)
			return -EBADMSG;
		view->msg_id = ntohs(response_data->request_msg_id);
		view->response_identity = ntohl(response_data->index_gen_job_id);
		packed = ntohs(response_data->list_opcode_version_rc);
		view->modified_len = ntohl(response_data->modified_len);
		view->message_offset = ntohl(response_data->message_offset);
		view->has_data_response = 1;
	} else if (next == UIB_UET_NEXT_RSP_DATA_SMALL) {
		if (len < header_len + sizeof(*small))
			return -EBADMSG;
		small = (const struct uib_uet_ses_rsp_data_small *)
			((const uint8_t *)packet + header_len);
		view->data = (const uint8_t *)(small + 1);
		view->data_len = len - header_len - sizeof(*small);
		if ((ntohs(small->payload_len) & UIB_UET_SES_LENGTH_MASK) !=
		    view->data_len)
			return -EBADMSG;
		small_request_psn = ntohl(small->request_psn);
		view->msg_id = (uint16_t)small_request_psn;
		view->response_identity = ntohl(small->index_gen_job_id);
		packed = ntohs(small->list_opcode_version_rc);
		view->modified_len = (uint32_t)view->data_len;
		view->has_data_response = 1;
	} else if (next == UIB_UET_NEXT_RSP) {
		if (len < header_len + sizeof(*response))
			return -EBADMSG;
		response = (const struct uib_uet_ses_rsp *)
			((const uint8_t *)packet + header_len);
		view->data = (const uint8_t *)(response + 1);
		view->data_len = len - header_len - sizeof(*response);
		if (view->data_len)
			return -EBADMSG;
		view->msg_id = ntohs(response->msg_id);
		view->response_identity = ntohl(response->index_gen_job_id);
		packed = ntohs(response->list_opcode_version_rc);
		view->modified_len = ntohl(response->modified_len);
	} else {
		return -EBADMSG;
	}
	view->cack = ntohl(view->ack->cack_psn);
	offset = (int16_t)ntohs(view->ack->ack_psn_offset);
	view->ack_psn = view->cack + offset;
	if (view->extension) {
		offset = (int16_t)ntohs(view->extension->sack_psn_offset);
		view->sack_psn = view->cack + offset;
		view->sack_bitmap = be64toh(view->extension->sack_bitmap);
	}
	if (next == UIB_UET_NEXT_RSP_DATA_SMALL &&
	    small_request_psn != view->ack_psn)
		return -EBADMSG;
	view->response_code = packed & UIB_UET_SES_RSP_RC_MASK;
	return 0;
}

static void uet_ack_path_feedback(struct uib_uet_qp *uet,
				  const struct uet_ack_view *view,
				  struct uib_uet_outstanding *out)
{
	if (!out || !out->entropy)
		return;
	uet->paths[out->path_index].acknowledged++;
	if (uet->multipath.mode != UIB_UET_MULTIPATH_PATH_AWARE ||
	    !(uib_uet_prologue_flags(&view->ack->prologue) &
	      UIB_UET_ACK_FLAG_M))
		return;
	uet->paths[out->path_index].ecn_marks++;
	uet->paths[out->path_index].blocked_until_ns =
		uib_now_ns() + uet->multipath.ecn_hold_ns;
}

static void uet_ack_congestion_feedback(struct uib_uet_qp *uet,
					 const struct uet_ack_view *view,
					 struct uib_uet_outstanding *out)
{
	const struct uib_uet_nscc_wire_state *cc_state;
	const struct uib_uet_rccc_wire_state *credit_state;
	struct uib_uet_outstanding *pending;
	struct uib_congestion_ack event = {0};
	uint32_t cumulative_credit;
	uint32_t received_units = 0;
	uint32_t service_time_ns = 0;
	uint32_t delta;
	uint16_t cc_fields;
	uint8_t cc_type;
	uint8_t restore = 0;
	uint8_t penalty = 0;

	if (!uib_congestion_enabled(&uet->congestion) || !view->extension)
		return;
	cc_fields = ntohs(view->extension->cc_type_flags_mpr);
	cc_type = (cc_fields >> UIB_UET_CC_TYPE_SHIFT) &
		UIB_UET_CC_TYPE_MASK;
	if (cc_type == UIB_UET_CC_TYPE_CREDIT) {
		credit_state = (const struct uib_uet_rccc_wire_state *)
			&view->extension->ack_cc_state;
		uib_uet_rccc_wire_unpack(credit_state, &cumulative_credit, NULL);
		for (pending = uet->ack_head; pending;
		     pending = pending->ack_next) {
			if (uet_psn_before(view->cack, pending->psn))
				break;
			if (UINT32_MAX - event.newly_received <
			    pending->nominal_len) {
				event.newly_received = UINT32_MAX;
				break;
			}
			event.newly_received += pending->nominal_len;
		}
		if (out && uet_out_requires_response(out))
			event.newly_received =
				UINT32_MAX - event.newly_received < out->nominal_len ?
				UINT32_MAX : event.newly_received + out->nominal_len;
		event.credit_valid = 1;
		event.cumulative_credit = cumulative_credit;
		goto common;
	}
	if (cc_type != UIB_UET_CC_TYPE_NSCC)
		return;
	cc_state = (const struct uib_uet_nscc_wire_state *)
		&view->extension->ack_cc_state;
	uib_uet_nscc_wire_unpack(cc_state, &service_time_ns, &restore,
				 &penalty, &received_units, NULL);
	delta = (received_units - uet->previous_received_units) &
		UIB_UET_NSCC_RCVD_MASK;
	if (delta < 0x00800000U) {
		event.newly_received = delta << 8;
		uet->previous_received_units = received_units;
	}
	event.service_time_ns = service_time_ns;
	event.receiver_cwnd_penalty = penalty;
	event.restore_cwnd = restore;
common:
	event.ack_time_ns = uib_now_ns();
	event.ecn_marked = !!(uib_uet_prologue_flags(&view->ack->prologue) &
			       UIB_UET_ACK_FLAG_M);
	event.retransmit = !!(uib_uet_prologue_flags(&view->ack->prologue) &
			       UIB_UET_ACK_FLAG_RETX);
	if (out) {
		event.tx_time_ns = out->tx_time_ns;
		event.tx_retries = out->retries;
	} else {
		event.tx_retries = 2;
	}
	uib_congestion_on_ack(&uet->congestion, &event);
}

static void uet_complete_cumulative_locked(struct uib_qp *qp, uint32_t cack)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;

	for (out = uet->ack_head; out; out = next) {
		next = out->ack_next;
		if (uet_psn_before(cack, out->psn))
			break;
		uet_complete_out_locked(qp, out, UIB_WC_SUCCESS);
	}
}

static void uet_apply_sack_locked(struct uib_qp *qp,
				  const struct uet_ack_view *view)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;
	uint64_t bitmap;
	uint32_t psn;
	uint32_t bit;

	bitmap = view->sack_bitmap;
	for (bit = 0; bitmap; bit++, bitmap >>= 1) {
		if (!(bitmap & 1))
			continue;
		psn = view->sack_psn + bit;
		out = uet_find_outstanding(uet, psn, 0);
		if (!out || out->sack_acked)
			continue;
		out->sack_acked = 1;
		if (out->sent) {
			out->sent = 0;
			uet->sent_count--;
		}
	}
	uet->tx_cursor = uet_first_unsent(uet);
}

static int uet_atomic_response_data(const struct uib_uet_wr_track *track,
				    const struct uet_ack_view *view,
				    uint8_t *storage,
				    const uint8_t **result)
{
	uint64_t value;
	size_t width;
	size_t i;

	*result = view->data;
	if (track->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    track->opcode == UIB_WR_ATOMIC_CMP_SWAP) {
		if (view->data_len != sizeof(value))
			return -EBADMSG;
		memcpy(&value, view->data, sizeof(value));
		value = be64toh(value);
		memcpy(storage, &value, sizeof(value));
		*result = storage;
		return 0;
	}
	if (track->opcode != UIB_WR_UET_ATOMIC)
		return 0;
	width = uib_uet_atomic_datatype_size(track->atomic_datatype);
	if (view->data_len != width)
		return -EBADMSG;
	for (i = 0; i < width; i++)
		storage[i] = view->data[width - i - 1];
	*result = storage;
	return 0;
}

static int uet_apply_read_response_locked(struct uib_qp *qp,
					  struct uib_uet_outstanding *out,
					  const struct uet_ack_view *view)
{
	uint8_t atomic_storage[16];
	const uint8_t *result;
	size_t consumed = 0;
	int rc;

	if (!out->track->total_len && !view->has_data_response &&
	    !view->modified_len) {
		uet_complete_out_locked(qp, out, UIB_WC_SUCCESS);
		return 0;
	}
	if (!view->has_data_response ||
	    view->modified_len != out->track->total_len ||
	    view->message_offset > view->modified_len ||
	    view->data_len > view->modified_len - view->message_offset) {
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
		return -EBADMSG;
	}
	rc = uet_track_read_fragment(out->track, view->message_offset,
				     (uint32_t)view->data_len);
	if (rc == 1)
		return 0;
	if (rc < 0) {
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
		return rc;
	}
	rc = uet_atomic_response_data(out->track, view, atomic_storage,
				      &result);
	if (rc < 0) {
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
		return rc;
	}
	rc = uib_qp_scatter(qp->pd, out->track->read_sgl,
			    out->track->read_num_sge, view->message_offset,
			    result, view->data_len, &consumed);
	if (rc < 0 || consumed != view->data_len) {
		uet_remove_track_locked(qp, out->track, UIB_WC_LOC_PROT_ERR);
		return rc < 0 ? rc : -EMSGSIZE;
	}
	if (out->track->read_received == out->track->total_len)
		uet_complete_out_locked(qp, out, UIB_WC_SUCCESS);
	return 0;
}

static enum uib_wc_status uet_response_wc_status(uint8_t response_code)
{
	switch (response_code) {
	case UIB_UET_SES_RC_AT_INVALID:
	case UIB_UET_SES_RC_AT_PERM:
	case UIB_UET_SES_RC_AT_ATS_ERROR:
	case UIB_UET_SES_RC_AT_NO_TRANS:
	case UIB_UET_SES_RC_AT_OUT_OF_RANGE:
	case UIB_UET_SES_RC_PERM_VIOLATION:
	case UIB_UET_SES_RC_OP_VIOLATION:
	case UIB_UET_SES_RC_BAD_INDEX:
	case UIB_UET_SES_RC_BAD_PID:
	case UIB_UET_SES_RC_BAD_JOB_ID:
	case UIB_UET_SES_RC_BAD_MKEY:
	case UIB_UET_SES_RC_BAD_ADDR:
		return UIB_WC_REM_ACCESS_ERR;
	default:
		return UIB_WC_REM_OP_ERR;
	}
}

static void uet_mark_retransmit(void *packet)
{
	struct uib_uet_prologue *prologue;
	uint16_t value;

	prologue = packet;
	value = ntohs(prologue->type_next_flags);
	value |= UIB_UET_REQ_FLAG_RETX | UIB_UET_REQ_FLAG_AR;
	prologue->type_next_flags = htons(value);
}

static int uet_handle_ack(struct uib_qp *qp, const void *packet, size_t len)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;
	struct uet_ack_view view;
	enum uib_wc_status status;
	int clear_required;
	int rc;

	rc = uet_parse_ack(packet, len, &view);
	if (rc < 0)
		return rc;
	uet_debug(qp, "rx ack psn=%u cack=%u msg=%u rc=%u", view.ack_psn,
		  view.cack, view.msg_id, view.response_code);
	pthread_mutex_lock(&qp->lock);
	uet->dpdcid = ntohs(view.ack->spdcid);
	uet->state = UIB_UET_PDC_ESTABLISHED;
	out = uet_find_outstanding(uet, view.ack_psn, view.msg_id);
	uet_ack_path_feedback(uet, &view, out);
	uet_ack_congestion_feedback(uet, &view, out);
	clear_required = !!(uib_uet_prologue_flags(&view.ack->prologue) &
			    UIB_UET_ACK_FLAG_REQ_CLEAR);
	if (clear_required &&
	    uet_psn_before(uet->tx_clear_psn, view.ack_psn))
		uet->tx_clear_psn = view.ack_psn;
	if (view.response_code != UIB_UET_SES_RC_OK) {
		if (out) {
			out->track->response_code = view.response_code;
			status = uet_response_wc_status(view.response_code);
			uet_remove_track_locked(qp, out->track, status);
		}
		uet_refill_locked(qp);
		pthread_mutex_unlock(&qp->lock);
		if (clear_required)
			uet_send_control(qp, &qp->remote_addr,
					 UIB_UET_CONTROL_CLEAR_COMMAND,
					 htonl(view.ack_psn));
		return out ? -EREMOTEIO : 0;
	}
	uet_complete_cumulative_locked(qp, view.cack);
	if (view.extension)
		uet_apply_sack_locked(qp, &view);
	out = uet_find_outstanding(uet, view.ack_psn, view.msg_id);
	if (!out) {
		rc = 0;
	} else if (uet_out_requires_response(out)) {
		rc = uet_apply_read_response_locked(qp, out, &view);
	} else {
		uet_complete_out_locked(qp, out, UIB_WC_SUCCESS);
		rc = 0;
	}
	uet_refill_locked(qp);
	pthread_mutex_unlock(&qp->lock);
	if (clear_required)
		uet_send_control(qp, &qp->remote_addr,
				 UIB_UET_CONTROL_CLEAR_COMMAND,
				 htonl(view.ack_psn));
	return rc;
}

static int uet_handle_nack(struct uib_qp *qp, const void *packet, size_t len)
{
	const struct uib_uet_pds_nack *nack = packet;
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;
	int found;

	if (len < sizeof(*nack))
		return -EBADMSG;
	pthread_mutex_lock(&qp->lock);
	out = uet_find_outstanding(uet, ntohl(nack->nack_psn), 0);
	found = !!out;
	if (out) {
		struct uib_congestion_nack event = {
			.bytes = out->nominal_len,
			.now_ns = uib_now_ns(),
			.trimmed = nack->nack_code == UIB_UET_NACK_TRIMMED ||
				nack->nack_code == UIB_UET_NACK_TRIMMED_LASTHOP,
			.last_hop =
				nack->nack_code == UIB_UET_NACK_TRIMMED_LASTHOP,
		};

		uib_congestion_on_nack(&uet->congestion, &event);
		uet_remove_track_locked(qp, out->track, UIB_WC_REM_OP_ERR);
		uet_refill_locked(qp);
	}
	pthread_mutex_unlock(&qp->lock);
	return found ? -EREMOTEIO : 0;
}

static int uet_handle_credit_request(struct uib_qp *qp,
				     const struct sockaddr_in *source,
				     const struct uib_uet_pds_control *control)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	uint32_t cumulative_credit;
	uint32_t credit_target;
	uint32_t payload;

	if (!uet->rccc_enabled)
		return -EOPNOTSUPP;
	uib_uet_rccc_request_unpack(control->payload, NULL, &credit_target);
	pthread_mutex_lock(&qp->lock);
	uet->dpdcid = ntohs(control->spdcid);
	uet->state = UIB_UET_PDC_ESTABLISHED;
	cumulative_credit = uib_uet_rccc_destination_update(
		&uet->rccc_destination, credit_target, uib_now_ns());
	pthread_mutex_unlock(&qp->lock);
	payload = uet_rccc_credit_pack(cumulative_credit);
	return uet_send_control(qp, source, UIB_UET_CONTROL_CREDIT, payload);
}

static int uet_handle_credit(struct uib_qp *qp,
			     const struct uib_uet_pds_control *control)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_rccc *rccc;
	uint32_t cumulative_credit;
	int rc;

	if (!uet->rccc_enabled)
		return -EOPNOTSUPP;
	cumulative_credit = uet_rccc_credit_unpack(control->payload);
	pthread_mutex_lock(&qp->lock);
	rccc = (struct uib_uet_rccc *)uet->congestion.state.bytes;
	uib_uet_rccc_on_credit(rccc, cumulative_credit);
	uet->credit_request_deadline_ns = 0;
	rc = uet_drain_locked(qp);
	pthread_mutex_unlock(&qp->lock);
	return rc;
}

static int uet_handle_control(struct uib_qp *qp,
			      const struct sockaddr_in *source,
			      const void *packet, size_t len)
{
	const struct uib_uet_pds_control *control = packet;
	uint8_t control_type;

	if (len < sizeof(*control))
		return -EBADMSG;
	if (control->psn)
		return -EBADMSG;
	control_type = uib_uet_prologue_next(&control->prologue);
	switch (control_type) {
	case UIB_UET_CONTROL_CREDIT_REQUEST:
		return uet_handle_credit_request(qp, source, control);
	case UIB_UET_CONTROL_CREDIT:
		return uet_handle_credit(qp, control);
	case UIB_UET_CONTROL_CLEAR_COMMAND:
		pthread_mutex_lock(&qp->lock);
		uet_clear_guaranteed(qp->protocol_private,
				     ntohl(control->payload));
		pthread_mutex_unlock(&qp->lock);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int uet_handle_packet(struct uib_qp *qp,
			     const struct sockaddr_in *src,
			     const void *packet, size_t len,
			     const struct uib_rx_meta *meta)
{
	const struct uib_uet_prologue *prologue = packet;

	if (!packet || len < sizeof(*prologue))
		return -EBADMSG;
	switch (uib_uet_prologue_type(prologue)) {
	case UIB_UET_PDS_TYPE_RUD_REQ:
	case UIB_UET_PDS_TYPE_RUD_CC_REQ:
		return uet_handle_request(qp, src, packet, len,
					  meta && meta->ecn == IPTOS_ECN_CE);
	case UIB_UET_PDS_TYPE_ACK:
	case UIB_UET_PDS_TYPE_ACK_CC:
		return uet_handle_ack(qp, packet, len);
	case UIB_UET_PDS_TYPE_NACK:
		return uet_handle_nack(qp, packet, len);
	case UIB_UET_PDS_TYPE_CTRL_MSG:
		return uet_handle_control(qp, src, packet, len);
	default:
		return -EOPNOTSUPP;
	}
}

static struct uib_uet_wr_track *uet_alloc_track(struct uib_qp *qp,
						struct uib_send_wr *wr,
						uint32_t total_len,
						uint32_t packets)
{
	struct uib_uet_qp *uet;
	struct uib_uet_wr_track *track;

	uet = qp->protocol_private;
	track = uet_track_pool_pop(uet->track_pool);
	if (!track)
		return NULL;
	track->read_fragments = track->inline_read_fragments;
	track->read_fragment_capacity = UIB_UET_READ_FRAGMENTS_INLINE;
	track->wr_id = wr->wr_id;
	track->opcode = wr->opcode;
	track->signaled = !!(wr->send_flags & UIB_SEND_SIGNALED);
	track->total_len = total_len;
	track->pending = packets;
	if (wr->opcode == UIB_WR_RDMA_READ ||
	    wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
	    (wr->opcode == UIB_WR_UET_ATOMIC &&
	     wr->atomic.uet_fetching)) {
		track->read_sgl = track->inline_read_sgl;
		memcpy(track->read_sgl, wr->sg_list,
		       wr->num_sge * sizeof(*track->read_sgl));
		track->read_num_sge = wr->num_sge;
	}
	if (wr->opcode == UIB_WR_UET_ATOMIC) {
		track->uet_atomic = 1;
		track->atomic_datatype = wr->atomic.uet_datatype;
	}
	return track;
}

struct uet_post_context {
	struct uib_qp *qp;
	struct uib_uet_qp *uet;
	struct uib_uet_wr_track *track;
	struct sockaddr_in destination;
	const uint8_t *message;
	uint8_t *owned_message;
	struct uib_uet_outstanding *owned_message_buffer;
	struct uib_mr *mr;
	uint32_t total_len;
	uint32_t chunk_max;
	uint32_t packets;
	uint32_t first_psn;
	uint32_t clear_psn;
	uint16_t msg_id;
	uint8_t ses_opcode;
	uint8_t request_header;
	uint8_t fetching_atomic;
	uint8_t send_syn;
	size_t pds_len;
};

static void uet_post_context_release(struct uet_post_context *context)
{
	uet_free_track(context->track);
	uib_mr_put(context->mr);
	if (context->owned_message_buffer)
		uet_free_outstanding(context->owned_message_buffer);
	else
		free(context->owned_message);
}

static int uet_ses_opcode(const struct uib_send_wr *wr, uint8_t *opcode)
{
	switch (wr->opcode) {
	case UIB_WR_SEND:
	case UIB_WR_SEND_WITH_IMM:
		*opcode = UIB_UET_SES_SEND;
		return 0;
	case UIB_WR_RDMA_WRITE:
	case UIB_WR_RDMA_WRITE_WITH_IMM:
		*opcode = UIB_UET_SES_WRITE;
		return 0;
	case UIB_WR_RDMA_READ:
		*opcode = UIB_UET_SES_READ;
		return 0;
	case UIB_WR_ATOMIC_FETCH_ADD:
	case UIB_WR_ATOMIC_CMP_SWAP:
		*opcode = UIB_UET_SES_FETCHING_ATOMIC;
		return 0;
	case UIB_WR_UET_ATOMIC:
		*opcode = wr->atomic.uet_fetching ?
			UIB_UET_SES_FETCHING_ATOMIC : UIB_UET_SES_ATOMIC;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int uet_post_total(const struct uib_send_wr *wr, uint32_t *total)
{
	int i;

	*total = 0;
	for (i = 0; i < wr->num_sge; i++) {
		if (UINT32_MAX - *total < wr->sg_list[i].length)
			return -EMSGSIZE;
		*total += wr->sg_list[i].length;
	}
	return 0;
}

static int uet_prepare_message(struct uib_qp *qp, struct uib_send_wr *wr,
			       struct uet_post_context *context)
{
	int rc;

	if (wr->opcode == UIB_WR_RDMA_READ || context->fetching_atomic)
		return 0;
	if (wr->num_sge == 1 && !(wr->send_flags & UIB_SEND_INLINE)) {
		context->mr = uib_mr_lookup_local(
			qp->pd, wr->sg_list[0].lkey,
			(const void *)(uintptr_t)wr->sg_list[0].addr,
			wr->sg_list[0].length, 0);
		if (!context->mr)
			return -EACCES;
		if (context->mr->kind == UIB_MR_HOST) {
			context->message = (const uint8_t *)(uintptr_t)
				wr->sg_list[0].addr;
			return 0;
		}
		uib_mr_put(context->mr);
		context->mr = NULL;
	}
	context->owned_message_buffer = uet_alloc_outstanding(
		context->uet, context->total_len ? context->total_len : 1);
	if (context->owned_message_buffer)
		context->owned_message = context->owned_message_buffer->packet;
	else
		context->owned_message = malloc(context->total_len ?
							context->total_len : 1);
	if (!context->owned_message)
		return -ENOMEM;
	rc = uib_qp_gather(qp->pd, wr->sg_list, wr->num_sge,
			   context->owned_message, context->total_len);
	if (rc < 0)
		return rc;
	context->message = context->owned_message;
	return 0;
}

static size_t uet_request_header_len(uint8_t request_header)
{
	if (request_header == UIB_UET_NEXT_REQ_SMALL)
		return sizeof(struct uib_uet_ses_req_small);
	if (request_header == UIB_UET_NEXT_REQ_MEDIUM)
		return sizeof(struct uib_uet_ses_req_medium);
	return sizeof(struct uib_uet_ses_req);
}

static size_t uet_request_data_len(const struct uib_send_wr *wr,
				   uint32_t chunk)
{
	size_t width;

	if (wr->opcode == UIB_WR_RDMA_READ)
		return 0;
	if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD)
		return sizeof(struct uib_uet_ses_atomic) + 8;
	if (wr->opcode == UIB_WR_ATOMIC_CMP_SWAP)
		return sizeof(struct uib_uet_ses_atomic) + 32;
	if (wr->opcode != UIB_WR_UET_ATOMIC)
		return chunk;
	width = uib_uet_atomic_datatype_size(wr->atomic.uet_datatype);
	if (uet_atomic_has_two_operands(wr->atomic.uet_opcode))
		width *= 2;
	else if (wr->atomic.uet_opcode == UIB_UET_AMO_READ)
		width = 0;
	return sizeof(struct uib_uet_ses_atomic) + width;
}

static int uet_prepare_post(struct uib_qp *qp, struct uib_send_wr *wr,
			    struct uet_post_context *context)
{
	uint32_t response_limit;
	int rc;

	memset(context, 0, sizeof(*context));
	context->qp = qp;
	context->uet = qp->protocol_private;
	if (wr->num_sge < 0 || (uint32_t)wr->num_sge > qp->max_send_sge ||
	    (wr->num_sge && !wr->sg_list))
		return -EINVAL;
	context->destination = qp->remote_addr;
	context->request_header = UIB_UET_NEXT_REQ_STD;
	context->fetching_atomic = wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
		(wr->opcode == UIB_WR_UET_ATOMIC && wr->atomic.uet_fetching);
	context->pds_len = context->uet->rccc_enabled ?
		sizeof(struct uib_uet_pds_cc_req) :
		sizeof(struct uib_uet_pds_req);
	rc = uet_post_total(wr, &context->total_len);
	if (rc < 0)
		return rc;
	rc = uet_ses_opcode(wr, &context->ses_opcode);
	if (rc < 0)
		return rc;
	rc = uet_prepare_message(qp, wr, context);
	if (rc < 0)
		return rc;
	if (qp->path_mtu <= context->pds_len +
	    sizeof(struct uib_uet_ses_req))
		return -EMSGSIZE;
	context->chunk_max = qp->path_mtu - context->pds_len -
		sizeof(struct uib_uet_ses_req);
	response_limit = qp->path_mtu - sizeof(struct uib_uet_pds_ack) -
		sizeof(struct uib_uet_ses_rsp);
	if (wr->opcode == UIB_WR_RDMA_READ &&
	    context->chunk_max > response_limit)
		context->chunk_max = response_limit;
	context->packets = wr->opcode == UIB_WR_RDMA_READ ||
		context->fetching_atomic ? 1 :
		(context->total_len ?
		 (context->total_len + context->chunk_max - 1) /
		 context->chunk_max : 1);
	if (wr->opcode != UIB_WR_RDMA_WRITE_WITH_IMM &&
	    context->packets == 1 &&
	    context->total_len <= UIB_UET_SES_LENGTH_MASK &&
	    context->uet->optimized_mode != UIB_UET_OPTIMIZED_OFF) {
		context->request_header =
			context->uet->optimized_mode == UIB_UET_OPTIMIZED_SMALL &&
			wr->opcode != UIB_WR_SEND &&
			wr->opcode != UIB_WR_SEND_WITH_IMM ?
			UIB_UET_NEXT_REQ_SMALL : UIB_UET_NEXT_REQ_MEDIUM;
		if (wr->opcode == UIB_WR_RDMA_READ &&
		    context->total_len + sizeof(struct uib_uet_pds_ack) +
		    sizeof(struct uib_uet_pds_ack_ext) +
		    sizeof(struct uib_uet_ses_rsp_data_small) > qp->path_mtu)
			context->request_header = UIB_UET_NEXT_REQ_STD;
	}
	context->track = uet_alloc_track(qp, wr, context->total_len,
					 context->packets);
	if (!context->track)
		return -ENOMEM;
	if (!context->destination.sin_port)
		context->destination.sin_port = htons(UIB_UET_DEFAULT_PORT);
	return 0;
}

static void uet_reserve_post(struct uib_qp *qp,
			     struct uet_post_context *context)
{
	pthread_mutex_lock(&qp->lock);
	context->msg_id = context->uet->next_msg_id++;
	if (!context->uet->tx_initialized) {
		context->uet->next_psn = qp->sq_psn;
		context->uet->publish_psn = qp->sq_psn;
		context->uet->tx_clear_psn = qp->sq_psn - 1;
		context->uet->tx_initialized = 1;
	}
	context->first_psn = context->uet->next_psn;
	context->uet->next_psn += context->packets;
	qp->sq_psn = context->uet->next_psn;
	context->clear_psn = context->uet->tx_clear_psn;
	context->send_syn = context->uet->state == UIB_UET_PDC_CLOSED;
	if (context->send_syn)
		context->uet->state = UIB_UET_PDC_SYN_SENT;
	pthread_mutex_unlock(&qp->lock);
}

static void uet_store_post(struct uib_send_wr *wr,
			   struct uet_post_context *context)
{
	struct uib_uet_wr_track *track = context->track;

	track->qp = context->qp;
	track->wr = *wr;
	track->wr.next = NULL;
	track->wr.sg_list = NULL;
	track->packets = context->packets;
	track->next_packet = 0;
	track->first_psn = context->first_psn;
	track->clear_psn = context->clear_psn;
	track->chunk_max = context->chunk_max;
	track->destination = context->destination;
	track->message = context->message;
	track->owned_message = context->owned_message;
	track->owned_message_buffer = context->owned_message_buffer;
	track->mr = context->mr;
	track->msg_id = context->msg_id;
	track->ses_opcode = context->ses_opcode;
	track->request_header = context->request_header;
	track->fetching_atomic = context->fetching_atomic;
	track->send_syn = context->send_syn;
	track->pds_len = context->pds_len;
	track->scheduled_packet = 0;
	context->owned_message = NULL;
	context->owned_message_buffer = NULL;
	context->mr = NULL;
}

static void uet_load_post(struct uib_uet_qp *uet,
			  struct uib_uet_wr_track *track,
			  struct uet_post_context *context)
{
	memset(context, 0, sizeof(*context));
	context->qp = track->qp;
	context->uet = uet;
	context->track = track;
	context->destination = track->destination;
	context->message = track->message;
	context->total_len = track->total_len;
	context->chunk_max = track->chunk_max;
	context->packets = track->packets;
	context->first_psn = track->first_psn;
	context->clear_psn = track->clear_psn;
	context->msg_id = track->msg_id;
	context->ses_opcode = track->ses_opcode;
	context->request_header = track->request_header;
	context->fetching_atomic = track->fetching_atomic;
	context->send_syn = track->send_syn;
	context->pds_len = track->pds_len;
}

static void uet_encode_atomic(struct uib_send_wr *wr,
			      struct uib_uet_ses_req *ses,
			      uint8_t *wire_data, size_t data_len)
{
	struct uib_uet_ses_atomic *atomic;
	uint8_t *operands;
	uint64_t value;
	size_t width;
	size_t i;

	atomic = (struct uib_uet_ses_atomic *)wire_data;
	operands = (uint8_t *)(atomic + 1);
	ses->buffer_offset = htobe64(wr->atomic.remote_addr);
	ses->match_bits = htobe64(wr->atomic.rkey);
	ses->request_len = htonl(wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ? 32 : 8);
	atomic->datatype = UIB_UET_AMO_UINT64;
	if (wr->opcode == UIB_WR_UET_ATOMIC) {
		width = uib_uet_atomic_datatype_size(wr->atomic.uet_datatype);
		atomic->opcode = wr->atomic.uet_opcode;
		atomic->datatype = wr->atomic.uet_datatype;
		atomic->semantic_control = wr->atomic.uet_semantic_control;
		if (atomic->opcode != UIB_UET_AMO_READ) {
			for (i = 0; i < width; i++)
				operands[i] = wr->atomic.uet_operand[width - i - 1];
		}
		if (uet_atomic_has_two_operands(atomic->opcode)) {
			for (i = 0; i < width; i++) {
				operands[i] = wr->atomic.uet_compare[width - i - 1];
				operands[width + i] =
					wr->atomic.uet_swap[width - i - 1];
			}
		}
		ses->request_len = htonl((uint32_t)(data_len - sizeof(*atomic)));
	} else if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD) {
		atomic->opcode = UIB_UET_AMO_SUM;
		value = htobe64(wr->atomic.compare_add);
		memcpy(operands, &value, sizeof(value));
	} else {
		atomic->opcode = UIB_UET_AMO_CSWAP;
		value = htobe64(wr->atomic.compare_add);
		memcpy(operands + 8, &value, sizeof(value));
		value = htobe64(wr->atomic.swap);
		memcpy(operands + 24, &value, sizeof(value));
	}
}

static void uet_encode_ses_header(uint8_t request_header, void *wire,
				  const struct uib_uet_ses_req *ses)
{
	struct uib_uet_ses_req_medium *medium;
	struct uib_uet_ses_req_small *small;

	if (request_header == UIB_UET_NEXT_REQ_STD) {
		memcpy(wire, ses, sizeof(*ses));
		return;
	}
	if (request_header == UIB_UET_NEXT_REQ_MEDIUM) {
		medium = wire;
		medium->opcode = ses->opcode;
		medium->flags = ses->flags;
		medium->request_len = htons((uint16_t)
			(ntohl(ses->request_len) & UIB_UET_SES_LENGTH_MASK));
		medium->index_gen_job_id = ses->index_gen_job_id;
		medium->pid_on_fep = ses->pid_on_fep;
		medium->index = ses->index;
		medium->buffer_offset = ses->opcode == UIB_UET_SES_SEND ?
			ses->header_data : ses->buffer_offset;
		medium->initiator = ses->initiator;
		medium->match_bits = ses->match_bits;
		return;
	}
	small = wire;
	small->opcode = ses->opcode;
	small->flags = ses->flags;
	small->request_len = htons((uint16_t)
		(ntohl(ses->request_len) & UIB_UET_SES_LENGTH_MASK));
	small->index_gen_job_id = ses->index_gen_job_id;
	small->pid_on_fep = ses->pid_on_fep;
	small->index = ses->index;
	small->buffer_offset = ses->buffer_offset;
}

static void uet_init_ses(struct uib_qp *qp, struct uib_send_wr *wr,
			 const struct uet_post_context *context,
			 struct uib_uet_outstanding *out,
			 struct uib_uet_ses_req *ses, uint32_t packet_index,
			 uint32_t chunk, uint32_t offset, uint8_t *wire_data,
			 size_t data_len)
{
	ses->opcode = context->ses_opcode;
	if (context->uet->session.relative_addressing)
		ses->flags |= UIB_UET_SES_FLAG_REL;
	if (!packet_index)
		ses->flags |= UIB_UET_SES_FLAG_SOM;
	if (packet_index == context->packets - 1)
		ses->flags |= UIB_UET_SES_FLAG_EOM;
	ses->msg_id = htons(out->msg_id);
	ses->index_gen_job_id = htonl(
		((uint32_t)context->uet->session.resource_generation << 24) |
		context->uet->session.job_id);
	ses->pid_on_fep = htons((uint16_t)qp->dest_qp_num &
				 UIB_UET_SES_PID_MASK);
	ses->index = htons(context->uet->session.resource_index);
	ses->initiator = htonl(context->uet->session.initiator_id);
	ses->request_len = htonl(context->total_len);
	if (wr->opcode == UIB_WR_SEND || wr->opcode == UIB_WR_SEND_WITH_IMM) {
		if (!packet_index && wr->opcode == UIB_WR_SEND_WITH_IMM) {
			ses->flags |= UIB_UET_SES_FLAG_HD;
			ses->header_data = htobe64(wr->imm_data);
		}
	} else if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
		   wr->opcode == UIB_WR_ATOMIC_CMP_SWAP ||
		   wr->opcode == UIB_WR_UET_ATOMIC) {
		uet_encode_atomic(wr, ses, wire_data, data_len);
	} else {
		ses->buffer_offset = htobe64(wr->rdma.remote_addr);
		ses->match_bits = htobe64(wr->rdma.rkey);
		if (!packet_index &&
		    wr->opcode == UIB_WR_RDMA_WRITE_WITH_IMM) {
			ses->flags |= UIB_UET_SES_FLAG_HD;
			ses->header_data = htobe64(wr->imm_data);
		}
	}
	if (packet_index)
		ses->header_data = uib_uet_ses_fragment_info(chunk, offset);
}

static void uet_assign_request_identity(struct uet_post_context *context,
					struct uib_uet_outstanding *out,
					uint32_t packet_index,
					uint8_t *flags,
					uint16_t *clear_psn_offset)
{
	int32_t offset;

	if (context->send_syn && !packet_index)
		*flags |= UIB_UET_REQ_FLAG_SYN;
	out->psn = context->first_psn + packet_index;
	out->msg_id = context->request_header == UIB_UET_NEXT_REQ_STD ?
		context->msg_id : (uint16_t)out->psn;
	out->request_header = context->request_header;
	offset = (int32_t)(context->clear_psn - out->psn);
	if (offset < INT16_MIN)
		offset = INT16_MIN;
	if (offset > 0)
		offset = 0;
	*clear_psn_offset = htons((uint16_t)(int16_t)offset);
}

struct uet_request_layout {
	size_t header_len;
	size_t packet_len;
	size_t data_len;
	uint32_t offset;
	uint32_t chunk;
};

static int uet_request_layout(struct uib_qp *qp,
			      const struct uib_send_wr *wr,
			      const struct uet_post_context *context,
			      uint32_t packet_index,
			      struct uet_request_layout *layout)
{
	layout->offset = packet_index * context->chunk_max;
	layout->chunk = wr->opcode == UIB_WR_RDMA_READ ||
		context->fetching_atomic ? context->total_len :
		context->total_len - layout->offset;
	if (wr->opcode != UIB_WR_RDMA_READ && !context->fetching_atomic &&
	    layout->chunk > context->chunk_max)
		layout->chunk = context->chunk_max;
	layout->header_len = uet_request_header_len(context->request_header);
	layout->data_len = uet_request_data_len(wr, layout->chunk);
	layout->packet_len = context->pds_len + layout->header_len +
		layout->data_len;
	return layout->packet_len > qp->path_mtu ? -EMSGSIZE : 0;
}

static struct uib_uet_outstanding *
uet_allocate_request(struct uib_qp *qp, struct uib_send_wr *wr,
		     struct uet_post_context *context, uint32_t packet_index)
{
	struct uet_request_layout layout;
	struct uib_uet_outstanding *out;
	size_t allocation_len;
	int direct_payload;
	int rc;

	rc = uet_request_layout(qp, wr, context, packet_index, &layout);
	if (rc < 0)
		return NULL;
	direct_payload = context->mr && layout.data_len &&
		layout.data_len == layout.chunk;
	allocation_len = direct_payload ? layout.packet_len - layout.data_len :
		layout.packet_len;
	out = uet_alloc_outstanding(context->uet, allocation_len);
	if (!out)
		return NULL;
	out->track = context->track;
	out->dst = context->destination;
	out->packet_len = layout.packet_len;
	out->header_len = allocation_len;
	out->read_offset = layout.offset;
	out->read_len = layout.chunk;
	out->nominal_len = (uint32_t)layout.packet_len + 40;
	if (direct_payload) {
		out->payload = context->message + layout.offset;
		out->payload_len = layout.data_len;
		out->payload_mr = context->mr;
		uib_mr_get(out->payload_mr);
	}
	return out;
}

static void uet_encode_request(struct uib_qp *qp, struct uib_send_wr *wr,
			       struct uet_post_context *context,
			       struct uib_uet_outstanding *out,
			       uint32_t packet_index)
{
	struct uib_uet_ses_req ses = {0};
	struct uib_uet_pds_cc_req *cc_request;
	struct uib_uet_pds_req *request;
	void *wire_header;
	uint8_t *wire_data;
	size_t header_len;
	size_t data_len;
	uint32_t offset;
	uint32_t chunk;
	uint32_t target;
	uint8_t flags = 0;
	uint16_t clear_psn_offset;

	if (!((packet_index + 1) % UIB_UET_ACK_INTERVAL) ||
	    packet_index == context->packets - 1)
		flags |= UIB_UET_REQ_FLAG_AR;
	offset = out->read_offset;
	chunk = out->read_len;
	header_len = uet_request_header_len(context->request_header);
	data_len = out->payload_len ? out->payload_len :
		out->packet_len - context->pds_len - header_len;
	request = (struct uib_uet_pds_req *)out->packet;
	wire_header = out->packet + context->pds_len;
	wire_data = (uint8_t *)wire_header + header_len;
	uet_assign_request_identity(context, out, packet_index, &flags,
				    &clear_psn_offset);
	request->prologue.type_next_flags = uib_uet_prologue_pack(
		context->uet->rccc_enabled ? UIB_UET_PDS_TYPE_RUD_CC_REQ :
		UIB_UET_PDS_TYPE_RUD_REQ, context->request_header, flags);
	request->psn = htonl(out->psn);
	request->clear_psn_offset = clear_psn_offset;
	request->spdcid = htons(context->uet->spdcid);
	request->dpdcid = htons((uint16_t)qp->dest_qp_num);
	if (context->uet->rccc_enabled) {
		cc_request = (struct uib_uet_pds_cc_req *)request;
		target = uib_uet_rccc_credit_target(
			(struct uib_uet_rccc *)context->uet->congestion.state.bytes);
		cc_request->req_cc_state = uib_uet_rccc_request_pack(
			(uint8_t)context->uet->spdcid, target);
	}
	uet_init_ses(qp, wr, context, out, &ses, packet_index, chunk, offset,
		     wire_data, data_len);
	if (data_len && !out->payload_len &&
	    wr->opcode != UIB_WR_ATOMIC_FETCH_ADD &&
	    wr->opcode != UIB_WR_ATOMIC_CMP_SWAP &&
	    wr->opcode != UIB_WR_UET_ATOMIC)
		memcpy(wire_data, context->message + offset, data_len);
	uet_encode_ses_header(context->request_header, wire_header, &ses);
	uet_debug(qp, "tx request psn=%u msg=%u opcode=%u len=%zu",
		  out->psn, out->msg_id, context->ses_opcode, data_len);
}

static int uet_materialize_locked(struct uib_qp *qp,
				  struct uib_uet_wr_track **failed)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uet_post_context context;
	struct uib_uet_wr_track *track;
	struct uib_uet_outstanding *out;

	*failed = NULL;
	for (track = uet->track_head;
	     track && uet->materialized_count < UIB_UET_PDC_MPR;
	     track = track->next) {
		if (track->scheduled_packet != track->next_packet)
			break;
		uet_load_post(uet, track, &context);
		while (track->next_packet < track->packets &&
		       uet->materialized_count < UIB_UET_PDC_MPR) {
			out = uet_allocate_request(qp, &track->wr, &context,
						   track->next_packet);
			if (!out) {
				*failed = track;
				return -ENOMEM;
			}
			uet_encode_request(qp, &track->wr, &context, out,
					   track->next_packet);
			uet->materialized_count++;
			uet_link_outstanding(uet, out);
			track->next_packet++;
			track->scheduled_packet = track->next_packet;
		}
	}
	return 0;
}

static int uet_refill_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *failed;
	struct uib_uet_rccc *rccc;
	int scheduled;
	int rc;

	scheduled = uet_schedule_refill_locked(qp);
	if (scheduled < 0)
		return scheduled;
	if (scheduled || uet->refill_head)
		return uet_drain_locked(qp);
	rccc = uet->rccc_enabled ?
		(struct uib_uet_rccc *)uet->congestion.state.bytes : NULL;
	do {
		rc = uet_materialize_locked(qp, &failed);
		if (rc >= 0)
			break;
		if (failed->rccc_accounted)
			uib_uet_rccc_on_cancel_data(rccc,
						failed->nominal_bytes);
		uet_remove_track_locked(qp, failed, UIB_WC_LOC_PROT_ERR);
	} while (uet->track_head);
	if (rc < 0)
		return rc;
	return uet_drain_locked(qp);
}

static int uet_commit_prepared_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_wr_track *track;

	while (uet->prep_head && uet->prep_head->prep_ready &&
	       uet->prep_head->first_psn == uet->publish_psn) {
		track = uet->prep_head;
		uet->prep_head = track->prep_next;
		track->prep_next = NULL;
		uet_link_track(uet, track);
		track->next_packet = 0;
		if (track->request_header == UIB_UET_NEXT_REQ_SMALL)
			uet->optimized_stats.small_requests += track->packets;
		else if (track->request_header == UIB_UET_NEXT_REQ_MEDIUM)
			uet->optimized_stats.medium_requests += track->packets;
		else
			uet->optimized_stats.standard_requests += track->packets;
		uet->publish_psn += track->packets;
		if (uet->prep_pending)
			uet->prep_pending--;
	}
	if (!uet->prep_pending)
		pthread_cond_broadcast(&uet->prep_cond);
	return uet_refill_locked(qp);
}

static int uet_admit_post(struct uib_uet_wr_track *track)
{
	struct uib_uet_wr_track **link;
	struct uib_uet_qp *uet;
	struct uib_qp *qp;

	qp = track->qp;
	uet = qp->protocol_private;
	pthread_mutex_lock(&qp->lock);
	if (uet->prep_closing) {
		pthread_mutex_unlock(&qp->lock);
		return -ESHUTDOWN;
	}
	link = &uet->prep_head;
	while (*link && (int32_t)((*link)->first_psn - track->first_psn) < 0)
		link = &(*link)->prep_next;
	track->prep_next = *link;
	*link = track;
	uet->prep_pending++;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static uint64_t uet_post_nominal_bytes(const struct uib_send_wr *wr,
				       const struct uet_post_context *context)
{
	uint64_t total = 0;
	uint32_t packet_index;
	uint32_t offset;
	uint32_t chunk;

	for (packet_index = 0; packet_index < context->packets; packet_index++) {
		offset = packet_index * context->chunk_max;
		chunk = wr->opcode == UIB_WR_RDMA_READ ||
			context->fetching_atomic ? context->total_len :
			context->total_len - offset;
		if (wr->opcode != UIB_WR_RDMA_READ &&
		    !context->fetching_atomic && chunk > context->chunk_max)
			chunk = context->chunk_max;
		total += context->pds_len +
			uet_request_header_len(context->request_header) +
			uet_request_data_len(wr, chunk) + 40;
	}
	return total;
}

static int uet_process_post(struct uib_uet_wr_track *track)
{
	struct uib_qp *qp;
	int rc;

	qp = track->qp;
	pthread_mutex_lock(&qp->lock);
	track->prep_ready = 1;
	rc = uet_commit_prepared_locked(qp);
	pthread_mutex_unlock(&qp->lock);
	return rc;
}

static void uet_free_task_packets(struct uib_uet_prep_task *task)
{
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;

	for (out = task->result_head; out; out = next) {
		next = out->next;
		uet_free_outstanding(out);
	}
	task->result_head = NULL;
	task->result_tail = NULL;
}

static void uet_commit_refills_locked(struct uib_qp *qp)
{
	struct uib_uet_qp *uet;
	struct uib_uet_prep_worker *worker;
	struct uib_uet_prep_task *task;
	struct uib_uet_wr_track *track;
	struct uib_uet_outstanding *next;
	struct uib_uet_outstanding *out;
	struct uib_uet_rccc *rccc;
	int cancelled;
	int remove;

	uet = qp->protocol_private;
	rccc = uet->rccc_enabled ?
		(struct uib_uet_rccc *)uet->congestion.state.bytes : NULL;
	while (uet->refill_head && uet->refill_head->ready) {
		task = uet->refill_head;
		uet->refill_head = task->refill_next;
		if (!uet->refill_head)
			uet->refill_tail = NULL;
		track = task->track;
		cancelled = __atomic_load_n(&track->cancelled,
					    __ATOMIC_ACQUIRE);
		if (task->rc < 0 && !track->refill_error)
			track->refill_error = task->rc;
		if (cancelled || track->refill_error) {
			uet_free_task_packets(task);
			uet->materialized_count -= task->packet_count;
		} else {
			for (out = task->result_head; out; out = next) {
				next = out->next;
				uet_link_outstanding(uet, out);
			}
			track->next_packet = task->packet_index +
				task->packet_count;
			task->result_head = NULL;
			task->result_tail = NULL;
		}
		if (track->refill_tasks)
			track->refill_tasks--;
		if (uet->prep_pending)
			uet->prep_pending--;
		remove = track->refill_error && !track->refill_tasks &&
			 !cancelled;
		worker = task->worker;
		memset(task, 0, sizeof(*task));
		task->worker = worker;
		uib_ptr_ring_push(&worker->free_tasks, task);
		if (remove) {
			if (track->rccc_accounted && rccc)
				uib_uet_rccc_on_cancel_data(rccc,
							track->nominal_bytes);
			uet_remove_track_locked(qp, track,
						UIB_WC_LOC_PROT_ERR);
		}
		uet_free_track(track);
	}
	if (!uet->prep_pending)
		pthread_cond_broadcast(&uet->prep_cond);
	uet_schedule_refill_locked(qp);
	uet_drain_locked(qp);
}

static struct uib_uet_prep_task *
uet_reserve_prep_task(struct uib_uet_context *context,
			      struct uib_uet_prep_worker **selected,
			      uint64_t *position, int *was_empty)
{
	struct uib_uet_prep_worker *worker;
	struct uib_uet_prep_task *task;
	unsigned int start;
	unsigned int index;
	unsigned int i;
	int rc;

	start = __atomic_fetch_add(&context->worker_cursor, 1,
				   __ATOMIC_RELAXED);
	for (i = 0; i < context->worker_count; i++) {
		index = (start + i) % context->worker_count;
		worker = &context->workers[index];
		task = uib_ptr_ring_pop(&worker->free_tasks);
		if (!task)
			continue;
		rc = uib_ptr_ring_reserve(&worker->ready, 1, position,
					  was_empty);
		if (!rc) {
			*selected = worker;
			return task;
		}
		uib_ptr_ring_push(&worker->free_tasks, task);
	}
	return NULL;
}

static void uet_process_refill_task(struct uib_uet_prep_task *task)
{
	struct uib_uet_wr_track *track;
	struct uib_uet_outstanding *out;
	struct uet_post_context post;
	struct uib_qp *qp;
	uint32_t i;
	int rc;

	track = task->track;
	qp = track->qp;
	uet_load_post(qp->protocol_private, track, &post);
	rc = 0;
	for (i = 0; i < task->packet_count; i++) {
		if (__atomic_load_n(&track->cancelled, __ATOMIC_ACQUIRE)) {
			rc = -ECANCELED;
			break;
		}
		out = uet_allocate_request(qp, &track->wr, &post,
					   task->packet_index + i);
		if (!out) {
			rc = -ENOMEM;
			break;
		}
		uet_encode_request(qp, &track->wr, &post, out,
				   task->packet_index + i);
		if (task->result_tail)
			task->result_tail->next = out;
		else
			task->result_head = out;
		task->result_tail = out;
	}
	if (rc < 0)
		uet_free_task_packets(task);
	pthread_mutex_lock(&qp->lock);
	task->rc = rc;
	task->ready = 1;
	uet_commit_refills_locked(qp);
	pthread_mutex_unlock(&qp->lock);
}

static int uet_schedule_refill_locked(struct uib_qp *qp)
{
	struct uib_uet_context *context;
	struct uib_uet_prep_worker *worker;
	struct uib_uet_prep_task *task;
	struct uib_uet_wr_track *track;
	struct uib_uet_qp *uet;
	uint64_t position;
	uint32_t capacity;
	uint32_t count;
	uint32_t remaining;
	int scheduled;
	int was_empty;

	uet = qp->protocol_private;
	context = qp->ctx->protocol_private;
	capacity = UIB_UET_PDC_MPR - uet->materialized_count;
	scheduled = 0;
	for (track = uet->track_head; track && capacity;
	     track = track->next) {
		while (track->scheduled_packet < track->packets && capacity) {
			task = uet_reserve_prep_task(context, &worker, &position,
						     &was_empty);
			if (!task)
				return scheduled;
			remaining = track->packets - track->scheduled_packet;
			count = remaining < UIB_UET_PREP_CHUNK ? remaining :
				UIB_UET_PREP_CHUNK;
			if (count > capacity)
				count = capacity;
			memset(task, 0, sizeof(*task));
			task->worker = worker;
			task->track = track;
			task->packet_index = track->scheduled_packet;
			task->packet_count = count;
			uet_track_get(track);
			if (uet->refill_tail)
				uet->refill_tail->refill_next = task;
			else
				uet->refill_head = task;
			uet->refill_tail = task;
			track->scheduled_packet += count;
			track->refill_tasks++;
			uet->materialized_count += count;
			uet->prep_pending++;
			capacity -= count;
			scheduled++;
			uib_ptr_ring_publish(&worker->ready, position, task);
			if (was_empty) {
				pthread_mutex_lock(&worker->lock);
				pthread_cond_signal(&worker->cond);
				pthread_mutex_unlock(&worker->lock);
			}
		}
	}
	return scheduled;
}

static int uet_post_one(struct uib_qp *qp, struct uib_send_wr *wr)
{
	struct uet_post_context context;
	struct uib_uet_wr_track *track;
	struct uib_uet_rccc *rccc = NULL;
	uint64_t nominal_bytes = 0;
	int rc;

	rc = uet_prepare_post(qp, wr, &context);
	if (rc < 0)
		goto out;
	uet_reserve_post(qp, &context);
	uet_store_post(wr, &context);
	if (context.uet->rccc_enabled) {
		rccc = (struct uib_uet_rccc *)context.uet->congestion.state.bytes;
		nominal_bytes = uet_post_nominal_bytes(wr, &context);
		context.track->nominal_bytes = nominal_bytes;
		context.track->rccc_accounted = 1;
		pthread_mutex_lock(&qp->lock);
		uib_uet_rccc_on_new_data(rccc, nominal_bytes);
		pthread_mutex_unlock(&qp->lock);
	}
	track = context.track;
	context.track = NULL;
	rc = uet_admit_post(track);
	if (rc < 0) {
		pthread_mutex_lock(&qp->lock);
		if (track->rccc_accounted && rccc)
			uib_uet_rccc_on_cancel_data(rccc,
						track->nominal_bytes);
		pthread_mutex_unlock(&qp->lock);
		uet_free_track(track);
		goto out;
	}
	rc = uet_process_post(track);
out:
	uet_post_context_release(&context);
	return rc;
}

static int uet_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			 struct uib_send_wr **bad)
{
	struct uib_send_wr *current;

	for (current = wr; current; current = current->next) {
		int rc = uet_post_one(qp, current);

		if (rc < 0) {
			if (bad)
				*bad = current;
			return rc;
		}
	}
	return 0;
}

static void uet_retransmit_tick(struct uib_qp *qp, uint64_t now_ns)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	struct uib_uet_outstanding *out;
	uint64_t next_deadline = UINT64_MAX;
	uint32_t sent_remaining;

	pthread_mutex_lock(&qp->lock);
	out = uet->out_head;
	sent_remaining = uet->sent_count;
	while (out && sent_remaining) {
		if (!out->sent) {
			out = out->next;
			continue;
		}
		sent_remaining--;
		if ((int64_t)(now_ns - out->deadline_ns) < 0) {
			out = out->next;
			continue;
		}
		if (out->rto_retries >= UIB_UET_MAX_RETRIES) {
			struct uib_uet_wr_track *track = out->track;

			uet_remove_track_locked(qp, track, UIB_WC_RETRY_EXC_ERR);
			out = uet->out_head;
			sent_remaining = uet->sent_count;
			continue;
		}
		uet_mark_retransmit(out->packet);
		uet_debug(qp, "retransmit psn=%u msg=%u rto-attempt=%u",
			  out->psn, out->msg_id, out->rto_retries + 1);
		uib_congestion_on_loss(&uet->congestion, out->nominal_len);
		if (out->entropy) {
			uet->paths[out->path_index].timeouts++;
			if (uet->multipath.mode ==
			    UIB_UET_MULTIPATH_PATH_AWARE)
				uet->paths[out->path_index].blocked_until_ns =
					now_ns + uet->multipath.failure_hold_ns;
			uet->multipath_stats.timeout_failovers++;
			out->entropy = 0;
		}
		uet->sent_count--;
		out->sent = 0;
		out->retries++;
		out->rto_retries++;
		out = out->next;
	}
	uet->tx_cursor = uet_first_unsent(uet);
	uet_refill_locked(qp);
	if (uet->rccc_enabled && uet->tx_cursor &&
	    uet->credit_request_deadline_ns &&
	    uet->credit_request_deadline_ns < next_deadline)
		next_deadline = uet->credit_request_deadline_ns;
	sent_remaining = uet->sent_count;
	for (out = uet->out_head; out && sent_remaining;
	     out = out->next) {
		if (!out->sent)
			continue;
		sent_remaining--;
		if (out->deadline_ns < next_deadline)
			next_deadline = out->deadline_ns;
	}
	if (next_deadline != UINT64_MAX)
		uib_qp_retry_schedule(qp, next_deadline);
	else
		uib_qp_retry_cancel(qp);
	pthread_mutex_unlock(&qp->lock);
}

const struct uib_protocol_ops uib_uet_protocol_ops = {
	.name = "uet",
	.default_port = UIB_UET_DEFAULT_PORT,
	.min_packet = sizeof(struct uib_uet_prologue),
	.init = uet_context_init,
	.fini = uet_context_fini,
	.qp_init = uet_qp_init,
	.qp_fini = uet_qp_fini,
	.qp_modified = uet_qp_modified,
	.packet_qpn = uet_packet_qpn,
	.post_send = uet_post_send,
	.handle_packet = uet_handle_packet,
	.retransmit_tick = uet_retransmit_tick,
};
