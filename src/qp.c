/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "uib/qp.h"
#include "uib/pd.h"
#include "uib/mr.h"
#include "uib/cq.h"
#include "uib/context.h"
#include "host_zc_internal.h"
#include "object_internal.h"
#include "qp_internal.h"

struct uib_srq {
	struct uib_pd      *pd;
	struct uib_rq_slot *rq;
	uint32_t            rq_size;
	uint32_t            rq_head;
	uint32_t            rq_tail;
	uint32_t            max_sge;
	uint32_t            srq_limit;
	uint32_t            refcnt;
	pthread_mutex_t     lock;
};

static void qp_srq_get(struct uib_srq *srq)
{
	if (!srq)
		return;
	pthread_mutex_lock(&srq->lock);
	srq->refcnt++;
	pthread_mutex_unlock(&srq->lock);
}

static void qp_srq_put(struct uib_srq *srq)
{
	if (!srq)
		return;
	pthread_mutex_lock(&srq->lock);
	srq->refcnt--;
	pthread_mutex_unlock(&srq->lock);
}

static void qp_put_objects(struct uib_pd *pd, struct uib_cq *send_cq,
			   struct uib_cq *recv_cq)
{
	uib_cq_put(recv_cq);
	uib_cq_put(send_cq);
	uib_pd_put(pd);
}

static int qp_init_attr_valid(const struct uib_pd *pd,
			      const struct uib_qp_init_attr *attr)
{
	return pd && attr && attr->send_cq && attr->recv_cq &&
		(attr->qp_type == UIB_QPT_RC || attr->qp_type == UIB_QPT_UC ||
		 attr->qp_type == UIB_QPT_UD) &&
		attr->send_cq->ctx == pd->ctx && attr->recv_cq->ctx == pd->ctx &&
		attr->max_send_wr <= UIB_MAX_QP_WR &&
		attr->max_recv_wr <= UIB_MAX_QP_WR &&
		attr->max_send_sge <= UIB_MAX_SGE &&
		attr->max_recv_sge <= UIB_MAX_SGE;
}

