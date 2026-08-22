/* SPDX-License-Identifier: Apache-2.0 AND Unlicense */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <urcu-bp.h>

#include "uib/context.h"
#include "uib/mr.h"
#include "uib/pd.h"
#include "mr_internal.h"

#define UIB_MR_TABLE_INITIAL_SIZE	64U
#define UIB_MR_TABLE_LOAD_NUMERATOR	3U
#define UIB_MR_TABLE_LOAD_DENOMINATOR	4U
/* Chris Wellons' lowbias32 mixer, released under the Unlicense. */
#define UIB_MR_HASH_MIX_FIRST		0x7feb352dU
#define UIB_MR_HASH_MIX_SECOND		0x846ca68bU

static uint32_t pd_handle_next = 1;

static void mr_get(struct uib_mr *mr)
{
	__atomic_add_fetch(&mr->refs, 1, __ATOMIC_RELAXED);
}

struct uib_mr_table {
	struct uib_mr **lkey;
	struct uib_mr **rkey;
	uint32_t        size;
};

static uint64_t mr_limit(const struct uib_mr *mr)
{
	return uib_mr_base(mr) + mr->length;
}

static uint32_t hash_key(uint32_t key)
{
	key ^= key >> 16;
	key *= UIB_MR_HASH_MIX_FIRST;
	key ^= key >> 15;
	key *= UIB_MR_HASH_MIX_SECOND;
	return key ^ (key >> 16);
}

static struct uib_mr_table *mr_table_alloc(uint32_t size)
{
	struct uib_mr_table *table;

	table = malloc(sizeof(*table));
	if (!table)
		return NULL;
	table->lkey = calloc(size, sizeof(*table->lkey));
	table->rkey = calloc(size, sizeof(*table->rkey));
	if (!table->lkey || !table->rkey) {
		free(table->rkey);
		free(table->lkey);
		free(table);
		return NULL;
	}
	table->size = size;
	return table;
}

static void mr_table_free(struct uib_mr_table *table)
{
	if (!table)
		return;
	free(table->rkey);
	free(table->lkey);
	free(table);
}

static void hash_insert(struct uib_mr **table, uint32_t size,
			struct uib_mr *mr, uint32_t key)
{
	uint32_t slot;

	slot = hash_key(key) & (size - 1U);
	while (table[slot])
		slot = (slot + 1U) & (size - 1U);
	table[slot] = mr;
}

static struct uib_mr *hash_lookup(struct uib_mr **table, uint32_t size,
				  uint32_t key, int remote)
{
	struct uib_mr *mr;
	uint32_t slot;

	slot = hash_key(key) & (size - 1U);
	while ((mr = table[slot]) != NULL) {
		if ((remote ? mr->rkey : mr->lkey) == key)
			return mr;
		slot = (slot + 1U) & (size - 1U);
	}
	return NULL;
}

static void addr_update(struct uib_mr *mr)
{
	uint64_t max;

	max = mr_limit(mr);
	if (mr->addr_left && mr->addr_left->addr_max > max)
		max = mr->addr_left->addr_max;
	if (mr->addr_right && mr->addr_right->addr_max > max)
		max = mr->addr_right->addr_max;
	mr->addr_max = max;
}

static void addr_update_ancestors(struct uib_mr *mr)
{
	while (mr) {
		addr_update(mr);
		mr = mr->addr_parent;
	}
}

static void addr_replace(struct uib_pd *pd, struct uib_mr *old,
			 struct uib_mr *new)
{
	if (!old->addr_parent)
		pd->mr_addr_root = new;
	else if (old == old->addr_parent->addr_left)
		old->addr_parent->addr_left = new;
	else
		old->addr_parent->addr_right = new;
	if (new)
		new->addr_parent = old->addr_parent;
}

static void addr_rotate_left(struct uib_pd *pd, struct uib_mr *mr)
{
	struct uib_mr *right = mr->addr_right;

	mr->addr_right = right->addr_left;
	if (right->addr_left)
		right->addr_left->addr_parent = mr;
	addr_replace(pd, mr, right);
	right->addr_left = mr;
	mr->addr_parent = right;
	addr_update(mr);
	addr_update(right);
}

