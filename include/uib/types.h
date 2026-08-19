#ifndef UIB_TYPES_H
#define UIB_TYPES_H

#include <stdint.h>
#include <stddef.h>

enum uib_qp_type {
	UIB_QPT_RC = 0,
	UIB_QPT_UC = 1,
	UIB_QPT_UD = 2,
};

enum uib_qp_state {
	UIB_QPS_RESET = 0,
	UIB_QPS_INIT  = 1,
	UIB_QPS_RTR   = 2,
	UIB_QPS_RTS   = 3,
	UIB_QPS_SQE   = 4,
	UIB_QPS_ERR   = 5,
};

enum uib_wr_opcode {
	UIB_WR_SEND          = 0,
	UIB_WR_SEND_WITH_IMM = 1,
	UIB_WR_RDMA_WRITE    = 2,
	UIB_WR_RDMA_READ     = 3,
	UIB_WR_ATOMIC_FETCH_ADD = 4,
	UIB_WR_ATOMIC_CMP_SWAP = 5,
	UIB_WR_UET_ATOMIC = 6,
};

enum uib_send_flags {
	UIB_SEND_SIGNALED = 1 << 0,
	UIB_SEND_SOLICITED = 1 << 1,
	UIB_SEND_INLINE   = 1 << 2,
};

enum uib_wc_status {
	UIB_WC_SUCCESS         = 0,
	UIB_WC_LOC_LEN_ERR     = 1,
	UIB_WC_LOC_PROT_ERR    = 2,
	UIB_WC_WR_FLUSH_ERR    = 3,
	UIB_WC_REM_ACCESS_ERR  = 4,
	UIB_WC_REM_OP_ERR      = 5,
	UIB_WC_RETRY_EXC_ERR   = 6,
	UIB_WC_RESP_TIMEOUT_ERR = 7,
	UIB_WC_GENERAL_ERR     = 8,
};

enum uib_wc_opcode {
	UIB_WC_SEND        = 0,
	UIB_WC_RDMA_WRITE  = 1,
	UIB_WC_RDMA_READ   = 2,
	UIB_WC_FETCH_ADD   = 3,
	UIB_WC_COMP_SWAP   = 4,
	UIB_WC_UET_ATOMIC  = 5,
	UIB_WC_RECV        = 128,
	UIB_WC_RECV_RDMA_WITH_IMM = 129,
};

enum uib_access_flags {
	UIB_ACCESS_LOCAL_WRITE   = 1 << 0,
	UIB_ACCESS_REMOTE_WRITE  = 1 << 1,
	UIB_ACCESS_REMOTE_READ   = 1 << 2,
	UIB_ACCESS_REMOTE_ATOMIC = 1 << 3,
	UIB_ACCESS_MW_BIND       = 1 << 4,
};

#endif
