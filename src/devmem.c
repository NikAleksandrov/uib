// SPDX-License-Identifier: MIT
#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <libmnl/libmnl.h>

#include "uib/devmem.h"

#define UIB_GENL_CTRL_VERSION 1

struct genl_family_ctx {
	uint16_t id;
};

static struct mnl_socket *genl_open(unsigned int *portid)
{
	struct mnl_socket *nl;
	int saved_errno;

	nl = mnl_socket_open2(NETLINK_GENERIC, SOCK_CLOEXEC);
	if (!nl)
		return NULL;
	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		saved_errno = errno;
		mnl_socket_close(nl);
		errno = saved_errno;
		return NULL;
	}
	*portid = mnl_socket_get_portid(nl);
	return nl;
}

static int genl_request(struct mnl_socket *nl, struct nlmsghdr *nlh,
			unsigned int portid, mnl_cb_t callback, void *data)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	ssize_t received;
	int rc;

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
		return -errno;

	received = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	while (received > 0) {
		rc = mnl_cb_run(buf, received, nlh->nlmsg_seq, portid,
				callback, data);
		if (rc <= MNL_CB_STOP)
			return rc < 0 ? -errno : 0;
		received = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	}
	return received < 0 ? -errno : -ENODATA;
}

static int genl_family_attr_cb(const struct nlattr *attr, void *data)
{
	struct genl_family_ctx *ctx = data;
	int type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, CTRL_ATTR_MAX) < 0)
		return MNL_CB_OK;
	if (type != CTRL_ATTR_FAMILY_ID)
		return MNL_CB_OK;
	if (mnl_attr_validate(attr, MNL_TYPE_U16) < 0)
		return MNL_CB_ERROR;
	ctx->id = mnl_attr_get_u16(attr);
	return MNL_CB_OK;
}

static int genl_family_cb(const struct nlmsghdr *nlh, void *data)
{
	const struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);
	int rc;

	rc = mnl_attr_parse(nlh, sizeof(*genl), genl_family_attr_cb, data);
	if (rc < 0)
		return MNL_CB_ERROR;
	return MNL_CB_STOP;
}

int uib_genl_family_id(const char *name)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct genl_family_ctx ctx = { 0 };
	struct genlmsghdr *genl;
	struct mnl_socket *nl;
	struct nlmsghdr *nlh;
	unsigned int portid;
	int rc;

	if (!name)
		return -EINVAL;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = 1;
	genl = mnl_nlmsg_put_extra_header(nlh, sizeof(*genl));
	genl->cmd = CTRL_CMD_GETFAMILY;
	genl->version = UIB_GENL_CTRL_VERSION;
	if (!mnl_attr_put_strz_check(nlh, sizeof(buf), CTRL_ATTR_FAMILY_NAME,
				       name))
		return -EMSGSIZE;

	nl = genl_open(&portid);
	if (!nl)
		return -errno;
	rc = genl_request(nl, nlh, portid, genl_family_cb, &ctx);
	mnl_socket_close(nl);
	if (rc < 0)
		return rc;
	return ctx.id ? ctx.id : -ENOENT;
}

#ifdef UIB_HAVE_DEVMEM
#include <linux/netdev.h>

struct devmem_ctx {
	uint32_t id;
};

static int devmem_attr_cb(const struct nlattr *attr, void *data)
{
	struct devmem_ctx *ctx = data;
	int type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, NETDEV_A_DMABUF_MAX) < 0)
		return MNL_CB_OK;
	if (type != NETDEV_A_DMABUF_ID)
		return MNL_CB_OK;
	if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
		return MNL_CB_ERROR;
	ctx->id = mnl_attr_get_u32(attr);
	return MNL_CB_OK;
}

static int devmem_cb(const struct nlmsghdr *nlh, void *data)
{
	const struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);

	return mnl_attr_parse(nlh, sizeof(*genl), devmem_attr_cb, data);
}

int uib_devmem_bind_rx(const char *ifname, uint32_t queue_id, int dmabuf_fd,
		       struct uib_devmem_binding *binding)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct devmem_ctx ctx = { 0 };
	struct genlmsghdr *genl;
	struct mnl_socket *nl;
	struct nlmsghdr *nlh;
	struct nlattr *queues;
	unsigned int portid;
	unsigned int ifindex;
	int family;
	int rc;

	if (!ifname || !binding || dmabuf_fd < 0)
		return -EINVAL;
	binding->nl = NULL;
	binding->dmabuf_id = 0;

	ifindex = if_nametoindex(ifname);
	if (!ifindex)
		return -ENODEV;
	family = uib_genl_family_id(NETDEV_FAMILY_NAME);
	if (family < 0)
		return family;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = 1;
	genl = mnl_nlmsg_put_extra_header(nlh, sizeof(*genl));
	genl->cmd = NETDEV_CMD_BIND_RX;
	genl->version = NETDEV_FAMILY_VERSION;

	if (!mnl_attr_put_u32_check(nlh, sizeof(buf), NETDEV_A_DMABUF_IFINDEX,
				    ifindex) ||
	    !mnl_attr_put_u32_check(nlh, sizeof(buf), NETDEV_A_DMABUF_FD,
				    (uint32_t)dmabuf_fd))
		return -EMSGSIZE;
	queues = mnl_attr_nest_start_check(nlh, sizeof(buf),
					   NETDEV_A_DMABUF_QUEUES);
	if (!queues)
		return -EMSGSIZE;
	if (!mnl_attr_put_u32_check(nlh, sizeof(buf), NETDEV_A_QUEUE_ID,
				    queue_id) ||
	    !mnl_attr_put_u32_check(nlh, sizeof(buf), NETDEV_A_QUEUE_TYPE,
				    NETDEV_QUEUE_TYPE_RX))
		return -EMSGSIZE;
	mnl_attr_nest_end(nlh, queues);

	nl = genl_open(&portid);
	if (!nl)
		return -errno;
	rc = genl_request(nl, nlh, portid, devmem_cb, &ctx);
	if (rc < 0) {
		mnl_socket_close(nl);
		return rc;
	}

	/* The binding lives for as long as this socket remains open. */
	binding->nl = nl;
	binding->dmabuf_id = ctx.id;
	return 0;
}

void uib_devmem_unbind(struct uib_devmem_binding *binding)
{
	if (binding && binding->nl) {
		mnl_socket_close(binding->nl);
		binding->nl = NULL;
	}
}

#else

int uib_devmem_bind_rx(const char *ifname, uint32_t queue_id, int dmabuf_fd,
		       struct uib_devmem_binding *binding)
{
	(void)ifname;
	(void)queue_id;
	(void)dmabuf_fd;
	if (binding) {
		binding->nl = NULL;
		binding->dmabuf_id = 0;
	}
	return -ENOSYS;
}

void uib_devmem_unbind(struct uib_devmem_binding *binding)
{
	(void)binding;
}

#endif
