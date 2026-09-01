/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_TRANSPORT_UDP_INTERNAL_H
#define UIB_TRANSPORT_UDP_INTERNAL_H

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <linux/filter.h>
#include <linux/udp.h>

#include <liburing.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/env.h"
#include "uib/protocol.h"
#include "uib/afxdp_frame.h"
#include "ptr_ring.h"
#include "transport_internal.h"
#include "utils.h"

#define UIB_URING_DEPTH              256
#define UIB_URING_BATCH_DEFAULT      64
#define UIB_URING_BATCH_MAX          256
#define UIB_URING_RX_BUFFERS         128
#define UIB_URING_RX_BUFFER_GROUP    7
#define UIB_URING_TX_BATCH_DEFAULT   64
#define UIB_URING_TX_LARGE_BATCH     16
#define UIB_URING_TX_LARGE_PACKET    2048
#define UIB_URING_TX_LIMIT           4096
#define UIB_URING_TX_POOL_LIMIT      4096
#define UIB_URING_COPY_POOL_LIMIT    1024
#define UIB_URING_GSO_POOL_LIMIT     256
#define UIB_URING_TX_POOL_WARM       1024
#define UIB_URING_COPY_POOL_WARM     256
#define UIB_URING_GSO_POOL_WARM      UIB_URING_GSO_POOL_LIMIT
#define UIB_UDP_GSO_MAX_SEGMENTS     64
#define UIB_UDP_REF_INLINE_IOVS      2
#define UIB_UDP_GSO_MAX_IOVS         (UIB_UDP_GSO_MAX_SEGMENTS * 2)
#define UIB_UDP_GSO_CAPACITY         65520
/* Stay below the common Linux MAX_SKB_FRAGS limit of 17. */
#define UIB_UDP_ZC_MAX_FRAGS         16
#define UIB_UDP_RX_CONTROL_SPACE \
	(CMSG_SPACE(sizeof(uint16_t)) + CMSG_SPACE(sizeof(uint8_t)))
#define UIB_URING_SQPOLL_IDLE_DEFAULT 2000
#define UIB_SOCKET_BUFFER_SIZE       (4 * 1024 * 1024)
/* RX workers shard QPs; they do not parallelize one QP. */
#define UIB_UDP_RX_WORKERS_AUTO_MAX  2U
#define UIB_UDP_RX_WORK_QUEUE        1024
#define UIB_UDP_RX_POOL_CLASSES      4
#define UIB_UDP_RX_POOL_LIMIT        256
#define UIB_UDP_RX_LARGE_POOL_LIMIT  16
#define UIB_UDP_RX_POOL_WARM_MIN     4
#define UIB_UDP_RX_POOL_WARM_TOTAL   2048
#define UIB_UDP_RX_WORKER_STACK      (512 * 1024)
#define UIB_UDP_COPY_CAPACITY        (8192 + sizeof(struct iphdr) + \
				       sizeof(struct udphdr))
#define UIB_UDP_FLOW_MAX             64U
#define UIB_UDP_FLOW_PORT_STRIDE     128U
#define UIB_UDP_FLOW_SOURCE_BASE     0xc000U
#define UIB_UDP_FLOW_QUEUE_SIZE      256U
#define UIB_UDP_FLOW_TX_BATCH        32U

enum uib_udp_op_type {
	UIB_UDP_OP_RX_MULTISHOT = 0,
	UIB_UDP_OP_RX_ONESHOT,
	UIB_UDP_OP_TX,
	UIB_UDP_OP_TIMER,
	UIB_UDP_OP_WAKE,
};

enum uib_udp_tx_pool {
	UIB_UDP_TX_UNPOOLED = 0,
	UIB_UDP_TX_REF_POOL,
	UIB_UDP_TX_COPY_POOL,
	UIB_UDP_TX_GSO_POOL,
};

enum uib_udp_flow_mode {
	UIB_UDP_FLOW_NONE = 0,
	UIB_UDP_FLOW_MULTIPATH,
	UIB_UDP_FLOW_FIXED_PORT,
};

