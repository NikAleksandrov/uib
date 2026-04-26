#ifndef UIB_PD_H
#define UIB_PD_H

#include <stdint.h>
#include <pthread.h>

struct uib_context;
struct uib_mr;

struct uib_pd {
	struct uib_context *ctx;
	uint32_t            handle;
	pthread_mutex_t     lock;
	struct uib_mr      *mr_list;
	int                 mr_count;
	int                 refcnt;
};

struct uib_pd *uib_alloc_pd(struct uib_context *ctx);
int uib_dealloc_pd(struct uib_pd *pd);

void uib_pd_attach_mr(struct uib_pd *pd, struct uib_mr *mr);
void uib_pd_detach_mr(struct uib_pd *pd, struct uib_mr *mr);

struct uib_mr *uib_pd_lookup_key(struct uib_pd *pd, uint32_t key);

#endif
