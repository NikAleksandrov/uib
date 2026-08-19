/* SPDX-License-Identifier: Apache-2.0 */
#include <limits.h>
#include <time.h>

#include "utils.h"

uint64_t uib_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL +
		(uint64_t)ts.tv_nsec;
}

uint64_t uib_mul_div_u64(uint64_t a, uint64_t b, uint64_t divisor)
{
	__uint128_t product;

	if (!divisor)
		return UINT64_MAX;
	product = (__uint128_t)a * b;
	product /= divisor;
	return product > UINT64_MAX ? UINT64_MAX : (uint64_t)product;
}
