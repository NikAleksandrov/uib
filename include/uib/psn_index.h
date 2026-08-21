/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_PSN_INDEX_H
#define UIB_PSN_INDEX_H

#include <stdint.h>

struct uib_psn_slot {
	void *value;
	uint32_t psn;
};

struct uib_psn_index {
	struct uib_psn_slot *slots;
	uint32_t size;
	uint32_t count;
};

int uib_psn_index_init(struct uib_psn_index *index, uint32_t initial_size);
void uib_psn_index_destroy(struct uib_psn_index *index);
int uib_psn_index_insert(struct uib_psn_index *index, uint32_t psn,
			 void *value);
void *uib_psn_index_lookup(const struct uib_psn_index *index, uint32_t psn);
int uib_psn_index_remove(struct uib_psn_index *index, uint32_t psn,
			 void *value);

#endif
