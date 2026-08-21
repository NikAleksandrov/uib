/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <infiniband/verbs.h>

#define TEST_IOVA 0x200000000000ULL
#define TEST_LENGTH 4096

int main(void)
{
	struct ibv_device **devices;
	struct ibv_context *context = NULL;
	struct ibv_device *device = NULL;
	struct ibv_mr *mr = NULL;
	struct ibv_pd *pd = NULL;
	int device_count = 0;
	int result = 1;
	int fd = -1;
	int i;

	devices = ibv_get_device_list(&device_count);
	if (!devices)
		goto out;
	for (i = 0; i < device_count; i++) {
		if (!strcmp(ibv_get_device_name(devices[i]), "uib0")) {
			device = devices[i];
			break;
		}
	}
	if (!device)
		goto out;
	context = ibv_open_device(device);
	if (!context)
		goto out;
	pd = ibv_alloc_pd(context);
	if (!pd)
		goto out;
	fd = memfd_create("uib-provider-dmabuf", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, TEST_LENGTH) < 0)
		goto out;
	mr = ibv_reg_dmabuf_mr(pd, 0, TEST_LENGTH, TEST_IOVA, fd,
			       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
			       IBV_ACCESS_REMOTE_READ);
	if (!mr || mr->addr != (void *)(uintptr_t)TEST_IOVA ||
	    mr->length != TEST_LENGTH)
		goto out;
	printf("provider.dmabuf             OK\n");
	result = 0;
out:
	if (mr)
		ibv_dereg_mr(mr);
	if (fd >= 0)
		close(fd);
	if (pd)
		ibv_dealloc_pd(pd);
	if (context)
		ibv_close_device(context);
	if (devices)
		ibv_free_device_list(devices);
	printf("RESULT %s\n", result ? "FAIL" : "PASS");
	return result;
}
