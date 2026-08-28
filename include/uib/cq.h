/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_CQ_H
#define UIB_CQ_H

#include <stdint.h>
#include <pthread.h>
#include "uib/types.h"

struct uib_context;
struct uib_cq;

struct uib_cq_event {
	struct uib_cq_event *next;
	struct uib_cq       *cq;
};

struct uib_comp_channel {
	struct uib_context  *ctx;
	struct uib_cq_event *tail;
	struct uib_cq_event *head;
	pthread_mutex_t      lock;
	unsigned int         cq_refs;
	int                  fd;
};

struct uib_wc {
	uint64_t         wr_id;
	enum uib_wc_status status;
	enum uib_wc_opcode opcode;
	uint32_t         byte_len;
	uint32_t         qp_num;
	uint32_t         src_qp;
	uint32_t         imm_data;
	uint32_t         vendor_err;
	void            *private_data;
};

struct uib_cq_slot {
	struct uib_wc wc;
	uint64_t      sequence;
};

struct uib_cq {
	struct uib_context *ctx;
	struct uib_cq_slot *ring;
	uint64_t            enqueue_pos;
	uint64_t            dequeue_pos;
	uint32_t            mask;
	uint32_t            cqe;
	struct uib_comp_channel *channel;
	struct uib_cq_event *armed_event;
	void                *event_context;
	unsigned int         events_outstanding;
	int                  notify_armed;
	pthread_mutex_t     lock;
	pthread_cond_t      cond;
};

struct uib_comp_channel *uib_create_comp_channel(struct uib_context *ctx);
int uib_destroy_comp_channel(struct uib_comp_channel *channel);
int uib_comp_channel_fd(const struct uib_comp_channel *channel);

struct uib_cq *uib_create_cq(struct uib_context *ctx, int cqe);
struct uib_cq *uib_create_cq_channel(struct uib_context *ctx, int cqe,
				     struct uib_comp_channel *channel,
				     void *event_context);
int uib_destroy_cq(struct uib_cq *cq);

int uib_cq_push(struct uib_cq *cq, const struct uib_wc *wc);
int uib_poll_cq(struct uib_cq *cq, int n, struct uib_wc *out);
int uib_req_notify_cq(struct uib_cq *cq, int solicited_only);
int uib_get_cq_event(struct uib_comp_channel *channel, struct uib_cq **cq,
			     void **event_context);
void uib_ack_cq_events(struct uib_cq *cq, unsigned int events);

#endif
