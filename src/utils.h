/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_UTILS_H
#define UIB_UTILS_H

#include <sched.h>
#include <stddef.h>
#include <stdint.h>

/* min_t/max_t names and single-evaluation semantics follow Linux minmax.h. */
#define __uib_cmp(op, type, x, y) ({ \
	type __uib_x = (x); \
	type __uib_y = (y); \
	__uib_x op __uib_y ? __uib_x : __uib_y; \
})

#ifndef min_t
#define min_t(type, x, y) __uib_cmp(<, type, x, y)
#endif

#ifndef max_t
#define max_t(type, x, y) __uib_cmp(>, type, x, y)
#endif

uint64_t uib_now_ns(void);
uint64_t uib_mul_div_u64(uint64_t a, uint64_t b, uint64_t divisor);
size_t uib_align_up(size_t value, size_t alignment);
int uib_parse_u64(const char *text, unsigned int base, uint64_t *value);
int uib_env_u64(const char *name, uint64_t *value);
int uib_env_bool(const char *name, int default_value, int *value);
int uib_worker_configuration(unsigned int *count, cpu_set_t *allowed,
			     int *affinity, const char *workers_env,
			     const char *affinity_env,
			     unsigned int automatic_limit);
int uib_worker_cpu(const cpu_set_t *allowed, unsigned int index);
unsigned int uib_worker_cpu_reserve(unsigned int count);

#endif
