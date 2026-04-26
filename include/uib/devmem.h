#ifndef UIB_DEVMEM_H
#define UIB_DEVMEM_H

#include <stdint.h>

int uib_genl_family_id(const char *name);

struct uib_devmem_binding {
	int      sock_fd;
	uint32_t dmabuf_id;
};

int uib_devmem_bind_rx(const char *ifname, uint32_t queue_id, int dmabuf_fd,
		       struct uib_devmem_binding *b);
void uib_devmem_unbind(struct uib_devmem_binding *b);

#endif
