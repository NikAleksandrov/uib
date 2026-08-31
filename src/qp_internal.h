/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_QP_INTERNAL_H
#define UIB_QP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#include "uib/qp.h"

struct uib_rx_meta;

int uib_qp_gather_total(struct uib_pd *pd, const struct uib_sge *sgl, int n,
			uint8_t *out, size_t cap);
int uib_qp_scatter_at(struct uib_pd *pd, struct uib_sge *sgl, int n,
		      uint32_t msg_offset, const uint8_t *in, size_t in_len,
		      size_t *consumed_out);
int uib_qp_pop_recv_slot(struct uib_qp *qp, struct uib_rq_slot *out);
int uib_post_recv_owned(struct uib_qp *qp, struct uib_recv_wr *request,
			void *private_data,
			void (*private_release)(void *private_data));
void uib_qp_emit_send_wc(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status status, uint32_t byte_len);
void uib_qp_emit_recv_slot_src(struct uib_qp *qp,
			       struct uib_rq_slot *slot,
			       enum uib_wc_status status,
			       enum uib_wc_opcode opcode,
			       uint32_t byte_len, uint32_t src_qp,
			       uint32_t imm_data,
			       const struct sockaddr_in *src);
int uib_qp_handle_packet_meta(struct uib_qp *qp,
			      const struct sockaddr_in *src, const void *pkt,
			      size_t len, const struct uib_rx_meta *meta);

void qp_emit_send_vendor(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status status, uint32_t byte_len,
			 uint32_t vendor_err);

#endif
