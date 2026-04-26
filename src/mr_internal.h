/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_MR_INTERNAL_H
#define UIB_MR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "uib/mr.h"

static inline uint64_t uib_mr_base(const struct uib_mr *mr)
{
	if (mr->kind == UIB_MR_DMABUF)
		return mr->iova;
	return (uint64_t)(uintptr_t)mr->addr;
}

static inline int uib_mr_contains(const struct uib_mr *mr, uint64_t address,
				  size_t length)
{
	uint64_t base;

	base = uib_mr_base(mr);
	if (address < base || address - base > mr->length)
		return 0;
	return length <= mr->length - (size_t)(address - base);
}

#endif
