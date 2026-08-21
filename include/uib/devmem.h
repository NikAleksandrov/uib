/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_DEVMEM_H
#define UIB_DEVMEM_H

#include <stdint.h>

struct mnl_socket;

/*
 * Resolve a generic-netlink family id by name (CTRL_CMD_GETFAMILY). Returns the
 * family id (>0) or a negative errno. Used to reach the "netdev" family.
 */
int uib_genl_family_id(const char *name);

/*
 * A live dma-buf-to-RX-queue binding. The binding is owned by the libmnl
 * socket in `nl`; closing it with uib_devmem_unbind releases the binding.
 */
struct uib_devmem_binding {
	struct mnl_socket *nl;
	uint32_t           dmabuf_id;
};

/*
 * Bind dmabuf_fd to (ifname, queue_id) as an RX target via the netdev family's
 * NETDEV_CMD_BIND_RX. On success fills *b and returns 0; negative errno on
 * failure. Requires a devmem-capable NIC/kernel; compile-verified only in a
 * sandbox without such hardware.
 */
int uib_devmem_bind_rx(const char *ifname, uint32_t queue_id, int dmabuf_fd,
		       struct uib_devmem_binding *b);
void uib_devmem_unbind(struct uib_devmem_binding *b);

#endif
