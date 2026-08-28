/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>

#include "uib/verbs.h"
#include "uib/env.h"
#include "uib/protocol.h"
#include "uib/transport.h"

/**
 * uibv_get_device_list - Enumerate UIB devices for local IPv4 interfaces.
 * @num: Output for the number of returned devices
 * @uib-api: Native verbs compatibility API
 *
 * Return: A NULL-terminated device array on success, or NULL on failure.
 */
struct uibv_device **uibv_get_device_list(int *num)
{
	struct ifaddrs *ifa, *cur;
	struct uibv_device **list;
	struct uibv_device *device;
	uint16_t protocol_port =
		uib_protocol_default_port(getenv(UIB_ENV_PROTOCOL));
	int count = 0;
	int i = 0;

	if (getifaddrs(&ifa) != 0)
		return NULL;

	for (cur = ifa; cur; cur = cur->ifa_next) {
		if (cur->ifa_addr && cur->ifa_addr->sa_family == AF_INET &&
		    !(cur->ifa_flags & IFF_LOOPBACK))
			count++;
	}
	if (count == 0)
		count = 1;

	list = calloc((size_t)count + 1, sizeof(*list));

	if (!list) {
		freeifaddrs(ifa);
		return NULL;
	}

	for (cur = ifa; cur; cur = cur->ifa_next) {
		if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET)
			continue;
		if (cur->ifa_flags & IFF_LOOPBACK)
			continue;
		device = calloc(1, sizeof(*device));

		if (!device)
			break;
		snprintf(device->name, sizeof(device->name), "uib_%s",
			 cur->ifa_name);
		device->bind_addr = *(struct sockaddr_in *)cur->ifa_addr;
		device->bind_addr.sin_port = htons(protocol_port);
		list[i++] = device;
	}
	if (i == 0) {
		device = calloc(1, sizeof(*device));

		if (device) {
			snprintf(device->name, sizeof(device->name), "uib_lo");
			device->bind_addr.sin_family = AF_INET;
			device->bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			device->bind_addr.sin_port = htons(protocol_port);
			list[i++] = device;
		}
	}
	list[i] = NULL;
	freeifaddrs(ifa);
	if (num)
		*num = i;
	return list;
}

/**
 * uibv_free_device_list - Release a UIB device enumeration result.
 * @list: Device list returned by uibv_get_device_list()
 * @uib-api: Native verbs compatibility API
 */
void uibv_free_device_list(struct uibv_device **list)
{
	int i;

	if (!list)
		return;
	for (i = 0; list[i]; i++)
		free(list[i]);
	free(list);
}

/**
 * uibv_get_device_name - Return an enumerated UIB device's name.
 * @dev: UIB device
 * @uib-api: Native verbs compatibility API
 *
 * Return: The device name, or NULL if @dev is NULL.
 */
const char *uibv_get_device_name(struct uibv_device *dev)
{
	return dev ? dev->name : NULL;
}

/**
 * uibv_open_device - Open an enumerated UIB device.
 * @dev: UIB device
 * @uib-api: Native verbs compatibility API
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_context *uibv_open_device(struct uibv_device *dev)
{
	char ip[INET_ADDRSTRLEN];
	uint16_t port;

	if (!dev) {
		errno = EINVAL;
		return NULL;
	}
	inet_ntop(AF_INET, &dev->bind_addr.sin_addr, ip, sizeof(ip));
	port = ntohs(dev->bind_addr.sin_port);

	return uib_open_device(ip, port);
}

/**
 * uibv_close_device - Close a UIB device context.
 * @ctx: UIB device context
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_close_device(struct uib_context *ctx)
{
	return uib_close_device(ctx);
}

/**
 * uibv_alloc_pd - Allocate a protection domain through the verbs facade.
 * @ctx: UIB device context
 * @uib-api: Native verbs compatibility API
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_pd *uibv_alloc_pd(struct uib_context *ctx)
{
	return uib_alloc_pd(ctx);
}

/**
 * uibv_dealloc_pd - Release a verbs-facade protection domain.
 * @pd: Protection domain
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_dealloc_pd(struct uib_pd *pd)
{
	return uib_dealloc_pd(pd);
}

/**
 * uibv_reg_mr - Register process memory through the verbs facade.
 * @pd: Protection domain
 * @addr: Start address of the memory range
 * @len: Length of the byte range
 * @access: Access permissions required or granted
 * @uib-api: Native verbs compatibility API
 *
 * Return: A registered memory region on success, or NULL on failure.
 */
