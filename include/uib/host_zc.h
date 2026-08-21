/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_HOST_ZC_H
#define UIB_HOST_ZC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

struct uib_qp;
struct uib_wc;
struct uib_context;

enum uib_rx_loan_flags {
	UIB_RX_LOAN_COPIED	= 1U << 0,
	UIB_RX_LOAN_DIRECT	= 1U << 1,
};

enum uib_host_zc_capability {
	UIB_HOST_ZC_PROVIDER_LOANS	= 1U << 0,
	UIB_HOST_ZC_KERNEL_ZCRX		= 1U << 1,
	UIB_HOST_ZC_FRAMED_DIRECT	= 1U << 2,
};

struct uib_rx_loan {
	void     *data;
	size_t    length;
	size_t    capacity;
	uint64_t  wr_id;
	uint32_t  flags;
	void     *private_data;
	const struct iovec *segments;
	size_t    segment_count;
};

int uib_post_recv_loan(struct uib_qp *qp, uint64_t wr_id, size_t capacity);
int uib_repost_rx_loan(struct uib_qp *qp, struct uib_rx_loan *loan,
		       uint64_t wr_id);
int uib_wc_take_rx_loan(struct uib_wc *wc, struct uib_rx_loan *loan);
void uib_release_rx_loan(struct uib_rx_loan *loan);
uint32_t uib_host_zc_capabilities(struct uib_context *ctx);

#endif
