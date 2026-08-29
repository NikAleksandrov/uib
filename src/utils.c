/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
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

int uib_env_bool(const char *name, int default_value, int *value)
{
	const char *setting;

	if (!value)
		return -EINVAL;
	setting = getenv(name);
	if (!setting || !*setting) {
		*value = !!default_value;
		return 0;
	}
	if (!strcmp(setting, "on") || !strcmp(setting, "1")) {
		*value = 1;
		return 0;
	}
	if (!strcmp(setting, "off") || !strcmp(setting, "0")) {
		*value = 0;
		return 0;
	}
	return -EINVAL;
}

int uib_worker_configuration(unsigned int *count, cpu_set_t *allowed,
			     int *affinity, const char *workers_env,
			     const char *affinity_env,
			     unsigned int automatic_limit)
{
	const char *setting;
	unsigned long value;
	unsigned int allowed_count;
	char *end;
	int rc;

	rc = uib_env_bool(affinity_env, 1, affinity);
	if (rc)
		return rc;
	CPU_ZERO(allowed);
	if (sched_getaffinity(0, sizeof(*allowed), allowed) < 0)
		return -errno;
	allowed_count = (unsigned int)CPU_COUNT(allowed);
	if (!allowed_count)
		return -EINVAL;
	setting = getenv(workers_env);
	if (!setting || !*setting || !strcmp(setting, "auto")) {
		*count = automatic_limit ?
			min_t(unsigned int, allowed_count, automatic_limit) :
			allowed_count;
		return 0;
	}
	errno = 0;
	value = strtoul(setting, &end, 10);
	if (errno || end == setting || *end || !value || value > CPU_SETSIZE)
		return -EINVAL;
	if (*affinity && value > allowed_count)
		return -EINVAL;
	*count = (unsigned int)value;
	return 0;
}

int uib_worker_cpu(const cpu_set_t *allowed, unsigned int index)
{
	unsigned int seen;
	int cpu;

	seen = 0;
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, allowed))
			continue;
		if (seen == index)
			return cpu;
		seen++;
	}
	return -1;
}
