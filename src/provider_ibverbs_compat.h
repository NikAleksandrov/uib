/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_PROVIDER_IBVERBS_COMPAT_H
#define UIB_PROVIDER_IBVERBS_COMPAT_H

#include <infiniband/verbs.h>

#ifndef UIB_HAVE_IBV_REG_MR_EX
struct ibv_dmah;
struct ibv_buf;

enum ibv_mr_init_attr_mask {
	IBV_REG_MR_MASK_IOVA		= 1 << 0,
	IBV_REG_MR_MASK_ADDR		= 1 << 1,
	IBV_REG_MR_MASK_FD		= 1 << 2,
	IBV_REG_MR_MASK_FD_OFFSET	= 1 << 3,
	IBV_REG_MR_MASK_DMAH		= 1 << 4,
	IBV_REG_MR_MASK_BUF		= 1 << 5,
};

struct ibv_mr_init_attr {
	size_t length;
	int access;
	uint64_t comp_mask;
	uint64_t iova;
	void *addr;
	int fd;
	uint64_t fd_offset;
	struct ibv_dmah *dmah;
	struct ibv_buf *buf;
};
#endif

#endif
