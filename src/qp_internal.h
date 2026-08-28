/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_QP_INTERNAL_H
#define UIB_QP_INTERNAL_H

#include <stdint.h>

#include "uib/qp.h"

void qp_emit_send_vendor(struct uib_qp *qp, uint64_t wr_id,
			 enum uib_wr_opcode wr_op, int signaled,
			 enum uib_wc_status status, uint32_t byte_len,
			 uint32_t vendor_err);

#endif
