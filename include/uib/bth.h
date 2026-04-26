#ifndef UIB_BTH_H
#define UIB_BTH_H

#include <stdint.h>
#include <stddef.h>

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

#define UIB_ROCE_V2_UDP_DPORT 4791
#define UIB_DEFAULT_PKEY      0xffff

#define UIB_OP_SEND_FIRST                0x00
#define UIB_OP_SEND_MIDDLE               0x01
#define UIB_OP_SEND_LAST                 0x02
#define UIB_OP_SEND_LAST_WITH_IMM        0x03
#define UIB_OP_SEND_ONLY                 0x04
#define UIB_OP_SEND_ONLY_WITH_IMM        0x05
#define UIB_OP_RDMA_WRITE_FIRST          0x06
#define UIB_OP_RDMA_WRITE_MIDDLE         0x07
#define UIB_OP_RDMA_WRITE_LAST           0x08
#define UIB_OP_RDMA_WRITE_LAST_WITH_IMM  0x09
#define UIB_OP_RDMA_WRITE_ONLY           0x0a
#define UIB_OP_RDMA_WRITE_ONLY_WITH_IMM  0x0b
#define UIB_OP_RDMA_READ_REQ             0x0c
#define UIB_OP_RDMA_READ_RESP_FIRST      0x0d
#define UIB_OP_RDMA_READ_RESP_MIDDLE     0x0e
#define UIB_OP_RDMA_READ_RESP_LAST       0x0f
#define UIB_OP_RDMA_READ_RESP_ONLY       0x10
#define UIB_OP_ACKNOWLEDGE               0x11

#define UIB_PMTU 4096

#define UIB_BTH_ACK_REQ (1u << 31)
#define UIB_BTH_PAD_MASK 0x30
#define UIB_BTH_PAD_SHIFT 4

struct uib_bth {
	uint8_t  opcode;
	uint8_t  flags;
	uint16_t pkey;
	uint32_t dqpn;
	uint32_t apsn;
} __packed;

struct uib_reth {
	uint64_t va;
	uint32_t rkey;
	uint32_t length;
} __packed;

struct uib_aeth {
	uint32_t syndrome_msn;
} __packed;

struct uib_deth {
	uint32_t qkey;
	uint32_t sqp;
} __packed;

#define UIB_BTH_BYTES  12
#define UIB_RETH_BYTES 16
#define UIB_AETH_BYTES 4
#define UIB_DETH_BYTES 8
#define UIB_ICRC_BYTES 4

#define UIB_AETH_ACK         0x1f
#define UIB_AETH_RNR         0x20
#define UIB_AETH_CODE_MASK   0xe0
#define UIB_AETH_ACK_CODE    0x00
#define UIB_AETH_RNR_CODE    0x20
#define UIB_AETH_NAK_PSN_SEQ 0x60
#define UIB_AETH_NAK_INV_REQ 0x61
#define UIB_AETH_NAK_REM_ACC 0x62
#define UIB_AETH_NAK_REM_OP  0x63

void uib_bth_pack(struct uib_bth *bth, uint8_t opcode, uint16_t pkey,
		  uint32_t dqpn, uint32_t psn, int ack_request);
void uib_bth_unpack(const struct uib_bth *bth, uint8_t *opcode,
		    uint32_t *dqpn, uint32_t *psn, int *ack_request);
void uib_bth_set_pad(struct uib_bth *bth, uint8_t pad);
uint8_t uib_bth_pad(const struct uib_bth *bth);

void uib_reth_pack(struct uib_reth *r, uint64_t va, uint32_t rkey, uint32_t len);
void uib_reth_unpack(const struct uib_reth *r, uint64_t *va, uint32_t *rkey, uint32_t *len);

void uib_deth_pack(struct uib_deth *d, uint32_t qkey, uint32_t sqp);
void uib_deth_unpack(const struct uib_deth *d, uint32_t *qkey, uint32_t *sqp);

void uib_aeth_pack(struct uib_aeth *a, uint8_t syndrome, uint32_t msn);
void uib_aeth_unpack(const struct uib_aeth *a, uint8_t *syndrome, uint32_t *msn);

uint32_t uib_crc32(uint32_t crc, const void *buf, size_t len);
uint32_t uib_icrc(const void *packet, size_t len);

uint32_t uib_rocev2_icrc_ipv4(uint32_t src_ip, uint32_t dst_ip,
			      uint16_t src_port, uint16_t dst_port,
			      uint16_t ip_id, const void *rdma,
			      size_t rdma_len);

#endif
