/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "uib/host_zc.h"
#include "uib/qp.h"
#include "uib/cq.h"
#include "uib/mr.h"
#include "uib/types.h"
#include "uib/transport.h"
#include "uib/context.h"
#include "host_zc_internal.h"
#include "qp_internal.h"

struct uib_host_loan {
	uib_transport_release_fn *segment_release;
	struct iovec             *segments;
	void                    **segment_opaque;
	struct uib_rx_loan public;
	struct uib_mr     *mr;
	struct uib_sge     sge;
	void              *fallback_data;
	size_t             fallback_capacity;
	size_t             direct_length;
	uint32_t           segment_capacity;
	uint32_t           segment_count;
	int                copied;
};

#define UIB_HOST_LOAN_MIN_FRAGMENT 256U
#define UIB_HOST_LOAN_MIN_SEGMENTS 4U
#define UIB_HOST_LOAN_MAX_SEGMENTS 1024U

static void host_loan_release_segments(struct uib_host_loan *host)
{
	uint32_t index;

	for (index = 0; index < host->segment_count; index++) {
		if (host->segment_release[index])
			host->segment_release[index](host->segment_opaque[index]);
	}
	host->segment_count = 0;
	host->direct_length = 0;
}

static void host_loan_release(void *private_data)
{
	struct uib_host_loan *host = private_data;

	if (!host)
		return;
	host_loan_release_segments(host);
	if (host->mr)
		uib_dereg_mr(host->mr);
	free(host->fallback_data);
	free(host->segment_opaque);
	free(host->segment_release);
	free(host->segments);
	free(host);
}

static void host_loan_copy_direct(struct uib_host_loan *host)
{
	size_t offset;
	uint32_t index;

	offset = 0;
	for (index = 0; index < host->segment_count; index++) {
		memcpy((uint8_t *)host->fallback_data + offset,
		       host->segments[index].iov_base,
		       host->segments[index].iov_len);
		offset += host->segments[index].iov_len;
	}
	host_loan_release_segments(host);
}

int uib_host_loan_place(struct uib_rq_slot *slot, uint32_t offset,
			const void *data, size_t length)
{
	struct uib_host_loan *host;
	uib_transport_release_fn release;
	void *opaque;
	int rc;

	if (!slot || !slot->private_data)
		return -ENOENT;
	host = slot->private_data;
	if (host->public.private_data != host ||
	    offset > host->fallback_capacity ||
	    length > host->fallback_capacity - offset)
		return -EINVAL;
	if (!host->copied && offset == host->direct_length &&
	    host->segment_count < host->segment_capacity) {
		rc = uib_transport_claim_direct(data, length, &release, &opaque);
		if (!rc) {
			host->segments[host->segment_count].iov_base =
				(void *)data;
			host->segments[host->segment_count].iov_len = length;
			host->segment_release[host->segment_count] = release;
			host->segment_opaque[host->segment_count] = opaque;
			host->segment_count++;
			host->direct_length += length;
			host->public.flags = UIB_RX_LOAN_DIRECT;
			return 0;
		}
	}
	if (host->segment_count)
		host_loan_copy_direct(host);
	host->copied = 1;
	host->public.flags = UIB_RX_LOAN_COPIED;
	return uib_mr_place(host->mr,
			    (uint64_t)(uintptr_t)host->fallback_data + offset,
			    data, length);
}

