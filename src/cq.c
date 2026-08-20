// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "uib/cq.h"
#include "uib/context.h"

struct uib_comp_channel *uib_create_comp_channel(struct uib_context *ctx)
{
	struct uib_comp_channel *channel;

	if (!ctx) {
		errno = EINVAL;
		return NULL;
	}
	channel = calloc(1, sizeof(*channel));
	if (!channel)
		return NULL;
	channel->fd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
	if (channel->fd < 0) {
		free(channel);
		return NULL;
	}
	channel->ctx = ctx;
	pthread_mutex_init(&channel->lock, NULL);
	return channel;
}

int uib_destroy_comp_channel(struct uib_comp_channel *channel)
{
	if (!channel)
		return -EINVAL;
	pthread_mutex_lock(&channel->lock);
	if (channel->cq_refs || channel->head) {
		pthread_mutex_unlock(&channel->lock);
		return -EBUSY;
	}
	pthread_mutex_unlock(&channel->lock);
	pthread_mutex_destroy(&channel->lock);
	close(channel->fd);
	free(channel);
	return 0;
}

int uib_comp_channel_fd(const struct uib_comp_channel *channel)
{
	return channel ? channel->fd : -1;
}

struct uib_cq *uib_create_cq_channel(struct uib_context *ctx, int cqe,
				     struct uib_comp_channel *channel,
				     void *event_context)
{
	struct uib_cq *cq;

	if (!ctx || cqe <= 0 || (channel && channel->ctx != ctx)) {
		errno = EINVAL;
		return NULL;
	}
	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return NULL;
	cq->ring = calloc((size_t)cqe + 1, sizeof(*cq->ring));
	if (!cq->ring) {
		free(cq);
		return NULL;
	}
	cq->ctx = ctx;
	cq->cqe = cqe + 1;
	cq->channel = channel;
	cq->event_context = event_context;
	pthread_mutex_init(&cq->lock, NULL);
	pthread_cond_init(&cq->cond, NULL);
	if (channel) {
		pthread_mutex_lock(&channel->lock);
		channel->cq_refs++;
		pthread_mutex_unlock(&channel->lock);
	}
	return cq;
}

struct uib_cq *uib_create_cq(struct uib_context *ctx, int cqe)
{
	return uib_create_cq_channel(ctx, cqe, NULL, NULL);
}

int uib_destroy_cq(struct uib_cq *cq)
{
	struct uib_comp_channel *channel;

	if (!cq)
		return -EINVAL;
	pthread_mutex_lock(&cq->lock);
	while (cq->events_outstanding)
		pthread_cond_wait(&cq->cond, &cq->lock);
	free(cq->armed_event);
	cq->armed_event = NULL;
	channel = cq->channel;
	pthread_mutex_unlock(&cq->lock);
	if (channel) {
		pthread_mutex_lock(&channel->lock);
		channel->cq_refs--;
		pthread_mutex_unlock(&channel->lock);
	}
	pthread_cond_destroy(&cq->cond);
	pthread_mutex_destroy(&cq->lock);
	free(cq->ring);
	free(cq);
	return 0;
}

static int channel_push(struct uib_comp_channel *channel,
			struct uib_cq_event *event)
{
	uint64_t signal = 1;
	ssize_t length;
	int saved;

	pthread_mutex_lock(&channel->lock);
	event->next = NULL;
	if (channel->tail)
		channel->tail->next = event;
	else
		channel->head = event;
	channel->tail = event;
	pthread_mutex_unlock(&channel->lock);
	do {
		length = write(channel->fd, &signal, sizeof(signal));
	} while (length < 0 && errno == EINTR);
	if (length == sizeof(signal))
		return 0;
	saved = errno;
	return -saved;
}

int uib_cq_push(struct uib_cq *cq, const struct uib_wc *wc)
{
	struct uib_cq_event *event = NULL;
	struct uib_comp_channel *channel = NULL;
	int next;

	pthread_mutex_lock(&cq->lock);
	next = (cq->head + 1) % cq->cqe;
	if (next == cq->tail) {
		pthread_mutex_unlock(&cq->lock);
		return -ENOSPC;
	}
	cq->ring[cq->head] = *wc;
	cq->head = next;
	if (cq->notify_armed) {
		cq->notify_armed = 0;
		event = cq->armed_event;
		cq->armed_event = NULL;
		channel = cq->channel;
		cq->events_outstanding++;
	}
	pthread_cond_signal(&cq->cond);
	pthread_mutex_unlock(&cq->lock);
	if (event)
		return channel_push(channel, event);
	return 0;
}

int uib_poll_cq(struct uib_cq *cq, int n, struct uib_wc *out)
{
	int got = 0;

	if (!cq || n < 0 || (n && !out))
		return -EINVAL;
	pthread_mutex_lock(&cq->lock);
	while (got < n && cq->tail != cq->head) {
		out[got++] = cq->ring[cq->tail];
		cq->tail = (cq->tail + 1) % cq->cqe;
	}
	pthread_mutex_unlock(&cq->lock);
	return got;
}

int uib_req_notify_cq(struct uib_cq *cq, int solicited_only)
{
	struct uib_cq_event *event;

	(void)solicited_only;
	if (!cq || !cq->channel)
		return -EINVAL;
	event = calloc(1, sizeof(*event));
	if (!event)
		return -ENOMEM;
	event->cq = cq;
	pthread_mutex_lock(&cq->lock);
	if (cq->notify_armed) {
		pthread_mutex_unlock(&cq->lock);
		free(event);
		return 0;
	}
	cq->armed_event = event;
	cq->notify_armed = 1;
	pthread_mutex_unlock(&cq->lock);
	return 0;
}

int uib_get_cq_event(struct uib_comp_channel *channel, struct uib_cq **cq,
			     void **event_context)
{
	struct uib_cq_event *event;
	uint64_t signal;
	ssize_t length;

	if (!channel || !cq)
		return -EINVAL;
	do {
		length = read(channel->fd, &signal, sizeof(signal));
	} while (length < 0 && errno == EINTR);
	if (length < 0)
		return -errno;
	if (length != sizeof(signal))
		return -EIO;
	pthread_mutex_lock(&channel->lock);
	event = channel->head;
	if (event) {
		channel->head = event->next;
		if (!channel->head)
			channel->tail = NULL;
	}
	pthread_mutex_unlock(&channel->lock);
	if (!event)
		return -EIO;
	*cq = event->cq;
	if (event_context)
		*event_context = event->cq->event_context;
	free(event);
	return 0;
}

void uib_ack_cq_events(struct uib_cq *cq, unsigned int events)
{
	if (!cq || !events)
		return;
	pthread_mutex_lock(&cq->lock);
	if (events > cq->events_outstanding)
		events = cq->events_outstanding;
	cq->events_outstanding -= events;
	if (!cq->events_outstanding)
		pthread_cond_broadcast(&cq->cond);
	pthread_mutex_unlock(&cq->lock);
}
