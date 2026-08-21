/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uib/uib.h"

#define TEST_LENGTH 4096

int main(void)
{
	unsigned long long parsed_offset = 0;
	struct uib_context *context;
	const char *offset_value = getenv("UIB_TEST_GPU_DMABUF_OFFSET");
	const char *fd_value = getenv("UIB_TEST_GPU_DMABUF_FD");
	struct uib_mr *mr = NULL;
	struct uib_pd *pd = NULL;
	uint8_t expected[64];
	uint8_t observed[64];
	long parsed_fd;
	char *end;
	int result = 1;
	int i;

	if (!fd_value || !*fd_value) {
		printf("external GPU DMA-BUF: SKIP (UIB_TEST_GPU_DMABUF_FD unset)\n");
		return 0;
	}
	errno = 0;
	parsed_fd = strtol(fd_value, &end, 0);
	if (errno || !end || *end || parsed_fd < 0 || parsed_fd > INT32_MAX)
		return 2;
	if (offset_value && *offset_value) {
		errno = 0;
		parsed_offset = strtoull(offset_value, &end, 0);
		if (errno || !end || *end || parsed_offset > SIZE_MAX)
			return 2;
	}
	context = uib_open_device("127.0.0.1", 19300);
	if (!context)
		return 2;
	pd = uib_alloc_pd(context);
	if (!pd)
		goto out;
	mr = uib_reg_mr_dmabuf(pd, (int)parsed_fd, (size_t)parsed_offset,
			       TEST_LENGTH, UIB_ACCESS_LOCAL_WRITE |
			       UIB_ACCESS_REMOTE_WRITE | UIB_ACCESS_REMOTE_READ);
	if (!mr) {
		if (errno == EOPNOTSUPP) {
			printf("external GPU DMA-BUF: SKIP (no CPU mapping or device datapath)\n");
			result = 0;
		}
		goto out;
	}
	for (i = 0; i < (int)sizeof(expected); i++)
		expected[i] = (uint8_t)(i * 13 + 7);
	if (uib_mr_place(mr, mr->iova + 31, expected, sizeof(expected)) < 0 ||
	    uib_mr_read(mr, mr->iova + 31, observed, sizeof(observed)) < 0 ||
	    memcmp(expected, observed, sizeof(expected)))
		goto out;
	printf("external GPU DMA-BUF import/read/write: PASS\n");
	result = 0;
out:
	if (mr)
		uib_dereg_mr(mr);
	if (pd)
		uib_dealloc_pd(pd);
	uib_close_device(context);
	return result;
}
