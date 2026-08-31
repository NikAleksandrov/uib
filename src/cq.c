/* SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause-Views */
/*
 * The bounded MPMC completion ring is adapted from Dmitry Vyukov's
 * bounded MPMC queue.
 * Copyright (c) 2010-2011 Dmitry Vyukov. All rights reserved.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "uib/cq.h"
#include "uib/context.h"
#include "object_internal.h"

/**
 * uib_create_comp_channel - Create a pollable completion-event channel.
 * @ctx: UIB device context
 * @uib-api: Completion queues and events
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
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

/**
 * uib_destroy_comp_channel - Release a completion-event channel.
 * @channel: Completion-event channel
 * @uib-api: Completion queues and events
 *
 * Return: 0 on success or a negative error code on failure.
 */
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

/**
 * uib_comp_channel_fd - Return the fd used to poll a completion channel.
 * @channel: Completion-event channel
 * @uib-api: Completion queues and events
 *
 * Return: The channel's pollable file descriptor, or -1 if @channel is NULL.
 */
int uib_comp_channel_fd(const struct uib_comp_channel *channel)
{
	return channel ? channel->fd : -1;
}

/**
 * uib_create_cq_channel - Create a CQ attached to an event channel.
 * @ctx: UIB device context
 * @cqe: Requested completion queue capacity
 * @channel: Completion-event channel
 * @event_context: Caller-defined completion-event context
 * @uib-api: Completion queues and events
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_cq *uib_create_cq_channel(struct uib_context *ctx, int cqe,
				     struct uib_comp_channel *channel,
				     void *event_context)
{
	struct uib_cq *cq;
	uint32_t capacity;
	uint32_t i;

	if (!ctx || cqe <= 0 || (channel && channel->ctx != ctx)) {
		errno = EINVAL;
		return NULL;
	}
	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return NULL;
	capacity = 1;
	while (capacity < (uint32_t)cqe) {
		if (capacity > UINT32_MAX / 2) {
			free(cq);
			errno = EOVERFLOW;
			return NULL;
		}
		capacity <<= 1;
	}
	cq->ring = calloc(capacity, sizeof(*cq->ring));
	if (!cq->ring) {
		free(cq);
		return NULL;
	}
	cq->ctx = ctx;
	cq->cqe = capacity;
	cq->mask = capacity - 1;
	for (i = 0; i < capacity; i++)
		cq->ring[i].sequence = i;
	cq->channel = channel;
	cq->event_context = event_context;
	cq->refcnt = 1;
	pthread_mutex_init(&cq->lock, NULL);
	pthread_cond_init(&cq->cond, NULL);
	if (channel) {
		pthread_mutex_lock(&channel->lock);
		channel->cq_refs++;
		pthread_mutex_unlock(&channel->lock);
	}
	return cq;
}

/**
 * uib_create_cq - Create a completion queue without an event channel.
 * @ctx: UIB device context
 * @cqe: Requested completion queue capacity
 * @uib-api: Completion queues and events
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_cq *uib_create_cq(struct uib_context *ctx, int cqe)
{
	return uib_create_cq_channel(ctx, cqe, NULL, NULL);
}

/**
 * uib_destroy_cq - Release an unused completion queue.
 * @cq: Completion queue
 * @uib-api: Completion queues and events
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_destroy_cq(struct uib_cq *cq)
{
	struct uib_comp_channel *channel;

	if (!cq)
		return -EINVAL;
	pthread_mutex_lock(&cq->lock);
	if (cq->refcnt != 1) {
		pthread_mutex_unlock(&cq->lock);
		return -EBUSY;
	}
	cq->refcnt = 0;
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

int uib_cq_get(struct uib_cq *cq)
{
	if (!cq)
		return -EINVAL;
	pthread_mutex_lock(&cq->lock);
	if (!cq->refcnt) {
		pthread_mutex_unlock(&cq->lock);
		return -EINVAL;
	}
	cq->refcnt++;
	pthread_mutex_unlock(&cq->lock);
	return 0;
}

void uib_cq_put(struct uib_cq *cq)
{
	if (!cq)
		return;
	pthread_mutex_lock(&cq->lock);
	if (cq->refcnt > 1)
		cq->refcnt--;
	pthread_mutex_unlock(&cq->lock);
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

/**
 * uib_cq_push - Publish one completion to a CQ.
 * @cq: Completion queue
 * @wc: Work completion to enqueue
 * @uib-api: Completion queues and events
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_cq_push(struct uib_cq *cq, const struct uib_wc *wc)
{
	struct uib_cq_event *event = NULL;
	struct uib_comp_channel *channel = NULL;
	struct uib_cq_slot *slot;
	uint64_t position;
	uint64_t sequence;
	intptr_t difference;

	position = __atomic_load_n(&cq->enqueue_pos, __ATOMIC_RELAXED);
	for (;;) {
		slot = &cq->ring[position & cq->mask];
		sequence = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
		difference = (intptr_t)(sequence - position);
		if (difference < 0) {
			__atomic_store_n(&cq->overrun, 1, __ATOMIC_RELEASE);
			return -ENOSPC;
		}
		if (difference > 0) {
			position = __atomic_load_n(&cq->enqueue_pos,
						   __ATOMIC_RELAXED);
			continue;
		}
		if (__atomic_compare_exchange_n(&cq->enqueue_pos, &position,
						position + 1, 0,
						__ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			break;
	}
	slot->wc = *wc;
	__atomic_store_n(&slot->sequence, position + 1, __ATOMIC_RELEASE);
	if (cq->channel) {
		pthread_mutex_lock(&cq->lock);
		if (cq->notify_armed) {
			cq->notify_armed = 0;
			event = cq->armed_event;
			cq->armed_event = NULL;
			channel = cq->channel;
			cq->events_outstanding++;
		}
		pthread_mutex_unlock(&cq->lock);
	}
	if (event)
		return channel_push(channel, event);
	return 0;
}

/**
 * uib_poll_cq - Poll completed work requests from a CQ.
 * @cq: Completion queue
 * @n: Maximum number of completions to return
 * @out: Output array for completed work requests
 * @uib-api: Completion queues and events
 *
 * Return: The number of completions written to @out, or a negative error code.
 */