struct uib_mr *uibv_reg_mr(struct uib_pd *pd, void *addr, size_t len,
			   int access)
{
	return uib_reg_mr(pd, addr, len, access);
}

/**
 * uibv_reg_dmabuf_mr - Register a DMA-BUF through the verbs facade.
 * @pd: Protection domain
 * @offset: Byte offset into the DMA-BUF
 * @len: Length of the byte range
 * @iova: I/O virtual address assigned to the memory region
 * @fd: DMA-BUF file descriptor
 * @access: Access permissions required or granted
 * @uib-api: Native verbs compatibility API
 *
 * Return: A registered memory region on success, or NULL on failure.
 */
struct uib_mr *uibv_reg_dmabuf_mr(struct uib_pd *pd, uint64_t offset,
				  size_t len, uint64_t iova, int fd,
				  int access)
{
	return uib_reg_mr_dmabuf_iova(pd, fd, (size_t)offset, len, iova,
				       access, UIB_DMABUF_ALLOW_MMAP |
				       UIB_DMABUF_ALLOW_DEVICE);
}
/**
 * uibv_dereg_mr - Deregister a verbs-facade memory region.
 * @mr: Memory region
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_dereg_mr(struct uib_mr *mr)
{
	return uib_dereg_mr(mr);
}

/**
 * uibv_create_cq - Create a verbs-facade completion queue.
 * @ctx: UIB device context
 * @cqe: Requested completion queue capacity
 * @uib-api: Native verbs compatibility API
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_cq *uibv_create_cq(struct uib_context *ctx, int cqe)
{
	return uib_create_cq(ctx, cqe);
}
/**
 * uibv_create_comp_channel - Create a verbs-facade completion channel.
 * @ctx: UIB device context
 * @uib-api: Native verbs compatibility API
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_comp_channel *uibv_create_comp_channel(struct uib_context *ctx)
{
	return uib_create_comp_channel(ctx);
}
/**
 * uibv_destroy_comp_channel - Release a verbs-facade completion channel.
 * @channel: Completion-event channel
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_destroy_comp_channel(struct uib_comp_channel *channel)
{
	return uib_destroy_comp_channel(channel);
}
/**
 * uibv_get_cq_event - Consume a verbs-facade completion event.
 * @channel: Completion-event channel
 * @cq: Output for the completion queue that raised the event
 * @event_context: Output for the completion queue's caller context
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_get_cq_event(struct uib_comp_channel *channel, struct uib_cq **cq,
			      void **event_context)
{
	return uib_get_cq_event(channel, cq, event_context);
}
/**
 * uibv_ack_cq_events - Acknowledge CQ events.
 * @cq: Completion queue
 * @events: Number of completion events to acknowledge
 * @uib-api: Native verbs compatibility API
 */
void uibv_ack_cq_events(struct uib_cq *cq, unsigned int events)
{
	uib_ack_cq_events(cq, events);
}
/**
 * uibv_req_notify_cq - Arm one-shot verbs-facade CQ notification.
 * @cq: Completion queue
 * @solicited_only: Nonzero to notify only for solicited completions
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_req_notify_cq(struct uib_cq *cq, int solicited_only)
{
	return uib_req_notify_cq(cq, solicited_only);
}
/**
 * uibv_destroy_cq - Release a verbs-facade completion queue.
 * @cq: Completion queue
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_destroy_cq(struct uib_cq *cq)
{
	return uib_destroy_cq(cq);
}
/**
 * uibv_poll_cq - Poll verbs-facade work completions.
 * @cq: Completion queue
 * @n: Maximum number of completions to return
 * @wc: Output array for completed work requests
 * @uib-api: Native verbs compatibility API
 *
 * Return: The number of completions written to @wc, or a negative error code.
 */
