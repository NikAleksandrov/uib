/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifdef UIB_HAVE_DMABUF
#include <linux/dma-buf.h>
#else
#define DMA_BUF_SYNC_READ	0
#define DMA_BUF_SYNC_WRITE	0
#define DMA_BUF_SYNC_START	0
#define DMA_BUF_SYNC_END		0
#endif

#include "uib/mr.h"
#include "uib/pd.h"
#include "uib/context.h"
#include "uib/types.h"
#include "mr_internal.h"

static uint32_t key_next = 0x10;
static uint64_t iova_next = 0x100000000000ULL;

static int mr_lifetime_init(struct uib_mr *mr)
{
	int rc;

	rc = pthread_mutex_init(&mr->ref_lock, NULL);
	if (rc)
		return -rc;
	rc = pthread_cond_init(&mr->ref_cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&mr->ref_lock);
		return -rc;
	}
	mr->refs = 1;
	return 0;
}

static void mr_lifetime_destroy(struct uib_mr *mr)
{
	pthread_cond_destroy(&mr->ref_cond);
	pthread_mutex_destroy(&mr->ref_lock);
}

/**
 * uib_mr_get - Retain an existing memory-region reference.
 * @mr: Referenced memory region
 * @uib-api: Memory regions
 *
 * The caller must already own a reference that keeps @mr alive.
 */
void uib_mr_get(struct uib_mr *mr)
{
	if (mr)
		__atomic_add_fetch(&mr->refs, 1, __ATOMIC_RELAXED);
}

/**
 * uib_mr_put - Release an MR reference returned by a lookup.
 * @mr: Memory region
 * @uib-api: Memory regions
 */
void uib_mr_put(struct uib_mr *mr)
{
	uint32_t refs;

	if (!mr)
		return;
	refs = __atomic_sub_fetch(&mr->refs, 1, __ATOMIC_RELEASE);
	if (refs == 1) {
		pthread_mutex_lock(&mr->ref_lock);
		pthread_cond_signal(&mr->ref_cond);
		pthread_mutex_unlock(&mr->ref_lock);
	}
}

static void mr_wait_unused(struct uib_mr *mr)
{
	pthread_mutex_lock(&mr->ref_lock);
	while (__atomic_load_n(&mr->refs, __ATOMIC_ACQUIRE) != 1)
		pthread_cond_wait(&mr->ref_cond, &mr->ref_lock);
	pthread_mutex_unlock(&mr->ref_lock);
}

