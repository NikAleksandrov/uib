// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#include "uib/devmem.h"

#ifndef NLA_HDRLEN
#define NLA_HDRLEN ((int)NLA_ALIGN(sizeof(struct nlattr)))
#endif
#define GENL_HDRLEN NLMSG_ALIGN(sizeof(struct genlmsghdr))

static void nla_put(uint8_t *b, size_t *len, uint16_t type,
		    const void *data, uint16_t dlen)
{
	struct nlattr *a = (struct nlattr *)(b + *len);

	a->nla_type = type;
	a->nla_len  = (uint16_t)(NLA_HDRLEN + dlen);
	if (dlen)
		memcpy((char *)a + NLA_HDRLEN, data, dlen);
	*len += NLA_ALIGN(a->nla_len);
}

static void nla_put_u32(uint8_t *b, size_t *len, uint16_t type, uint32_t v)
{
	nla_put(b, len, type, &v, sizeof(v));
}

static size_t nla_nest_start(uint8_t *b, size_t *len, uint16_t type)
{
	struct nlattr *a = (struct nlattr *)(b + *len);
	size_t at = *len;

	a->nla_type = type | NLA_F_NESTED;
	*len += NLA_HDRLEN;
	return at;
}

static void nla_nest_end(uint8_t *b, size_t *len, size_t at)
{
	struct nlattr *a = (struct nlattr *)(b + at);

	a->nla_len = (uint16_t)(*len - at);
}

static int genl_open(void)
{
	return socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
}

static int genl_xchg(int fd, uint8_t *buf, size_t len, size_t cap)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	ssize_t n;

	nlh->nlmsg_len = (uint32_t)len;
	if (send(fd, buf, len, 0) < 0)
		return -errno;

	n = recv(fd, buf, cap, 0);
	if (n < 0)
		return -errno;
	nlh = (struct nlmsghdr *)buf;
	if (!NLMSG_OK(nlh, (int)n))
		return -EBADMSG;
	if (nlh->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *e = NLMSG_DATA(nlh);

		return e->error;
	}
	return (int)(n - NLMSG_HDRLEN);
}

static int genl_find_attr(const uint8_t *buf, int paylen, uint16_t want,
			  void *out, int dcap)
{
	const uint8_t *p = buf + NLMSG_HDRLEN + GENL_HDRLEN;
	int rem = paylen - (int)GENL_HDRLEN;

	while (rem >= NLA_HDRLEN) {
		const struct nlattr *a = (const struct nlattr *)p;
		int alen = a->nla_len;

		if (alen < NLA_HDRLEN || alen > rem)
			break;
		if ((a->nla_type & NLA_TYPE_MASK) == want) {
			int dl = alen - NLA_HDRLEN;

			if (dl > dcap)
				dl = dcap;
			memcpy(out, p + NLA_HDRLEN, dl);
			return dl;
		}
		p   += NLA_ALIGN(alen);
		rem -= NLA_ALIGN(alen);
	}
	return -ENOENT;
}

int uib_genl_family_id(const char *name)
{
	uint8_t buf[1024];
	struct nlmsghdr *nlh;
	struct genlmsghdr *gh;
	size_t len;
	int fd, rc;
	uint16_t id = 0;

	fd = genl_open();
	if (fd < 0)
		return -errno;

	memset(buf, 0, sizeof(buf));
	nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_type  = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq   = 1;
	gh = (struct genlmsghdr *)(buf + NLMSG_HDRLEN);
	gh->cmd     = CTRL_CMD_GETFAMILY;
	gh->version = 1;
	len = NLMSG_HDRLEN + GENL_HDRLEN;
	nla_put(buf, &len, CTRL_ATTR_FAMILY_NAME, name, (uint16_t)(strlen(name) + 1));

	rc = genl_xchg(fd, buf, len, sizeof(buf));
	if (rc < 0) {
		close(fd);
		return rc;
	}
	if (genl_find_attr(buf, rc, CTRL_ATTR_FAMILY_ID, &id, sizeof(id)) < 0) {
		close(fd);
		return -ENOENT;
	}
	close(fd);
	return id ? id : -ENOENT;
}

#ifdef UIB_HAVE_DEVMEM
#include <linux/netdev.h>

int uib_devmem_bind_rx(const char *ifname, uint32_t queue_id, int dmabuf_fd,
		       struct uib_devmem_binding *b)
{
	uint8_t buf[1024];
	struct nlmsghdr *nlh;
	struct genlmsghdr *gh;
	size_t len, qs, q0;
	unsigned int ifindex;
	int fd, fam, rc;
	uint32_t id = 0;

	memset(b, 0, sizeof(*b));
	b->sock_fd = -1;

	ifindex = if_nametoindex(ifname);
	if (!ifindex)
		return -ENODEV;
	fam = uib_genl_family_id("netdev");
	if (fam < 0)
		return fam;

	fd = genl_open();
	if (fd < 0)
		return -errno;

	memset(buf, 0, sizeof(buf));
	nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_type  = (uint16_t)fam;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq   = 1;
	gh = (struct genlmsghdr *)(buf + NLMSG_HDRLEN);
	gh->cmd     = NETDEV_CMD_BIND_RX;
	gh->version = 1;
	len = NLMSG_HDRLEN + GENL_HDRLEN;

	nla_put_u32(buf, &len, NETDEV_A_DMABUF_IFINDEX, ifindex);
	nla_put_u32(buf, &len, NETDEV_A_DMABUF_FD, (uint32_t)dmabuf_fd);
	qs = nla_nest_start(buf, &len, NETDEV_A_DMABUF_QUEUES);
	q0 = nla_nest_start(buf, &len, 0);
	nla_put_u32(buf, &len, NETDEV_A_QUEUE_ID, queue_id);
	nla_put_u32(buf, &len, NETDEV_A_QUEUE_TYPE, NETDEV_QUEUE_TYPE_RX);
	nla_nest_end(buf, &len, q0);
	nla_nest_end(buf, &len, qs);

	rc = genl_xchg(fd, buf, len, sizeof(buf));
	if (rc < 0) {
		close(fd);
		return rc;
	}
	if (rc > 0)
		genl_find_attr(buf, rc, NETDEV_A_DMABUF_ID, &id, sizeof(id));

	/* The binding lives for as long as this socket remains open. */
	b->sock_fd   = fd;
	b->dmabuf_id = id;
	return 0;
}

void uib_devmem_unbind(struct uib_devmem_binding *b)
{
	if (b && b->sock_fd >= 0) {
		close(b->sock_fd);
		b->sock_fd = -1;
	}
}

#else

int uib_devmem_bind_rx(const char *ifname, uint32_t queue_id, int dmabuf_fd,
		       struct uib_devmem_binding *b)
{
	(void)ifname;
	(void)queue_id;
	(void)dmabuf_fd;
	if (b) {
		b->sock_fd = -1;
		b->dmabuf_id = 0;
	}
	return -ENOSYS;
}

void uib_devmem_unbind(struct uib_devmem_binding *b)
{
	(void)b;
}

#endif
