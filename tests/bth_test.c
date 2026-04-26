#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uib/bth.h"

static int failures;

static void check(const char *name, int ok)
{
	printf("%-28s %s\n", name, ok ? "OK" : "FAIL");
	if (!ok)
		failures++;
}

int main(void)
{
	uint8_t rdma[UIB_BTH_BYTES + 64] = {0};
	struct uib_bth *bth = (struct uib_bth *)rdma;
	uint32_t src, dst, crc;

	uib_bth_pack(bth, UIB_OP_SEND_ONLY, UIB_DEFAULT_PKEY, 1, 0, 1);
	memcpy(rdma + UIB_BTH_BYTES, "RXE-to-UIB", 10);
	inet_pton(AF_INET, "192.0.2.1", &src);
	inet_pton(AF_INET, "192.0.2.2", &dst);
	crc = uib_rocev2_icrc_ipv4(src, dst, htons(49169), htons(4791),
				      1, rdma, sizeof(rdma));
	check("rocev2.icrc.known-vector", crc == 0x33e5bba5u);

	uib_bth_set_pad(bth, 3);
	check("bth.pad.set", uib_bth_pad(bth) == 3);
	uib_bth_set_pad(bth, 1);
	check("bth.pad.replace", uib_bth_pad(bth) == 1);
	check("bth.pad.preserves-opcode", bth->opcode == UIB_OP_SEND_ONLY);

	printf("RESULT %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
