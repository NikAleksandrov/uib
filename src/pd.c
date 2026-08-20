#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "uib/context.h"
#include "uib/mr.h"
#include "uib/pd.h"

#define UIB_MR_TABLE_INITIAL_SIZE	64U
#define UIB_MR_TABLE_LOAD_NUMERATOR	3U
#define UIB_MR_TABLE_LOAD_DENOMINATOR	4U
/* Chris Wellons' lowbias32 mixer, released under the Unlicense. */
#define UIB_MR_HASH_MIX_FIRST		0x7feb352dU
#define UIB_MR_HASH_MIX_SECOND		0x846ca68bU

static uint32_t pd_handle_next = 1;

static uint64_t mr_base(const struct uib_mr *mr)
{
	if (mr->kind == UIB_MR_DMABUF)
		return mr->iova;
	return (uint64_t)(uintptr_t)mr->addr;
}

static uint64_t mr_limit(const struct uib_mr *mr)
{
	return mr_base(mr) + mr->length;
}

static uint32_t hash_key(uint32_t key)
{
	key ^= key >> 16;
	key *= UIB_MR_HASH_MIX_FIRST;
	key ^= key >> 15;
	key *= UIB_MR_HASH_MIX_SECOND;
	return key ^ (key >> 16);
}

static void hash_insert(struct uib_mr **table, uint32_t size,
			struct uib_mr *mr, uint32_t key)
{
	uint32_t slot = hash_key(key) & (size - 1U);

	while (table[slot])
		slot = (slot + 1U) & (size - 1U);
	table[slot] = mr;
}

static struct uib_mr *hash_lookup(struct uib_mr **table, uint32_t size,
				  uint32_t key, int remote)
{
	struct uib_mr *mr;
	uint32_t slot;

	if (!table || !size)
		return NULL;
	slot = hash_key(key) & (size - 1U);
	while ((mr = table[slot]) != NULL) {
		if ((remote ? mr->rkey : mr->lkey) == key)
			return mr;
		slot = (slot + 1U) & (size - 1U);
	}
	return NULL;
}

static void hash_remove(struct uib_mr **table, uint32_t size,
			struct uib_mr *mr, uint32_t key, int remote)
{
	struct uib_mr *move;
	uint32_t slot;

	slot = hash_key(key) & (size - 1U);
	while (table[slot] && table[slot] != mr)
		slot = (slot + 1U) & (size - 1U);
	if (!table[slot])
		return;
	table[slot] = NULL;
	slot = (slot + 1U) & (size - 1U);
	while ((move = table[slot]) != NULL) {
		table[slot] = NULL;
		hash_insert(table, size, move, remote ? move->rkey : move->lkey);
		slot = (slot + 1U) & (size - 1U);
	}
}

static void hash_tree(struct uib_mr *mr, struct uib_mr **lkey_table,
		      struct uib_mr **rkey_table, uint32_t size)
{
	if (!mr)
		return;
	hash_tree(mr->addr_left, lkey_table, rkey_table, size);
	hash_insert(lkey_table, size, mr, mr->lkey);
	hash_insert(rkey_table, size, mr, mr->rkey);
	hash_tree(mr->addr_right, lkey_table, rkey_table, size);
}