/**
 * uib_post_recv_loan - Post a provider-owned host-memory receive.
 * @qp: Queue pair
 * @wr_id: Caller-defined work request identifier
 * @capacity: Minimum receive-buffer capacity in bytes
 * @uib-api: Provider-owned host-memory receive
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_post_recv_loan(struct uib_qp *qp, uint64_t wr_id, size_t capacity)
{
	struct uib_host_loan *host;
	struct uib_recv_wr receive;
	long page_size;
	int rc;

	if (!qp || !capacity || capacity > UINT32_MAX)
		return -EINVAL;
	host = calloc(1, sizeof(*host));
	if (!host)
		return -ENOMEM;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		page_size = 4096;
	rc = posix_memalign(&host->public.data, (size_t)page_size, capacity);
	if (rc) {
		free(host);
		return -rc;
	}
	host->mr = uib_reg_mr(qp->pd, host->public.data, capacity,
			      UIB_ACCESS_LOCAL_WRITE);
	if (!host->mr) {
		rc = -errno;
		free(host->public.data);
		free(host);
		return rc;
	}
	host->public.capacity = capacity;
	host->public.wr_id = wr_id;
	host->public.flags = UIB_RX_LOAN_COPIED;
	host->public.private_data = host;
	host->fallback_data = host->public.data;
	host->fallback_capacity = capacity;
	host->segment_capacity = (uint32_t)
		((capacity + UIB_HOST_LOAN_MIN_FRAGMENT - 1) /
		 UIB_HOST_LOAN_MIN_FRAGMENT);
	if (!host->segment_capacity)
		host->segment_capacity = 1;
	if (host->segment_capacity < UIB_HOST_LOAN_MIN_SEGMENTS)
		host->segment_capacity = UIB_HOST_LOAN_MIN_SEGMENTS;
	if (host->segment_capacity > UIB_HOST_LOAN_MAX_SEGMENTS)
		host->segment_capacity = UIB_HOST_LOAN_MAX_SEGMENTS;
	host->segments = calloc(host->segment_capacity, sizeof(*host->segments));
	host->segment_release = calloc(host->segment_capacity,
				       sizeof(*host->segment_release));
	host->segment_opaque = calloc(host->segment_capacity,
				      sizeof(*host->segment_opaque));
	if (!host->segments || !host->segment_release ||
	    !host->segment_opaque) {
		host_loan_release(host);
		return -ENOMEM;
	}
	host->sge.addr = (uint64_t)(uintptr_t)host->public.data;
	host->sge.length = (uint32_t)capacity;
	host->sge.lkey = host->mr->lkey;
	memset(&receive, 0, sizeof(receive));
	receive.wr_id = wr_id;
	receive.sg_list = &host->sge;
	receive.num_sge = 1;
	rc = uib_post_recv_owned(qp, &receive, host, host_loan_release);
	if (rc < 0)
		host_loan_release(host);
	return rc;
}

/**
 * uib_repost_rx_loan - Repost a released completion without reallocating it.
 * @qp: Queue pair
 * @loan: Released receive-buffer loan to post again
 * @wr_id: Caller-defined work request identifier
 * @uib-api: Provider-owned host-memory receive
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_repost_rx_loan(struct uib_qp *qp, struct uib_rx_loan *loan,
		       uint64_t wr_id)
{
	struct uib_host_loan *host;
	struct uib_recv_wr receive;
	int rc;

	if (!qp || !loan || !loan->private_data)
		return -EINVAL;
	host = loan->private_data;
	if (host->public.private_data != host || !host->mr ||
	    host->mr->pd != qp->pd)
		return -EINVAL;
	host_loan_release_segments(host);
	host->public.data = host->fallback_data;
	host->public.length = 0;
	host->public.capacity = host->fallback_capacity;
	host->public.wr_id = wr_id;
	host->public.flags = UIB_RX_LOAN_COPIED;
	host->public.segments = NULL;
	host->public.segment_count = 0;
	host->copied = 0;
	memset(&receive, 0, sizeof(receive));
	receive.wr_id = wr_id;
	receive.sg_list = &host->sge;
	receive.num_sge = 1;
	rc = uib_post_recv_owned(qp, &receive, host, host_loan_release);
	if (!rc)
		memset(loan, 0, sizeof(*loan));
	return rc;
}

/**
 * uib_wc_take_rx_loan - Take ownership of a completed receive loan.
 * @wc: Work completion to consume or fill
 * @loan: Output for the loan detached from the completion
 * @uib-api: Provider-owned host-memory receive
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_wc_take_rx_loan(struct uib_wc *wc, struct uib_rx_loan *loan)
{
	struct uib_host_loan *host;

	if (!wc || !loan || !wc->private_data || wc->opcode != UIB_WC_RECV)
		return -EINVAL;
	host = wc->private_data;
	host->public.length = wc->byte_len;
	if (host->segment_count && !host->copied) {
		host->public.segments = host->segments;
		host->public.segment_count = host->segment_count;
		host->public.data = host->segment_count == 1 ?
			host->segments[0].iov_base : NULL;
	} else {
		host->public.segments = NULL;
		host->public.segment_count = 0;
		host->public.data = host->fallback_data;
	}
	*loan = host->public;
	wc->private_data = NULL;
	return 0;
}

/**
 * uib_release_rx_loan - Return a receive loan to its provider.
 * @loan: Receive-buffer loan
 * @uib-api: Provider-owned host-memory receive
 */
void uib_release_rx_loan(struct uib_rx_loan *loan)
{
	if (!loan || !loan->private_data)
		return;
	host_loan_release(loan->private_data);
	memset(loan, 0, sizeof(*loan));
}

/**
 * uib_host_zc_capabilities - Query provider-owned receive capabilities.
 * @ctx: UIB device context
 * @uib-api: Provider-owned host-memory receive
 *
 * Return: A bitmask of enum uib_host_zc_capability values, or 0 for an invalid
 * context.
 */
uint32_t uib_host_zc_capabilities(struct uib_context *ctx)
{
	uint32_t capabilities;

	if (!ctx)
		return 0;
	capabilities = UIB_HOST_ZC_PROVIDER_LOANS;
	if (ctx->tops && ctx->tops->host_zc_capabilities)
		capabilities |= ctx->tops->host_zc_capabilities(ctx);
	return capabilities;
}