static void addr_rotate_right(struct uib_pd *pd, struct uib_mr *mr)
{
	struct uib_mr *left = mr->addr_left;

	mr->addr_left = left->addr_right;
	if (left->addr_right)
		left->addr_right->addr_parent = mr;
	addr_replace(pd, mr, left);
	left->addr_right = mr;
	mr->addr_parent = left;
	addr_update(mr);
	addr_update(left);
}

static int addr_less(const struct uib_mr *left, const struct uib_mr *right)
{
	uint64_t left_base = uib_mr_base(left);
	uint64_t right_base = uib_mr_base(right);

	if (left_base != right_base)
		return left_base < right_base;
	return left->lkey < right->lkey;
}

static void addr_insert(struct uib_pd *pd, struct uib_mr *mr)
{
	struct uib_mr **link = &pd->mr_addr_root;
	struct uib_mr *parent = NULL;

	mr->addr_priority = hash_key(mr->lkey);
	mr->addr_max = mr_limit(mr);
	while (*link) {
		parent = *link;
		if (addr_less(mr, parent))
			link = &parent->addr_left;
		else
			link = &parent->addr_right;
	}
	*link = mr;
	mr->addr_parent = parent;
	addr_update_ancestors(parent);
	while (mr->addr_parent &&
	       mr->addr_priority < mr->addr_parent->addr_priority) {
		if (mr == mr->addr_parent->addr_left)
			addr_rotate_right(pd, mr->addr_parent);
		else
			addr_rotate_left(pd, mr->addr_parent);
	}
	addr_update_ancestors(mr);
}

static void addr_remove(struct uib_pd *pd, struct uib_mr *mr)
{
	struct uib_mr *parent;

	while (mr->addr_left || mr->addr_right) {
		if (!mr->addr_left)
			addr_rotate_left(pd, mr);
		else if (!mr->addr_right)
			addr_rotate_right(pd, mr);
		else if (mr->addr_left->addr_priority <
			 mr->addr_right->addr_priority)
			addr_rotate_right(pd, mr);
		else
			addr_rotate_left(pd, mr);
	}
	parent = mr->addr_parent;
	addr_replace(pd, mr, NULL);
	mr->addr_parent = NULL;
	addr_update_ancestors(parent);
}

static void hash_tree(struct uib_mr *mr, struct uib_mr_table *table)
{
	if (!mr)
		return;
	hash_tree(mr->addr_left, table);
	hash_insert(table->lkey, table->size, mr, mr->lkey);
	hash_insert(table->rkey, table->size, mr, mr->rkey);
	hash_tree(mr->addr_right, table);
}

static struct uib_mr *key_lookup(struct uib_pd *pd, uint32_t key, int remote)
{
	struct uib_mr_table *table;
	struct uib_mr *mr;

	rcu_read_lock();
	table = rcu_dereference(pd->mr_table);
	if (remote)
		mr = hash_lookup(table->rkey, table->size, key, 1);
	else
		mr = hash_lookup(table->lkey, table->size, key, 0);
	if (mr)
		mr_get(mr);
	rcu_read_unlock();
	return mr;
}

static struct uib_mr *addr_lookup(struct uib_mr *mr, uint64_t va,
				  size_t len, int access)
{
	struct uib_mr *found;

	if (!mr || mr->addr_max < va)
		return NULL;
	found = addr_lookup(mr->addr_left, va, len, access);
	if (found)
		return found;
	if (uib_mr_contains(mr, va, len) &&
	    (mr->access & access) == access)
		return mr;
	if (uib_mr_base(mr) > va)
		return NULL;
	return addr_lookup(mr->addr_right, va, len, access);
}

struct uib_pd *uib_alloc_pd(struct uib_context *ctx)
{
	struct uib_pd *pd;