static int hash_grow(struct uib_pd *pd)
{
	struct uib_mr **rkey_table;
	struct uib_mr **lkey_table;
	uint32_t size;

	if ((pd->mr_count + 1U) * UIB_MR_TABLE_LOAD_DENOMINATOR <=
	    pd->mr_table_size * UIB_MR_TABLE_LOAD_NUMERATOR)
		return 0;
	size = pd->mr_table_size << 1;
	lkey_table = calloc(size, sizeof(*lkey_table));
	rkey_table = calloc(size, sizeof(*rkey_table));
	if (!lkey_table || !rkey_table) {
		free(lkey_table);
		free(rkey_table);
		return -ENOMEM;
	}
	hash_tree(pd->mr_addr_root, lkey_table, rkey_table, size);
	free(pd->lkey_table);
	free(pd->rkey_table);
	pd->lkey_table = lkey_table;
	pd->rkey_table = rkey_table;
	pd->mr_table_size = size;
	return 0;
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
	uint64_t left_base = mr_base(left);
	uint64_t right_base = mr_base(right);

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

static int region_contains(const struct uib_mr *mr, uint64_t va, size_t len)
{
	uint64_t base = mr_base(mr);

	if (va < base || va - base > mr->length)
		return 0;
	return len <= mr->length - (size_t)(va - base);
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
	if (region_contains(mr, va, len) &&
	    (mr->access & access) == access)
		return mr;
	if (mr_base(mr) > va)
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
	pd->lkey_table = calloc(UIB_MR_TABLE_INITIAL_SIZE,
				sizeof(*pd->lkey_table));
	pd->rkey_table = calloc(UIB_MR_TABLE_INITIAL_SIZE,
				sizeof(*pd->rkey_table));
	if (!pd->lkey_table || !pd->rkey_table) {
		free(pd->lkey_table);
		free(pd->rkey_table);
		free(pd);
		return NULL;
	}
	pd->ctx = ctx;
	pd->handle = __atomic_fetch_add(&pd_handle_next, 1, __ATOMIC_RELAXED);
	pd->mr_table_size = UIB_MR_TABLE_INITIAL_SIZE;
	pd->refcnt = 1;
	if (pthread_rwlock_init(&pd->mr_lock, NULL)) {
		free(pd->lkey_table);
		free(pd->rkey_table);
		free(pd);
		return NULL;
	}
	return pd;
}

int uib_dealloc_pd(struct uib_pd *pd)
{
	if (!pd)
		return -EINVAL;
	pthread_rwlock_wrlock(&pd->mr_lock);
	if (pd->mr_count) {
		pthread_rwlock_unlock(&pd->mr_lock);
		return -EBUSY;
	}
	pthread_rwlock_unlock(&pd->mr_lock);
	pthread_rwlock_destroy(&pd->mr_lock);
	free(pd->lkey_table);
	free(pd->rkey_table);
	free(pd);
	return 0;
}

int uib_pd_attach_mr(struct uib_pd *pd, struct uib_mr *mr)
{
	int rc;

	pthread_rwlock_wrlock(&pd->mr_lock);
	rc = hash_grow(pd);
	if (rc < 0)
		goto out;
	hash_insert(pd->lkey_table, pd->mr_table_size, mr, mr->lkey);
	hash_insert(pd->rkey_table, pd->mr_table_size, mr, mr->rkey);
	addr_insert(pd, mr);
	pd->mr_count++;
out:
	pthread_rwlock_unlock(&pd->mr_lock);
	return rc;
}

void uib_pd_detach_mr(struct uib_pd *pd, struct uib_mr *mr)
{
	pthread_rwlock_wrlock(&pd->mr_lock);
	hash_remove(pd->lkey_table, pd->mr_table_size, mr, mr->lkey, 0);
	hash_remove(pd->rkey_table, pd->mr_table_size, mr, mr->rkey, 1);
	addr_remove(pd, mr);
	pd->mr_count--;
	pthread_rwlock_unlock(&pd->mr_lock);
}

struct uib_mr *uib_pd_lookup_lkey(struct uib_pd *pd, uint32_t lkey)
{
	struct uib_mr *mr;

	if (!pd)
		return NULL;
	pthread_rwlock_rdlock(&pd->mr_lock);
	mr = hash_lookup(pd->lkey_table, pd->mr_table_size, lkey, 0);
	pthread_rwlock_unlock(&pd->mr_lock);
	return mr;
}

struct uib_mr *uib_pd_lookup_rkey(struct uib_pd *pd, uint32_t rkey)
{
	struct uib_mr *mr;

	if (!pd)
		return NULL;
	pthread_rwlock_rdlock(&pd->mr_lock);
	mr = hash_lookup(pd->rkey_table, pd->mr_table_size, rkey, 1);
	pthread_rwlock_unlock(&pd->mr_lock);
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
	pthread_rwlock_rdlock(&pd->mr_lock);
	mr = addr_lookup(pd->mr_addr_root, va, len, access);
	pthread_rwlock_unlock(&pd->mr_lock);
	return mr;
}