/**
 * uib_create_qp - Create a queue pair from PD, CQ, and depth attributes.
 * @pd: Protection domain
 * @attr: Queue pair attributes
 * @uib-api: Queue pairs and data transfer
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_qp *uib_create_qp(struct uib_pd *pd,
			     const struct uib_qp_init_attr *attr)
{
	struct uib_qp *qp;
	int lock_initialized;
	int registered;
	int srq_referenced;
	int rc;

	if (!qp_init_attr_valid(pd, attr)) {
		errno = EINVAL;
		return NULL;
	}
	if (uib_pd_get(pd) < 0) {
		errno = EINVAL;
		return NULL;
	}
	if (uib_cq_get(attr->send_cq) < 0) {
		uib_pd_put(pd);
		errno = EINVAL;
		return NULL;
	}
	if (uib_cq_get(attr->recv_cq) < 0) {
		qp_put_objects(pd, attr->send_cq, NULL);
		errno = EINVAL;
		return NULL;
	}
	qp = calloc(1, sizeof(*qp));
	if (!qp) {
		qp_put_objects(pd, attr->send_cq, attr->recv_cq);
		return NULL;
	}
	lock_initialized = 0;
	registered = 0;
	srq_referenced = 0;

	qp->ctx          = pd->ctx;
	qp->pd           = pd;
	qp->send_cq      = attr->send_cq;
	qp->recv_cq      = attr->recv_cq;
	qp->srq          = attr->srq;
	qp->qp_type      = attr->qp_type;
	qp->state        = UIB_QPS_RESET;
	qp->max_send_sge = attr->max_send_sge ? attr->max_send_sge : 1;
	qp->max_recv_sge = attr->srq ? attr->srq->max_sge :
		(attr->max_recv_sge ? attr->max_recv_sge : 1);
	qp->max_send_wr  = attr->max_send_wr ? attr->max_send_wr : 64;
	qp->path_mtu     = UIB_DEFAULT_PATH_MTU;

	if (attr->srq) {
		if (attr->srq->pd != pd) {
			rc = -EINVAL;
			goto error;
		}
		qp_srq_get(attr->srq);
		srq_referenced = 1;
	} else {
		qp->rq_size = attr->max_recv_wr ?
			(int)attr->max_recv_wr + 1 : 65;
		qp->rq = calloc(qp->rq_size, sizeof(*qp->rq));
		if (!qp->rq) {
			rc = -ENOMEM;
			goto error;
		}
	}

	rc = pthread_mutex_init(&qp->lock, NULL);
	if (rc) {
		rc = -rc;
		goto error;
	}
	lock_initialized = 1;

	qp->qp_num = uib_register_qp(pd->ctx, qp);
	if (qp->qp_num == 0) {
		rc = -ENOSPC;
		goto error;
	}
	registered = 1;
	rc = qp->ctx->pops->qp_init ? qp->ctx->pops->qp_init(qp) : 0;
	if (rc < 0)
		goto error;
	return qp;

error:
	if (registered)
		uib_unregister_qp(pd->ctx, qp);
	if (lock_initialized)
		pthread_mutex_destroy(&qp->lock);
	if (srq_referenced)
		qp_srq_put(qp->srq);
	free(qp->rq);
	free(qp);
	qp_put_objects(pd, attr->send_cq, attr->recv_cq);
	errno = -rc;
	return NULL;
}

/**
 * uib_destroy_qp - Destroy a QP and protocol-specific outstanding state.
 * @qp: Queue pair
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_destroy_qp(struct uib_qp *qp)
{
	struct uib_context *ctx;
	uint32_t i;

	if (!qp)
		return -EINVAL;
	ctx = qp->ctx;
	uib_unregister_qp(ctx, qp);
	if (qp->ctx->pops->qp_fini)
		qp->ctx->pops->qp_fini(qp);

	pthread_mutex_lock(&qp->lock);
	if (!qp->srq) {
		for (i = 0; i < qp->rq_size; i++) {
			if (qp->rq[i].valid)
				uib_qp_release_recv(&qp->rq[i]);
		}
	}
	pthread_mutex_unlock(&qp->lock);

	pthread_mutex_destroy(&qp->lock);
	qp_srq_put(qp->srq);
	qp_put_objects(qp->pd, qp->send_cq, qp->recv_cq);
	free(qp->rq);
	free(qp);
	return 0;
}

/**
 * uib_create_srq - Create a receive queue shared by multiple QPs.
 * @pd: Protection domain
 * @attr: Shared receive queue capacity and scatter/gather limits
 * @uib-api: Queue pairs and data transfer
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_srq *uib_create_srq(struct uib_pd *pd,
			       const struct uib_srq_init_attr *attr)
{
	struct uib_srq *srq;

	if (!pd || !attr || !attr->max_wr || !attr->max_sge ||
	    attr->srq_limit > attr->max_wr ||
	    attr->max_wr > UIB_MAX_QP_WR || attr->max_sge > UIB_MAX_SGE) {
		errno = EINVAL;
		return NULL;
	}
	if (uib_pd_get(pd) < 0) {
		errno = EINVAL;
		return NULL;
	}
	srq = calloc(1, sizeof(*srq));
	if (!srq) {
		uib_pd_put(pd);
		return NULL;
	}
	srq->rq_size = attr->max_wr + 1;
	srq->rq = calloc(srq->rq_size, sizeof(*srq->rq));
	if (!srq->rq) {
		free(srq);
		uib_pd_put(pd);
		return NULL;
	}
	srq->pd = pd;
	srq->max_sge = attr->max_sge;
	srq->srq_limit = attr->srq_limit;
	pthread_mutex_init(&srq->lock, NULL);
	return srq;
}

/**
 * uib_modify_srq - Change supported shared receive queue attributes.
 * @srq: Shared receive queue
 * @attr: New attribute values
 * @attr_mask: Bitmask selecting attributes to change
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_modify_srq(struct uib_srq *srq, const struct uib_srq_attr *attr,
		   int attr_mask)
{
	if (!srq || !attr ||
	    (attr_mask & ~(UIB_SRQ_MAX_WR | UIB_SRQ_LIMIT)))
		return -EINVAL;
	pthread_mutex_lock(&srq->lock);
	if ((attr_mask & UIB_SRQ_MAX_WR) &&
	    attr->max_wr != srq->rq_size - 1) {
		pthread_mutex_unlock(&srq->lock);
		return -EOPNOTSUPP;
	}
	if ((attr_mask & UIB_SRQ_LIMIT) &&
	    attr->srq_limit > srq->rq_size - 1) {
		pthread_mutex_unlock(&srq->lock);
		return -EINVAL;
	}
	if (attr_mask & UIB_SRQ_LIMIT)
		srq->srq_limit = attr->srq_limit;
	pthread_mutex_unlock(&srq->lock);
	return 0;
}

/**
 * uib_query_srq - Read shared receive queue capacity and limits.
 * @srq: Shared receive queue
 * @attr: Output attribute structure
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_query_srq(struct uib_srq *srq, struct uib_srq_attr *attr)
{
	if (!srq || !attr)
		return -EINVAL;
	pthread_mutex_lock(&srq->lock);
	attr->max_wr = srq->rq_size - 1;
	attr->max_sge = srq->max_sge;
	attr->srq_limit = srq->srq_limit;
	pthread_mutex_unlock(&srq->lock);
	return 0;
}

/**
 * uib_destroy_srq - Destroy an unused shared receive queue.
 * @srq: Shared receive queue
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success, or -EBUSY while a QP still uses the SRQ.
 */