struct uib_udp_release {
	uib_transport_release_fn release;
	void                    *opaque;
	unsigned int             iov_count;
};

struct uib_udp_op {
	enum uib_udp_op_type type;
};

struct uib_udp_rx_slot {
	struct uib_udp_op  op;
	struct sockaddr_in source;
	struct iovec       iov;
	struct msghdr      message;
	void              *buffer;
	union {
		struct cmsghdr header;
		uint8_t bytes[UIB_UDP_RX_CONTROL_SPACE];
	} control;
	int                armed;
};

struct uib_udp_tx {
	struct uib_udp_op  op;
	struct uib_udp_tx *inflight_next;
	struct uib_udp_tx *inflight_prev;
	struct uib_udp_tx *queue_next;
	struct sockaddr_in destination;
	struct iovec      *iov_list;
	struct iovec       iov[UIB_UDP_REF_INLINE_IOVS];
	struct msghdr      message;
	void              *data;
	uib_transport_release_fn release;
	struct uib_udp_release  *release_list;
	void              *release_opaque;
	size_t             length;
	size_t             storage_capacity;
	unsigned int       release_count;
	unsigned int       packets;
	unsigned int       iov_capacity;
	int                iov_allocated;
	uint16_t           gso_size;
	int                zc_result;
	int                zerocopy;
	enum uib_udp_tx_pool pool;
	union {
		struct cmsghdr header;
		uint8_t bytes[CMSG_SPACE(sizeof(uint16_t))];
	} control;
	uint8_t            storage[];
};

struct uib_udp_backend;

struct uib_udp_flow {
	struct uib_udp_backend *backend;
	struct uib_ptr_ring     tx_queue;
	pthread_cond_t          cond;
	pthread_mutex_t         lock;
	pthread_t               tx_thread;
	pthread_t               rx_thread;
	void                   *rx_buffer;
	unsigned int            index;
	int                     rx_fd;
	int                     tx_fd;
	int                     run;
	int                     queue_initialized;
	int                     sync_initialized;
	int                     tx_started;
	int                     rx_started;
};

struct uib_udp_rx_work {
	struct sockaddr_in source;
	uint64_t           rx_time_ns;
	size_t             length;
	uint8_t            tos;
	uint8_t            integrity_validated;
	uint8_t            pool_class;
	uint8_t            data[];
};

struct uib_udp_rx_worker {
	struct uib_udp_backend *backend;
	struct uib_ptr_ring     queue;
	struct uib_ptr_ring     pool[UIB_UDP_RX_POOL_CLASSES];
	pthread_cond_t          cond;
	pthread_mutex_t         lock;
	pthread_t               thread;
	unsigned int            cpu;
	unsigned int            pools_initialized;
	int                     cpu_set;
	int                     started;
	int                     initialized;
	int                     queue_initialized;
	int                     init_done;
	int                     init_rc;
};

struct uib_udp_backend {
	struct uib_udp_tx       *tx_inflight;
	struct uib_udp_tx       *tx_retry_head;
	struct io_uring_buf_ring *buffer_ring;
	struct uib_udp_rx_slot  *rx_slots;
	struct uib_ptr_ring      tx_queue;
	struct uib_ptr_ring      tx_ref_pool;
	struct uib_ptr_ring      tx_copy_pool;
	struct uib_ptr_ring      tx_gso_pool;
	struct io_uring          ring;
	struct uib_udp_op        multishot_op;
	struct uib_udp_op        timer_op;
	struct uib_udp_op        wake_op;
	struct msghdr            multishot_message;
	struct __kernel_timespec timeout;
	struct uib_transport_stats stats;
	struct sockaddr_in       local_addr;
	pthread_cond_t           init_cond;
	pthread_mutex_t          init_lock;
	pthread_t                worker;
	struct uib_context      *ctx;
	struct uib_udp_rx_worker *rx_workers;
	struct uib_udp_flow     *flows;
	void                    *rx_buffers;
	size_t                   rx_buffer_len;
	size_t                   page_size;
	unsigned int             rx_batch;
	unsigned int             tx_batch;
	unsigned int             sqpoll_idle;
	unsigned int             sqpoll_cpu;
	unsigned int             test_drop_tx;
	unsigned int             rx_worker_count;
	unsigned int             flow_count;
	unsigned int             requested_flow_count;
	uint32_t                 zerocopy_sequence;
	int                      zerocopy_enabled;
	int                      multishot_required;
	int                      multishot_armed;
	int                      timer_armed;
	int                      wake_armed;
	int                      worker_run;
	int                      stop_requested;
	int                      accepting;
	int                      worker_waiting;
	int                      init_done;
	int                      init_rc;
	int                      raw_mode;
	int                      fixed_file;
	int                      sqpoll_requested;
	int                      sqpoll_active;
	int                      sqpoll_cpu_set;
	int                      tx_batch_set;
	int                      gso_enabled;
	int                      gso_required;
	int                      gro_enabled;
	enum uib_udp_flow_mode   flow_mode;
	int                      wake_fd;
	int                      fd;
};

