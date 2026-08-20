#ifndef UIB_QP_H
#define UIB_QP_H

#include <stdint.h>
#include <pthread.h>
#include <netinet/in.h>
#include "uib/types.h"

struct uib_context;
struct uib_pd;
struct uib_cq;

struct uib_sge {
	uint64_t addr;
	uint32_t length;
	uint32_t lkey;
};

struct uib_ah_attr {
	struct sockaddr_in remote_addr;
	uint32_t           remote_qpn;
	uint32_t           qkey;
};

struct uib_send_wr {
	struct uib_send_wr *next;
	uint64_t            wr_id;
	enum uib_wr_opcode  opcode;
	int                 send_flags;
	uint32_t            imm_data;
	struct uib_sge     *sg_list;
	int                 num_sge;
	struct {
		uint64_t remote_addr;
		uint32_t rkey;
	} rdma;
	struct {
		uint64_t remote_addr;
		uint64_t compare_add;
		uint64_t swap;
		uint32_t rkey;
		uint8_t uet_opcode;
		uint8_t uet_datatype;
		uint8_t uet_semantic_control;
		uint8_t uet_fetching;
		uint8_t uet_operand[16];
		uint8_t uet_compare[16];
		uint8_t uet_swap[16];
	} atomic;
	struct uib_ah_attr  ud;
};

struct uib_recv_wr {
	struct uib_recv_wr *next;
	uint64_t            wr_id;
	struct uib_sge     *sg_list;
	int                 num_sge;
};

struct uib_qp_init_attr {
	struct uib_cq *send_cq;
	struct uib_cq *recv_cq;
	enum uib_qp_type qp_type;
	uint32_t       max_send_wr;
	uint32_t       max_recv_wr;
	uint32_t       max_send_sge;
	uint32_t       max_recv_sge;
};

struct uib_qp_attr {
	enum uib_qp_state  qp_state;
	uint32_t           qp_access_flags;
	uint16_t           pkey_index;
	uint8_t            port_num;
	uint32_t           qkey;
	uint32_t           dest_qp_num;
	uint32_t           rq_psn;
	uint32_t           sq_psn;
	uint32_t           path_mtu;
	struct sockaddr_in remote_addr;
};

#define UIB_QP_STATE         (1u << 0)
#define UIB_QP_ACCESS_FLAGS  (1u << 1)
#define UIB_QP_PKEY_INDEX    (1u << 2)
#define UIB_QP_PORT          (1u << 3)
#define UIB_QP_QKEY          (1u << 4)
#define UIB_QP_AV            (1u << 5)
#define UIB_QP_DEST_QPN      (1u << 6)
#define UIB_QP_RQ_PSN        (1u << 7)
#define UIB_QP_SQ_PSN        (1u << 8)
#define UIB_QP_PATH_MTU      (1u << 9)

struct uib_rq_slot {
	int               valid;
	uint64_t          wr_id;
	struct uib_sge   *sg_list;
	int               num_sge;
};

struct uib_outstanding {
	struct uib_outstanding *next;
	uint32_t                psn;
	uint8_t                 wire_opcode;
	uint8_t                 retries;
	uint8_t                 signaled;
	uint8_t                 is_read;
	uint8_t                 is_final;
	uint64_t                wr_id;
	enum uib_wr_opcode      wr_opcode;
	uint64_t                deadline_ns;
	void                   *pkt;
	size_t                  pkt_len;
	struct sockaddr_in      dst;
	struct uib_sge         *read_sgl;
	int                     read_num_sge;
	uint32_t                read_total_len;
};

struct uib_qp {
	struct uib_context *ctx;
	struct uib_pd      *pd;
	struct uib_cq      *send_cq;
	struct uib_cq      *recv_cq;
	uint32_t            qp_num;
	enum uib_qp_type    qp_type;
	enum uib_qp_state   state;

	uint32_t            dest_qp_num;
	uint32_t            sq_psn;
	uint32_t            rq_psn;
	uint32_t            qkey;
	uint32_t            path_mtu;
	struct sockaddr_in  remote_addr;
	uint32_t            access_flags;
	void               *protocol_private;

	struct uib_rq_slot *rq;
	uint32_t            rq_size;
	uint32_t            rq_head;
	uint32_t            rq_tail;

	uint32_t            max_send_sge;
	uint32_t            max_recv_sge;

	struct uib_outstanding *out_head;
	struct uib_outstanding *out_tail;
	uint32_t                out_count;
	uint32_t                last_acked_psn;
	uint32_t                last_rx_psn;
	uint8_t                 last_rx_valid;

	struct uib_rq_slot      frag_recv;
	uint8_t                 frag_recv_active;
	uint32_t                frag_recv_offset;

	uint64_t                frag_write_va;
	uint32_t                frag_write_total;
	uint32_t                frag_write_offset;
	uint8_t                 frag_write_active;
	struct uib_mr          *frag_write_mr;

	struct uib_outstanding *frag_read_out;
	uint32_t                frag_read_offset;

	pthread_mutex_t     lock;
};

#define UIB_RTO_NS         (200ULL * 1000 * 1000)
#define UIB_MAX_RETRIES    7

void uib_qp_retransmit_tick(struct uib_qp *qp, uint64_t now_ns);

struct uib_qp *uib_create_qp(struct uib_pd *pd,
			     const struct uib_qp_init_attr *attr);
int uib_modify_qp(struct uib_qp *qp, const struct uib_qp_attr *attr,
		  int attr_mask);
int uib_destroy_qp(struct uib_qp *qp);

int uib_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
		  struct uib_send_wr **bad);
int uib_post_recv(struct uib_qp *qp, struct uib_recv_wr *wr,
		  struct uib_recv_wr **bad);

int uib_qp_handle_packet(struct uib_qp *qp, const struct sockaddr_in *src,
			 const void *pkt, size_t len);

int uib_qp_gather(struct uib_pd *pd, const struct uib_sge *sgl, int n,
		  uint8_t *out, size_t cap);
int uib_qp_scatter(struct uib_pd *pd, struct uib_sge *sgl, int n,
		   uint32_t msg_offset, const uint8_t *in, size_t in_len,
		   size_t *consumed_out);
int uib_qp_pop_recv(struct uib_qp *qp, struct uib_rq_slot *out);
void uib_qp_emit_send(struct uib_qp *qp, uint64_t wr_id,
		      enum uib_wr_opcode wr_op, int signaled,
		      enum uib_wc_status status, uint32_t byte_len);
void uib_qp_emit_recv(struct uib_qp *qp, uint64_t wr_id,
		      enum uib_wc_status status, enum uib_wc_opcode opcode,
		      uint32_t byte_len, uint32_t src_qp, uint32_t imm_data);

int uib_rocev2_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			 struct uib_send_wr **bad);
int uib_rocev2_handle_packet(struct uib_qp *qp,
			    const struct sockaddr_in *src,
			    const void *packet, size_t len);
void uib_rocev2_retransmit_tick(struct uib_qp *qp, uint64_t now_ns);

#endif
