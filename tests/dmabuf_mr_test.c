/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Unit test for DMA-BUF MR data access. A memfd exercises the system-memory
 * mapping path without special hardware. A pipe models a non-mappable exporter
 * and verifies that device-only memory is rejected without a true datapath.
 */
#include <errno.h>
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
	struct uib_mr *dmr;
	struct uib_mr *hmr;
	static uint8_t hostbuf[4096];
	uint8_t src[64];
#ifdef UIB_HAVE_DMABUF
	uint8_t observed[64];
	struct uib_sge sge;
	uint8_t gathered[64];
	unsigned int i;
	int pipefd[2];
#endif
	int fd;

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
#ifdef UIB_HAVE_DMABUF
	for (i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)(i + 9);
	check("dmabuf.reg", dmr != NULL);
	check("dmabuf.kind", dmr && dmr->kind == UIB_MR_DMABUF);
	check("dmabuf.cpu_mapping",
	      dmr && (dmr->capabilities & UIB_MR_CAP_CPU_MAPPED));
	check("dmabuf.iova_set", dmr && dmr->iova != 0);

	check("remote.in_range",
	      uib_mr_check_remote(pd, dmr->rkey, dmr->iova, 100,
				  UIB_ACCESS_REMOTE_WRITE) == dmr);
	check("remote.out_of_range",
	      uib_mr_check_remote(pd, dmr->rkey, dmr->iova + 5000, 1,
				  UIB_ACCESS_REMOTE_WRITE) == NULL);

	check("place.valid",
	      uib_mr_place(dmr, dmr->iova + 17, src, sizeof(src)) == 0);
	memset(observed, 0, sizeof(observed));
	check("dmabuf.file_data",
	      pread(fd, observed, sizeof(observed), 17) == sizeof(observed) &&
	      !memcmp(observed, src, sizeof(src)));
	memset(observed, 0, sizeof(observed));
	check("dmabuf.read",
	      !uib_mr_read(dmr, dmr->iova + 17, observed, sizeof(observed)) &&
	      !memcmp(observed, src, sizeof(src)));
	check("dmabuf.cpu_sync",
	      !uib_mr_cpu_begin(dmr, UIB_MR_CPU_READ | UIB_MR_CPU_WRITE) &&
	      !uib_mr_cpu_end(dmr, UIB_MR_CPU_READ | UIB_MR_CPU_WRITE));
	check("place.overflow", uib_mr_place(dmr, dmr->iova + 5000, NULL, 100) != 0);
	sge.addr = dmr->iova + 17;
	sge.length = sizeof(src);
	sge.lkey = dmr->lkey;
	memset(gathered, 0, sizeof(gathered));
	check("dmabuf.gather",
	      uib_qp_gather(pd, &sge, 1, gathered, sizeof(gathered)) ==
	      sizeof(gathered) && !memcmp(gathered, src, sizeof(src)));
	check("dmabuf.require_zc",
	      !uib_reg_mr_dmabuf_ex(pd, fd, 0, 4096,
				    UIB_ACCESS_REMOTE_WRITE,
				    UIB_DMABUF_ALLOW_MMAP |
				    UIB_DMABUF_REQUIRE_ZEROCOPY) &&
	      errno == EOPNOTSUPP);
	if (!pipe(pipefd)) {
		check("dmabuf.nonmappable",
		      !uib_reg_mr_dmabuf_ex(pd, pipefd[0], 0, 4096,
					    UIB_ACCESS_REMOTE_WRITE,
					    UIB_DMABUF_ALLOW_MMAP) &&
		      errno == EOPNOTSUPP);
		close(pipefd[0]);
		close(pipefd[1]);
	} else {
		check("dmabuf.nonmappable", 0);
	}
#else
	memset(src, 0x5a, sizeof(src));
	check("dmabuf.disabled", !dmr && errno == EOPNOTSUPP);
#endif

	hmr = uib_reg_mr(pd, hostbuf, sizeof(hostbuf), UIB_ACCESS_REMOTE_WRITE);
	memset(hostbuf, 0, sizeof(src));
	check("host.place",
	      uib_mr_place(hmr, (uint64_t)(uintptr_t)hostbuf, src, sizeof(src)) == 0 &&
	      memcmp(hostbuf, src, sizeof(src)) == 0);

	check("genl.nlctrl", uib_genl_family_id("nlctrl") == GENL_ID_CTRL);
	check("genl.bogus", uib_genl_family_id("uib_no_such_family_zzz") < 0);

	if (dmr)
		uib_dereg_mr(dmr);
	uib_dereg_mr(hmr);
	close(fd);
	uib_dealloc_pd(pd);
	uib_close_device(ctx);

	printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
