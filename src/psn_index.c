/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "uib/psn_index.h"

#define UIB_PSN_INDEX_INITIAL_SIZE	64U
#define UIB_PSN_INDEX_LOAD_NUMERATOR	3U
#define UIB_PSN_INDEX_LOAD_DENOMINATOR	4U
#define UIB_PSN_HASH_MULTIPLIER		0x9e370001U

static uint32_t psn_hash(uint32_t size, uint32_t psn)
{
	return (psn * UIB_PSN_HASH_MULTIPLIER) & (size - 1U);
}

static int index_insert_raw(struct uib_psn_slot *slots, uint32_t size,
			    uint32_t psn, void *value)
{
	uint32_t slot;
	uint32_t probes;

	slot = psn_hash(size, psn);
	for (probes = 0; probes < size; probes++) {
		if (!slots[slot].value) {
			slots[slot].value = value;
			slots[slot].psn = psn;
			return 0;
		}
		if (slots[slot].psn == psn)
			return -EEXIST;
		slot = (slot + 1U) & (size - 1U);
	}
	return -ENOSPC;
}

static int index_reserve(struct uib_psn_index *index, uint32_t additional)
{
	struct uib_psn_slot *new_slots;
	uint32_t new_size;
	uint32_t i;

	new_size = index->size ? index->size : UIB_PSN_INDEX_INITIAL_SIZE;
	while ((uint64_t)(index->count + additional) *
	       UIB_PSN_INDEX_LOAD_DENOMINATOR >
	       (uint64_t)new_size * UIB_PSN_INDEX_LOAD_NUMERATOR) {
		if (new_size > UINT32_MAX / 2U)
			return -EOVERFLOW;
		new_size <<= 1;
	}
	if (index->slots && new_size == index->size)
		return 0;
	new_slots = calloc(new_size, sizeof(*new_slots));
	if (!new_slots)
		return -ENOMEM;
	for (i = 0; i < index->size; i++) {
		if (index->slots[i].value)
			index_insert_raw(new_slots, new_size, index->slots[i].psn,
					 index->slots[i].value);
	}
	free(index->slots);
	index->slots = new_slots;
	index->size = new_size;
	return 0;
}

int uib_psn_index_init(struct uib_psn_index *index, uint32_t initial_size)
{
	uint32_t size = UIB_PSN_INDEX_INITIAL_SIZE;

	if (!index)
		return -EINVAL;
	while (size < initial_size) {
		if (size > UINT32_MAX / 2U)
			return -EOVERFLOW;
		size <<= 1;
	}
	index->slots = calloc(size, sizeof(*index->slots));
	if (!index->slots)
		return -ENOMEM;
	index->size = size;
	index->count = 0;
	return 0;
}

void uib_psn_index_destroy(struct uib_psn_index *index)
{
	if (!index)
		return;
	free(index->slots);
	index->slots = NULL;
	index->size = 0;
	index->count = 0;
}

int uib_psn_index_insert(struct uib_psn_index *index, uint32_t psn,
			 void *value)
{
	int rc;

	if (!index || !value)
		return -EINVAL;
	rc = index_reserve(index, 1);
	if (rc < 0)
		return rc;
	rc = index_insert_raw(index->slots, index->size, psn, value);
	if (!rc)
		index->count++;
	return rc;
}

void *uib_psn_index_lookup(const struct uib_psn_index *index, uint32_t psn)
{
	uint32_t slot;
	uint32_t probes;

	if (!index || !index->slots)
		return NULL;
	slot = psn_hash(index->size, psn);
	for (probes = 0; probes < index->size; probes++) {
		if (!index->slots[slot].value)
			return NULL;
		if (index->slots[slot].psn == psn)
			return index->slots[slot].value;
		slot = (slot + 1U) & (index->size - 1U);
	}
	return NULL;
}

int uib_psn_index_remove(struct uib_psn_index *index, uint32_t psn,
			 void *value)
{
	struct uib_psn_slot move;
	uint32_t slot;
	uint32_t probes;

	if (!index || !index->slots || !value)
		return -EINVAL;
	slot = psn_hash(index->size, psn);
	for (probes = 0; probes < index->size; probes++) {
		if (!index->slots[slot].value)
			return -ENOENT;
		if (index->slots[slot].psn == psn &&
		    index->slots[slot].value == value)
			break;
		slot = (slot + 1U) & (index->size - 1U);
	}
	if (probes == index->size)
		return -ENOENT;
	index->slots[slot].value = NULL;
	index->count--;
	slot = (slot + 1U) & (index->size - 1U);
	while (index->slots[slot].value) {
		move = index->slots[slot];
		index->slots[slot].value = NULL;
		index_insert_raw(index->slots, index->size, move.psn, move.value);
		slot = (slot + 1U) & (index->size - 1U);
	}
	return 0;
}
