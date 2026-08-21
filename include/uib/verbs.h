/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_VERBS_H
#define UIB_VERBS_H

#include "uib/context.h"
#include "uib/pd.h"
#include "uib/mr.h"
#include "uib/cq.h"
#include "uib/qp.h"

struct uibv_device {
	char                 name[64];
	struct sockaddr_in   bind_addr;
};

struct uibv_device **uibv_get_device_list(int *num);
void                 uibv_free_device_list(struct uibv_device **list);
const char          *uibv_get_device_name(struct uibv_device *dev);

struct uib_context *uibv_open_device(struct uibv_device *dev);
int                 uibv_close_device(struct uib_context *ctx);

struct uib_pd *uibv_alloc_pd(struct uib_context *ctx);
int            uibv_dealloc_pd(struct uib_pd *pd);

struct uib_mr *uibv_reg_mr(struct uib_pd *pd, void *addr, size_t len,
			   int access);
struct uib_mr *uibv_reg_dmabuf_mr(struct uib_pd *pd, uint64_t offset,
				  size_t len, uint64_t iova, int fd,
				  int access);
int            uibv_dereg_mr(struct uib_mr *mr);

struct uib_cq *uibv_create_cq(struct uib_context *ctx, int cqe);
struct uib_comp_channel *uibv_create_comp_channel(struct uib_context *ctx);
int uibv_destroy_comp_channel(struct uib_comp_channel *channel);
int uibv_get_cq_event(struct uib_comp_channel *channel, struct uib_cq **cq,
			      void **event_context);
void uibv_ack_cq_events(struct uib_cq *cq, unsigned int events);
int uibv_req_notify_cq(struct uib_cq *cq, int solicited_only);
int            uibv_destroy_cq(struct uib_cq *cq);
int            uibv_poll_cq(struct uib_cq *cq, int n, struct uib_wc *wc);

struct uib_qp *uibv_create_qp(struct uib_pd *pd, struct uib_qp_init_attr *attr);
int            uibv_modify_qp(struct uib_qp *qp,
			      struct uib_qp_attr *attr, int mask);
int            uibv_destroy_qp(struct uib_qp *qp);
int            uibv_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
			      struct uib_send_wr **bad);
int            uibv_post_recv(struct uib_qp *qp, struct uib_recv_wr *wr,
			      struct uib_recv_wr **bad);

#ifdef UIB_VERBS_COMPAT
#define ibv_device         uibv_device
#define ibv_context        uib_context
#define ibv_pd             uib_pd
#define ibv_mr             uib_mr
#define ibv_cq             uib_cq
#define ibv_qp             uib_qp
#define ibv_wc             uib_wc
#define ibv_sge            uib_sge
#define ibv_send_wr        uib_send_wr
#define ibv_recv_wr        uib_recv_wr
#define ibv_qp_init_attr   uib_qp_init_attr
#define ibv_qp_attr        uib_qp_attr
#define ibv_get_device_list  uibv_get_device_list
#define ibv_free_device_list uibv_free_device_list
#define ibv_open_device      uibv_open_device
#define ibv_close_device     uibv_close_device
#define ibv_alloc_pd         uibv_alloc_pd
#define ibv_dealloc_pd       uibv_dealloc_pd
#define ibv_reg_mr           uibv_reg_mr
#define ibv_reg_dmabuf_mr    uibv_reg_dmabuf_mr
#define ibv_dereg_mr         uibv_dereg_mr
#define ibv_create_cq(c, n, ...) uibv_create_cq(c, n)
#define ibv_create_comp_channel uibv_create_comp_channel
#define ibv_destroy_comp_channel uibv_destroy_comp_channel
#define ibv_get_cq_event      uibv_get_cq_event
#define ibv_ack_cq_events     uibv_ack_cq_events
#define ibv_req_notify_cq     uibv_req_notify_cq
#define ibv_destroy_cq       uibv_destroy_cq
#define ibv_poll_cq          uibv_poll_cq
#define ibv_create_qp        uibv_create_qp
#define ibv_modify_qp        uibv_modify_qp
#define ibv_destroy_qp       uibv_destroy_qp
#define ibv_post_send        uibv_post_send
#define ibv_post_recv        uibv_post_recv
#endif

#endif
