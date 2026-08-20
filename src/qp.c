#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#include "uib/qp.h"
#include "uib/pd.h"
#include "uib/mr.h"
#include "uib/cq.h"
#include "uib/context.h"
#include "uib/transport.h"
#include "uib/bth.h"
#include "utils.h"

#define UIB_PKT_MAX (UIB_PMTU + 64)

struct uib_qp *uib_create_qp(struct uib_pd *pd,
			     const struct uib_qp_init_attr *attr)
{
	if (!pd || !attr) {
		errno = EINVAL;
		return NULL;
	}
	struct uib_qp *qp = calloc(1, sizeof(*qp));

	if (!qp)
		return NULL;

	qp->ctx          = pd->ctx;
	qp->pd           = pd;
	qp->send_cq      = attr->send_cq;
	qp->recv_cq      = attr->recv_cq;
	qp->qp_type      = attr->qp_type;
	qp->state        = UIB_QPS_RESET;
	qp->max_send_sge = attr->max_send_sge ? attr->max_send_sge : 1;
	qp->max_recv_sge = attr->max_recv_sge ? attr->max_recv_sge : 1;
	qp->path_mtu     = UIB_PMTU;

	qp->rq_size = attr->max_recv_wr ? (int)attr->max_recv_wr + 1 : 65;
	qp->rq      = calloc(qp->rq_size, sizeof(*qp->rq));
	if (!qp->rq) {
		free(qp);
		return NULL;
	}

	pthread_mutex_init(&qp->lock, NULL);

	qp->qp_num = uib_register_qp(pd->ctx, qp);
	if (qp->qp_num == 0) {
		free(qp->rq);
		free(qp);
		errno = ENOSPC;
		return NULL;
	}
	int rc = qp->ctx->pops->qp_init ? qp->ctx->pops->qp_init(qp) : 0;

	if (rc < 0) {
		uib_unregister_qp(pd->ctx, qp);
		pthread_mutex_destroy(&qp->lock);
		free(qp->rq);
		free(qp);
		errno = -rc;
		return NULL;
	}
	pd->refcnt++;
	return qp;
}

static void free_outstanding(struct uib_outstanding *o)
{
	if (!o)
		return;
	free(o->pkt);
	free(o->read_sgl);
	free(o);
}

int uib_destroy_qp(struct uib_qp *qp)
{
	struct uib_context *ctx;
	struct uib_outstanding *o;
	uint32_t i;

	if (!qp)
		return -EINVAL;
	ctx = qp->ctx;
	pthread_rwlock_wrlock(&ctx->qp_io_lock);
	uib_unregister_qp(ctx, qp);
	if (qp->ctx->pops->qp_fini)
		qp->ctx->pops->qp_fini(qp);

	pthread_mutex_lock(&qp->lock);
	o = qp->out_head;

	while (o) {
		struct uib_outstanding *n = o->next;

		free_outstanding(o);
		o = n;
	}
	qp->out_head = qp->out_tail = NULL;
	if (qp->frag_recv_active)
		free(qp->frag_recv.sg_list);
	for (i = 0; i < qp->rq_size; i++) {
		if (qp->rq[i].valid)
			free(qp->rq[i].sg_list);
	}
	pthread_mutex_unlock(&qp->lock);

	pthread_mutex_destroy(&qp->lock);
	if (qp->pd)
		qp->pd->refcnt--;
	free(qp->rq);
	free(qp);
	pthread_rwlock_unlock(&ctx->qp_io_lock);
	return 0;
}