#define UIB_UDP_INTERNAL __attribute__((visibility("hidden")))

UIB_UDP_INTERNAL int test_drop_packet(struct uib_udp_backend *be);
UIB_UDP_INTERNAL void deliver_udp_payload(struct uib_context *ctx,
					  void *buffer, size_t length,
					  const struct sockaddr_in *source,
					  uint16_t gro_size, uint8_t tos);
UIB_UDP_INTERNAL uint16_t message_gro_size(const struct msghdr *message);
UIB_UDP_INTERNAL uint8_t message_tos(const struct msghdr *message);
UIB_UDP_INTERNAL struct uib_udp_rx_work *
rx_work_alloc(struct uib_udp_rx_worker *worker, size_t length);
UIB_UDP_INTERNAL void rx_work_release(struct uib_udp_rx_worker *worker,
				      struct uib_udp_rx_work *work);
UIB_UDP_INTERNAL void *rx_worker_loop(void *argument);
UIB_UDP_INTERNAL void inflight_add(struct uib_udp_backend *be,
				   struct uib_udp_tx *tx);
UIB_UDP_INTERNAL void inflight_remove(struct uib_udp_backend *be,
				      struct uib_udp_tx *tx);
UIB_UDP_INTERNAL struct uib_udp_tx *
alloc_ref_tx(struct uib_udp_backend *be);
UIB_UDP_INTERNAL struct uib_udp_tx *
alloc_copy_tx(struct uib_udp_backend *be, size_t capacity);
UIB_UDP_INTERNAL struct uib_udp_tx *
alloc_gso_tx(struct uib_udp_backend *be);
UIB_UDP_INTERNAL size_t buffer_page_count(const struct uib_udp_backend *be,
					  const void *buffer, size_t length);
UIB_UDP_INTERNAL int gso_zerocopy_eligible(const struct uib_context *ctx,
					   const struct uib_udp_tx *tx);
UIB_UDP_INTERNAL size_t limit_zerocopy_run(const struct uib_context *ctx,
					   const struct uib_transport_ref *refs,
					   size_t start, size_t run);
UIB_UDP_INTERNAL int gso_compatible(const struct uib_udp_tx *head,
				    const struct uib_udp_tx *next);
UIB_UDP_INTERNAL void merge_gso_tx(struct uib_udp_backend *be,
				   struct uib_udp_tx *head,
				   struct uib_udp_tx *next);
UIB_UDP_INTERNAL void free_tx(struct uib_udp_backend *be,
			      struct uib_udp_tx *tx);
UIB_UDP_INTERNAL void requeue_tx_list(struct uib_udp_backend *be,
				      struct uib_udp_tx *head);
UIB_UDP_INTERNAL void requeue_tx(struct uib_udp_backend *be,
				 struct uib_udp_tx *tx);
UIB_UDP_INTERNAL unsigned int dequeue_tx_batch(struct uib_udp_backend *be,
					       struct uib_udp_tx **head,
					       unsigned int limit);
