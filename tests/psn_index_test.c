// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "uib/psn_index.h"

#define TEST_ENTRIES	1024U
#define TEST_COLLISION_STEP	64U
#define TEST_KEY_BASE	7U

static int failed;

static void check(const char *name, int condition)
{
	printf("%-32s %s\n", name, condition ? "OK" : "FAIL");
	if (!condition)
		failed = 1;
}

int main(void)
{
	struct uib_psn_index index = { 0 };
	uint32_t values[TEST_ENTRIES];
	uint32_t key;
	uint32_t i;
	int rc;

	rc = uib_psn_index_init(&index, TEST_COLLISION_STEP);
	check("psn.init", !rc && index.size == TEST_COLLISION_STEP);
	for (i = 0; i < TEST_ENTRIES; i++) {
		values[i] = i + 1U;
		key = TEST_KEY_BASE + i * TEST_COLLISION_STEP;
		rc = uib_psn_index_insert(&index, key, &values[i]);
		if (rc)
			break;
	}
	check("psn.collision.insert", i == TEST_ENTRIES);
	check("psn.growth", index.size > TEST_COLLISION_STEP &&
	      index.count == TEST_ENTRIES);
	for (i = 0; i < TEST_ENTRIES; i++) {
		key = TEST_KEY_BASE + i * TEST_COLLISION_STEP;
		if (uib_psn_index_lookup(&index, key) != &values[i])
			break;
	}
	check("psn.collision.lookup", i == TEST_ENTRIES);
	for (i = 0; i < TEST_ENTRIES; i += 3U) {
		key = TEST_KEY_BASE + i * TEST_COLLISION_STEP;
		if (uib_psn_index_remove(&index, key, &values[i]))
			break;
	}
	check("psn.cluster.delete", i >= TEST_ENTRIES);
	for (i = 0; i < TEST_ENTRIES; i++) {
		key = TEST_KEY_BASE + i * TEST_COLLISION_STEP;
		if (i % 3U) {
			if (uib_psn_index_lookup(&index, key) != &values[i])
				break;
		} else if (uib_psn_index_lookup(&index, key)) {
			break;
		}
	}
	check("psn.delete.preserves-cluster", i == TEST_ENTRIES);
	rc = uib_psn_index_insert(&index, UINT32_MAX, &values[0]);
	check("psn.maximum.insert", !rc);
	check("psn.maximum.lookup",
	      uib_psn_index_lookup(&index, UINT32_MAX) == &values[0]);
	check("psn.duplicate", uib_psn_index_insert(&index, UINT32_MAX,
						    &values[1]) == -EEXIST);
	uib_psn_index_destroy(&index);
	check("psn.destroy", !index.slots && !index.size && !index.count);
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
