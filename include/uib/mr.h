#ifndef UIB_MR_H
#define UIB_MR_H

#include <stdint.h>
#include <stddef.h>

struct uib_pd;

enum uib_mr_kind {
	UIB_MR_HOST = 0,
	UIB_MR_DMABUF,
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
	void           *backend_mr;

	struct uib_mr  *next;
	struct uib_mr  *prev;
};

struct uib_mr *uib_reg_mr(struct uib_pd *pd, void *addr, size_t length, int access);
struct uib_mr *uib_reg_mr_dmabuf(struct uib_pd *pd, int dmabuf_fd, size_t offset,
				 size_t length, int access);
int uib_dereg_mr(struct uib_mr *mr);

int uib_mr_check_local(struct uib_pd *pd, uint32_t lkey,
		       const void *addr, size_t len, int access);
struct uib_mr *uib_mr_check_remote(struct uib_pd *pd, uint32_t rkey,
				   uint64_t va, size_t len, int access);
struct uib_mr *uib_mr_check_remote_address(struct uib_pd *pd, uint64_t va,
					   size_t len, int access);

int uib_mr_place(struct uib_mr *mr, uint64_t va, const void *src, size_t len);

#endif
