/* SPDX-License-Identifier: Apache-2.0 */
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
#define UIB_UET_MAX_PATHS 256
#define UIB_UET_DEFAULT_ENTROPY_BASE 0xc000
#define UIB_UET_DEFAULT_FAILURE_HOLD_NS (1000ULL * 1000 * 1000)

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
	UIB_UET_PDS_TYPE_NACK_CCX,
	UIB_UET_PDS_TYPE_RUD_CC_REQ,
	UIB_UET_PDS_TYPE_ROD_CC_REQ,
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
#define UIB_UET_ACK_FLAG_REQ_CLEAR (1U << 1)

#define UIB_UET_NACK_TRIMMED          0x01
#define UIB_UET_NACK_TRIMMED_LASTHOP  0x02

#define UIB_UET_CC_TYPE_SHIFT 12
#define UIB_UET_CC_TYPE_MASK  0x0f
#define UIB_UET_CC_TYPE_NSCC  0x00
#define UIB_UET_CC_TYPE_CREDIT 0x01
#define UIB_UET_RCCC_COUNTER_MASK 0x00ffffffU

enum uib_uet_control_type {
	UIB_UET_CONTROL_NOOP,
	UIB_UET_CONTROL_ACK_REQUEST,
	UIB_UET_CONTROL_CLEAR_COMMAND,
	UIB_UET_CONTROL_CLEAR_REQUEST,
	UIB_UET_CONTROL_CLOSE_COMMAND,
	UIB_UET_CONTROL_CLOSE_REQUEST,
	UIB_UET_CONTROL_PROBE,
	UIB_UET_CONTROL_CREDIT,
	UIB_UET_CONTROL_CREDIT_REQUEST,
	UIB_UET_CONTROL_NEGOTIATION,
};

enum uib_uet_ses_opcode {
	UIB_UET_SES_NOOP = 0x00,
	UIB_UET_SES_WRITE = 0x01,
	UIB_UET_SES_READ = 0x02,
	UIB_UET_SES_ATOMIC = 0x03,
	UIB_UET_SES_FETCHING_ATOMIC = 0x04,
	UIB_UET_SES_SEND = 0x05,
	UIB_UET_SES_RENDEZVOUS_SEND = 0x06,
	UIB_UET_SES_DATAGRAM_SEND = 0x07,
	UIB_UET_SES_DEFERRABLE_SEND = 0x08,
	UIB_UET_SES_TAGGED_SEND = 0x09,
	UIB_UET_SES_RENDEZVOUS_TSEND = 0x0a,
	UIB_UET_SES_DEFERRABLE_TSEND = 0x0b,
	UIB_UET_SES_DEFERRABLE_RTR = 0x0c,
	UIB_UET_SES_TSEND_ATOMIC = 0x0d,
	UIB_UET_SES_TSEND_FETCH_ATOMIC = 0x0e,
	UIB_UET_SES_MSG_ERROR = 0x0f,
};

#define UIB_UET_SES_FLAG_SOM (1U << 0)
#define UIB_UET_SES_FLAG_EOM (1U << 1)
#define UIB_UET_SES_FLAG_HD  (1U << 2)
#define UIB_UET_SES_FLAG_REL (1U << 3)
#define UIB_UET_SES_FLAG_IE  (1U << 4)
#define UIB_UET_SES_FLAG_DC  (1U << 5)
#define UIB_UET_SES_VERSION_SHIFT 6
#define UIB_UET_SES_FLAGS_MASK ((1U << UIB_UET_SES_VERSION_SHIFT) - 1)
#define UIB_UET_SES_OPCODE_MASK 0x3fU
#define UIB_UET_SES_LENGTH_MASK 0x3fffU
#define UIB_UET_SES_JOB_ID_MASK 0x00ffffff
#define UIB_UET_SES_PID_MASK 0x0fff

