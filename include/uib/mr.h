/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_MR_H
#define UIB_MR_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

struct uib_pd;

/*
 * Where an MR's memory lives. HOST is ordinary process memory. DMABUF is an
 * imported file descriptor, including system dma-bufs and GPU dma-bufs. A
 * dma-buf uses a synthetic IOVA even when the exporter permits a CPU mapping.
 */
enum uib_mr_kind {
	UIB_MR_HOST = 0,
	UIB_MR_DMABUF,
};

enum uib_dmabuf_reg_flags {
	UIB_DMABUF_ALLOW_MMAP		= 1U << 0,
	UIB_DMABUF_ALLOW_DEVICE		= 1U << 1,
	UIB_DMABUF_REQUIRE_ZEROCOPY	= 1U << 2,
};

enum uib_mr_capability {
	UIB_MR_CAP_DMABUF		= 1U << 0,
	UIB_MR_CAP_CPU_MAPPED		= 1U << 1,
	UIB_MR_CAP_DEVICE_RX		= 1U << 2,
	UIB_MR_CAP_DEVICE_TX		= 1U << 3,
};

enum uib_mr_cpu_access {
	UIB_MR_CPU_READ			= 1U << 0,
	UIB_MR_CPU_WRITE		= 1U << 1,
};

struct uib_mr {
	struct uib_pd  *pd;
	void           *addr;
	size_t          length;
	uint32_t        lkey;
	uint32_t        rkey;
	int             access;
	int             pinned;

	enum uib_mr_kind kind;
	int             dmabuf_fd;
	size_t          dmabuf_offset;
	uint64_t        iova;
	void           *map_base;
	size_t          map_length;
	uint32_t        capabilities;
	uint32_t        dmabuf_flags;
	int             dmabuf_sync;
	void           *backend_mr;
	pthread_mutex_t ref_lock;
	pthread_cond_t  ref_cond;
	uint32_t        refs;

	struct uib_mr  *addr_parent;
	struct uib_mr  *addr_right;
	struct uib_mr  *addr_left;
	uint64_t        addr_max;
	uint32_t        addr_priority;
};

struct uib_mr *uib_reg_mr(struct uib_pd *pd, void *addr, size_t length,
			  int access);
struct uib_mr *uib_reg_mr_dmabuf(struct uib_pd *pd, int dmabuf_fd,
				 size_t offset, size_t length, int access);
struct uib_mr *uib_reg_mr_dmabuf_ex(struct uib_pd *pd, int dmabuf_fd,
				    size_t offset, size_t length, int access,
				    uint32_t flags);
struct uib_mr *uib_reg_mr_dmabuf_iova(struct uib_pd *pd, int dmabuf_fd,
				      size_t offset, size_t length,
				      uint64_t iova, int access,
				      uint32_t flags);
int uib_dereg_mr(struct uib_mr *mr);

/* Lookups return a referenced MR. Release it with uib_mr_put(). */
void uib_mr_put(struct uib_mr *mr);

int uib_mr_check_local(struct uib_pd *pd, uint32_t lkey,
		       const void *addr, size_t len, int access);
struct uib_mr *uib_mr_lookup_local(struct uib_pd *pd, uint32_t lkey,
				   const void *addr, size_t len, int access);
struct uib_mr *uib_mr_check_remote(struct uib_pd *pd, uint32_t rkey,
				   uint64_t va, size_t len, int access);
struct uib_mr *uib_mr_check_remote_address(struct uib_pd *pd, uint64_t va,
					   size_t len, int access);

/*
 * Copy between protocol buffers and an MR. DMA-BUF CPU mappings are enclosed
 * by DMA_BUF_IOCTL_SYNC when the exporter implements that ioctl.
 */
int uib_mr_place(struct uib_mr *mr, uint64_t va, const void *src, size_t len);
int uib_mr_read(struct uib_mr *mr, uint64_t va, void *dst, size_t len);
int uib_mr_cpu_begin(struct uib_mr *mr, uint32_t access);
int uib_mr_cpu_end(struct uib_mr *mr, uint32_t access);

#endif
