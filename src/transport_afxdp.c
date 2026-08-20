// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/protocol.h"
#include "uib/afxdp_frame.h"
#include "uib/neigh.h"

#ifdef UIB_HAVE_AFXDP

#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_xdp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <xdp/xsk.h>

#define UIB_AFXDP_FRAME_SIZE  4096
#define UIB_AFXDP_NUM_FRAMES  4096
#define UIB_AFXDP_FILL_FRAMES XSK_RING_PROD__DEFAULT_NUM_DESCS
#define UIB_AFXDP_RX_FRAMES   XSK_RING_CONS__DEFAULT_NUM_DESCS
#define UIB_AFXDP_TX_FRAMES   XSK_RING_PROD__DEFAULT_NUM_DESCS

struct uib_afxdp_backend {
	struct xsk_umem     *umem;
	struct xsk_socket   *xsk;
	struct xsk_ring_prod fq;
	struct xsk_ring_cons cq;
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
	void                *umem_area;

	uint64_t            *tx_free;
	uint32_t             tx_free_n;

	uint32_t             queue_id;
	uint8_t              src_mac[6];
	uint8_t              peer_mac[6];
	int                  have_peer_mac;
	uint32_t             local_ip;
	uint16_t             port;

	pthread_mutex_t      tx_lock;
	int                  timer_fd;
	pthread_t            worker;
	int                  worker_run;
	int                  zerocopy;
};

static int parse_mac(const char *s, uint8_t mac[6])
{
	unsigned int v[6];
	int i;

	if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x",
			 &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	for (i = 0; i < 6; i++)
		mac[i] = (uint8_t)v[i];
	return 0;
}

static int get_if_mac(const char *ifname, uint8_t mac[6])
{
	struct ifreq ifr;
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
		close(s);
		return -1;
	}
	memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	close(s);
	return 0;
}

static int get_if_ip(const char *ifname, uint32_t *ip_net)
{
	struct ifreq ifr;
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_addr.sa_family = AF_INET;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
		close(s);
		return -1;
	}
	*ip_net = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
	close(s);
	return 0;
}

static const uint8_t *resolve_dst_mac(struct uib_afxdp_backend *be,
				      uint32_t dst_ip, uint8_t out[6])
{
	if (uib_neigh_lookup(dst_ip, out) == 0)
		return out;
	if (be->have_peer_mac)
		return be->peer_mac;
	return NULL;
}

static void reap_completions(struct uib_afxdp_backend *be)
{
	uint32_t idx = 0, n, i;

	n = xsk_ring_cons__peek(&be->cq, UIB_AFXDP_TX_FRAMES, &idx);
	for (i = 0; i < n; i++) {
		uint64_t addr = *xsk_ring_cons__comp_addr(&be->cq, idx + i);

		be->tx_free[be->tx_free_n++] = addr;
	}
	if (n)
		xsk_ring_cons__release(&be->cq, n);
}

static int afxdp_probe(void)
{
	const char *ifn = getenv("UIB_AFXDP_IFACE");

	return (ifn && *ifn) ? 1 : 0;
}

static int afxdp_decode_packet(struct uib_context *ctx, uint8_t *frame,
			       size_t frame_len, const uint8_t *payload,
			       size_t paylen)
{
	const struct iphdr *ip;
	const struct udphdr *udp;
	struct uib_wire_meta meta;

	if (frame_len < UIB_L2_HDRS_LEN || paylen < ctx->pops->min_packet)
		return -1;
	ip = (const struct iphdr *)(frame + ETH_HLEN);
	udp = (const struct udphdr *)((const uint8_t *)ip + (size_t)ip->ihl * 4);
	meta.src_ip = ip->saddr;
	meta.dst_ip = ip->daddr;
	meta.src_port = udp->source;
	meta.dst_port = udp->dest;
	meta.ip_id = ntohs(ip->id);
	if (ctx->pops->wire_decode &&
	    ctx->pops->wire_decode(&meta, (void *)payload, paylen) < 0)
		return -1;
	return 0;
}

