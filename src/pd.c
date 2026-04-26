#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "uib/pd.h"
#include "uib/mr.h"
#include "uib/context.h"

static uint32_t pd_handle_next = 1;

struct uib_pd *uib_alloc_pd(struct uib_context *ctx)
{
	if (!ctx)
		return NULL;
	struct uib_pd *pd = calloc(1, sizeof(*pd));

	if (!pd)
		return NULL;
	pd->ctx     = ctx;
	pd->handle  = __atomic_fetch_add(&pd_handle_next, 1, __ATOMIC_RELAXED);
	pd->mr_list = NULL;
	pd->mr_count = 0;
	pd->refcnt  = 1;
	pthread_mutex_init(&pd->lock, NULL);
	return pd;
}

int uib_dealloc_pd(struct uib_pd *pd)
{
	if (!pd)
		return -EINVAL;
	pthread_mutex_lock(&pd->lock);
	if (pd->mr_count > 0) {
		pthread_mutex_unlock(&pd->lock);
		return -EBUSY;
	}
	pthread_mutex_unlock(&pd->lock);
	pthread_mutex_destroy(&pd->lock);
	free(pd);
	return 0;
}

void uib_pd_attach_mr(struct uib_pd *pd, struct uib_mr *mr)
{
	pthread_mutex_lock(&pd->lock);
	mr->prev = NULL;
	mr->next = pd->mr_list;
	if (pd->mr_list)
		pd->mr_list->prev = mr;
	pd->mr_list = mr;
	pd->mr_count++;
	pthread_mutex_unlock(&pd->lock);
}

void uib_pd_detach_mr(struct uib_pd *pd, struct uib_mr *mr)
{
	pthread_mutex_lock(&pd->lock);
	if (mr->prev)
		mr->prev->next = mr->next;
	else
		pd->mr_list = mr->next;
	if (mr->next)
		mr->next->prev = mr->prev;
	mr->prev = mr->next = NULL;
	pd->mr_count--;
	pthread_mutex_unlock(&pd->lock);
}

struct uib_mr *uib_pd_lookup_key(struct uib_pd *pd, uint32_t key)
{
	pthread_mutex_lock(&pd->lock);
	struct uib_mr *m;

	for (m = pd->mr_list; m; m = m->next) {
		if (m->lkey == key || m->rkey == key)
			break;
	}
	pthread_mutex_unlock(&pd->lock);
	return m;
}
