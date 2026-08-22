/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uib/context.h"
#include "uib/mr.h"
#include "uib/pd.h"

struct deregistration {
	struct uib_mr *mr;
	int            finished;
	int            rc;
};

static void *deregister_mr(void *opaque)
{
	struct deregistration *state = opaque;

	state->rc = uib_dereg_mr(state->mr);
	__atomic_store_n(&state->finished, 1, __ATOMIC_RELEASE);
	return NULL;
}

static int wait_until_unpublished(struct uib_pd *pd, uint32_t lkey)
{
	uint32_t attempts;

	for (attempts = 0; attempts < 1000000; attempts++) {
		struct uib_mr *mr = uib_pd_lookup_lkey(pd, lkey);

		if (!mr)
			return 0;
		uib_mr_put(mr);
		sched_yield();
	}
	return -ETIMEDOUT;
}

static int run_case(struct uib_pd *pd, uint8_t *buffer, int address_lookup)
{
	struct deregistration state = { 0 };
	struct uib_mr *held;
	struct uib_mr *mr;
	pthread_t thread;
	uint32_t lkey;
	uint8_t value;
	int rc;

	mr = uib_reg_mr(pd, buffer, 64, 0);
	if (!mr)
		return -errno;
	lkey = mr->lkey;
	held = address_lookup ?
		uib_pd_lookup_address(pd, (uint64_t)(uintptr_t)buffer, 1, 0) :
		uib_pd_lookup_lkey(pd, lkey);
	if (held != mr) {
		uib_mr_put(held);
		uib_dereg_mr(mr);
		return -EIO;
	}

	state.mr = mr;
	rc = pthread_create(&thread, NULL, deregister_mr, &state);
	if (rc) {
		uib_mr_put(held);
		uib_dereg_mr(mr);
		return -rc;
	}
	rc = wait_until_unpublished(pd, lkey);
	if (!rc && __atomic_load_n(&state.finished, __ATOMIC_ACQUIRE))
		rc = -EFAULT;
	if (!rc && (held->lkey != lkey ||
		    uib_mr_read(held, (uint64_t)(uintptr_t)buffer,
				&value, sizeof(value)) < 0 || value != *buffer))
		rc = -EIO;

	uib_mr_put(held);
	pthread_join(thread, NULL);
	if (!rc && state.rc)
		rc = state.rc;
	return rc;
}

int main(void)
{
	struct uib_context ctx = { 0 };
	struct uib_pd *pd;
	uint8_t buffers[2][64];
	int rc;

	memset(buffers, 0x5a, sizeof(buffers));
	pd = uib_alloc_pd(&ctx);
	if (!pd)
		return 2;
	rc = run_case(pd, buffers[0], 0);
	if (!rc)
		rc = run_case(pd, buffers[1], 1);
	if (uib_dealloc_pd(pd) < 0 && !rc)
		rc = -EBUSY;
	printf("MR lookup lifetime: %s\n", rc ? "FAIL" : "PASS");
	return rc ? 1 : 0;
}