static void *worker_loop(void *arg)
{
	struct uib_context *ctx = arg;
	struct uib_afxdp_backend *be = ctx->backend;
	struct pollfd pfds[2];

	pfds[0].fd = xsk_socket__fd(be->xsk);
	pfds[0].events = POLLIN;
	pfds[1].fd = be->timer_fd;
	pfds[1].events = POLLIN;

	while (__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE)) {
		if (poll(pfds, 2, 200) <= 0)
			continue;
		if (!__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE))
			break;

		if (pfds[1].revents & POLLIN) {
			uint64_t exp;

			if (read(be->timer_fd, &exp, sizeof(exp)) == sizeof(exp))
				uib_transport_retransmit_walk(ctx);
		}

		if (pfds[0].revents & POLLIN) {
			uint32_t idx_rx = 0, idx_fq = 0, n, i;

			n = xsk_ring_cons__peek(&be->rx, UIB_AFXDP_RX_FRAMES, &idx_rx);
			if (!n)
				continue;

			while (xsk_ring_prod__reserve(&be->fq, n, &idx_fq) != n) {
				if (!__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE))
					break;
			}

			for (i = 0; i < n; i++) {
				const struct xdp_desc *d =
					xsk_ring_cons__rx_desc(&be->rx, idx_rx + i);
				uint64_t addr = xsk_umem__extract_addr(d->addr);
				uint8_t *frame = xsk_umem__get_data(be->umem_area, d->addr);
				struct sockaddr_in src;
				const uint8_t *pay;
				size_t paylen;

				if (uib_frame_parse(frame, d->len, be->port, &src,
						    &pay, &paylen) == 0 &&
				    afxdp_decode_packet(ctx, frame, d->len, pay,
						 paylen) == 0)
					uib_transport_deliver(ctx, &src, pay, paylen);

				*xsk_ring_prod__fill_addr(&be->fq, idx_fq + i) = addr;
			}
			xsk_ring_prod__submit(&be->fq, n);
			xsk_ring_cons__release(&be->rx, n);
		}
	}
	return NULL;
}

