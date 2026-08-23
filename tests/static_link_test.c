/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>

#include "uib/context.h"

int main(void)
{
	struct uib_context *context;

	if (setenv("UIB_TRANSPORT_MODE", "udp-sync", 1) < 0) {
		perror("setenv");
		return 1;
	}

	context = uib_open_device_protocol("127.0.0.1", 0, "dummy");
	if (!context) {
		perror("uib_open_device_protocol");
		return 1;
	}
	if (uib_close_device(context) < 0) {
		fprintf(stderr, "uib_close_device failed\n");
		return 1;
	}

	puts("standalone static libuib: PASS");
	return 0;
}
