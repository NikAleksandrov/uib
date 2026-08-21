/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "uib/protocol.h"
#include "uib/uet.h"

static int failed;

static void check(const char *name, int ok)
{
	printf("%-28s %s\n", name, ok ? "OK" : "FAIL");
	if (!ok)
		failed = 1;
}

int main(void)
{
	static const uint8_t expected_ack[] = {
		0x3a, 0x00,
		0xc0, 0x02,
		0xde, 0xad, 0xfe, 0xed,
		0x11, 0x22, 0x33, 0x44,
	};
	struct uib_uet_pds_ack ack;
	struct uib_uet_nscc_wire_state cc_state;
	uint32_t service, received;
	uint16_t out_of_order;
	uint8_t restore, penalty;
	struct uib_uet_ses_req request;
	int len;

	check("layout.prologue", sizeof(struct uib_uet_prologue) == 2);
	check("layout.pds_req", sizeof(struct uib_uet_pds_req) == 12);
	check("layout.pds_ack", sizeof(struct uib_uet_pds_ack) == 12);
	check("layout.ses_req", sizeof(struct uib_uet_ses_req) == 44);
	check("layout.ses_req_small",
	      sizeof(struct uib_uet_ses_req_small) == 20);
	check("layout.ses_req_medium",
	      sizeof(struct uib_uet_ses_req_medium) == 32);
	check("layout.ses_rsp", sizeof(struct uib_uet_ses_rsp) == 12);
	check("layout.ses_rsp_data",
	      sizeof(struct uib_uet_ses_rsp_data) == 20);
	check("layout.ses_rsp_data_small",
	      sizeof(struct uib_uet_ses_rsp_data_small) == 12);
	check("layout.ses_rendezvous",
	      sizeof(struct uib_uet_ses_rendezvous) == 24);
	check("layout.ses_atomic", sizeof(struct uib_uet_ses_atomic) == 4);
	check("layout.nscc", sizeof(cc_state) == 8);
	check("select.uet", uib_protocol_find("uet") == &uib_uet_protocol_ops);
	check("port.uet", uib_protocol_default_port("uet") == 5432);

	memset(&ack, 0, sizeof(ack));
	len = uib_uet_build_ack(&ack, sizeof(ack), 0xdeadfeed, 0xdeadbeef,
				0x1122, 0x3344, UIB_UET_NEXT_RSP);
	check("ack.length", len == sizeof(ack));
	check("ack.kernel-layout",
	      !memcmp(&ack, expected_ack, sizeof(expected_ack)));
	check("ack.type", uib_uet_prologue_type(&ack.prologue) ==
	      UIB_UET_PDS_TYPE_ACK);
	check("ack.next", uib_uet_prologue_next(&ack.prologue) ==
	      UIB_UET_NEXT_RSP);
	check("ack.offset", (int16_t)ntohs(ack.ack_psn_offset) == -16382);
	check("ack.cumulative", ntohl(ack.cack_psn) == 0xdeadfeed);
	check("ack.pdc_ids", ntohs(ack.spdcid) == 0x1122 &&
	      ntohs(ack.dpdcid) == 0x3344);

	uib_uet_nscc_wire_pack(&cc_state, 129, 1, 64, 65537, 17);
	uib_uet_nscc_wire_unpack(&cc_state, &service, &restore, &penalty,
				  &received, &out_of_order);
	check("nscc.service-time", service == 256);
	check("nscc.restore", restore == 1);
	check("nscc.penalty", penalty == 64);
	check("nscc.received", received == 257);
	check("nscc.out-of-order", out_of_order == 17);

	memset(&request, 0, sizeof(request));
	request.flags = UIB_UET_SES_FLAG_SOM;
	check("ses.som-payload", uib_uet_ses_payload_len(&request, 123) == 123);
	check("ses.som-offset", uib_uet_ses_message_offset(&request) == 0);
	request.flags = 0;
	request.header_data = uib_uet_ses_fragment_info(4093, 8192);
	check("ses.fragment-payload",
	      uib_uet_ses_payload_len(&request, 0) == 4093);
	check("ses.fragment-offset",
	      uib_uet_ses_message_offset(&request) == 8192);

	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