int uibv_poll_cq(struct uib_cq *cq, int n, struct uib_wc *wc)
{
	return uib_poll_cq(cq, n, wc);
}

/**
 * uibv_create_srq - Create a verbs-facade shared receive queue.
 * @pd: Protection domain
 * @attr: Shared receive queue capacity and scatter/gather limits
 * @uib-api: Native verbs compatibility API
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_srq *uibv_create_srq(struct uib_pd *pd,
				struct uib_srq_init_attr *attr)
{
	return uib_create_srq(pd, attr);
}

/**
 * uibv_modify_srq - Change verbs-facade shared receive queue attributes.
 * @srq: Shared receive queue
 * @attr: New attribute values
 * @attr_mask: Bitmask selecting attributes to change
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_modify_srq(struct uib_srq *srq, struct uib_srq_attr *attr,
		    int attr_mask)
{
	return uib_modify_srq(srq, attr, attr_mask);
}

/**
 * uibv_query_srq - Read verbs-facade shared receive queue attributes.
 * @srq: Shared receive queue
 * @attr: Output attribute structure
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_query_srq(struct uib_srq *srq, struct uib_srq_attr *attr)
{
	return uib_query_srq(srq, attr);
}

/**
 * uibv_destroy_srq - Destroy an unused verbs-facade shared receive queue.
 * @srq: Shared receive queue
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success, or -EBUSY while a QP still uses the SRQ.
 */
int uibv_destroy_srq(struct uib_srq *srq)
{
	return uib_destroy_srq(srq);
}

/**
 * uibv_post_srq_recv - Post receives to a verbs-facade shared receive queue.
 * @srq: Shared receive queue
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_post_srq_recv(struct uib_srq *srq, struct uib_recv_wr *wr,
		       struct uib_recv_wr **bad)
{
	return uib_post_srq_recv(srq, wr, bad);
}

/**
 * uibv_create_qp - Create a verbs-facade queue pair.
 * @pd: Protection domain
 * @attr: Queue pair attributes
 * @uib-api: Native verbs compatibility API
 *
 * Return: A pointer to the new object on success, or NULL on failure.
 */
struct uib_qp *uibv_create_qp(struct uib_pd *pd, struct uib_qp_init_attr *attr)
{
	return uib_create_qp(pd, attr);
}
/**
 * uibv_modify_qp - Change verbs-facade QP state and attributes.
 * @qp: Queue pair
 * @attr: Queue pair attributes
 * @mask: Bitmask selecting attributes to modify
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_modify_qp(struct uib_qp *qp, struct uib_qp_attr *attr, int mask)
{
	return uib_modify_qp(qp, attr, mask);
}
/**
 * uibv_destroy_qp - Release a verbs-facade queue pair.
 * @qp: Queue pair
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_destroy_qp(struct uib_qp *qp)
{
	return uib_destroy_qp(qp);
}

/**
 * uibv_post_send - Post a verbs-facade send work-request chain.
 * @qp: Queue pair
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_post_send(struct uib_qp *qp, struct uib_send_wr *wr,
		   struct uib_send_wr **bad)
{
	return uib_post_send(qp, wr, bad);
}

/**
 * uibv_post_recv - Post a verbs-facade receive work-request chain.
 * @qp: Queue pair
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Native verbs compatibility API
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uibv_post_recv(struct uib_qp *qp, struct uib_recv_wr *wr,
		   struct uib_recv_wr **bad)
{
	return uib_post_recv(qp, wr, bad);
}