int uib_destroy_srq(struct uib_srq *srq)
{
	uint32_t i;

	if (!srq)
		return -EINVAL;
	pthread_mutex_lock(&srq->lock);
	if (srq->refcnt) {
		pthread_mutex_unlock(&srq->lock);
		return -EBUSY;
	}
	for (i = 0; i < srq->rq_size; i++) {
		if (srq->rq[i].valid)
			uib_qp_release_recv(&srq->rq[i]);
	}
	pthread_mutex_unlock(&srq->lock);
	pthread_mutex_destroy(&srq->lock);
	uib_pd_put(srq->pd);
	free(srq->rq);
	free(srq);
	return 0;
}

/**
 * uib_modify_qp - Change QP state, peer, PSNs, access, or path MTU.
 * @qp: Queue pair
 * @attr: Queue pair attributes
 * @attr_mask: Bitmask selecting attributes to modify
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_modify_qp(struct uib_qp *qp, const struct uib_qp_attr *attr,
		  int attr_mask)
{
	if (!qp || !attr)
		return -EINVAL;
	/*
	 * Reject an unsupported path MTU before any attribute is applied. The
	 * packet buffers of a protocol are sized from the maximum it declares,
	 * so accepting a larger value would overrun them on the first send.
	 */
	if ((attr_mask & UIB_QP_PATH_MTU) && attr->path_mtu &&
	    qp->ctx->pops && qp->ctx->pops->path_mtu_valid &&
	    !qp->ctx->pops->path_mtu_valid(attr->path_mtu))
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	if (attr_mask & UIB_QP_STATE)
		qp->state = attr->qp_state;
	if (attr_mask & UIB_QP_DEST_QPN)
		qp->dest_qp_num = attr->dest_qp_num;
	if (attr_mask & UIB_QP_RQ_PSN)
		qp->rq_psn = attr->rq_psn;
	if (attr_mask & UIB_QP_SQ_PSN)
		qp->sq_psn = attr->sq_psn;
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

int uib_qp_gather_total(struct uib_pd *pd, const struct uib_sge *sgl, int n,
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
		mr = uib_mr_lookup_local(pd, s->lkey,
					 (const void *)(uintptr_t)s->addr,
					 s->length, 0);
		if (!mr)
			return -EACCES;
		rc = uib_mr_read(mr, s->addr, out + off, s->length);
		uib_mr_put(mr);
		if (rc < 0)
			return rc;
		off += s->length;
	}
	return (int)off;
}

/**
 * uib_qp_gather - Gather an SGE list into a contiguous packet buffer.
 * @pd: Protection domain
 * @sgl: Scatter-gather list
 * @n: Number of scatter-gather entries
 * @out: Contiguous output buffer
 * @cap: Capacity of the output buffer in bytes
 * @uib-api: Queue pairs and data transfer
 *
 * Return: The total number of gathered bytes on success, or a negative error
 * code.
 */
int uib_qp_gather(struct uib_pd *pd, const struct uib_sge *sgl, int n,
		  uint8_t *out, size_t cap)
{
	return uib_qp_gather_total(pd, sgl, n, out, cap);
}

int uib_qp_scatter_at(struct uib_pd *pd, struct uib_sge *sgl, int n,
		      uint32_t msg_offset,
		      const uint8_t *in, size_t in_len, size_t *consumed_out)
{
	size_t skip = msg_offset;
	size_t off_in_sge;
	size_t consumed = 0;
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
	off_in_sge = skip;

	for (; i < n && consumed < in_len; i++) {
		struct uib_mr *mr;
		size_t avail = sgl[i].length - off_in_sge;
		size_t take = (avail < in_len - consumed) ? avail : (in_len - consumed);
		int rc;

		mr = uib_mr_lookup_local(pd, sgl[i].lkey,
					 (void *)(uintptr_t)(sgl[i].addr +
							     off_in_sge),
					 take, UIB_ACCESS_LOCAL_WRITE);
		if (!mr)
			return -EACCES;
		rc = uib_mr_place(mr, sgl[i].addr + off_in_sge,
				  in + consumed, take);
		uib_mr_put(mr);
		if (rc < 0)
			return rc;
		consumed += take;
		off_in_sge = 0;
	}
	*consumed_out = consumed;
	return 0;
}

