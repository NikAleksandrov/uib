/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_PD_H
#define UIB_PD_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct uib_context;
struct uib_mr;
struct uib_mr_table;

struct uib_pd {
	struct uib_context  *ctx;
	struct uib_mr_table *mr_table;
	struct uib_mr       *mr_addr_root;
	pthread_mutex_t      mr_lock;
	uint32_t             handle;
	uint32_t             mr_count;
	int                  refcnt;
};

struct uib_pd *uib_alloc_pd(struct uib_context *ctx);
int uib_dealloc_pd(struct uib_pd *pd);

int uib_pd_attach_mr(struct uib_pd *pd, struct uib_mr *mr);
int uib_pd_detach_mr(struct uib_pd *pd, struct uib_mr *mr);

/* Successful lookups return a referenced MR; release it with uib_mr_put(). */
struct uib_mr *uib_pd_lookup_key(struct uib_pd *pd, uint32_t key);
struct uib_mr *uib_pd_lookup_lkey(struct uib_pd *pd, uint32_t lkey);
struct uib_mr *uib_pd_lookup_rkey(struct uib_pd *pd, uint32_t rkey);
struct uib_mr *uib_pd_lookup_address(struct uib_pd *pd, uint64_t va,
				      size_t len, int access);

#endif
