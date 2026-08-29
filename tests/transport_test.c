/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "uib/context.h"
#include "uib/env.h"
#include "uib/transport.h"

static int failed;

static void check(const char *name, int condition)
{
	printf("%-28s %s\n", name, condition ? "OK" : "FAIL");
	if (!condition)
		failed = 1;
}

int main(void)
{
	struct uib_context *ctx;
	struct uib_transport_stats stats;

	unsetenv("UIB_TRANSPORT_MODE");
	check("transport.default.async",
	      uib_transport_mode_from_env() == UIB_MODE_UDP_IO_URING);
	setenv("UIB_TRANSPORT_MODE", "udp-io-uring", 1);
	check("transport.explicit.async",
	      uib_transport_mode_from_env() == UIB_MODE_UDP_IO_URING);
	setenv("UIB_TRANSPORT_MODE", "udp-sync", 1);
	check("transport.explicit.sync",
	      uib_transport_mode_from_env() == UIB_MODE_UDP_SYNC);
	setenv("UIB_TRANSPORT_MODE", "udp-blocking", 1);
	check("transport.legacy.sync",
	      uib_transport_mode_from_env() == UIB_MODE_UDP_SYNC);
	setenv("UIB_TRANSPORT_MODE", "invalid", 1);
	check("transport.invalid.async",
	      uib_transport_mode_from_env() == UIB_MODE_UDP_IO_URING);
	setenv(UIB_ENV_TRANSPORT_MODE, "udp-sync", 1);
	setenv(UIB_ENV_ZEROCOPY, "off", 1);
	setenv(UIB_ENV_UDP_WORKERS, "2", 1);
	setenv(UIB_ENV_UDP_WORKER_AFFINITY, "off", 1);
	ctx = uib_open_device_protocol("127.0.0.1", 19731, "dummy");
	check("transport.workers.open", ctx != NULL);
	check("transport.workers.count", ctx &&
	      !uib_transport_get_stats(ctx, &stats) &&
	      stats.rx_workers == 2 && !stats.rx_workers_pinned);
	if (ctx)
		uib_close_device(ctx);
	setenv(UIB_ENV_UDP_WORKER_AFFINITY, "invalid", 1);
	errno = 0;
	ctx = uib_open_device_protocol("127.0.0.1", 19731, "dummy");
	check("transport.workers.affinity", !ctx && errno == EINVAL);
	if (ctx)
		uib_close_device(ctx);
	setenv(UIB_ENV_UDP_WORKER_AFFINITY, "off", 1);
	setenv(UIB_ENV_UDP_WORKERS, "0", 1);
	errno = 0;
	ctx = uib_open_device_protocol("127.0.0.1", 19731, "dummy");
	check("transport.workers.count-invalid", !ctx && errno == EINVAL);
	if (ctx)
		uib_close_device(ctx);
	unsetenv(UIB_ENV_UDP_WORKERS);
	unsetenv(UIB_ENV_UDP_WORKER_AFFINITY);
#ifdef UIB_HAVE_AFXDP
	setenv("UIB_TRANSPORT_MODE", "udp-zc-afxdp", 1);
	setenv("UIB_AFXDP_IFACE", "lo", 1);
	unsetenv("UIB_AFXDP_PEER_MAC");
	ctx = uib_open_device("127.0.0.1", 19730);
	check("transport.afxdp.mac-required", !ctx);
	if (ctx)
		uib_close_device(ctx);
	setenv("UIB_AFXDP_PEER_MAC", "not-a-mac", 1);
	ctx = uib_open_device("127.0.0.1", 19730);
	check("transport.afxdp.mac-invalid", !ctx);
	if (ctx)
		uib_close_device(ctx);
	unsetenv("UIB_AFXDP_IFACE");
	unsetenv("UIB_AFXDP_PEER_MAC");
#endif
	unsetenv("UIB_TRANSPORT_MODE");
	unsetenv(UIB_ENV_ZEROCOPY);
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