	if (!ctx)
		return NULL;
	pd = calloc(1, sizeof(*pd));
	if (!pd)
		return NULL;
	pd->mr_table = mr_table_alloc(UIB_MR_TABLE_INITIAL_SIZE);
	if (!pd->mr_table) {
		free(pd);
		return NULL;
	}
	pd->ctx = ctx;
	pd->handle = __atomic_fetch_add(&pd_handle_next, 1, __ATOMIC_RELAXED);
	pd->refcnt = 1;
	if (pthread_mutex_init(&pd->mr_lock, NULL)) {
		mr_table_free(pd->mr_table);
		free(pd);
		return NULL;
	}
	return pd;
}

int uib_dealloc_pd(struct uib_pd *pd)
{
	if (!pd)
		return -EINVAL;
	pthread_mutex_lock(&pd->mr_lock);
	if (pd->mr_count) {
		pthread_mutex_unlock(&pd->mr_lock);
		return -EBUSY;
	}
	pthread_mutex_unlock(&pd->mr_lock);
	pthread_mutex_destroy(&pd->mr_lock);
	mr_table_free(pd->mr_table);
	free(pd);
	return 0;
}

int uib_pd_attach_mr(struct uib_pd *pd, struct uib_mr *mr)
{
	struct uib_mr_table *old_table;
	struct uib_mr_table *new_table;
	uint32_t size;

	pthread_mutex_lock(&pd->mr_lock);
	old_table = pd->mr_table;
	size = old_table->size;
	if ((pd->mr_count + 1U) * UIB_MR_TABLE_LOAD_DENOMINATOR >
	    size * UIB_MR_TABLE_LOAD_NUMERATOR)
		size <<= 1;
	new_table = mr_table_alloc(size);
	if (!new_table) {
		pthread_mutex_unlock(&pd->mr_lock);
		return -ENOMEM;
	}
	hash_tree(pd->mr_addr_root, new_table);
	hash_insert(new_table->lkey, size, mr, mr->lkey);
	hash_insert(new_table->rkey, size, mr, mr->rkey);
	addr_insert(pd, mr);
	pd->mr_count++;
	rcu_assign_pointer(pd->mr_table, new_table);
	pthread_mutex_unlock(&pd->mr_lock);
	synchronize_rcu();
	mr_table_free(old_table);
	return 0;
}

int uib_pd_detach_mr(struct uib_pd *pd, struct uib_mr *mr)
{
	struct uib_mr_table *old_table;
	struct uib_mr_table *new_table;

	pthread_mutex_lock(&pd->mr_lock);
	old_table = pd->mr_table;
	new_table = mr_table_alloc(old_table->size);
	if (!new_table) {
		pthread_mutex_unlock(&pd->mr_lock);
		return -ENOMEM;
	}
	addr_remove(pd, mr);
	pd->mr_count--;
	hash_tree(pd->mr_addr_root, new_table);
	rcu_assign_pointer(pd->mr_table, new_table);
	pthread_mutex_unlock(&pd->mr_lock);
	synchronize_rcu();
	mr_table_free(old_table);
	return 0;
}

struct uib_mr *uib_pd_lookup_lkey(struct uib_pd *pd, uint32_t lkey)
{
	struct uib_mr *mr;

	if (!pd)
		return NULL;
	mr = key_lookup(pd, lkey, 0);
	return mr;
}

struct uib_mr *uib_pd_lookup_rkey(struct uib_pd *pd, uint32_t rkey)
{
	struct uib_mr *mr;

	if (!pd)
		return NULL;
	mr = key_lookup(pd, rkey, 1);
	return mr;
}

struct uib_mr *uib_pd_lookup_key(struct uib_pd *pd, uint32_t key)
{
	struct uib_mr *mr;

	mr = uib_pd_lookup_lkey(pd, key);
	if (!mr)
		mr = uib_pd_lookup_rkey(pd, key);
	return mr;
}

struct uib_mr *uib_pd_lookup_address(struct uib_pd *pd, uint64_t va,
				      size_t len, int access)
{
	struct uib_mr *mr;

	if (!pd)
		return NULL;
	pthread_mutex_lock(&pd->mr_lock);
	mr = addr_lookup(pd->mr_addr_root, va, len, access);
	if (mr)
		mr_get(mr);
	pthread_mutex_unlock(&pd->mr_lock);
	return mr;
}
