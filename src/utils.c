/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
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

size_t uib_align_up(size_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

int uib_parse_u64(const char *text, unsigned int base, uint64_t *value)
{
	unsigned long long parsed;
	char *end;

	if (!text || !*text || !value)
		return -EINVAL;
	errno = 0;
	parsed = strtoull(text, &end, base);
	if (errno || end == text || *end)
		return -EINVAL;
	*value = parsed;
	return 0;
}

int uib_env_u64(const char *name, uint64_t *value)
{
	const char *text;

	text = getenv(name);
	if (!text)
		return 0;
	return uib_parse_u64(text, 0, value);
}