UIB_UDP_INTERNAL int init_rx_work_pools(struct uib_udp_rx_worker *worker);
UIB_UDP_INTERNAL void cleanup_rx_work_pools(
	struct uib_udp_rx_worker *worker);
UIB_UDP_INTERNAL int init_tx_rings(struct uib_udp_backend *be);
UIB_UDP_INTERNAL void warm_tx_pools(struct uib_udp_backend *be);
UIB_UDP_INTERNAL void free_all_tx(struct uib_udp_backend *be);
UIB_UDP_INTERNAL void *worker_loop(void *argument);
UIB_UDP_INTERNAL int zerocopy_retryable(int result);
UIB_UDP_INTERNAL int build_raw_packet(struct uib_context *ctx,
				      const struct sockaddr_in *destination,
				      const void *buffer, size_t length,
				      uint16_t entropy, struct uib_udp_tx *tx);
UIB_UDP_INTERNAL int build_raw_iov_packet(
	struct uib_context *ctx, const struct uib_transport_iov_ref *ref,
	struct uib_udp_tx *tx);
UIB_UDP_INTERNAL int udp_iov_ref_validate(
	const struct uib_transport_iov_ref *ref);
UIB_UDP_INTERNAL size_t udp_iov_ref_run(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_iov_ref *refs, size_t start, size_t count);
UIB_UDP_INTERNAL struct uib_udp_tx *udp_iov_ref_gso_tx(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_iov_ref *refs, size_t start, size_t count);
UIB_UDP_INTERNAL struct uib_udp_tx *udp_iov_ref_one_tx(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_iov_ref *ref);
UIB_UDP_INTERNAL int udp_send_iov_ref_fallback(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count);
UIB_UDP_INTERNAL size_t udp_ref_run(struct uib_context *ctx,
				    struct uib_udp_backend *be,
				    const struct uib_transport_ref *refs,
				    size_t start, size_t count);
UIB_UDP_INTERNAL struct uib_udp_tx *udp_ref_gso_tx(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_ref *refs, size_t start, size_t count);
UIB_UDP_INTERNAL struct uib_udp_tx *udp_ref_one_tx(
	struct uib_context *ctx, struct uib_udp_backend *be,
	const struct uib_transport_ref *ref);
UIB_UDP_INTERNAL int udp_send_ref_fallback(
	struct uib_context *ctx, const struct uib_transport_ref *refs,
	size_t count);
UIB_UDP_INTERNAL int udp_publish_ref_batch(struct uib_udp_backend *be,
					   struct uib_udp_tx *head,
					   size_t queued, size_t count);
UIB_UDP_INTERNAL int udp_flow_count(unsigned int *count);
UIB_UDP_INTERNAL int init_udp_flows(struct uib_context *ctx,
				    struct uib_udp_backend *be);
UIB_UDP_INTERNAL void cleanup_udp_flows(struct uib_udp_backend *be);
UIB_UDP_INTERNAL int udp_flow_send_iov_ref_batch(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count);
UIB_UDP_INTERNAL int udp_send_iov_ref_batch(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count);
UIB_UDP_INTERNAL int udp_send_ref_batch(
	struct uib_context *ctx, const struct uib_transport_ref *refs,
	size_t count);
UIB_UDP_INTERNAL int udp_sync_send_entropy(
	struct uib_context *ctx, const struct sockaddr_in *destination,
	const void *buffer, size_t length, uint16_t entropy);
UIB_UDP_INTERNAL int udp_sync_send(struct uib_context *ctx,
				   const struct sockaddr_in *destination,
				   const void *buffer, size_t length);
UIB_UDP_INTERNAL int udp_sync_send_ref_batch(
	struct uib_context *ctx, const struct uib_transport_ref *refs,
	size_t count);
UIB_UDP_INTERNAL int udp_sync_send_iov_ref_batch(
	struct uib_context *ctx, const struct uib_transport_iov_ref *refs,
	size_t count);

#endif
