/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "uib/uib.h"
#include "../src/uring_zcrx.h"

static int failed;

static void check(const char *name, int ok)
{
	printf("%-28s %s\n", name, ok ? "OK" : "FAIL");
	if (!ok)
		failed = 1;
}

int main(void)
{
	struct uib_zcrx *engine;
	struct uib_context *context;
	uint32_t capabilities;
	int compiled;
	int rc;

	engine = NULL;
	compiled = uib_zcrx_compiled();
#ifdef UIB_HAVE_URING_ZCRX
	check("zcrx.compiled", compiled);
#else
	check("zcrx.compiled", !compiled);
#endif
	rc = uib_zcrx_create(&engine, "uib-no-such-interface", 0, 64,
			     1U << 20);
	check("zcrx.bad_interface",
	      rc == (compiled ? -ENODEV : -EOPNOTSUPP) && !engine);
	rc = uib_zcrx_create(&engine, "lo", 0, 64, 1U << 20);
	if (!rc) {
		check("zcrx.runtime", uib_zcrx_event_fd(engine) >= 0);
		uib_zcrx_destroy(engine);
	} else {
		printf("%-28s SKIP (%s)\n", "zcrx.runtime",
		       strerror(-rc));
	}
	setenv("UIB_TRANSPORT_MODE", "tcp-devmem", 1);
	setenv("UIB_HOST_ZC", "invalid", 1);
	context = uib_open_device_protocol("127.0.0.1", 20409, "dummy");
	check("zcrx.invalid_mode", context == NULL && errno == EINVAL);
	setenv("UIB_HOST_ZC", "auto", 1);
	setenv("UIB_HOST_ZC_IFACE", "lo", 1);
	context = uib_open_device_protocol("127.0.0.1", 20410, "dummy");
	check("zcrx.auto_fallback", context != NULL);
	if (context) {
		capabilities = uib_host_zc_capabilities(context);
		check("zcrx.capabilities",
		      capabilities & UIB_HOST_ZC_PROVIDER_LOANS);
		uib_close_device(context);
	}
	setenv("UIB_HOST_ZC", "required", 1);
	context = uib_open_device_protocol("127.0.0.1", 20411, "dummy");
	if (context) {
		capabilities = uib_host_zc_capabilities(context);
		check("zcrx.required",
		      capabilities & UIB_HOST_ZC_KERNEL_ZCRX);
		uib_close_device(context);
	} else {
		printf("%-28s SKIP (unavailable)\n", "zcrx.required");
	}
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