enum uib_uet_ses_response_code {
	UIB_UET_SES_RC_NULL = 0x00,
	UIB_UET_SES_RC_OK = 0x01,
	UIB_UET_SES_RC_BAD_GENERATION = 0x02,
	UIB_UET_SES_RC_DISABLED = 0x03,
	UIB_UET_SES_RC_DISABLED_GENERATION = 0x04,
	UIB_UET_SES_RC_NO_MATCH = 0x05,
	UIB_UET_SES_RC_UNSUPP_OP = 0x06,
	UIB_UET_SES_RC_UNSUPP_SIZE = 0x07,
	UIB_UET_SES_RC_AT_INVALID = 0x08,
	UIB_UET_SES_RC_AT_PERM = 0x09,
	UIB_UET_SES_RC_AT_ATS_ERROR = 0x0a,
	UIB_UET_SES_RC_AT_NO_TRANS = 0x0b,
	UIB_UET_SES_RC_AT_OUT_OF_RANGE = 0x0c,
	UIB_UET_SES_RC_HOST_POISONED = 0x0d,
	UIB_UET_SES_RC_HOST_UNSUCCESS_CMPL = 0x0e,
	UIB_UET_SES_RC_AMO_UNSUPP_OP = 0x0f,
	UIB_UET_SES_RC_AMO_UNSUPP_DT = 0x10,
	UIB_UET_SES_RC_AMO_UNSUPP_SIZE = 0x11,
	UIB_UET_SES_RC_AMO_UNALIGNED = 0x12,
	UIB_UET_SES_RC_AMO_FP_NAN = 0x13,
	UIB_UET_SES_RC_AMO_FP_UNDERFLOW = 0x14,
	UIB_UET_SES_RC_AMO_FP_OVERFLOW = 0x15,
	UIB_UET_SES_RC_AMO_FP_INEXACT = 0x16,
	UIB_UET_SES_RC_PERM_VIOLATION = 0x17,
	UIB_UET_SES_RC_OP_VIOLATION = 0x18,
	UIB_UET_SES_RC_BAD_INDEX = 0x19,
	UIB_UET_SES_RC_BAD_PID = 0x1a,
	UIB_UET_SES_RC_BAD_JOB_ID = 0x1b,
	UIB_UET_SES_RC_BAD_MKEY = 0x1c,
	UIB_UET_SES_RC_BAD_ADDR = 0x1d,
	UIB_UET_SES_RC_CANCELLED = 0x1e,
	UIB_UET_SES_RC_UNDELIVERABLE = 0x1f,
	UIB_UET_SES_RC_UNCOR = 0x20,
	UIB_UET_SES_RC_UNCOR_TRANSIENT = 0x21,
	UIB_UET_SES_RC_TOO_LONG = 0x22,
	UIB_UET_SES_RC_INITIATOR_ERROR = 0x23,
	UIB_UET_SES_RC_DROPPED = 0x24,
	UIB_UET_SES_RC_EXTENDED = 0x3e,
};

enum uib_uet_ses_response_list {
	UIB_UET_SES_LIST_EXPECTED = 0x00,
	UIB_UET_SES_LIST_OVERFLOW = 0x01,
};