/**
 * uib_qp_scatter - Scatter received bytes into an SGE list.
 * @pd: Protection domain
 * @sgl: Scatter-gather list
 * @n: Number of scatter-gather entries
 * @msg_offset: Offset in the scatter list at which to begin
 * @in: Contiguous input buffer
 * @in_len: Number of bytes available in the input buffer
 * @consumed_out: Output for the number of input bytes consumed
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_qp_scatter(struct uib_pd *pd, struct uib_sge *sgl, int n,
		   uint32_t msg_offset, const uint8_t *in, size_t in_len,
		   size_t *consumed_out)
{
	return uib_qp_scatter_at(pd, sgl, n, msg_offset, in, in_len, consumed_out);
}

int uib_qp_scatter_recv(struct uib_qp *qp, struct uib_rq_slot *slot,
			uint32_t msg_offset, const uint8_t *in,
			size_t in_len, size_t *consumed_out)
{
	if (!qp || !slot || !consumed_out)
		return -EINVAL;
	if (slot->private_data &&
	    !uib_host_loan_place(slot, msg_offset, in, in_len)) {
		*consumed_out = in_len;
		return 0;
	}
	return uib_qp_scatter_at(qp->pd, slot->sg_list, slot->num_sge, msg_offset,
			  in, in_len, consumed_out);
}

void qp_emit_send_vendor(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status st, uint32_t byte_len,
			 uint32_t vendor_err)
{
	struct uib_wc wc = {0};

	if (!qp->send_cq)
		return;
	if (st == UIB_WC_SUCCESS && !signaled)
		return;
	wc.wr_id    = wr_id;
	wc.status   = st;
	wc.qp_num   = qp->qp_num;
	wc.byte_len = byte_len;
	wc.vendor_err = vendor_err;
	switch (wr_op) {
	case UIB_WR_SEND:
	case UIB_WR_SEND_WITH_IMM:
		wc.opcode = UIB_WC_SEND;
		break;
	case UIB_WR_RDMA_WRITE:
	case UIB_WR_RDMA_WRITE_WITH_IMM:
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

void uib_qp_emit_send_wc(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status status, uint32_t byte_len)
{
	qp_emit_send_vendor(qp, wr_id, wr_op, signaled, status, byte_len, 0);
}

/**
 * uib_qp_emit_send - Publish a send work completion.
 * @qp: Queue pair
 * @wr_id: Caller-defined work request identifier
 * @wr_op: Work request operation code
 * @signaled: Nonzero to request a completion
 * @status: Completion status
 * @byte_len: Number of bytes completed
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_emit_send(struct uib_qp *qp, uint64_t wr_id,
		      enum uib_wr_opcode wr_op, int signaled,
		      enum uib_wc_status status, uint32_t byte_len)
{
	uib_qp_emit_send_wc(qp, wr_id, wr_op, signaled, status, byte_len);
}

static int emit_recv_wc(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wc_status st, enum uib_wc_opcode op,
			 uint32_t byte_len, uint32_t src_qp,
			 uint32_t imm_data, void *private_data,
			 const struct sockaddr_in *src)
{
	struct uib_wc wc = {0};

	if (!qp->recv_cq)
		return -EINVAL;

	wc.wr_id    = wr_id;
	wc.status   = st;
	wc.opcode   = op;
	wc.byte_len = byte_len;
	wc.qp_num   = qp->qp_num;
	wc.src_qp   = src_qp;
	wc.imm_data = imm_data;
	wc.private_data = private_data;
	if (src)
		wc.src_addr = *src;
	return uib_cq_push(qp->recv_cq, &wc);
}

/**
 * uib_qp_emit_recv - Publish a receive work completion.
 * @qp: Queue pair
 * @wr_id: Caller-defined work request identifier
 * @status: Completion status
 * @opcode: Completion operation code
 * @byte_len: Number of bytes completed
 * @src_qp: Source queue pair number
 * @imm_data: Immediate data in network byte order
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_emit_recv(struct uib_qp *qp, uint64_t wr_id,
		      enum uib_wc_status status, enum uib_wc_opcode opcode,
		      uint32_t byte_len, uint32_t src_qp, uint32_t imm_data)
{
	emit_recv_wc(qp, wr_id, status, opcode, byte_len, src_qp, imm_data,
		     NULL, NULL);
}

/**
 * uib_qp_emit_recv_slot - Publish a receive completion with private ownership.
 * @qp: Queue pair
 * @slot: Receive queue slot
 * @status: Completion status
 * @opcode: Completion operation code
 * @byte_len: Number of bytes completed
 * @src_qp: Source queue pair number
 * @imm_data: Immediate data in network byte order
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_emit_recv_slot(struct uib_qp *qp, struct uib_rq_slot *slot,
			   enum uib_wc_status status,
			   enum uib_wc_opcode opcode, uint32_t byte_len,
			   uint32_t src_qp, uint32_t imm_data)
{
	uib_qp_emit_recv_slot_src(qp, slot, status, opcode, byte_len, src_qp,
				  imm_data, NULL);
}

void uib_qp_emit_recv_slot_src(struct uib_qp *qp,
			       struct uib_rq_slot *slot,
			       enum uib_wc_status status,
			       enum uib_wc_opcode opcode,
			       uint32_t byte_len, uint32_t src_qp,
			       uint32_t imm_data,
			       const struct sockaddr_in *src)
{
	if (!qp || !slot)
		return;
	if (!emit_recv_wc(qp, slot->wr_id, status, opcode, byte_len, src_qp,
			  imm_data, slot->private_data, src)) {
		slot->private_data = NULL;
		slot->private_release = NULL;
	}
}

/**
 * uib_post_send - Post SEND, WRITE, READ, or atomic work requests.
 * @qp: Queue pair
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
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

static int post_recv_queue(struct uib_rq_slot *rq, uint32_t rq_size,
			   uint32_t *rq_head, uint32_t *rq_tail,
			   uint32_t max_sge, pthread_mutex_t *lock,
			   struct uib_recv_wr *wr,
			   struct uib_recv_wr **bad, void *private_data,
			   void (*private_release)(void *private_data))
{
	struct uib_rq_slot *slot;
	struct uib_recv_wr *cur;
	uint32_t next;

	if (!wr)
		return -EINVAL;

	pthread_mutex_lock(lock);
	for (cur = wr; cur; cur = cur->next) {
		if (cur->num_sge < 0 || (uint32_t)cur->num_sge > max_sge ||
		    (cur->num_sge && !cur->sg_list)) {
			pthread_mutex_unlock(lock);
			if (bad)
				*bad = cur;
			return -EINVAL;
		}
		next = (*rq_head + 1) % rq_size;

		if (next == *rq_tail) {
			pthread_mutex_unlock(lock);
			if (bad)
				*bad = cur;
			return -ENOSPC;
		}
		slot = &rq[*rq_head];

		slot->wr_id = cur->wr_id;
		slot->num_sge = cur->num_sge;
		slot->heap_sge = cur->num_sge > UIB_RQ_INLINE_SGE;
		if (slot->heap_sge) {
			slot->sg_list = malloc((size_t)cur->num_sge *
					       sizeof(*slot->sg_list));
			if (!slot->sg_list) {
				pthread_mutex_unlock(lock);
				if (bad)
					*bad = cur;
				return -ENOMEM;
			}
		} else {
			slot->sg_list = slot->inline_sge;
		}
		if (cur->num_sge)
			memcpy(slot->sg_list, cur->sg_list,
			       cur->num_sge * sizeof(struct uib_sge));
		if (private_data) {
			slot->private_data = private_data;
			slot->private_release = private_release;
			private_data = NULL;
			private_release = NULL;
		}
		slot->valid = 1;
		*rq_head = next;
	}
	pthread_mutex_unlock(lock);
	return 0;
}

static int post_recv_internal(struct uib_qp *qp, struct uib_recv_wr *wr,
			      struct uib_recv_wr **bad, void *private_data,
			      void (*private_release)(void *private_data))
{
	if (!qp || qp->srq)
		return -EINVAL;
	return post_recv_queue(qp->rq, qp->rq_size, &qp->rq_head,
			       &qp->rq_tail, qp->max_recv_sge, &qp->lock,
			       wr, bad, private_data, private_release);
}

/**
 * uib_post_recv - Post one or more receive work requests.
 * @qp: Queue pair
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_post_recv(struct uib_qp *qp, struct uib_recv_wr *wr,
		  struct uib_recv_wr **bad)
{
	return post_recv_internal(qp, wr, bad, NULL, NULL);
}

/**
 * uib_post_srq_recv - Post receive requests to a shared receive queue.
 * @srq: Shared receive queue
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_post_srq_recv(struct uib_srq *srq, struct uib_recv_wr *wr,
		      struct uib_recv_wr **bad)
{
	if (!srq)
		return -EINVAL;
	return post_recv_queue(srq->rq, srq->rq_size, &srq->rq_head,
			       &srq->rq_tail, srq->max_sge, &srq->lock,
			       wr, bad, NULL, NULL);
}

int uib_post_recv_owned(struct uib_qp *qp, struct uib_recv_wr *request,
			void *private_data,
			void (*private_release)(void *private_data))
{
	return post_recv_internal(qp, request, NULL, private_data,
				  private_release);
}

static int pop_recv_queue(struct uib_rq_slot *rq, uint32_t rq_size,
			  uint32_t *rq_head, uint32_t *rq_tail,
			  pthread_mutex_t *lock, struct uib_rq_slot *out)
{
	pthread_mutex_lock(lock);
	if (*rq_tail == *rq_head) {
		pthread_mutex_unlock(lock);
		return -ENOENT;
	}
	*out = rq[*rq_tail];
	if (!out->heap_sge)
		out->sg_list = out->inline_sge;
	rq[*rq_tail].valid = 0;
	rq[*rq_tail].sg_list = NULL;
	rq[*rq_tail].private_data = NULL;
	rq[*rq_tail].private_release = NULL;
	*rq_tail = (*rq_tail + 1) % rq_size;
	pthread_mutex_unlock(lock);
	return 0;
}

int uib_qp_pop_recv_slot(struct uib_qp *qp, struct uib_rq_slot *out)
{
	if (qp->srq)
		return pop_recv_queue(qp->srq->rq, qp->srq->rq_size,
				      &qp->srq->rq_head, &qp->srq->rq_tail,
				      &qp->srq->lock, out);
	return pop_recv_queue(qp->rq, qp->rq_size, &qp->rq_head,
			      &qp->rq_tail, &qp->lock, out);
}

/**
 * uib_qp_pop_recv - Consume the oldest posted receive request.
 * @qp: Queue pair
 * @out: Receive slot structure to fill
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_qp_pop_recv(struct uib_qp *qp, struct uib_rq_slot *out)
{
	return uib_qp_pop_recv_slot(qp, out);
}

/**
 * uib_qp_release_recv - Release storage retained by a popped receive slot.
 * @slot: Receive queue slot
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_release_recv(struct uib_rq_slot *slot)
{
	if (!slot)
		return;
	if (slot->heap_sge)
		free(slot->sg_list);
	if (slot->private_release)
		slot->private_release(slot->private_data);
	slot->sg_list = NULL;
	slot->heap_sge = 0;
	slot->private_data = NULL;
	slot->private_release = NULL;
}

/**
 * uib_qp_handle_packet - Dispatch a received protocol packet to a QP.
 * @qp: Queue pair
 * @src: Source transport address
 * @pkt: Received protocol packet
 * @len: Length of the byte range
 * @uib-api: Queue pairs and data transfer
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_qp_handle_packet(struct uib_qp *qp, const struct sockaddr_in *src,
			 const void *pkt, size_t len)
{
	return uib_qp_handle_packet_meta(qp, src, pkt, len, NULL);
}

int uib_qp_handle_packet_meta(struct uib_qp *qp,
			      const struct sockaddr_in *src, const void *pkt,
			      size_t len, const struct uib_rx_meta *meta)
{
	if (!qp || !src || !pkt)
		return -EINVAL;
	if (!qp->ctx || !qp->ctx->pops)
		return -EOPNOTSUPP;
	if (qp->ctx->pops->handle_packet)
		return qp->ctx->pops->handle_packet(qp, src, pkt, len, meta);
	return -EOPNOTSUPP;
}

/**
 * uib_qp_retransmit_tick - Run protocol retransmission work for a QP.
 * @qp: Queue pair
 * @now_ns: Current monotonic time in nanoseconds
 * @uib-api: Queue pairs and data transfer
 */
void uib_qp_retransmit_tick(struct uib_qp *qp, uint64_t now_ns)
{
	if (qp && qp->ctx->pops && qp->ctx->pops->retransmit_tick)
		qp->ctx->pops->retransmit_tick(qp, now_ns);
}