static int afxdp_send_entropy(struct uib_context *ctx,
			      const struct sockaddr_in *dst,
			      const void *buf, size_t len, uint16_t entropy)
{
	struct uib_afxdp_backend *be = ctx->backend;
	struct uib_frame_hdr hi;
	struct uib_wire_meta meta;
	const uint8_t *dmac;
	uint8_t dmac_buf[6];
	uint32_t idx = 0;
	uint64_t addr;
	uint8_t *frame;
	int flen, rc = 0;

	if (UIB_L2_HDRS_LEN + len > UIB_AFXDP_FRAME_SIZE)
		return -EMSGSIZE;

	dmac = resolve_dst_mac(be, dst->sin_addr.s_addr, dmac_buf);
	if (!dmac)
		return -EHOSTUNREACH;

	pthread_mutex_lock(&be->tx_lock);
	reap_completions(be);
	if (be->tx_free_n == 0) {
		rc = -EAGAIN;
		goto out;
	}
	if (xsk_ring_prod__reserve(&be->tx, 1, &idx) != 1) {
		rc = -EAGAIN;
		goto out;
	}
	addr  = be->tx_free[--be->tx_free_n];
	frame = xsk_umem__get_data(be->umem_area, addr);

	memset(&hi, 0, sizeof(hi));
	memcpy(hi.src_mac, be->src_mac, 6);
	memcpy(hi.dst_mac, dmac, 6);
	hi.src_ip   = be->local_ip;
	hi.dst_ip   = dst->sin_addr.s_addr;
	hi.src_port = entropy ? htons(entropy) : be->port;
	hi.dst_port = dst->sin_port ? dst->sin_port : be->port;

	flen = uib_frame_build(frame, UIB_AFXDP_FRAME_SIZE, &hi, buf, len);
	if (flen < 0) {
		be->tx_free[be->tx_free_n++] = addr;
		rc = -EMSGSIZE;
		goto out;
	}
	if (len < ctx->pops->min_packet) {
		be->tx_free[be->tx_free_n++] = addr;
		rc = -EINVAL;
		goto out;
	}
	meta.src_ip = hi.src_ip;
	meta.dst_ip = hi.dst_ip;
	meta.src_port = hi.src_port;
	meta.dst_port = hi.dst_port;
	meta.ip_id = 0;
	if (ctx->pops->wire_encode &&
	    ctx->pops->wire_encode(&meta, frame + UIB_L2_HDRS_LEN, len) < 0) {
		be->tx_free[be->tx_free_n++] = addr;
		rc = -EBADMSG;
		goto out;
	}

	struct xdp_desc *txd = xsk_ring_prod__tx_desc(&be->tx, idx);

	txd->addr = addr;
	txd->len  = (uint32_t)flen;
	xsk_ring_prod__submit(&be->tx, 1);

	if (xsk_ring_prod__needs_wakeup(&be->tx))
		sendto(xsk_socket__fd(be->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (be->zerocopy)
		uib_zerocopy_account_direct(ctx, len, UIB_ZEROCOPY_CAP_AF_XDP);
	else
		uib_zerocopy_account_copy(ctx, len);
out:
	pthread_mutex_unlock(&be->tx_lock);
	return rc;
}

static int afxdp_send(struct uib_context *ctx, const struct sockaddr_in *dst,
		      const void *buf, size_t len)
{
	return afxdp_send_entropy(ctx, dst, buf, len, 0);
}

static void afxdp_fini(struct uib_context *ctx)
{
	struct uib_afxdp_backend *be = ctx->backend;

	if (!be)
		return;

	if (__atomic_exchange_n(&be->worker_run, 0, __ATOMIC_ACQ_REL))
		pthread_join(be->worker, NULL);

	if (be->xsk)
		xsk_socket__delete(be->xsk);
	if (be->umem)
		xsk_umem__delete(be->umem);
	if (be->timer_fd >= 0)
		close(be->timer_fd);
	pthread_mutex_destroy(&be->tx_lock);
	free(be->tx_free);
	free(be->umem_area);
	free(be);
	ctx->backend = NULL;
}

static int afxdp_init(struct uib_context *ctx)
{
	struct uib_afxdp_backend *be;
	struct xsk_umem_config ucfg = {
		.fill_size = UIB_AFXDP_FILL_FRAMES,
		.comp_size = UIB_AFXDP_TX_FRAMES,
		.frame_size = UIB_AFXDP_FRAME_SIZE,
		.frame_headroom = 0,
		.flags = 0,
	};
	struct xsk_socket_config scfg = {
		.rx_size = UIB_AFXDP_RX_FRAMES,
		.tx_size = UIB_AFXDP_TX_FRAMES,
		.libxdp_flags = 0,
		.xdp_flags = 0,
		.bind_flags = XDP_USE_NEED_WAKEUP,
	};
	const char *ifn = getenv("UIB_AFXDP_IFACE");
	const char *qs  = getenv("UIB_AFXDP_QUEUE");
	const char *pm  = getenv("UIB_AFXDP_PEER_MAC");
	uint64_t umem_sz = (uint64_t)UIB_AFXDP_NUM_FRAMES * UIB_AFXDP_FRAME_SIZE;
	uint32_t idx = 0, i;
	int rc;

	if (!ifn || !*ifn)
		return -ENODEV;

	be = calloc(1, sizeof(*be));
	if (!be)
		return -ENOMEM;
	be->timer_fd = -1;
	be->port = htons(ctx->bind_port ? ctx->bind_port :
			 ctx->pops->default_port);
	be->queue_id = qs ? (uint32_t)atoi(qs) : 0;
	be->zerocopy = ctx->zerocopy.mode != UIB_ZEROCOPY_OFF;
	scfg.bind_flags |= be->zerocopy ? XDP_ZEROCOPY : XDP_COPY;
	ctx->backend = be;
	pthread_mutex_init(&be->tx_lock, NULL);

	be->have_peer_mac = (parse_mac(pm, be->peer_mac) == 0);
	if (get_if_mac(ifn, be->src_mac) < 0) {
		rc = -errno;
		goto err;
	}
	if (ctx->bind_ip[0])
		inet_pton(AF_INET, ctx->bind_ip, &be->local_ip);
	else if (get_if_ip(ifn, &be->local_ip) < 0) {
		rc = -errno;
		goto err;
	}
	ctx->local_addr.sin_family = AF_INET;
	ctx->local_addr.sin_addr.s_addr = be->local_ip;
	ctx->local_addr.sin_port = be->port;

	if (posix_memalign(&be->umem_area, sysconf(_SC_PAGESIZE), umem_sz) != 0) {
		rc = -ENOMEM;
		goto err;
	}
	rc = xsk_umem__create(&be->umem, be->umem_area, umem_sz,
			      &be->fq, &be->cq, &ucfg);
	if (rc) {
		rc = rc < 0 ? rc : -rc;
		goto err;
	}
	rc = xsk_socket__create(&be->xsk, ifn, be->queue_id, be->umem,
				&be->rx, &be->tx, &scfg);
	if (rc && ctx->zerocopy.mode == UIB_ZEROCOPY_AUTO) {
		be->zerocopy = 0;
		scfg.bind_flags &= ~XDP_ZEROCOPY;
		scfg.bind_flags |= XDP_COPY;
		rc = xsk_socket__create(&be->xsk, ifn, be->queue_id, be->umem,
					&be->rx, &be->tx, &scfg);
	}
	if (rc) {
		rc = rc < 0 ? rc : -rc;
		goto err;
	}

	be->tx_free = calloc(UIB_AFXDP_NUM_FRAMES, sizeof(*be->tx_free));
	if (!be->tx_free) {
		rc = -ENOMEM;
		goto err;
	}
	if (xsk_ring_prod__reserve(&be->fq, UIB_AFXDP_FILL_FRAMES, &idx)
	    != UIB_AFXDP_FILL_FRAMES) {
		rc = -EIO;
		goto err;
	}
	for (i = 0; i < UIB_AFXDP_FILL_FRAMES; i++)
		*xsk_ring_prod__fill_addr(&be->fq, idx + i) =
			(uint64_t)i * UIB_AFXDP_FRAME_SIZE;
	xsk_ring_prod__submit(&be->fq, UIB_AFXDP_FILL_FRAMES);

	for (i = UIB_AFXDP_FILL_FRAMES; i < UIB_AFXDP_NUM_FRAMES; i++)
		be->tx_free[be->tx_free_n++] = (uint64_t)i * UIB_AFXDP_FRAME_SIZE;

	be->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (be->timer_fd >= 0) {
		struct itimerspec its = {0};

		its.it_interval.tv_nsec = UIB_TIMER_TICK_MS * 1000000L;
		its.it_value = its.it_interval;
		timerfd_settime(be->timer_fd, 0, &its, NULL);
	}

	__atomic_store_n(&be->worker_run, 1, __ATOMIC_RELEASE);
	if (pthread_create(&be->worker, NULL, worker_loop, ctx) != 0) {
		__atomic_store_n(&be->worker_run, 0, __ATOMIC_RELEASE);
		rc = -errno;
		goto err;
	}
	return 0;

err:
	afxdp_fini(ctx);
	return rc;
}

const struct uib_transport_ops uib_afxdp_ops = {
	.name  = "udp-zc-afxdp",
	.probe = afxdp_probe,
	.init  = afxdp_init,
	.fini  = afxdp_fini,
	.send  = afxdp_send,
	.send_entropy = afxdp_send_entropy,
};

#else

static int afxdp_probe(void)
{
	return 0;
}

static int afxdp_init(struct uib_context *ctx)
{
	(void)ctx;
	return -ENOSYS;
}

static void afxdp_fini(struct uib_context *ctx)
{
	(void)ctx;
}
static int afxdp_send(struct uib_context *ctx, const struct sockaddr_in *dst,
		      const void *buf, size_t len)
{
	(void)ctx;
	(void)dst;
	(void)buf;
	(void)len;
	return -ENOSYS;
}

const struct uib_transport_ops uib_afxdp_ops = {
	.name  = "udp-zc-afxdp",
	.probe = afxdp_probe,
	.init  = afxdp_init,
	.fini  = afxdp_fini,
	.send  = afxdp_send,
};

#endif