enum uib_uet_ses_response_opcode {
	UIB_UET_SES_RSP_DEFAULT = 0x00,
	UIB_UET_SES_RSP_RESPONSE = 0x01,
	UIB_UET_SES_RSP_WITH_DATA = 0x02,
	UIB_UET_SES_RSP_NONE = 0x03,
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

struct uib_uet_pds_cc_req {
	struct uib_uet_pds_req request;
	uint32_t req_cc_state;
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

struct uib_uet_rccc_wire_state {
	uint8_t credit[3];
	uint8_t reserved[3];
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

struct uib_uet_pds_control {
	struct uib_uet_prologue prologue;
	uint16_t probe_opaque;
	uint32_t psn;
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

struct uib_uet_ses_req_small {
	uint8_t opcode;
	uint8_t flags;
	uint16_t request_len;
	uint32_t index_gen_job_id;
	uint16_t pid_on_fep;
	uint16_t index;
	uint64_t buffer_offset;
} __packed;

struct uib_uet_ses_req_medium {
	uint8_t opcode;
	uint8_t flags;
	uint16_t request_len;
	uint32_t index_gen_job_id;
	uint16_t pid_on_fep;
	uint16_t index;
	uint64_t buffer_offset;
	uint32_t initiator;
	uint64_t match_bits;
} __packed;

struct uib_uet_ses_rsp {
	uint16_t list_opcode_version_rc;
	uint16_t msg_id;
	uint32_t index_gen_job_id;
	uint32_t modified_len;
} __packed;

struct uib_uet_ses_rsp_data {
	uint16_t list_opcode_version_rc;
	uint16_t response_msg_id;
	uint32_t index_gen_job_id;
	uint16_t request_msg_id;
	uint16_t payload_len;
	uint32_t modified_len;
	uint32_t message_offset;
} __packed;

struct uib_uet_ses_rsp_data_small {
	uint16_t list_opcode_version_rc;
	uint16_t payload_len;
	uint32_t index_gen_job_id;
	uint32_t request_psn;
} __packed;

struct uib_uet_ses_rendezvous {
	uint32_t eager_len;
	uint8_t read_generation;
	uint8_t read_pid_index[3];
	uint64_t read_offset;
	uint64_t read_memory_key;
} __packed;

struct uib_uet_ses_atomic {
	uint8_t opcode;
	uint8_t datatype;
	uint8_t semantic_control;
	uint8_t reserved;
} __packed;

enum uib_uet_ses_atomic_opcode {
	UIB_UET_AMO_MIN = 0x00,
	UIB_UET_AMO_MAX = 0x01,
	UIB_UET_AMO_SUM = 0x02,
	UIB_UET_AMO_DIFF = 0x03,
	UIB_UET_AMO_PROD = 0x04,
	UIB_UET_AMO_LOR = 0x05,
	UIB_UET_AMO_LAND = 0x06,
	UIB_UET_AMO_BOR = 0x07,
	UIB_UET_AMO_BAND = 0x08,
	UIB_UET_AMO_LXOR = 0x09,
	UIB_UET_AMO_BXOR = 0x0a,
	UIB_UET_AMO_READ = 0x0b,
	UIB_UET_AMO_WRITE = 0x0c,
	UIB_UET_AMO_CSWAP = 0x0d,
	UIB_UET_AMO_CSWAP_NE = 0x0e,
	UIB_UET_AMO_CSWAP_LE = 0x0f,
	UIB_UET_AMO_CSWAP_LT = 0x10,
	UIB_UET_AMO_CSWAP_GE = 0x11,
	UIB_UET_AMO_CSWAP_GT = 0x12,
	UIB_UET_AMO_MSWAP = 0x13,
	UIB_UET_AMO_INVAL = 0x14,
};

enum uib_uet_ses_atomic_datatype {
	UIB_UET_AMO_INT8 = 0x00,
	UIB_UET_AMO_UINT8 = 0x01,
	UIB_UET_AMO_INT16 = 0x02,
	UIB_UET_AMO_UINT16 = 0x03,
	UIB_UET_AMO_INT32 = 0x04,
	UIB_UET_AMO_UINT32 = 0x05,
	UIB_UET_AMO_INT64 = 0x06,
	UIB_UET_AMO_UINT64 = 0x07,
	UIB_UET_AMO_INT128 = 0x08,
	UIB_UET_AMO_UINT128 = 0x09,
	UIB_UET_AMO_FLOAT = 0x0a,
	UIB_UET_AMO_DOUBLE = 0x0b,
	UIB_UET_AMO_FLOAT_COMPLEX = 0x0c,
	UIB_UET_AMO_DOUBLE_COMPLEX = 0x0d,
	UIB_UET_AMO_LONG_DOUBLE = 0x0e,
	UIB_UET_AMO_LONG_DOUBLE_COMPLEX = 0x0f,
	UIB_UET_AMO_BF16 = 0x10,
	UIB_UET_AMO_FP16 = 0x11,
};

#define UIB_UET_AMO_CTRL_CACHEABLE   (1U << 0)
#define UIB_UET_AMO_CTRL_CPU_COHERENT (1U << 1)

struct uib_uet_session_config {
	uint32_t job_id;
	uint32_t initiator_id;
	uint16_t pid_on_fep;
	uint16_t resource_index;
	uint8_t resource_generation;
	uint8_t relative_addressing;
};

enum uib_uet_optimized_mode {
	UIB_UET_OPTIMIZED_OFF,
	UIB_UET_OPTIMIZED_AUTO,
	UIB_UET_OPTIMIZED_SMALL,
	UIB_UET_OPTIMIZED_MEDIUM,
};

struct uib_uet_optimized_stats {
	uint64_t standard_requests;
	uint64_t small_requests;
	uint64_t medium_requests;
	uint64_t small_responses;
};

struct uib_uet_atomic_wr {
	struct uib_uet_atomic_wr *next;
	uint64_t wr_id;
	uint64_t remote_addr;
	uint32_t rkey;
	uint32_t result_lkey;
	int send_flags;
	uint8_t opcode;
	uint8_t datatype;
	uint8_t semantic_control;
	uint8_t fetching;
	const void *operand;
	const void *compare;
	const void *swap;
	void *result;
};

enum uib_uet_multipath_mode {
	UIB_UET_MULTIPATH_OFF,
	UIB_UET_MULTIPATH_OBLIVIOUS,
	UIB_UET_MULTIPATH_PATH_AWARE,
};

struct uib_uet_multipath_config {
	enum uib_uet_multipath_mode mode;
	uint16_t entropy_base;
	uint16_t path_count;
	uint64_t ecn_hold_ns;
	uint64_t failure_hold_ns;
};

struct uib_uet_multipath_stats {
	uint64_t packets;
	uint64_t path_switches;
	uint64_t ecn_avoids;
	uint64_t timeout_failovers;
	uint16_t active_entropy;
	uint16_t paths_used;
};

struct uib_qp;

int uib_uet_session_configure(struct uib_qp *qp,
			      const struct uib_uet_session_config *config);
int uib_uet_session_query(struct uib_qp *qp,
			  struct uib_uet_session_config *config);
int uib_uet_optimized_configure(struct uib_qp *qp,
				enum uib_uet_optimized_mode mode);
int uib_uet_optimized_query(struct uib_qp *qp,
			    enum uib_uet_optimized_mode *mode,
			    struct uib_uet_optimized_stats *stats);
size_t uib_uet_atomic_datatype_size(uint8_t datatype);
int uib_uet_post_atomic(struct uib_qp *qp, struct uib_uet_atomic_wr *wr,
			struct uib_uet_atomic_wr **bad);
int uib_uet_multipath_configure(struct uib_qp *qp,
				const struct uib_uet_multipath_config *config);
int uib_uet_multipath_query(struct uib_qp *qp,
			    struct uib_uet_multipath_config *config,
			    struct uib_uet_multipath_stats *stats);
uint64_t uib_uet_ses_fragment_info(uint32_t payload_len,
				   uint32_t message_offset);
uint32_t uib_uet_ses_payload_len(const struct uib_uet_ses_req *request,
				 size_t packet_payload_len);
uint32_t uib_uet_ses_message_offset(const struct uib_uet_ses_req *request);

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
uint32_t uib_uet_rccc_request_pack(uint8_t ccc_id, uint32_t credit_target);
void uib_uet_rccc_request_unpack(uint32_t state, uint8_t *ccc_id,
				 uint32_t *credit_target);
void uib_uet_rccc_wire_pack(struct uib_uet_rccc_wire_state *state,
			    uint32_t cumulative_credit,
			    uint16_t out_of_order);
void uib_uet_rccc_wire_unpack(const struct uib_uet_rccc_wire_state *state,
			      uint32_t *cumulative_credit,
			      uint16_t *out_of_order);

#endif
