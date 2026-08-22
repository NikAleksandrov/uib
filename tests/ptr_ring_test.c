/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/ptr_ring.h"

#define PTR_RING_CAPACITY 1024
#define PTR_RING_PRODUCERS 4
#define PTR_RING_CONSUMERS 4
#define PTR_RING_ITEMS_PER_PRODUCER 100000

struct ptr_ring_test {
	struct uib_ptr_ring ring;
	uint8_t            *seen;
	uint32_t            consumed;
	uint32_t            errors;
};

struct producer_arg {
	struct ptr_ring_test *test;
	uint32_t              first;
};

static void report(const char *name, int pass)
{
	printf("%-32s %s\n", name, pass ? "OK" : "FAIL");
}

static void *producer_loop(void *opaque)
{
	struct producer_arg *arg = opaque;
	struct ptr_ring_test *test = arg->test;
	uint32_t value;
	int rc;

	for (value = arg->first;
	     value < arg->first + PTR_RING_ITEMS_PER_PRODUCER; value++) {
		do {
			rc = uib_ptr_ring_push(&test->ring,
					       (void *)(uintptr_t)(value + 1));
			if (rc == -ENOSPC)
				sched_yield();
		} while (rc == -ENOSPC);
		if (rc < 0) {
			__atomic_add_fetch(&test->errors, 1, __ATOMIC_RELAXED);
			break;
		}
	}
	return NULL;
}

static void *consumer_loop(void *opaque)
{
	struct ptr_ring_test *test = opaque;
	const uint32_t total = PTR_RING_PRODUCERS *
		PTR_RING_ITEMS_PER_PRODUCER;
	uint32_t value;
	void *pointer;

	while (__atomic_load_n(&test->consumed, __ATOMIC_ACQUIRE) < total) {
		pointer = uib_ptr_ring_pop(&test->ring);
		if (!pointer) {
			sched_yield();
			continue;
		}
		value = (uint32_t)(uintptr_t)pointer - 1;
		if (value >= total ||
		    __atomic_exchange_n(&test->seen[value], 1,
					__ATOMIC_RELAXED))
			__atomic_add_fetch(&test->errors, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&test->consumed, 1, __ATOMIC_RELEASE);
	}
	return NULL;
}

static int test_batch_order(void)
{
	struct uib_ptr_ring ring = {0};
	uint64_t position;
	unsigned int i;
	int was_empty;
	int pass;
	int rc;

	rc = uib_ptr_ring_init(&ring, 8);
	if (rc < 0)
		return 0;
	rc = uib_ptr_ring_reserve(&ring, 4, &position, &was_empty);
	if (rc < 0) {
		uib_ptr_ring_destroy(&ring);
		return 0;
	}
	for (i = 0; i < 4; i++)
		uib_ptr_ring_publish(&ring, position + i,
				     (void *)(uintptr_t)(i + 1));
	pass = was_empty && uib_ptr_ring_count(&ring) == 4;
	for (i = 0; i < 4; i++) {
		if (uib_ptr_ring_pop(&ring) != (void *)(uintptr_t)(i + 1))
			pass = 0;
	}
	if (uib_ptr_ring_pop(&ring) || uib_ptr_ring_count(&ring))
		pass = 0;
	uib_ptr_ring_destroy(&ring);
	return pass;
}

static int test_mpmc(void)
{
	struct producer_arg producers[PTR_RING_PRODUCERS];
	pthread_t producer_threads[PTR_RING_PRODUCERS];
	pthread_t consumer_threads[PTR_RING_CONSUMERS];
	struct ptr_ring_test test = {0};
	const uint32_t total = PTR_RING_PRODUCERS *
		PTR_RING_ITEMS_PER_PRODUCER;
	unsigned int consumers_started;
	unsigned int producers_started;
	unsigned int i;
	int pass;

	test.seen = calloc(total, sizeof(*test.seen));
	if (!test.seen || uib_ptr_ring_init(&test.ring,
					    PTR_RING_CAPACITY) < 0) {
		free(test.seen);
		return 0;
	}
	consumers_started = 0;
	producers_started = 0;
	for (i = 0; i < PTR_RING_CONSUMERS; i++) {
		if (pthread_create(&consumer_threads[i], NULL, consumer_loop,
				   &test))
			break;
		consumers_started++;
	}
	if (consumers_started == PTR_RING_CONSUMERS) {
		for (i = 0; i < PTR_RING_PRODUCERS; i++) {
			producers[i].test = &test;
			producers[i].first = i * PTR_RING_ITEMS_PER_PRODUCER;
			if (pthread_create(&producer_threads[i], NULL,
					   producer_loop, &producers[i]))
				break;
			producers_started++;
		}
	}
	for (i = 0; i < producers_started; i++)
		pthread_join(producer_threads[i], NULL);
	if (producers_started != PTR_RING_PRODUCERS ||
	    consumers_started != PTR_RING_CONSUMERS)
		__atomic_store_n(&test.consumed, total, __ATOMIC_RELEASE);
	for (i = 0; i < consumers_started; i++)
		pthread_join(consumer_threads[i], NULL);
	pass = producers_started == PTR_RING_PRODUCERS &&
		consumers_started == PTR_RING_CONSUMERS && !test.errors &&
		test.consumed == total && !uib_ptr_ring_count(&test.ring);
	uib_ptr_ring_destroy(&test.ring);
	free(test.seen);
	return pass;
}

int main(void)
{
	int batch;
	int mpmc;

	batch = test_batch_order();
	mpmc = test_mpmc();
	report("ptr-ring.batch-order", batch);
	report("ptr-ring.mpmc", mpmc);
	printf("RESULT %s\n", batch && mpmc ? "PASS" : "FAIL");
	return batch && mpmc ? 0 : 1;
}
