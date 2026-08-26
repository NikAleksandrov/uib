/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_TEST_CONGESTION_PLUGIN_H
#define UIB_TEST_CONGESTION_PLUGIN_H

#include <stdint.h>

struct uib_test_congestion_stats {
	uint64_t initializations;
	uint64_t admissions;
	uint64_t sends;
	uint64_t acknowledgements;
	uint64_t losses;
	uint64_t nacks;
};

void uib_test_congestion_get_stats(struct uib_test_congestion_stats *stats);

#endif
