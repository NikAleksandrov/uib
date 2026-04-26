#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>

#include "uib/verbs.h"
#include "uib/bth.h"
#include "uib/transport.h"

struct uibv_device **uibv_get_device_list(int *num)
{
	struct ifaddrs *ifa, *cur;

	if (getifaddrs(&ifa) != 0)
		return NULL;

	int count = 0;

	for (cur = ifa; cur; cur = cur->ifa_next) {
		if (cur->ifa_addr && cur->ifa_addr->sa_family == AF_INET &&
		    !(cur->ifa_flags & IFF_LOOPBACK))
			count++;
	}
	if (count == 0)
		count = 1;

	struct uibv_device **list = calloc((size_t)count + 1, sizeof(*list));

	if (!list) {
		freeifaddrs(ifa);
		return NULL;
	}

	int i = 0;

	for (cur = ifa; cur; cur = cur->ifa_next) {
		if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET)
			continue;
		if (cur->ifa_flags & IFF_LOOPBACK)
			continue;
		struct uibv_device *d = calloc(1, sizeof(*d));

		if (!d)
			break;
		snprintf(d->name, sizeof(d->name), "uib_%s", cur->ifa_name);
		d->bind_addr = *(struct sockaddr_in *)cur->ifa_addr;
		d->bind_addr.sin_port = htons(UIB_ROCE_V2_UDP_DPORT);
		list[i++] = d;
	}
	if (i == 0) {
		struct uibv_device *d = calloc(1, sizeof(*d));

		if (d) {
			snprintf(d->name, sizeof(d->name), "uib_lo");
			d->bind_addr.sin_family = AF_INET;
			d->bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			d->bind_addr.sin_port = htons(UIB_ROCE_V2_UDP_DPORT);
			list[i++] = d;
		}
	}
	list[i] = NULL;
	freeifaddrs(ifa);
	if (num)
		*num = i;
	return list;
}

void uibv_free_device_list(struct uibv_device **list)
{
	if (!list)
		return;
	for (int i = 0; list[i]; i++)
		free(list[i]);
	free(list);
}

const char *uibv_get_device_name(struct uibv_device *dev)
{
	return dev ? dev->name : NULL;
}

struct uib_context *uibv_open_device(struct uibv_device *dev)
{
	if (!dev) {
		errno = EINVAL;
		return NULL;
	}
	char ip[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, &dev->bind_addr.sin_addr, ip, sizeof(ip));
	uint16_t port = ntohs(dev->bind_addr.sin_port);

	return uib_open_device(ip, port);
}

int uibv_close_device(struct uib_context *ctx)
{
	return uib_close_device(ctx);
}

struct uib_pd *uibv_alloc_pd(struct uib_context *ctx)
{
	return uib_alloc_pd(ctx);
}

int uibv_dealloc_pd(struct uib_pd *pd)
{
	return uib_dealloc_pd(pd);
}

struct uib_mr *uibv_reg_mr(struct uib_pd *pd, void *addr, size_t len, int access)
{
	return uib_reg_mr(pd, addr, len, access);
}
int uibv_dereg_mr(struct uib_mr *mr)
{
	return uib_dereg_mr(mr);
}

struct uib_cq *uibv_create_cq(struct uib_context *ctx, int cqe)
{
	return uib_create_cq(ctx, cqe);
}
int uibv_destroy_cq(struct uib_cq *cq)
{
	return uib_destroy_cq(cq);
}
int uibv_poll_cq(struct uib_cq *cq, int n, struct uib_wc *wc)
{
	return uib_poll_cq(cq, n, wc);
}

struct uib_qp *uibv_create_qp(struct uib_pd *pd, struct uib_qp_init_attr *attr)
{
	return uib_create_qp(pd, attr);
}
int uibv_modify_qp(struct uib_qp *qp, struct uib_qp_attr *attr, int mask)
{
	return uib_modify_qp(qp, attr, mask);
}
int uibv_destroy_qp(struct uib_qp *qp)
{
	return uib_destroy_qp(qp);
}
int uibv_post_send(struct uib_qp *qp, struct uib_send_wr *wr, struct uib_send_wr **bad)
{
	return uib_post_send(qp, wr, bad);
}
int uibv_post_recv(struct uib_qp *qp, struct uib_recv_wr *wr, struct uib_recv_wr **bad)
{
	return uib_post_recv(qp, wr, bad);
}
