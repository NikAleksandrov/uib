#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>

#include "uib/mr.h"
#include "uib/pd.h"
#include "uib/context.h"
#include "uib/types.h"
#include "mr_internal.h"

static uint32_t key_next = 0x10;
static uint64_t iova_next = 0x100000000000ULL;

struct uib_mr *uib_reg_mr(struct uib_pd *pd, void *addr, size_t length, int access)
{
	if (!pd || !addr || !length) {
		errno = EINVAL;
		return NULL;
	}
	struct uib_mr *mr = calloc(1, sizeof(*mr));

	if (!mr) {
		errno = ENOMEM;
		return NULL;
	}
	mr->pd     = pd;
	mr->kind   = UIB_MR_HOST;
	mr->addr   = addr;
	mr->length = length;
	mr->access = access;
	mr->lkey   = __atomic_fetch_add(&key_next, 1, __ATOMIC_RELAXED);
	mr->rkey   = mr->lkey;
	mr->pinned = (mlock(addr, length) == 0) ? 1 : 0;
	uib_pd_attach_mr(pd, mr);
	return mr;
}

struct uib_mr *uib_reg_mr_dmabuf(struct uib_pd *pd, int dmabuf_fd, size_t offset,
				 size_t length, int access)
{
	if (!pd || dmabuf_fd < 0 || !length) {
		errno = EINVAL;
		return NULL;
	}
	struct uib_mr *mr = calloc(1, sizeof(*mr));

	if (!mr) {
		errno = ENOMEM;
		return NULL;
	}
	mr->pd            = pd;
	mr->kind          = UIB_MR_DMABUF;
	mr->addr          = NULL;
	mr->length        = length;
	mr->access        = access;
	mr->dmabuf_fd     = dmabuf_fd;
	mr->dmabuf_offset = offset;
	mr->lkey          = __atomic_fetch_add(&key_next, 1, __ATOMIC_RELAXED);
	mr->rkey          = mr->lkey;
	mr->iova = __atomic_fetch_add(&iova_next, (length + 0xfff) & ~0xfffULL,
				      __ATOMIC_RELAXED);

	if (pd->ctx && pd->ctx->tops && pd->ctx->tops->mr_register) {
		if (pd->ctx->tops->mr_register(pd->ctx, mr) < 0) {
			free(mr);
			errno = ENOTSUP;
			return NULL;
		}
	}
	uib_pd_attach_mr(pd, mr);
	return mr;
}

int uib_dereg_mr(struct uib_mr *mr)
{
	if (!mr)
		return -EINVAL;
	if (mr->kind == UIB_MR_DMABUF) {
		struct uib_pd *pd = mr->pd;

		if (pd && pd->ctx && pd->ctx->tops && pd->ctx->tops->mr_deregister)
			pd->ctx->tops->mr_deregister(pd->ctx, mr);
	} else if (mr->pinned) {
		munlock(mr->addr, mr->length);
	}
	uib_pd_detach_mr(mr->pd, mr);
	free(mr);
	return 0;
}

int uib_mr_check_local(struct uib_pd *pd, uint32_t lkey,
		       const void *addr, size_t len, int access)
{
	struct uib_mr *mr = uib_pd_lookup_key(pd, lkey);

	if (!mr || mr->lkey != lkey)
		return -EACCES;
	if (!uib_mr_contains(mr, (uint64_t)(uintptr_t)addr, len))
		return -ERANGE;
	if (access && (mr->access & access) != access)
		return -EPERM;
	return 0;
}

struct uib_mr *uib_mr_check_remote(struct uib_pd *pd, uint32_t rkey,
				   uint64_t va, size_t len, int access)
{
	struct uib_mr *mr = uib_pd_lookup_key(pd, rkey);

	if (!mr || mr->rkey != rkey)
		return NULL;
	if (!uib_mr_contains(mr, va, len))
		return NULL;
	if ((mr->access & access) != access)
		return NULL;
	return mr;
}

struct uib_mr *uib_mr_check_remote_address(struct uib_pd *pd, uint64_t va,
					   size_t len, int access)
{
	struct uib_mr *mr;

	if (!pd)
		return NULL;
	pthread_mutex_lock(&pd->lock);
	for (mr = pd->mr_list; mr; mr = mr->next) {
		if (region_contains(mr, va, len) &&
		    (mr->access & access) == access)
			break;
	}
	pthread_mutex_unlock(&pd->lock);
	return mr;
}

int uib_mr_place(struct uib_mr *mr, uint64_t va, const void *src, size_t len)
{
	if (!mr)
		return -EINVAL;
	if (mr->kind == UIB_MR_DMABUF) {
		if (!uib_mr_contains(mr, va, len))
			return -ERANGE;
		return 0;
	}
	memcpy((void *)(uintptr_t)va, src, len);
	return 0;
}
