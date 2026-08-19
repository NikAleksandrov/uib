#ifndef UIB_UET_H
#define UIB_UET_H

#include <stddef.h>
#include <stdint.h>

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

#define UIB_UET_DEFAULT_PORT 5432
#define UIB_UET_PDC_MPR 128
#define UIB_UET_RTX_NS (200ULL * 1000 * 1000)
#define UIB_UET_MAX_RETRIES 7

enum uib_uet_pds_type {
	UIB_UET_PDS_TYPE_RSVD0,
	UIB_UET_PDS_TYPE_ENC_HDR,
	UIB_UET_PDS_TYPE_RUD_REQ,
	UIB_UET_PDS_TYPE_ROD_REQ,
	UIB_UET_PDS_TYPE_RUDI_REQ,
	UIB_UET_PDS_TYPE_RUDI_RESPONSE,
	UIB_UET_PDS_TYPE_UUD_REQ,
	UIB_UET_PDS_TYPE_ACK,
	UIB_UET_PDS_TYPE_ACK_CC,
	UIB_UET_PDS_TYPE_ACK_CCX,
	UIB_UET_PDS_TYPE_NACK,
	UIB_UET_PDS_TYPE_CTRL_MSG,
};

enum uib_uet_next_header {
	UIB_UET_NEXT_NONE = 0x00,
	UIB_UET_NEXT_REQ_SMALL = 0x01,
	UIB_UET_NEXT_REQ_MEDIUM = 0x02,
	UIB_UET_NEXT_REQ_STD = 0x03,
	UIB_UET_NEXT_RSP = 0x04,
	UIB_UET_NEXT_RSP_DATA = 0x05,
	UIB_UET_NEXT_RSP_DATA_SMALL = 0x06,
	UIB_UET_NEXT_PDS = 0x0f,
};

#define UIB_UET_PROLOGUE_FLAGS_MASK 0x7f
#define UIB_UET_PROLOGUE_NEXT_MASK 0x0f
#define UIB_UET_PROLOGUE_NEXT_SHIFT 7
#define UIB_UET_PROLOGUE_TYPE_MASK 0x1f
#define UIB_UET_PROLOGUE_TYPE_SHIFT 11

#define UIB_UET_REQ_FLAG_SYN  (1U << 2)
#define UIB_UET_REQ_FLAG_AR   (1U << 3)
#define UIB_UET_REQ_FLAG_RETX (1U << 4)
#define UIB_UET_ACK_FLAG_RETX (1U << 4)
#define UIB_UET_ACK_FLAG_M    (1U << 5)

#define UIB_UET_NACK_TRIMMED          0x01
#define UIB_UET_NACK_TRIMMED_LASTHOP  0x02

enum uib_uet_ses_opcode {
	UIB_UET_SES_WRITE = 0x01,
	UIB_UET_SES_READ = 0x02,
	UIB_UET_SES_SEND = 0x05,
};

#define UIB_UET_SES_FLAG_SOM (1U << 0)
#define UIB_UET_SES_FLAG_EOM (1U << 1)
#define UIB_UET_SES_JOB_ID_MASK 0x00ffffff
#define UIB_UET_SES_PID_MASK 0x0fff

enum uib_uet_ses_response_code {
	UIB_UET_SES_RC_OK = 0x01,
	UIB_UET_SES_RC_UNSUPP_OP = 0x06,
	UIB_UET_SES_RC_BAD_MKEY = 0x1c,
	UIB_UET_SES_RC_BAD_ADDR = 0x1d,
};

enum uib_uet_ses_response_opcode {
	UIB_UET_SES_RSP_RESPONSE = 0x01,
	UIB_UET_SES_RSP_WITH_DATA = 0x02,
};

#define UIB_UET_SES_RSP_RC_MASK 0x3f
#define UIB_UET_SES_RSP_OPCODE_SHIFT 8
#define UIB_UET_SES_RSP_OPCODE_MASK 0x3f

struct uib_uet_prologue {
	uint16_t type_next_flags;
} __packed;

struct uib_uet_pds_req {
	struct uib_uet_prologue prologue;
	uint16_t clear_psn_offset;
	uint32_t psn;
	uint16_t spdcid;
	uint16_t dpdcid;
} __packed;

struct uib_uet_pds_ack {
	struct uib_uet_prologue prologue;
	uint16_t ack_psn_offset;
	uint32_t cack_psn;
	uint16_t spdcid;
	uint16_t dpdcid;
} __packed;

struct uib_uet_pds_ack_ext {
	uint16_t cc_type_flags_mpr;
	uint16_t sack_psn_offset;
	uint64_t sack_bitmap;
	uint64_t ack_cc_state;
} __packed;

struct uib_uet_nscc_wire_state {
	uint16_t service_time;
	uint32_t receiver_state;
	uint16_t out_of_order_count;
} __packed;

#define UIB_UET_NSCC_RESTORE_MASK 0x80000000U
#define UIB_UET_NSCC_PENALTY_SHIFT 24
#define UIB_UET_NSCC_PENALTY_MASK 0x7fU
#define UIB_UET_NSCC_RCVD_MASK 0x00ffffffU

struct uib_uet_pds_nack {
	struct uib_uet_prologue prologue;
	uint8_t nack_code;
	uint8_t vendor_code;
	uint32_t nack_psn;
	uint16_t spdcid;
	uint16_t dpdcid;
	uint32_t payload;
} __packed;

struct uib_uet_ses_req {
	uint8_t opcode;
	uint8_t flags;
	uint16_t msg_id;
	uint32_t index_gen_job_id;
	uint16_t pid_on_fep;
	uint16_t index;
	uint64_t buffer_offset;
	uint32_t initiator;
	uint64_t match_bits;
	uint64_t header_data;
	uint32_t request_len;
} __packed;

struct uib_uet_ses_rsp {
	uint16_t list_opcode_version_rc;
	uint16_t msg_id;
	uint32_t index_gen_job_id;
	uint32_t modified_len;
} __packed;

uint16_t uib_uet_prologue_pack(uint8_t type, uint8_t next, uint8_t flags);
uint8_t uib_uet_prologue_type(const struct uib_uet_prologue *prologue);
uint8_t uib_uet_prologue_next(const struct uib_uet_prologue *prologue);
uint8_t uib_uet_prologue_flags(const struct uib_uet_prologue *prologue);
int uib_uet_build_ack(void *packet, size_t capacity, uint32_t cack_psn,
		      uint32_t ack_psn, uint16_t spdcid, uint16_t dpdcid,
		      uint8_t next_header);
void uib_uet_nscc_wire_pack(struct uib_uet_nscc_wire_state *state,
			    uint32_t service_time_ns, uint8_t restore,
			    uint8_t penalty, uint32_t received_bytes,
			    uint16_t out_of_order);
void uib_uet_nscc_wire_unpack(const struct uib_uet_nscc_wire_state *state,
			      uint32_t *service_time_ns, uint8_t *restore,
			      uint8_t *penalty, uint32_t *received_units,
			      uint16_t *out_of_order);

#endif