static int dmabuf_sync(struct uib_mr *mr, uint64_t flags)
{
#ifdef UIB_HAVE_DMABUF
	struct dma_buf_sync sync = { .flags = flags };
	int rc;

	if (!mr->dmabuf_sync)
		return 0;
	do {
		rc = ioctl(mr->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (rc < 0 && (errno == EINTR || errno == EAGAIN));
	if (!rc)
		return 0;
	if (errno == ENOTTY) {
		mr->dmabuf_sync = 0;
		return 0;
	}
	return -errno;
#else
	(void)mr;
	(void)flags;
	return 0;
#endif
}

static int dmabuf_map(struct uib_mr *mr)
{
#ifdef UIB_HAVE_DMABUF
	size_t aligned_offset;
	size_t page_offset;
	size_t map_length;
	off_t map_offset;
	long page_size;
	void *mapping;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return -EINVAL;
	page_offset = mr->dmabuf_offset % (size_t)page_size;
	if (mr->length > SIZE_MAX - page_offset)
		return -EOVERFLOW;
	map_length = mr->length + page_offset;
	aligned_offset = mr->dmabuf_offset - page_offset;
	map_offset = (off_t)aligned_offset;
	if (map_offset < 0 || (uintmax_t)map_offset != (uintmax_t)aligned_offset)
		return -EOVERFLOW;
	mapping = mmap(NULL, map_length, PROT_READ | PROT_WRITE, MAP_SHARED,
		       mr->dmabuf_fd, map_offset);
	if (mapping == MAP_FAILED)
		return -errno;
	mr->map_base = mapping;
	mr->map_length = map_length;
	mr->addr = (uint8_t *)mapping + page_offset;
	mr->capabilities |= UIB_MR_CAP_CPU_MAPPED;
	mr->dmabuf_sync = 1;
	return 0;
#else
	(void)mr;
	return -EOPNOTSUPP;
#endif
}

/**
 * uib_reg_mr - Register ordinary process memory for RDMA access.
 * @pd: Protection domain
 * @addr: Start address of the memory range
 * @length: Length of the memory range
 * @access: Access permissions required or granted
 * @uib-api: Memory regions
 *
 * Return: A registered memory region on success, or NULL on failure.
 */
struct uib_mr *uib_reg_mr(struct uib_pd *pd, void *addr, size_t length,
			  int access)
{
	struct uib_mr *mr;
	int rc;

	if (!pd || !addr || !length ||
	    (uint64_t)(uintptr_t)addr > UINT64_MAX - length) {
		errno = EINVAL;
		return NULL;
	}
	mr = calloc(1, sizeof(*mr));
	if (!mr) {
		errno = ENOMEM;
		return NULL;
	}
	rc = mr_lifetime_init(mr);
	if (rc < 0) {
		free(mr);
		errno = -rc;
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
	if (uib_pd_attach_mr(pd, mr) < 0) {
		if (mr->pinned)
			munlock(addr, length);
		mr_lifetime_destroy(mr);
		free(mr);
		errno = ENOMEM;
		return NULL;
	}
	return mr;
}

/**
 * uib_reg_mr_dmabuf - Import a CPU-mappable DMA-BUF with a synthetic IOVA.
 * @pd: Protection domain
 * @dmabuf_fd: File descriptor for the DMA-BUF
 * @offset: Byte offset into the DMA-BUF
 * @length: Length of the memory range
 * @access: Access permissions required or granted
 * @uib-api: Memory regions
 *
 * Return: A registered memory region on success, or NULL on failure.
 */
struct uib_mr *uib_reg_mr_dmabuf(struct uib_pd *pd, int dmabuf_fd,
				 size_t offset, size_t length, int access)
{
	return uib_reg_mr_dmabuf_ex(pd, dmabuf_fd, offset, length, access,
				     UIB_DMABUF_ALLOW_MMAP |
				     UIB_DMABUF_ALLOW_DEVICE);
}

static int dmabuf_registration_valid(struct uib_pd *pd, int dmabuf_fd,
				     size_t offset, size_t length,
				     uint32_t flags)
{
	if (!pd || dmabuf_fd < 0 || !length)
		return -EINVAL;
	if (offset > SIZE_MAX - length)
		return -EOVERFLOW;
	if (flags & ~(UIB_DMABUF_ALLOW_MMAP | UIB_DMABUF_ALLOW_DEVICE |
		      UIB_DMABUF_REQUIRE_ZEROCOPY))
		return -EINVAL;
	return 0;
}

static int dmabuf_set_iova(struct uib_mr *mr, uint64_t requested_iova,
			   int fixed_iova)
{
	uint64_t span;

	if (mr->length > UINT64_MAX - 0xfffULL)
		return -EOVERFLOW;
	span = ((uint64_t)mr->length + 0xfffULL) & ~0xfffULL;
	if (!fixed_iova) {
		mr->iova = __atomic_fetch_add(&iova_next, span,
					     __ATOMIC_RELAXED);
		return 0;
	}
	if (requested_iova > UINT64_MAX - mr->length)
		return -EOVERFLOW;
	mr->iova = requested_iova;
	return 0;
}

static int dmabuf_enable_backends(struct uib_mr *mr)
{
	struct uib_pd *pd = mr->pd;
	int rc = -EOPNOTSUPP;

	if (mr->dmabuf_flags & UIB_DMABUF_ALLOW_MMAP)
		rc = dmabuf_map(mr);
	if ((mr->dmabuf_flags & UIB_DMABUF_ALLOW_DEVICE) && pd->ctx &&
	    pd->ctx->tops && pd->ctx->tops->mr_capabilities &&
	    pd->ctx->tops->mr_register &&
	    !pd->ctx->tops->mr_register(pd->ctx, mr)) {
		mr->capabilities |= pd->ctx->tops->mr_capabilities;
		rc = 0;
	}
	if (!(mr->capabilities & (UIB_MR_CAP_CPU_MAPPED |
				 UIB_MR_CAP_DEVICE_RX | UIB_MR_CAP_DEVICE_TX)))
		return -EOPNOTSUPP;
	if ((mr->dmabuf_flags & UIB_DMABUF_REQUIRE_ZEROCOPY) &&
	    !(mr->capabilities & (UIB_MR_CAP_DEVICE_RX |
				  UIB_MR_CAP_DEVICE_TX)))
		return -EOPNOTSUPP;
	return rc;
}

static void dmabuf_registration_cleanup(struct uib_mr *mr)
{
	struct uib_pd *pd = mr->pd;

	if (mr->backend_mr && pd->ctx && pd->ctx->tops &&
	    pd->ctx->tops->mr_deregister)
		pd->ctx->tops->mr_deregister(pd->ctx, mr);
	if (mr->map_base)
		munmap(mr->map_base, mr->map_length);
	if (mr->dmabuf_fd >= 0)
		close(mr->dmabuf_fd);
	mr_lifetime_destroy(mr);
	free(mr);
}

static struct uib_mr *reg_mr_dmabuf(struct uib_pd *pd, int dmabuf_fd,
				    size_t offset, size_t length,
				    uint64_t requested_iova, int fixed_iova,
				    int access, uint32_t flags)
{
	struct uib_mr *mr;
	int rc;

#ifndef UIB_HAVE_DMABUF
	(void)offset;
	(void)requested_iova;
	(void)fixed_iova;
	(void)access;
	(void)flags;
	if (!pd || dmabuf_fd < 0 || !length)
		errno = EINVAL;
	else
		errno = EOPNOTSUPP;
	return NULL;
#endif
	rc = dmabuf_registration_valid(pd, dmabuf_fd, offset, length, flags);
	if (rc < 0) {
		errno = -rc;
		return NULL;
	}
	mr = calloc(1, sizeof(*mr));
	if (!mr) {
		errno = ENOMEM;
		return NULL;
	}
	rc = mr_lifetime_init(mr);
	if (rc < 0) {
		free(mr);
		errno = -rc;
		return NULL;
	}
	mr->pd            = pd;
	mr->kind          = UIB_MR_DMABUF;
	mr->length        = length;
	mr->access        = access;
	mr->dmabuf_fd     = fcntl(dmabuf_fd, F_DUPFD_CLOEXEC, 0);
	if (mr->dmabuf_fd < 0) {
		rc = -errno;
		goto err;
	}
	mr->dmabuf_offset = offset;
	mr->dmabuf_flags  = flags;
	mr->capabilities  = UIB_MR_CAP_DMABUF;
	mr->lkey          = __atomic_fetch_add(&key_next, 1, __ATOMIC_RELAXED);
	mr->rkey          = mr->lkey;
	rc = dmabuf_set_iova(mr, requested_iova, fixed_iova);
	if (rc < 0)
		goto err;
	rc = dmabuf_enable_backends(mr);
	if (rc < 0)
		goto err;
	rc = uib_pd_attach_mr(pd, mr);
	if (rc < 0)
		goto err;
	return mr;

err:
	dmabuf_registration_cleanup(mr);
	errno = -rc;
	return NULL;
}

/**
 * uib_reg_mr_dmabuf_ex - Import a DMA-BUF with explicit registration flags.
 * @pd: Protection domain
 * @dmabuf_fd: File descriptor for the DMA-BUF
 * @offset: Byte offset into the DMA-BUF
 * @length: Length of the memory range
 * @access: Access permissions required or granted
 * @flags: DMA-BUF registration flags
 * @uib-api: Memory regions
 *
 * Return: A registered memory region on success, or NULL on failure.
 */
struct uib_mr *uib_reg_mr_dmabuf_ex(struct uib_pd *pd, int dmabuf_fd,
				    size_t offset, size_t length, int access,
				    uint32_t flags)
{
	return reg_mr_dmabuf(pd, dmabuf_fd, offset, length, 0, 0, access,
			      flags);
}

/**
 * uib_reg_mr_dmabuf_iova - Import a DMA-BUF at a caller-selected IOVA.
 * @pd: Protection domain
 * @dmabuf_fd: File descriptor for the DMA-BUF
 * @offset: Byte offset into the DMA-BUF
 * @length: Length of the memory range
 * @iova: I/O virtual address assigned to the memory region
 * @access: Access permissions required or granted
 * @flags: DMA-BUF registration flags
 * @uib-api: Memory regions
 *
 * Return: A registered memory region on success, or NULL on failure.
 */
struct uib_mr *uib_reg_mr_dmabuf_iova(struct uib_pd *pd, int dmabuf_fd,
				      size_t offset, size_t length,
				      uint64_t iova, int access,
				      uint32_t flags)
{
	return reg_mr_dmabuf(pd, dmabuf_fd, offset, length, iova, 1, access,
			      flags);
}

/**
 * uib_dereg_mr - Unpublish an MR and wait for lookup users before release.
 * @mr: Memory region
 * @uib-api: Memory regions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_dereg_mr(struct uib_mr *mr)
{
	int rc;

	if (!mr)
		return -EINVAL;
	rc = uib_pd_detach_mr(mr->pd, mr);
	if (rc < 0)
		return rc;
	mr_wait_unused(mr);
	if (mr->kind == UIB_MR_DMABUF) {
		struct uib_pd *pd = mr->pd;

		if (pd && pd->ctx && pd->ctx->tops && pd->ctx->tops->mr_deregister)
			pd->ctx->tops->mr_deregister(pd->ctx, mr);
		if (mr->map_base)
			munmap(mr->map_base, mr->map_length);
		if (mr->dmabuf_fd >= 0)
			close(mr->dmabuf_fd);
	} else if (mr->pinned) {
		munlock(mr->addr, mr->length);
	}
	mr_lifetime_destroy(mr);
	free(mr);
	return 0;
}

/**
 * uib_mr_check_local - Validate a local key, range, and access mask.
 * @pd: Protection domain
 * @lkey: Local memory region key
 * @addr: Start address of the memory range
 * @len: Length of the byte range
 * @access: Access permissions required or granted
 * @uib-api: Memory regions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_mr_check_local(struct uib_pd *pd, uint32_t lkey,
		       const void *addr, size_t len, int access)
{
	struct uib_mr *mr = uib_pd_lookup_lkey(pd, lkey);
	int rc = 0;

	if (!mr)
		return -EACCES;
	if (mr->lkey != lkey)
		rc = -EACCES;
	else if (!uib_mr_contains(mr, (uint64_t)(uintptr_t)addr, len))
		rc = -ERANGE;
	else if (access && (mr->access & access) != access)
		rc = -EPERM;
	uib_mr_put(mr);
	return rc;
}

/**
 * uib_mr_lookup_local - Find and reference a validated local MR.
 * @pd: Protection domain
 * @lkey: Local memory region key
 * @addr: Start address of the memory range
 * @len: Length of the byte range
 * @access: Access permissions required or granted
 * @uib-api: Memory regions
 *
 * Return: A referenced matching MR, or NULL if validation fails.
 */
struct uib_mr *uib_mr_lookup_local(struct uib_pd *pd, uint32_t lkey,
				   const void *addr, size_t len, int access)
{
	struct uib_mr *mr = uib_pd_lookup_lkey(pd, lkey);

	if (!mr)
		return NULL;
	if (mr->lkey != lkey) {
		uib_mr_put(mr);
		return NULL;
	}
	if (!uib_mr_contains(mr, (uint64_t)(uintptr_t)addr, len)) {
		uib_mr_put(mr);
		return NULL;
	}
	if (access && (mr->access & access) != access) {
		uib_mr_put(mr);
		return NULL;
	}
	return mr;
}

/**
 * uib_mr_check_remote - Find and reference a validated remote MR by rkey.
 * @pd: Protection domain
 * @rkey: Remote memory region key
 * @va: I/O virtual address
 * @len: Length of the byte range
 * @access: Access permissions required or granted
 * @uib-api: Memory regions
 *
 * Return: A referenced matching MR, or NULL if validation fails.
 */
struct uib_mr *uib_mr_check_remote(struct uib_pd *pd, uint32_t rkey,
				   uint64_t va, size_t len, int access)
{
	struct uib_mr *mr = uib_pd_lookup_rkey(pd, rkey);

	if (!mr)
		return NULL;
	if (mr->rkey != rkey) {
		uib_mr_put(mr);
		return NULL;
	}
	if (!uib_mr_contains(mr, va, len)) {
		uib_mr_put(mr);
		return NULL;
	}
	if ((mr->access & access) != access) {
		uib_mr_put(mr);
		return NULL;
	}
	return mr;
}

/**
 * uib_mr_check_remote_address - Find and reference a remote MR by address.
 * @pd: Protection domain
 * @va: I/O virtual address
 * @len: Length of the byte range
 * @access: Access permissions required or granted
 * @uib-api: Memory regions
 *
 * Return: A referenced matching MR, or NULL if validation fails.
 */
struct uib_mr *uib_mr_check_remote_address(struct uib_pd *pd, uint64_t va,
					   size_t len, int access)
{
	return uib_pd_lookup_address(pd, va, len, access);
}

/**
 * uib_mr_place - Copy received bytes into a registered memory range.
 * @mr: Memory region
 * @va: I/O virtual address
 * @src: Source byte buffer
 * @len: Length of the byte range
 * @uib-api: Memory regions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_mr_place(struct uib_mr *mr, uint64_t va, const void *src, size_t len)
{
	uint64_t base;
	void *target;
	int rc;

	if (!mr)
		return -EINVAL;
	if (!uib_mr_contains(mr, va, len))
		return -ERANGE;
	if (len && !src)
		return -EINVAL;
	base = uib_mr_base(mr);
	if (mr->kind == UIB_MR_HOST)
		target = (void *)(uintptr_t)va;
	else if (mr->capabilities & UIB_MR_CAP_CPU_MAPPED)
		target = (uint8_t *)mr->addr + (va - base);
	else
		return -EOPNOTSUPP;
	rc = dmabuf_sync(mr, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	if (rc < 0)
		return rc;
	memcpy(target, src, len);
	return dmabuf_sync(mr, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

/**
 * uib_mr_read - Copy bytes out of a registered memory range.
 * @mr: Memory region
 * @va: I/O virtual address
 * @dst: Destination byte buffer
 * @len: Length of the byte range
 * @uib-api: Memory regions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_mr_read(struct uib_mr *mr, uint64_t va, void *dst, size_t len)
{
	const void *source;
	uint64_t base;
	int rc;

	if (!mr || (len && !dst))
		return -EINVAL;
	if (!uib_mr_contains(mr, va, len))
		return -ERANGE;
	base = uib_mr_base(mr);
	if (mr->kind == UIB_MR_HOST)
		source = (const void *)(uintptr_t)va;
	else if (mr->capabilities & UIB_MR_CAP_CPU_MAPPED)
		source = (const uint8_t *)mr->addr + (va - base);
	else
		return -EOPNOTSUPP;
	rc = dmabuf_sync(mr, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	if (rc < 0)
		return rc;
	memcpy(dst, source, len);
	return dmabuf_sync(mr, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

static uint64_t cpu_sync_flags(uint32_t access)
{
	uint64_t flags = 0;

	if (access & UIB_MR_CPU_READ)
		flags |= DMA_BUF_SYNC_READ;
	if (access & UIB_MR_CPU_WRITE)
		flags |= DMA_BUF_SYNC_WRITE;
	return flags;
}

/**
 * uib_mr_cpu_begin - Begin synchronized CPU access to a DMA-BUF MR.
 * @mr: Memory region
 * @access: CPU read and write access mask
 * @uib-api: Memory regions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_mr_cpu_begin(struct uib_mr *mr, uint32_t access)
{
	if (!mr || !access ||
	    access & ~(UIB_MR_CPU_READ | UIB_MR_CPU_WRITE))
		return -EINVAL;
	if (mr->kind == UIB_MR_DMABUF &&
	    !(mr->capabilities & UIB_MR_CAP_CPU_MAPPED))
		return -EOPNOTSUPP;
	return dmabuf_sync(mr, DMA_BUF_SYNC_START | cpu_sync_flags(access));
}

/**
 * uib_mr_cpu_end - End synchronized CPU access to a DMA-BUF MR.
 * @mr: Memory region
 * @access: CPU read and write access mask
 * @uib-api: Memory regions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_mr_cpu_end(struct uib_mr *mr, uint32_t access)
{
	if (!mr || !access ||
	    access & ~(UIB_MR_CPU_READ | UIB_MR_CPU_WRITE))
		return -EINVAL;
	if (mr->kind == UIB_MR_DMABUF &&
	    !(mr->capabilities & UIB_MR_CAP_CPU_MAPPED))
		return -EOPNOTSUPP;
	return dmabuf_sync(mr, DMA_BUF_SYNC_END | cpu_sync_flags(access));
}