int uib_modify_qp(struct uib_qp *qp, const struct uib_qp_attr *attr,
		  int attr_mask)
{
	if (!qp || !attr)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	if (attr_mask & UIB_QP_STATE)
		qp->state = attr->qp_state;
	if (attr_mask & UIB_QP_DEST_QPN)
		qp->dest_qp_num = attr->dest_qp_num;
	if (attr_mask & UIB_QP_RQ_PSN) {
		qp->rq_psn       = attr->rq_psn;
		qp->last_acked_psn = attr->rq_psn ? attr->rq_psn - 1 :
			UIB_BTH_PSN_MASK;
	}
	if (attr_mask & UIB_QP_SQ_PSN) {
		qp->sq_psn       = attr->sq_psn;
		qp->last_acked_psn = attr->sq_psn ? attr->sq_psn - 1 :
			UIB_BTH_PSN_MASK;
	}
	if ((attr_mask & UIB_QP_PATH_MTU) && attr->path_mtu)
		qp->path_mtu = attr->path_mtu;
	if (attr_mask & UIB_QP_QKEY)
		qp->qkey = attr->qkey;
	if (attr_mask & UIB_QP_AV)
		qp->remote_addr = attr->remote_addr;
	if (attr_mask & UIB_QP_ACCESS_FLAGS)
		qp->access_flags = attr->qp_access_flags;
	if (qp->ctx->pops->qp_modified)
		qp->ctx->pops->qp_modified(qp, attr_mask);
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static int gather_total(struct uib_pd *pd, const struct uib_sge *sgl, int n,
			uint8_t *out, size_t cap)
{
	size_t off = 0;
	int i;

	for (i = 0; i < n; i++) {
		const struct uib_sge *s = &sgl[i];
		struct uib_mr *mr;
		int rc;

		if (off + s->length > cap)
			return -EMSGSIZE;
		rc = uib_mr_check_local(pd, s->lkey,
					(const void *)(uintptr_t)s->addr,
					s->length, 0);
		if (rc < 0)
			return rc;
		mr = uib_pd_lookup_key(pd, s->lkey);
		rc = uib_mr_read(mr, s->addr, out + off, s->length);
		if (rc < 0)
			return rc;
		off += s->length;
	}
	return (int)off;
}

int uib_qp_gather(struct uib_pd *pd, const struct uib_sge *sgl, int n,
		  uint8_t *out, size_t cap)
{
	return gather_total(pd, sgl, n, out, cap);
}

static int scatter_at(struct uib_pd *pd, struct uib_sge *sgl, int n,
		      uint32_t msg_offset,
		      const uint8_t *in, size_t in_len, size_t *consumed_out)
{
	size_t skip = msg_offset;
	int i;

	for (i = 0; i < n && skip > 0; i++) {
		if (skip < sgl[i].length)
			break;
		skip -= sgl[i].length;
	}
	if (i == n) {
		*consumed_out = 0;
		return 0;
	}
	size_t off_in_sge = skip;
	size_t consumed = 0;

	for (; i < n && consumed < in_len; i++) {
		size_t avail = sgl[i].length - off_in_sge;
		size_t take = (avail < in_len - consumed) ? avail : (in_len - consumed);
		int rc;

		rc = uib_mr_check_local(pd, sgl[i].lkey,
					(void *)(uintptr_t)(sgl[i].addr + off_in_sge),
					take, UIB_ACCESS_LOCAL_WRITE);
		if (rc < 0)
			return rc;
		rc = uib_mr_place(uib_pd_lookup_key(pd, sgl[i].lkey),
				  sgl[i].addr + off_in_sge, in + consumed, take);
		if (rc < 0)
			return rc;
		consumed += take;
		off_in_sge = 0;
	}
	*consumed_out = consumed;
	return 0;
}

int uib_qp_scatter(struct uib_pd *pd, struct uib_sge *sgl, int n,
		   uint32_t msg_offset, const uint8_t *in, size_t in_len,
		   size_t *consumed_out)
{
	return scatter_at(pd, sgl, n, msg_offset, in, in_len, consumed_out);
}

static void emit_send_wc(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status st, uint32_t byte_len)
{
	if (!qp->send_cq)
		return;
	if (st == UIB_WC_SUCCESS && !signaled)
		return;
	struct uib_wc wc = {0};

	wc.wr_id    = wr_id;
	wc.status   = st;
	wc.qp_num   = qp->qp_num;
	wc.byte_len = byte_len;
	switch (wr_op) {
	case UIB_WR_SEND:
	case UIB_WR_SEND_WITH_IMM:
		wc.opcode = UIB_WC_SEND;
		break;
	case UIB_WR_RDMA_WRITE:
		wc.opcode = UIB_WC_RDMA_WRITE;
		break;
	case UIB_WR_RDMA_READ:
		wc.opcode = UIB_WC_RDMA_READ;
		break;
	case UIB_WR_ATOMIC_FETCH_ADD:
		wc.opcode = UIB_WC_FETCH_ADD;
		break;
	case UIB_WR_ATOMIC_CMP_SWAP:
		wc.opcode = UIB_WC_COMP_SWAP;
		break;
	case UIB_WR_UET_ATOMIC:
		wc.opcode = UIB_WC_UET_ATOMIC;
		break;
	}
	uib_cq_push(qp->send_cq, &wc);
}

void uib_qp_emit_send(struct uib_qp *qp, uint64_t wr_id,
		      enum uib_wr_opcode wr_op, int signaled,
		      enum uib_wc_status status, uint32_t byte_len)
{
	emit_send_wc(qp, wr_id, wr_op, signaled, status, byte_len);
}

static void emit_recv_wc(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wc_status st, enum uib_wc_opcode op,
			 uint32_t byte_len, uint32_t src_qp,
			 uint32_t imm_data)
{
	if (!qp->recv_cq)
		return;
	struct uib_wc wc = {0};

	wc.wr_id    = wr_id;
	wc.status   = st;
	wc.opcode   = op;
	wc.byte_len = byte_len;
	wc.qp_num   = qp->qp_num;
	wc.src_qp   = src_qp;
	wc.imm_data = imm_data;
	uib_cq_push(qp->recv_cq, &wc);
}

void uib_qp_emit_recv(struct uib_qp *qp, uint64_t wr_id,
		      enum uib_wc_status status, enum uib_wc_opcode opcode,
		      uint32_t byte_len, uint32_t src_qp, uint32_t imm_data)
{
	emit_recv_wc(qp, wr_id, status, opcode, byte_len, src_qp, imm_data);
}

static void enqueue_outstanding(struct uib_qp *qp, struct uib_outstanding *o)
{
	o->next = NULL;
	o->deadline_ns = uib_now_ns() + UIB_RTO_NS;
	if (qp->out_tail)
		qp->out_tail->next = o;
	else
		qp->out_head = o;
	qp->out_tail = o;
	qp->out_count++;
}

static void send_ack(struct uib_qp *qp, const struct sockaddr_in *dst,
		     uint32_t ack_psn, uint8_t syndrome)
{
	struct sockaddr_in target = *dst;
	if (qp->remote_addr.sin_family == AF_INET) {
		target.sin_addr = qp->remote_addr.sin_addr;
		target.sin_port = qp->remote_addr.sin_port;
	}
	uint8_t pkt[UIB_BTH_BYTES + UIB_AETH_BYTES + UIB_ICRC_BYTES];
	struct uib_bth *bth = (struct uib_bth *)pkt;

	uib_bth_pack(bth, UIB_OP_ACKNOWLEDGE, UIB_DEFAULT_PKEY,
		     qp->dest_qp_num, ack_psn, 0);
	struct uib_aeth *aeth = (struct uib_aeth *)(pkt + UIB_BTH_BYTES);

	uib_aeth_pack(aeth, syndrome, ack_psn);
	uint32_t crc = uib_icrc(pkt, UIB_BTH_BYTES + UIB_AETH_BYTES);
	uint32_t bcrc = htonl(crc);

	memcpy(pkt + UIB_BTH_BYTES + UIB_AETH_BYTES, &bcrc, UIB_ICRC_BYTES);
	uib_transport_send(qp->ctx, &target, pkt, sizeof(pkt));
}

static int send_one_packet(struct uib_qp *qp, struct uib_send_wr *wr,
			   uint8_t op, int is_first, int is_last,
			   const uint8_t *payload, size_t chunk,
			   uint64_t reth_va, uint32_t reth_rkey, uint32_t reth_total,
			   uint32_t *out_psn, void **out_pkt, size_t *out_pkt_len,
			   struct sockaddr_in *out_dst)
{
	uint8_t pkt[UIB_PKT_MAX];
	uint8_t *p = pkt;
	int is_ud = (qp->qp_type == UIB_QPT_UD);
	int is_rc = (qp->qp_type == UIB_QPT_RC);

	uint32_t dqpn = is_ud ? wr->ud.remote_qpn : qp->dest_qp_num;
	struct sockaddr_in dst = is_ud ? wr->ud.remote_addr : qp->remote_addr;
	uint32_t psn = qp->sq_psn++;

	uib_bth_pack((struct uib_bth *)p, op, UIB_DEFAULT_PKEY, dqpn, psn,
		     is_rc ? 1 : 0);
	uint8_t pad = (uint8_t)((-(int)chunk) & 3);

	uib_bth_set_pad((struct uib_bth *)p, pad);
	p += UIB_BTH_BYTES;

	if (is_first && (op == UIB_OP_RDMA_WRITE_FIRST ||
			 op == UIB_OP_RDMA_WRITE_ONLY ||
			 op == UIB_OP_RDMA_READ_REQ)) {
		uib_reth_pack((struct uib_reth *)p, reth_va, reth_rkey, reth_total);
		p += UIB_RETH_BYTES;
	}

	if (is_ud) {
		uib_deth_pack((struct uib_deth *)p, wr->ud.qkey, qp->qp_num);
		p += UIB_DETH_BYTES;
	}

	if (chunk > 0) {
		if ((size_t)(p - pkt) + chunk + pad + UIB_ICRC_BYTES > sizeof(pkt))
			return -EMSGSIZE;
		memcpy(p, payload, chunk);
		p += chunk;
	}
	if (pad) {
		memset(p, 0, pad);
		p += pad;
	}

	uint32_t crc = uib_icrc(pkt, p - pkt);
	uint32_t bcrc = htonl(crc);

	memcpy(p, &bcrc, UIB_ICRC_BYTES);
	p += UIB_ICRC_BYTES;

	size_t pkt_len = p - pkt;
	int rc = uib_transport_send(qp->ctx, &dst, pkt, pkt_len);

	if (rc < 0)
		return rc;

	if (out_psn)
		*out_psn = psn;
	if (out_dst)
		*out_dst = dst;
	if (out_pkt && out_pkt_len) {
		void *copy = malloc(pkt_len);

		if (!copy)
			return -ENOMEM;
		memcpy(copy, pkt, pkt_len);
		*out_pkt     = copy;
		*out_pkt_len = pkt_len;
	}
	(void)is_last;
	return 0;
}

static int post_rdma_read(struct uib_qp *qp, struct uib_send_wr *wr,
			  size_t total)
{
	uint32_t mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
	uint32_t n_resp = (total + mtu - 1) / mtu;

	if (n_resp == 0)
		n_resp = 1;

	uint32_t psn;
	void *pkt = NULL;
	size_t pkt_len = 0;
	struct sockaddr_in dst;
	/*
	 * Serialize transmission with insertion into the outstanding list so a
	 * fast peer cannot ACK before the request becomes visible locally.
	 */
	pthread_mutex_lock(&qp->lock);
	int rc = send_one_packet(qp, wr, UIB_OP_RDMA_READ_REQ, 1, 1,
				 NULL, 0,
				 wr->rdma.remote_addr, wr->rdma.rkey, (uint32_t)total,
				 &psn, &pkt, &pkt_len, &dst);
	if (rc < 0) {
		pthread_mutex_unlock(&qp->lock);
		return rc;
	}
	qp->sq_psn += (n_resp - 1);

	struct uib_outstanding *o = calloc(1, sizeof(*o));

	if (!o) {
		free(pkt);
		pthread_mutex_unlock(&qp->lock);
		return -ENOMEM;
	}
	o->psn          = psn;
	o->wire_opcode  = UIB_OP_RDMA_READ_REQ;
	o->signaled     = (wr->send_flags & UIB_SEND_SIGNALED) ? 1 : 0;
	o->is_read      = 1;
	o->is_final     = 1;
	o->wr_id        = wr->wr_id;
	o->wr_opcode    = wr->opcode;
	o->dst          = dst;
	o->pkt          = pkt;
	o->pkt_len      = pkt_len;

	if (wr->num_sge > 0) {
		o->read_sgl = calloc(wr->num_sge, sizeof(*o->read_sgl));
		if (!o->read_sgl) {
			free_outstanding(o);
			pthread_mutex_unlock(&qp->lock);
			return -ENOMEM;
		}
		memcpy(o->read_sgl, wr->sg_list,
		       wr->num_sge * sizeof(*o->read_sgl));
		o->read_num_sge   = wr->num_sge;
		o->read_total_len = (uint32_t)total;
	}
	enqueue_outstanding(qp, o);
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static uint8_t pick_opcode(enum uib_qp_type qpt, enum uib_wr_opcode op,
			   int with_imm, uint32_t n, uint32_t i)
{
	int is_ud = (qpt == UIB_QPT_UD);

	if (is_ud)
		return with_imm ? UIB_OP_UD_SEND_ONLY_WITH_IMM :
			UIB_OP_UD_SEND_ONLY;
	int is_only  = (n == 1);
	int is_first = (i == 0);
	int is_last  = (i == n - 1);

	switch (op) {
	case UIB_WR_SEND:
	case UIB_WR_SEND_WITH_IMM:
		if (is_only)
			return with_imm ? UIB_OP_SEND_ONLY_WITH_IMM :
				UIB_OP_SEND_ONLY;
		if (is_first)
			return UIB_OP_SEND_FIRST;
		if (is_last)
			return with_imm ? UIB_OP_SEND_LAST_WITH_IMM :
				UIB_OP_SEND_LAST;
		return UIB_OP_SEND_MIDDLE;
	case UIB_WR_RDMA_WRITE:
		if (is_only)
			return UIB_OP_RDMA_WRITE_ONLY;
		if (is_first)
			return UIB_OP_RDMA_WRITE_FIRST;
		if (is_last)
			return UIB_OP_RDMA_WRITE_LAST;
		return UIB_OP_RDMA_WRITE_MIDDLE;
	case UIB_WR_RDMA_READ:
		return UIB_OP_RDMA_READ_REQ;
	case UIB_WR_ATOMIC_FETCH_ADD:
	case UIB_WR_ATOMIC_CMP_SWAP:
	case UIB_WR_UET_ATOMIC:
		return UIB_BTH_INVALID_OPCODE;
	}
	return UIB_BTH_INVALID_OPCODE;
}

static int post_one_send(struct uib_qp *qp, struct uib_send_wr *wr)
{
	int with_imm = (wr->opcode == UIB_WR_SEND_WITH_IMM);
	int is_ud = (qp->qp_type == UIB_QPT_UD);
	int is_rc = (qp->qp_type == UIB_QPT_RC);
	size_t total = 0;
	uint32_t i;

	for (i = 0; i < (uint32_t)wr->num_sge; i++)
		total += wr->sg_list[i].length;

	if (wr->opcode == UIB_WR_RDMA_READ)
		return post_rdma_read(qp, wr, total);
	if (wr->opcode == UIB_WR_ATOMIC_FETCH_ADD ||
	    wr->opcode == UIB_WR_ATOMIC_CMP_SWAP)
		return -EOPNOTSUPP;

	uint32_t mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;

	if (is_ud && total > mtu)
		return -EMSGSIZE;

	uint8_t *msgbuf = NULL;

	if (total > 0) {
		msgbuf = malloc(total);
		if (!msgbuf)
			return -ENOMEM;
		int rc = gather_total(qp->pd, wr->sg_list, wr->num_sge, msgbuf, total);

		if (rc < 0) {
			free(msgbuf);
			return rc;
		}
	}

	uint32_t n = (total <= mtu) ? 1 : (uint32_t)((total + mtu - 1) / mtu);

	if (n == 0)
		n = 1;

	int err = 0;

	for (i = 0; i < n; i++) {
		size_t off    = (size_t)i * mtu;
		size_t chunk  = (off + mtu > total) ? (total - off) : mtu;
		int    is_first = (i == 0);
		int    is_last  = (i == n - 1);
		uint8_t op = pick_opcode(qp->qp_type, wr->opcode, with_imm, n, i);

		if (op == UIB_BTH_INVALID_OPCODE) {
			err = -EINVAL;
			break;
		}

		uint32_t psn = 0;
		void    *pkt_copy = NULL;
		size_t   pkt_len = 0;
		struct sockaddr_in dst;

		if (is_rc)
			pthread_mutex_lock(&qp->lock);
		int rc = send_one_packet(qp, wr, op, is_first, is_last,
					 msgbuf ? msgbuf + off : NULL, chunk,
					 wr->rdma.remote_addr, wr->rdma.rkey,
					 (uint32_t)total,
					 &psn,
					 is_rc ? &pkt_copy : NULL,
					 is_rc ? &pkt_len : NULL,
					 &dst);
		if (rc < 0) {
			if (is_rc)
				pthread_mutex_unlock(&qp->lock);
			err = rc;
			break;
		}

		if (is_rc) {
			struct uib_outstanding *o = calloc(1, sizeof(*o));

			if (!o) {
				free(pkt_copy);
				pthread_mutex_unlock(&qp->lock);
				err = -ENOMEM;
				break;
			}
			o->psn         = psn;
			o->wire_opcode = op;
			o->signaled    = (wr->send_flags & UIB_SEND_SIGNALED) ? 1 : 0;
			o->is_final    = is_last;
			o->wr_id       = wr->wr_id;
			o->wr_opcode   = wr->opcode;
			o->dst         = dst;
			o->pkt         = pkt_copy;
			o->pkt_len     = pkt_len;
			enqueue_outstanding(qp, o);
			pthread_mutex_unlock(&qp->lock);
		}
	}
	free(msgbuf);

	if (!is_rc && !err)
		emit_send_wc(qp, wr->wr_id, wr->opcode,
			     wr->send_flags & UIB_SEND_SIGNALED,
			     UIB_WC_SUCCESS, (uint32_t)total);
	return err;
}

int uib_rocev2_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			 struct uib_send_wr **bad)
{
	struct uib_send_wr *cur;

	if (!qp || !wr)
		return -EINVAL;

	pthread_mutex_lock(&qp->lock);
	if (qp->state != UIB_QPS_RTS && qp->state != UIB_QPS_RTR) {
		pthread_mutex_unlock(&qp->lock);
		if (bad)
			*bad = wr;
		return -EINVAL;
	}
	pthread_mutex_unlock(&qp->lock);

	for (cur = wr; cur; cur = cur->next) {
		int rc = post_one_send(qp, cur);

		if (rc < 0) {
			emit_send_wc(qp, cur->wr_id, cur->opcode,
				     cur->send_flags & UIB_SEND_SIGNALED,
				     rc == -ERANGE ? UIB_WC_LOC_LEN_ERR
						   : UIB_WC_LOC_PROT_ERR, 0);
			if (bad)
				*bad = cur;
			return rc;
		}
	}
	return 0;
}

int uib_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
		  struct uib_send_wr **bad)
{
	if (!qp || !wr)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	if (qp->state != UIB_QPS_RTS && qp->state != UIB_QPS_RTR) {
		pthread_mutex_unlock(&qp->lock);
		if (bad)
			*bad = wr;
		return -EINVAL;
	}
	pthread_mutex_unlock(&qp->lock);
	if (!qp->ctx->pops || !qp->ctx->pops->post_send)
		return -EOPNOTSUPP;

