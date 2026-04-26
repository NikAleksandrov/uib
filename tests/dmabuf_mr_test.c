#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/genetlink.h>

#include "uib/uib.h"
#include "uib/devmem.h"

static int g_fail;

static void check(const char *tag, int ok)
{
	printf("%-26s %s\n", tag, ok ? "OK" : "FAIL");
	if (!ok)
		g_fail = 1;
}

int main(void)
{
	struct uib_context *ctx;
	struct uib_pd *pd;
	struct uib_mr *dmr, *hmr;
	static uint8_t hostbuf[4096];
	uint8_t src[64];
	int fd;
	unsigned int i;

	ctx = uib_open_device("127.0.0.1", 18517);
	if (!ctx) {
		printf("open failed\n");
		return 2;
	}
	pd = uib_alloc_pd(ctx);

	fd = memfd_create("uib-dmabuf", 0);
	if (fd < 0 || ftruncate(fd, 1 << 20) < 0) {
		printf("memfd failed\n");
		return 2;
	}

	dmr = uib_reg_mr_dmabuf(pd, fd, 0, 4096, UIB_ACCESS_REMOTE_WRITE);
	check("dmabuf.reg", dmr != NULL);
	check("dmabuf.kind", dmr && dmr->kind == UIB_MR_DMABUF);
	check("dmabuf.no_host_va", dmr && dmr->addr == NULL);
	check("dmabuf.iova_set", dmr && dmr->iova != 0);

	check("remote.in_range",
	      uib_mr_check_remote(pd, dmr->rkey, dmr->iova, 100,
				  UIB_ACCESS_REMOTE_WRITE) == dmr);
	check("remote.out_of_range",
	      uib_mr_check_remote(pd, dmr->rkey, dmr->iova + 5000, 1,
				  UIB_ACCESS_REMOTE_WRITE) == NULL);

	check("place.valid", uib_mr_place(dmr, dmr->iova, NULL, 100) == 0);
	check("place.overflow", uib_mr_place(dmr, dmr->iova + 5000, NULL, 100) != 0);

	for (i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)(i + 9);
	hmr = uib_reg_mr(pd, hostbuf, sizeof(hostbuf), UIB_ACCESS_REMOTE_WRITE);
	memset(hostbuf, 0, sizeof(src));
	check("host.place",
	      uib_mr_place(hmr, (uint64_t)(uintptr_t)hostbuf, src, sizeof(src)) == 0 &&
	      memcmp(hostbuf, src, sizeof(src)) == 0);

	check("genl.nlctrl", uib_genl_family_id("nlctrl") == GENL_ID_CTRL);
	check("genl.bogus", uib_genl_family_id("uib_no_such_family_zzz") < 0);

	uib_dereg_mr(dmr);
	uib_dereg_mr(hmr);
	close(fd);
	uib_dealloc_pd(pd);
	uib_close_device(ctx);

	printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
