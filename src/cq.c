#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "uib/cq.h"
#include "uib/context.h"

struct uib_cq *uib_create_cq(struct uib_context *ctx, int cqe)
{
	if (!ctx || cqe <= 0) {
		errno = EINVAL;
		return NULL;
	}
	struct uib_cq *cq = calloc(1, sizeof(*cq));

	if (!cq)
		return NULL;
	cq->ring = calloc((size_t)cqe + 1, sizeof(*cq->ring));
	if (!cq->ring) {
		free(cq);
		return NULL;
	}
	cq->ctx  = ctx;
	cq->cqe  = cqe + 1;
	cq->head = 0;
	cq->tail = 0;
	pthread_mutex_init(&cq->lock, NULL);
	pthread_cond_init(&cq->cond, NULL);
	return cq;
}

int uib_destroy_cq(struct uib_cq *cq)
{
	if (!cq)
		return -EINVAL;
	pthread_cond_destroy(&cq->cond);
	pthread_mutex_destroy(&cq->lock);
	free(cq->ring);
	free(cq);
	return 0;
}

int uib_cq_push(struct uib_cq *cq, const struct uib_wc *wc)
{
	pthread_mutex_lock(&cq->lock);
	int next = (cq->head + 1) % cq->cqe;

	if (next == cq->tail) {
		pthread_mutex_unlock(&cq->lock);
		return -ENOSPC;
	}
	cq->ring[cq->head] = *wc;
	cq->head = next;
	pthread_cond_signal(&cq->cond);
	pthread_mutex_unlock(&cq->lock);
	return 0;
}

int uib_poll_cq(struct uib_cq *cq, int n, struct uib_wc *out)
{
	int got = 0;

	pthread_mutex_lock(&cq->lock);
	while (got < n && cq->tail != cq->head) {
		out[got++] = cq->ring[cq->tail];
		cq->tail = (cq->tail + 1) % cq->cqe;
	}
	pthread_mutex_unlock(&cq->lock);
	return got;
}
