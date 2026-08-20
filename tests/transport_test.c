// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>

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
	unsetenv("UIB_TRANSPORT_MODE");
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