int uib_poll_cq(struct uib_cq *cq, int n, struct uib_wc *out)
{
	struct uib_cq_slot *slot;
	uint64_t position;
	uint64_t sequence;
	intptr_t difference;
	int got = 0;

	if (!cq || n < 0 || (n && !out))
		return -EINVAL;
	while (got < n) {
		position = __atomic_load_n(&cq->dequeue_pos, __ATOMIC_RELAXED);
		for (;;) {
			slot = &cq->ring[position & cq->mask];
			sequence = __atomic_load_n(&slot->sequence,
						   __ATOMIC_ACQUIRE);
			difference = (intptr_t)(sequence - (position + 1));
			if (difference < 0)
				goto out;
			if (difference > 0) {
				position = __atomic_load_n(&cq->dequeue_pos,
							   __ATOMIC_RELAXED);
				continue;
			}
			if (__atomic_compare_exchange_n(&cq->dequeue_pos,
							&position,
							position + 1, 0,
							__ATOMIC_RELAXED,
							__ATOMIC_RELAXED))
				break;
		}
		out[got++] = slot->wc;
		__atomic_store_n(&slot->sequence, position + cq->cqe,
				 __ATOMIC_RELEASE);
	}
out:
	if (n && !got &&
	    __atomic_exchange_n(&cq->overrun, 0, __ATOMIC_ACQ_REL))
		return -EOVERFLOW;
	return got;
}

/**
 * uib_req_notify_cq - Arm one-shot notification for the next completion.
 * @cq: Completion queue
 * @solicited_only: Nonzero to notify only for solicited completions
 * @uib-api: Completion queues and events
 *
 * Return: 0 on success or a negative error code on failure.
 */
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

/**
 * uib_get_cq_event - Consume the next completion-channel event.
 * @channel: Completion-event channel
 * @cq: Output for the completion queue that raised the event
 * @event_context: Output for the completion queue's caller context
 * @uib-api: Completion queues and events
 *
 * Return: 0 on success or a negative error code on failure.
 */
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

/**
 * uib_ack_cq_events - Acknowledge CQ events.
 * @cq: Completion queue
 * @events: Number of completion events to acknowledge
 * @uib-api: Completion queues and events
 */
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
