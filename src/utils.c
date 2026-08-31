/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "utils.h"

static unsigned int worker_cpu_next;

struct uib_thread_start {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	void *(*routine)(void *);
	void *argument;
	int cpu;
	int ready;
	int rc;
};

static void *uib_worker_thread(void *argument)
{
	struct uib_thread_start *start = argument;
	void *(*routine)(void *);
	void *routine_argument;
	cpu_set_t cpus;
	int cpu;
	int rc;

	routine = start->routine;
	routine_argument = start->argument;
	cpu = start->cpu;
	rc = 0;
	if (cpu >= 0) {
		CPU_ZERO(&cpus);
		CPU_SET(cpu, &cpus);
		rc = pthread_setaffinity_np(pthread_self(), sizeof(cpus),
					    &cpus);
	}
	pthread_mutex_lock(&start->lock);
	start->rc = rc ? -rc : 0;
	start->ready = 1;
	pthread_cond_signal(&start->cond);
	pthread_mutex_unlock(&start->lock);
	if (rc)
		return NULL;
	return routine(routine_argument);
}

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
	unsigned int allowed_count;
	unsigned int seen;
	int cpu;

	allowed_count = (unsigned int)CPU_COUNT(allowed);
	if (!allowed_count)
		return -1;
	index %= allowed_count;
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

unsigned int uib_worker_cpu_reserve(unsigned int count)
{
	return __atomic_fetch_add(&worker_cpu_next, count, __ATOMIC_RELAXED);
}

int uib_worker_thread_start(pthread_t *thread,
			    void *(*routine)(void *), void *argument,
			    size_t stack_size, const cpu_set_t *allowed,
			    unsigned int cpu_index, int affinity)
{
	struct uib_thread_start start;
	pthread_attr_t attr;
	int cpu;
	int rc;
	int start_rc;

	if (!thread || !routine || (affinity && !allowed))
		return -EINVAL;
	cpu = -1;
	if (affinity) {
		cpu = uib_worker_cpu(allowed, cpu_index);
		if (cpu < 0)
			return -EINVAL;
	}
	memset(&start, 0, sizeof(start));
	start.routine = routine;
	start.argument = argument;
	start.cpu = cpu;
	rc = pthread_mutex_init(&start.lock, NULL);
	if (rc)
		return -rc;
	rc = pthread_cond_init(&start.cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&start.lock);
		return -rc;
	}
	rc = pthread_attr_init(&attr);
	if (rc)
		goto destroy_cond;
	if (stack_size) {
		rc = pthread_attr_setstacksize(&attr, stack_size);
		if (rc)
			goto destroy_attr;
	}
	pthread_mutex_lock(&start.lock);
	rc = pthread_create(thread, &attr, uib_worker_thread, &start);
	pthread_attr_destroy(&attr);
	if (rc) {
		pthread_mutex_unlock(&start.lock);
		goto destroy_cond;
	}
	while (!start.ready)
		pthread_cond_wait(&start.cond, &start.lock);
	start_rc = start.rc;
	pthread_mutex_unlock(&start.lock);
	if (start_rc)
		pthread_join(*thread, NULL);
	pthread_cond_destroy(&start.cond);
	pthread_mutex_destroy(&start.lock);
	return start_rc;

destroy_attr:
	pthread_attr_destroy(&attr);
destroy_cond:
	pthread_cond_destroy(&start.cond);
	pthread_mutex_destroy(&start.lock);
	return -rc;
}
