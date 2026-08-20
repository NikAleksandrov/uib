#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>

#include "uib/neigh.h"

#ifndef NDA_RTA
#define NDA_RTA(r) \
	((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#endif
#ifndef NDA_PAYLOAD
#define NDA_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct ndmsg))
#endif

#define UIB_NEIGH_HAS_LLADDR \
	(NUD_REACHABLE | NUD_PERMANENT | NUD_STALE | NUD_DELAY | NUD_PROBE)

struct neigh_req {
	struct nlmsghdr nlh;
	struct ndmsg    ndm;
};

static int match_neigh(const struct nlmsghdr *nlh, uint32_t dst_ip,
		       uint8_t mac[6])
{
	const struct ndmsg *ndm = NLMSG_DATA(nlh);
	const struct rtattr *rta;
	int len = NDA_PAYLOAD(nlh);
	uint32_t found_ip = 0;
	const uint8_t *ll = NULL;
	int have_ip = 0;

	if (ndm->ndm_family != AF_INET)
		return 0;
	if (!(ndm->ndm_state & UIB_NEIGH_HAS_LLADDR))
		return 0;

	for (rta = NDA_RTA(ndm); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
		if (rta->rta_type == NDA_DST && RTA_PAYLOAD(rta) == 4) {
			memcpy(&found_ip, RTA_DATA(rta), 4);
			have_ip = 1;
		} else if (rta->rta_type == NDA_LLADDR && RTA_PAYLOAD(rta) == 6) {
			ll = RTA_DATA(rta);
		}
	}
	if (have_ip && ll && found_ip == dst_ip) {
		memcpy(mac, ll, 6);
		return 1;
	}
	return 0;
}

int uib_neigh_lookup(uint32_t dst_ip, uint8_t mac[6])
{
	struct neigh_req req;
	struct sockaddr_nl sa;
	uint8_t buf[8192];
	int fd, rc = -ENOENT;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;

	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.ndm));
	req.nlh.nlmsg_type  = RTM_GETNEIGH;
	req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nlh.nlmsg_seq   = 1;
	req.ndm.ndm_family  = AF_INET;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (sendto(fd, &req, req.nlh.nlmsg_len, 0,
		   (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		rc = -errno;
		goto out;
	}

	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);

		if (n < 0) {
			rc = -errno;
			break;
		}
		struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
		int done = 0;

		for (; NLMSG_OK(nlh, n); nlh = NLMSG_NEXT(nlh, n)) {
			if (nlh->nlmsg_type == NLMSG_DONE) {
				done = 1;
				break;
			}
			if (nlh->nlmsg_type == NLMSG_ERROR) {
				rc = -EIO;
				done = 1;
				break;
			}
			if (nlh->nlmsg_type == RTM_NEWNEIGH &&
			    match_neigh(nlh, dst_ip, mac)) {
				rc = 0;
				done = 1;
				break;
			}
		}
		if (done)
			break;
	}
out:
	close(fd);
	return rc;
}