	return qp->ctx->pops->post_send(qp, wr, bad);
}

int uib_post_recv(struct uib_qp *qp, struct uib_recv_wr *wr,
		  struct uib_recv_wr **bad)
{
	struct uib_recv_wr *cur;

	if (!qp || !wr)
		return -EINVAL;

	pthread_mutex_lock(&qp->lock);
	for (cur = wr; cur; cur = cur->next) {
		uint32_t next = (qp->rq_head + 1) % qp->rq_size;

		if (next == qp->rq_tail) {
			pthread_mutex_unlock(&qp->lock);
			if (bad)
				*bad = cur;
			return -ENOSPC;
		}
		struct uib_rq_slot *slot = &qp->rq[qp->rq_head];

		slot->wr_id   = cur->wr_id;
		slot->num_sge = cur->num_sge;
		slot->sg_list = calloc(cur->num_sge, sizeof(struct uib_sge));
		if (!slot->sg_list) {
			pthread_mutex_unlock(&qp->lock);
			if (bad)
				*bad = cur;
			return -ENOMEM;
		}
		memcpy(slot->sg_list, cur->sg_list,
		       cur->num_sge * sizeof(struct uib_sge));
		slot->valid = 1;
		qp->rq_head = next;
	}
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static int pop_recv_slot(struct uib_qp *qp, struct uib_rq_slot *out)
{
	pthread_mutex_lock(&qp->lock);
	if (qp->rq_tail == qp->rq_head) {
		pthread_mutex_unlock(&qp->lock);
		return -ENOENT;
	}
	*out = qp->rq[qp->rq_tail];
	qp->rq[qp->rq_tail].valid = 0;
	qp->rq[qp->rq_tail].sg_list = NULL;
	qp->rq_tail = (qp->rq_tail + 1) % qp->rq_size;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

int uib_qp_pop_recv(struct uib_qp *qp, struct uib_rq_slot *out)
{
	return pop_recv_slot(qp, out);
}

static int verify_icrc(const uint8_t *pkt, size_t len)
{
	if (len < UIB_ICRC_BYTES)
		return -1;
	uint32_t want;

	memcpy(&want, pkt + len - UIB_ICRC_BYTES, UIB_ICRC_BYTES);
	want = ntohl(want);
	uint32_t got = uib_icrc(pkt, len - UIB_ICRC_BYTES);
	return got == want ? 0 : -1;
}

static int psn_cmp(uint32_t a, uint32_t b)
{
	int32_t d = (int32_t)((a - b) << 8);

	if (d == 0)
		return 0;
	return d < 0 ? -1 : 1;
}

static void retire_tail(struct uib_qp *qp)
{
	struct uib_outstanding *o;

	qp->out_tail = NULL;
	for (o = qp->out_head; o; o = o->next)
		qp->out_tail = o;
}

static void ack_outstanding(struct uib_qp *qp, uint32_t ack_psn)
{
	pthread_mutex_lock(&qp->lock);
	struct uib_outstanding **pp = &qp->out_head;

	while (*pp) {
		struct uib_outstanding *o = *pp;

		if (o->is_read || psn_cmp(o->psn, ack_psn) > 0) {
			pp = &o->next;
			continue;
		}
		if (o->is_final && !o->is_read) {
			emit_send_wc(qp, o->wr_id, o->wr_opcode, o->signaled,
				     UIB_WC_SUCCESS, 0);
		}
		*pp = o->next;
		free_outstanding(o);
		qp->out_count--;
	}
	retire_tail(qp);
	qp->last_acked_psn = ack_psn;
	pthread_mutex_unlock(&qp->lock);
}

static void fail_outstanding(struct uib_qp *qp, enum uib_wc_status st)
{
	pthread_mutex_lock(&qp->lock);
	struct uib_outstanding *o = qp->out_head;

	while (o) {
		struct uib_outstanding *n = o->next;

		if (o->is_final || o->is_read)
			emit_send_wc(qp, o->wr_id, o->wr_opcode, 1, st, 0);
		free_outstanding(o);
		o = n;
	}
	qp->out_head = qp->out_tail = NULL;
	qp->out_count = 0;
	qp->frag_read_out = NULL;
	qp->frag_read_offset = 0;
	pthread_mutex_unlock(&qp->lock);
}

static void send_read_response(struct uib_qp *qp, const struct sockaddr_in *dst,
			       uint32_t start_psn, uint64_t va, uint32_t rkey,
			       uint32_t length)
{
	struct uib_mr *mr;
	uint32_t mtu;
	uint32_t n;
	uint32_t i;

	mr = uib_mr_check_remote(qp->pd, rkey, va, length,
				  UIB_ACCESS_REMOTE_READ);
	if (!mr) {
		send_ack(qp, dst, start_psn, UIB_AETH_NAK_REM_ACC);
		return;
	}

	mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
	n = (length + mtu - 1) / mtu;

	if (n == 0)
		n = 1;

	for (i = 0; i < n; i++) {
		uint32_t off   = i * mtu;
		uint32_t chunk = (off + mtu > length) ? (length - off) : mtu;
		uint8_t pad = (uint8_t)((-(int)chunk) & 3);
		uint8_t op;
		int has_aeth;

		if (n == 1) {
			op = UIB_OP_RDMA_READ_RESP_ONLY;
			has_aeth = 1;
		} else if (i == 0) {
			op = UIB_OP_RDMA_READ_RESP_FIRST;
			has_aeth = 1;
		} else if (i == n - 1) {
			op = UIB_OP_RDMA_READ_RESP_LAST;
			has_aeth = 1;
		} else {
			op = UIB_OP_RDMA_READ_RESP_MIDDLE;
			has_aeth = 0;
		}

		size_t pkt_len = UIB_BTH_BYTES + (has_aeth ? UIB_AETH_BYTES : 0)
				 + chunk + pad + UIB_ICRC_BYTES;
		uint8_t *pkt = malloc(pkt_len);

		if (!pkt)
			return;
		uint8_t *p = pkt;

		uib_bth_pack((struct uib_bth *)p, op, UIB_DEFAULT_PKEY,
			     qp->dest_qp_num, start_psn + i, 0);
		uib_bth_set_pad((struct uib_bth *)p, pad);
		p += UIB_BTH_BYTES;
		if (has_aeth) {
			uib_aeth_pack((struct uib_aeth *)p, UIB_AETH_ACK,
				      start_psn + i);
			p += UIB_AETH_BYTES;
		}
		if (uib_mr_read(mr, va + off, p, chunk) < 0) {
			free(pkt);
			send_ack(qp, dst, start_psn, UIB_AETH_NAK_REM_ACC);
			return;
		}
		p += chunk;
		if (pad) {
			memset(p, 0, pad);
			p += pad;
		}
		uint32_t crc = uib_icrc(pkt, p - pkt);
		uint32_t bcrc = htonl(crc);

		memcpy(p, &bcrc, UIB_ICRC_BYTES);

		uib_transport_send(qp->ctx, dst, pkt, pkt_len);
		free(pkt);
	}
}

static struct uib_outstanding *find_read_by_psn(struct uib_qp *qp, uint32_t psn)
{
	pthread_mutex_lock(&qp->lock);
	struct uib_outstanding *o = qp->out_head;

	while (o) {
		if (o->is_read && o->read_total_len > 0) {
			uint32_t mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
			uint32_t span = (o->read_total_len + mtu - 1) / mtu;

			if (span == 0)
				span = 1;
			uint32_t lo = o->psn;
			uint32_t hi = o->psn + span - 1;

			if (psn_cmp(psn, lo) >= 0 && psn_cmp(psn, hi) <= 0)
				break;
		}
		o = o->next;
	}
	pthread_mutex_unlock(&qp->lock);
	return o;
}

static void detach_outstanding(struct uib_qp *qp,
			       struct uib_outstanding *target)
{
	pthread_mutex_lock(&qp->lock);
	struct uib_outstanding **pp = &qp->out_head;

	while (*pp && *pp != target)
		pp = &(*pp)->next;
	if (*pp == target) {
		*pp = target->next;
		qp->out_count--;
		retire_tail(qp);
	}
	pthread_mutex_unlock(&qp->lock);
}

int uib_rocev2_handle_packet(struct uib_qp *qp,
			    const struct sockaddr_in *src,
			    const void *pkt, size_t len)
{
	const uint8_t *p = pkt;

	if (verify_icrc(p, len) < 0)
		return -EBADMSG;

	const struct uib_bth *bth = (const struct uib_bth *)p;
	uint8_t op;
	uint32_t dqpn, psn;

	uib_bth_unpack(bth, &op, &dqpn, &psn, NULL);
	(void)dqpn;

	const uint8_t *body = p + UIB_BTH_BYTES;
	size_t body_len = len - UIB_BTH_BYTES - UIB_ICRC_BYTES;
	uint8_t pad = uib_bth_pad(bth);

	if (pad > body_len)
		return -EBADMSG;
	body_len -= pad;

	int is_rc = (qp->qp_type == UIB_QPT_RC);

	int is_resp = (op == UIB_OP_ACKNOWLEDGE
		       || op == UIB_OP_RDMA_READ_RESP_FIRST
		       || op == UIB_OP_RDMA_READ_RESP_MIDDLE
		       || op == UIB_OP_RDMA_READ_RESP_LAST
		       || op == UIB_OP_RDMA_READ_RESP_ONLY);

	if (is_rc && !is_resp) {
		uint32_t expected = qp->rq_psn & UIB_BTH_PSN_MASK;
		int order = psn_cmp(psn, expected);

		if (order < 0) {
			send_ack(qp, src, qp->last_rx_psn, UIB_AETH_ACK);
			return 0;
		}
		if (order > 0) {
			send_ack(qp, src, expected, UIB_AETH_NAK_PSN_SEQ);
			return 0;
		}
	}

	switch (op) {
	case UIB_OP_SEND_ONLY:
	case UIB_OP_SEND_ONLY_WITH_IMM: {
		struct uib_rq_slot slot;

		if (pop_recv_slot(qp, &slot) < 0) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_RNR);
			return -ENOENT;
		}
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, slot.sg_list, slot.num_sge,
				    0, body, body_len, &consumed);
		emit_recv_wc(qp, slot.wr_id,
			     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			     UIB_WC_RECV, (uint32_t)consumed, qp->dest_qp_num, 0);
		free(slot.sg_list);
		qp->last_rx_psn = psn;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn,
				 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
		return rc;
	}
	case UIB_OP_SEND_FIRST: {
		struct uib_rq_slot slot;

		if (pop_recv_slot(qp, &slot) < 0) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_RNR);
			return -ENOENT;
		}
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, slot.sg_list, slot.num_sge,
				    0, body, body_len, &consumed);
		qp->frag_recv = slot;
		qp->frag_recv_active = 1;
		qp->frag_recv_offset = (uint32_t)consumed;
		qp->last_rx_psn = psn;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn,
				 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
		return rc;
	}
	case UIB_OP_SEND_MIDDLE: {
		if (!qp->frag_recv_active) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, qp->frag_recv.sg_list,
				    qp->frag_recv.num_sge,
				    qp->frag_recv_offset, body, body_len, &consumed);
		qp->frag_recv_offset += (uint32_t)consumed;
		qp->last_rx_psn = psn;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn,
				 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
		return rc;
	}
	case UIB_OP_SEND_LAST:
	case UIB_OP_SEND_LAST_WITH_IMM: {
		if (!qp->frag_recv_active) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, qp->frag_recv.sg_list,
				    qp->frag_recv.num_sge,
				    qp->frag_recv_offset, body, body_len, &consumed);
		uint32_t total = qp->frag_recv_offset + (uint32_t)consumed;

		emit_recv_wc(qp, qp->frag_recv.wr_id,
			     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			     UIB_WC_RECV, total, qp->dest_qp_num, 0);
		free(qp->frag_recv.sg_list);
		qp->frag_recv.sg_list = NULL;
		qp->frag_recv_active = 0;
		qp->frag_recv_offset = 0;
		qp->last_rx_psn = psn;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn,
				 rc < 0 ? UIB_AETH_NAK_INV_REQ : UIB_AETH_ACK);
		return rc;
	}
	case UIB_OP_UD_SEND_ONLY: {
		if (body_len < UIB_DETH_BYTES)
			return -EBADMSG;
		const struct uib_deth *deth = (const struct uib_deth *)body;
		uint32_t qkey, sqp;

		uib_deth_unpack(deth, &qkey, &sqp);
		if (qkey != qp->qkey)
			return -EACCES;
		body     += UIB_DETH_BYTES;
		body_len -= UIB_DETH_BYTES;
		struct uib_rq_slot slot;

		if (pop_recv_slot(qp, &slot) < 0)
			return -ENOENT;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, slot.sg_list, slot.num_sge,
				    0, body, body_len, &consumed);
		emit_recv_wc(qp, slot.wr_id,
			     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			     UIB_WC_RECV, (uint32_t)consumed, sqp, 0);
		free(slot.sg_list);
		qp->rq_psn++;
		return rc;
	}
	case UIB_OP_RDMA_WRITE_ONLY: {
		if (body_len < UIB_RETH_BYTES) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		const struct uib_reth *reth = (const struct uib_reth *)body;
		uint64_t va; uint32_t rkey, rlen;

		uib_reth_unpack(reth, &va, &rkey, &rlen);
		body     += UIB_RETH_BYTES;
		body_len -= UIB_RETH_BYTES;
		if (rlen > body_len)
			rlen = body_len;
		struct uib_mr *mr = uib_mr_check_remote(qp->pd, rkey, va, rlen,
							UIB_ACCESS_REMOTE_WRITE);
		if (!mr) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EACCES;
		}
		if (uib_mr_place(mr, va, body, rlen) < 0) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EIO;
		}
		qp->last_rx_psn = psn;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn, UIB_AETH_ACK);
		return 0;
	}
	case UIB_OP_RDMA_WRITE_FIRST: {
		if (body_len < UIB_RETH_BYTES) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		const struct uib_reth *reth = (const struct uib_reth *)body;
		uint64_t va; uint32_t rkey, rlen;

		uib_reth_unpack(reth, &va, &rkey, &rlen);
		body     += UIB_RETH_BYTES;
		body_len -= UIB_RETH_BYTES;
		struct uib_mr *mr = uib_mr_check_remote(qp->pd, rkey, va, rlen,
							UIB_ACCESS_REMOTE_WRITE);
		if (!mr) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EACCES;
		}
		size_t take = body_len > rlen ? rlen : body_len;

		if (uib_mr_place(mr, va, body, take) < 0) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_REM_ACC);
			return -EIO;
		}
		qp->frag_write_va     = va;
		qp->frag_write_total  = rlen;
		qp->frag_write_offset = (uint32_t)take;
		qp->frag_write_active = 1;
		qp->frag_write_mr     = mr;
		qp->last_rx_psn = psn;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn, UIB_AETH_ACK);
		return 0;
	}
	case UIB_OP_RDMA_WRITE_MIDDLE: {
		if (!qp->frag_write_active) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		uint32_t remain = qp->frag_write_total - qp->frag_write_offset;
		size_t take = body_len > remain ? remain : body_len;

		if (uib_mr_place(qp->frag_write_mr,
				 qp->frag_write_va + qp->frag_write_offset,
				 body, take) < 0)
			return -EIO;
		qp->frag_write_offset += (uint32_t)take;
		qp->last_rx_psn = psn;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn, UIB_AETH_ACK);
		return 0;
	}
	case UIB_OP_RDMA_WRITE_LAST:
	case UIB_OP_RDMA_WRITE_LAST_WITH_IMM: {
		if (!qp->frag_write_active) {
			if (is_rc)
				send_ack(qp, src, psn, UIB_AETH_NAK_INV_REQ);
			return -EBADMSG;
		}
		uint32_t remain = qp->frag_write_total - qp->frag_write_offset;
		size_t take = body_len > remain ? remain : body_len;

		if (uib_mr_place(qp->frag_write_mr,
				 qp->frag_write_va + qp->frag_write_offset,
				 body, take) < 0)
			return -EIO;
		qp->frag_write_offset += (uint32_t)take;
		qp->frag_write_active = 0;
		qp->last_rx_psn = psn;
		qp->rq_psn = psn + 1;
		if (is_rc)
			send_ack(qp, src, psn, UIB_AETH_ACK);
		return 0;
	}
	case UIB_OP_RDMA_READ_REQ: {
		if (body_len < UIB_RETH_BYTES)
			return -EBADMSG;
		const struct uib_reth *reth = (const struct uib_reth *)body;
		uint64_t va; uint32_t rkey, rlen;

		uib_reth_unpack(reth, &va, &rkey, &rlen);
		send_read_response(qp, &qp->remote_addr, psn, va, rkey, rlen);
		uint32_t mtu = qp->path_mtu ? qp->path_mtu : UIB_PMTU;
		uint32_t span = (rlen + mtu - 1) / mtu;

		if (span == 0)
			span = 1;
		qp->last_rx_psn = psn + span - 1;
		qp->last_rx_valid = 1;
		qp->rq_psn = psn + span;
		return 0;
	}
	case UIB_OP_RDMA_READ_RESP_ONLY: {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		body += UIB_AETH_BYTES; body_len -= UIB_AETH_BYTES;
		struct uib_outstanding *o = find_read_by_psn(qp, psn);

		if (!o)
			return 0;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, o->read_sgl, o->read_num_sge,
				    0, body, body_len, &consumed);
		detach_outstanding(qp, o);
		emit_send_wc(qp, o->wr_id, o->wr_opcode, o->signaled,
			     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			     (uint32_t)consumed);
		free_outstanding(o);
		return rc;
	}
	case UIB_OP_RDMA_READ_RESP_FIRST: {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		body += UIB_AETH_BYTES; body_len -= UIB_AETH_BYTES;
		struct uib_outstanding *o = find_read_by_psn(qp, psn);

		if (!o)
			return 0;
		qp->frag_read_out = o;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, o->read_sgl, o->read_num_sge,
				    0, body, body_len, &consumed);
		qp->frag_read_offset = (uint32_t)consumed;
		return rc;
	}
	case UIB_OP_RDMA_READ_RESP_MIDDLE: {
		struct uib_outstanding *o = qp->frag_read_out;

		if (!o)
			return 0;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, o->read_sgl, o->read_num_sge,
				    qp->frag_read_offset, body, body_len, &consumed);
		qp->frag_read_offset += (uint32_t)consumed;
		return rc;
	}
	case UIB_OP_RDMA_READ_RESP_LAST: {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		body += UIB_AETH_BYTES; body_len -= UIB_AETH_BYTES;
		struct uib_outstanding *o = qp->frag_read_out;

		if (!o)
			return 0;
		size_t consumed = 0;
		int rc = scatter_at(qp->pd, o->read_sgl, o->read_num_sge,
				    qp->frag_read_offset, body, body_len, &consumed);
		uint32_t total = qp->frag_read_offset + (uint32_t)consumed;

		detach_outstanding(qp, o);
		emit_send_wc(qp, o->wr_id, o->wr_opcode, o->signaled,
			     rc < 0 ? UIB_WC_LOC_PROT_ERR : UIB_WC_SUCCESS,
			     total);
		free_outstanding(o);
		qp->frag_read_out = NULL;
		qp->frag_read_offset = 0;
		return rc;
	}
	case UIB_OP_ACKNOWLEDGE: {
		if (body_len < UIB_AETH_BYTES)
			return -EBADMSG;
		const struct uib_aeth *aeth = (const struct uib_aeth *)body;
		uint8_t syndrome; uint32_t msn;

		uib_aeth_unpack(aeth, &syndrome, &msn);
		(void)msn;
		if ((syndrome & UIB_AETH_CODE_MASK) == UIB_AETH_ACK_CODE) {
			ack_outstanding(qp, psn);
		} else if ((syndrome & UIB_AETH_CODE_MASK) == UIB_AETH_RNR_CODE) {
		} else if (syndrome != UIB_AETH_NAK_PSN_SEQ) {
			pthread_mutex_lock(&qp->lock);
			qp->state = UIB_QPS_ERR;
			pthread_mutex_unlock(&qp->lock);
			fail_outstanding(qp,
					 syndrome == UIB_AETH_NAK_REM_ACC
					     ? UIB_WC_REM_ACCESS_ERR
					     : UIB_WC_REM_OP_ERR);
		}
		return 0;
	}
	default:
		return -ENOTSUP;
	}
}

