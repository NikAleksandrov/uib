/* SPDX-License-Identifier: Apache-2.0 */
#include <time.h>

#include "utils.h"

uint64_t uib_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL +
		(uint64_t)ts.tv_nsec;
}
