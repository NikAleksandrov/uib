/* SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause-Views */
/*
 * The bounded MPMC pointer ring is adapted from Dmitry Vyukov's bounded
 * MPMC queue.
 * Copyright (c) 2010-2011 Dmitry Vyukov. All rights reserved.
 */
#ifndef UIB_PTR_RING_INTERNAL_H
#define UIB_PTR_RING_INTERNAL_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

struct uib_ptr_ring_slot {
	uint64_t sequence;
	void *pointer;
};

struct uib_ptr_ring {
	struct uib_ptr_ring_slot *slots;
	uint64_t enqueue_pos __attribute__((aligned(64)));
	uint64_t dequeue_pos __attribute__((aligned(64)));
	uint32_t capacity;
	uint32_t mask;
};

static inline int uib_ptr_ring_init(struct uib_ptr_ring *ring,
				    uint32_t capacity)
{
	uint32_t i;

	if (!ring || !capacity || (capacity & (capacity - 1)))
		return -EINVAL;
	ring->slots = calloc(capacity, sizeof(*ring->slots));
	if (!ring->slots)
		return -ENOMEM;
	ring->enqueue_pos = 0;
	ring->dequeue_pos = 0;
	ring->capacity = capacity;
	ring->mask = capacity - 1;
	for (i = 0; i < capacity; i++)
		ring->slots[i].sequence = i;
	return 0;
}

static inline void uib_ptr_ring_destroy(struct uib_ptr_ring *ring)
{
	if (!ring)
		return;
	free(ring->slots);
	ring->slots = NULL;
	ring->enqueue_pos = 0;
	ring->dequeue_pos = 0;
	ring->capacity = 0;
	ring->mask = 0;
}

static inline int uib_ptr_ring_reserve(struct uib_ptr_ring *ring,
				       uint32_t count, uint64_t *start,
				       int *was_empty)
{
	uint64_t dequeue;
	uint64_t position;

	if (!ring || !count || count > ring->capacity || !start)
		return -EINVAL;
	position = __atomic_load_n(&ring->enqueue_pos, __ATOMIC_RELAXED);
	for (;;) {
		dequeue = __atomic_load_n(&ring->dequeue_pos, __ATOMIC_ACQUIRE);
		if (position - dequeue > ring->capacity - count)
			return -ENOSPC;
		if (__atomic_compare_exchange_n(&ring->enqueue_pos, &position,
						position + count, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			break;
	}
	*start = position;
	if (was_empty)
		*was_empty = position == dequeue;
	return 0;
}

static inline void uib_ptr_ring_publish(struct uib_ptr_ring *ring,
					uint64_t position, void *pointer)
{
	struct uib_ptr_ring_slot *slot;

	slot = &ring->slots[position & ring->mask];
	while (__atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE) != position)
		__atomic_signal_fence(__ATOMIC_ACQ_REL);
	slot->pointer = pointer;
	__atomic_store_n(&slot->sequence, position + 1, __ATOMIC_RELEASE);
}

static inline int uib_ptr_ring_push(struct uib_ptr_ring *ring, void *pointer)
{
	uint64_t position;
	int rc;

	rc = uib_ptr_ring_reserve(ring, 1, &position, NULL);
	if (rc < 0)
		return rc;
	uib_ptr_ring_publish(ring, position, pointer);
	return 0;
}

static inline void *uib_ptr_ring_pop(struct uib_ptr_ring *ring)
{
	struct uib_ptr_ring_slot *slot;
	uint64_t sequence;
	uint64_t position;
	intptr_t difference;
	void *pointer;

	if (!ring || !ring->slots)
		return NULL;
	position = __atomic_load_n(&ring->dequeue_pos, __ATOMIC_RELAXED);
	for (;;) {
		slot = &ring->slots[position & ring->mask];
		sequence = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
		difference = (intptr_t)(sequence - (position + 1));
		if (difference < 0)
			return NULL;
		if (difference > 0) {
			position = __atomic_load_n(&ring->dequeue_pos,
						   __ATOMIC_RELAXED);
			continue;
		}
		if (__atomic_compare_exchange_n(&ring->dequeue_pos, &position,
						position + 1, 0,
						__ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			break;
	}
	pointer = slot->pointer;
	__atomic_store_n(&slot->sequence, position + ring->capacity,
			 __ATOMIC_RELEASE);
	return pointer;
}

static inline uint64_t uib_ptr_ring_count(const struct uib_ptr_ring *ring)
{
	uint64_t enqueue;
	uint64_t dequeue;

	if (!ring || !ring->slots)
		return 0;
	enqueue = __atomic_load_n(&ring->enqueue_pos, __ATOMIC_ACQUIRE);
	dequeue = __atomic_load_n(&ring->dequeue_pos, __ATOMIC_ACQUIRE);
	return enqueue - dequeue;
}

#endif