int uib_qp_handle_packet(struct uib_qp *qp, const struct sockaddr_in *src,
			 const void *packet, size_t len)
{
	if (!qp || !qp->ctx->pops || !qp->ctx->pops->handle_packet)
		return -EOPNOTSUPP;

	return qp->ctx->pops->handle_packet(qp, src, packet, len);
}

void uib_rocev2_retransmit_tick(struct uib_qp *qp, uint64_t now)
{
	pthread_mutex_lock(&qp->lock);
	struct uib_outstanding **pp = &qp->out_head;

	while (*pp) {
		struct uib_outstanding *o = *pp;

		if ((int64_t)(now - o->deadline_ns) < 0) {
			pp = &o->next;
			continue;
		}
		if (o->retries >= UIB_MAX_RETRIES) {
			if (o->is_final || o->is_read)
				emit_send_wc(qp, o->wr_id, o->wr_opcode, 1,
					     UIB_WC_RETRY_EXC_ERR, 0);
			*pp = o->next;
			free_outstanding(o);
			qp->out_count--;
			qp->state = UIB_QPS_ERR;
		} else {
			uib_transport_send(qp->ctx, &o->dst, o->pkt, o->pkt_len);
			o->retries++;
			o->deadline_ns = now + UIB_RTO_NS;
			pp = &o->next;
		}
	}
	retire_tail(qp);
	pthread_mutex_unlock(&qp->lock);
}

void uib_qp_retransmit_tick(struct uib_qp *qp, uint64_t now)
{
	if (qp && qp->ctx->pops && qp->ctx->pops->retransmit_tick)
		qp->ctx->pops->retransmit_tick(qp, now);
}
