#ifndef UIB_CQ_H
#define UIB_CQ_H

#include <stdint.h>
#include <pthread.h>
#include "uib/types.h"

struct uib_context;

struct uib_wc {
	uint64_t         wr_id;
	enum uib_wc_status status;
	enum uib_wc_opcode opcode;
	uint32_t         byte_len;
	uint32_t         qp_num;
	uint32_t         src_qp;
	uint32_t         imm_data;
};

struct uib_cq {
	struct uib_context *ctx;
	int                 cqe;
	int                 head;
	int                 tail;
	struct uib_wc      *ring;
	pthread_mutex_t     lock;
	pthread_cond_t      cond;
};

struct uib_cq *uib_create_cq(struct uib_context *ctx, int cqe);
int uib_destroy_cq(struct uib_cq *cq);

int uib_cq_push(struct uib_cq *cq, const struct uib_wc *wc);
int uib_poll_cq(struct uib_cq *cq, int n, struct uib_wc *out);

#endif
