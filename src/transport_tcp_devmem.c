// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/env.h"
#include "uib/protocol.h"
#include "uib/mr.h"
#include "uib/devmem.h"

#define UIB_TCP_MAX_PEERS 64
#define UIB_TCP_MAX_FRAME 65536
#define UIB_TCP_RXCHUNK   65536
#define UIB_TCP_BACKLOG   16

#define EP_TAG_LISTEN ((void *)0)
#define EP_TAG_TIMER  ((void *)1)

struct tcp_peer {
	int                 used;
	int                 fd;
	int                 zerocopy_enabled;
	uint32_t            zerocopy_sequence;
	int                 hs_expect;
	/* The peer listening address is both the key and delivery source. */
	struct sockaddr_in  laddr;
	uint8_t            *rx;
	size_t              rx_len;
	size_t              rx_cap;
};

struct uib_tcp_hs {
	uint32_t ip;
	uint16_t port;
	uint16_t pad;
};

struct uib_tcp_backend {
	int                 listen_fd;
	int                 epfd;
	int                 timer_fd;
	struct sockaddr_in  local;
	struct tcp_peer     peers[UIB_TCP_MAX_PEERS];
	pthread_mutex_t     lock;
	pthread_t           worker;
	int                 worker_run;
};

static int write_all(int fd, const struct iovec *iov_in, int n)
{
	struct iovec store[2];
	struct iovec *iov = store;
	int i;

	for (i = 0; i < n; i++)
		store[i] = iov_in[i];
	while (n > 0) {
		ssize_t w = writev(fd, iov, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		while (n > 0 && (size_t)w >= iov[0].iov_len) {
			w -= (ssize_t)iov[0].iov_len;
			iov++;
			n--;
		}
		if (n > 0 && w > 0) {
			iov[0].iov_base = (char *)iov[0].iov_base + w;
			iov[0].iov_len -= (size_t)w;
		}
	}
	return 0;
}

static struct tcp_peer *peer_alloc(struct uib_tcp_backend *be)
{
	int i;

	for (i = 0; i < UIB_TCP_MAX_PEERS; i++) {
		if (!be->peers[i].used) {
			memset(&be->peers[i], 0, sizeof(be->peers[i]));
			be->peers[i].used = 1;
			be->peers[i].fd = -1;
			return &be->peers[i];
		}
	}
	return NULL;
}

/* Provide a total order on IPv4 endpoints for connect-race tie-breaking. */
static int addr_cmp(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	uint32_t ia = ntohl(a->sin_addr.s_addr), ib = ntohl(b->sin_addr.s_addr);

	if (ia != ib)
		return ia < ib ? -1 : 1;
	uint16_t pa = ntohs(a->sin_port), pb = ntohs(b->sin_port);

	if (pa != pb)
		return pa < pb ? -1 : 1;
	return 0;
}

static struct tcp_peer *peer_by_addr(struct uib_tcp_backend *be,
				     const struct sockaddr_in *a)
{
	int i;

	for (i = 0; i < UIB_TCP_MAX_PEERS; i++) {
		struct tcp_peer *p = &be->peers[i];

		if (p->used && !p->hs_expect &&
		    p->laddr.sin_addr.s_addr == a->sin_addr.s_addr &&
		    p->laddr.sin_port == a->sin_port)
			return p;
	}
	return NULL;
}

static void peer_drop(struct uib_tcp_backend *be, struct tcp_peer *p)
{
	if (!p->used)
		return;
	epoll_ctl(be->epfd, EPOLL_CTL_DEL, p->fd, NULL);
	close(p->fd);
	free(p->rx);
	memset(p, 0, sizeof(*p));
}

static struct tcp_peer *tcp_connect(struct uib_context *ctx,
				    const struct sockaddr_in *dst)
{
	struct uib_tcp_backend *be = ctx->backend;
	struct epoll_event ev;
	struct uib_tcp_hs hs;
	struct iovec iov;
	struct tcp_peer *p;
	int zc;
	int fd;
	int one = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	zc = uib_zerocopy_enable_socket(ctx, fd);
	if (zc < 0) {
		close(fd);
		return NULL;
	}
	if (connect(fd, (const struct sockaddr *)dst, sizeof(*dst)) < 0) {
		close(fd);
		return NULL;
	}

	hs.ip   = be->local.sin_addr.s_addr;
	hs.port = be->local.sin_port;
	hs.pad  = 0;
	iov.iov_base = &hs;
	iov.iov_len  = sizeof(hs);
	if (write_all(fd, &iov, 1) < 0) {
		close(fd);
		return NULL;
	}

	p = peer_alloc(be);
	if (!p) {
		close(fd);
		return NULL;
	}
	p->fd = fd;
	p->zerocopy_enabled = zc;
	p->laddr = *dst;
	p->hs_expect = 0;

	ev.events = EPOLLIN;
	ev.data.ptr = p;
	epoll_ctl(be->epfd, EPOLL_CTL_ADD, fd, &ev);
	return p;
}

static int tcp_send(struct uib_context *ctx, const struct sockaddr_in *dst,
		    const void *buf, size_t len)
{
	struct uib_tcp_backend *be = ctx->backend;
	struct msghdr message = {0};
	struct iovec iov;
	struct tcp_peer *p;
	uint8_t *frame;
	uint32_t nlen;
	ssize_t sent;
	int rc;

	if (len == 0 || len > UIB_TCP_MAX_FRAME)
		return -EMSGSIZE;

	pthread_mutex_lock(&be->lock);
	p = peer_by_addr(be, dst);
	if (!p)
		p = tcp_connect(ctx, dst);
	if (!p) {
		pthread_mutex_unlock(&be->lock);
		return -EIO;
	}
	frame = malloc(sizeof(uint32_t) + len);
	if (!frame) {
		pthread_mutex_unlock(&be->lock);
		return -ENOMEM;
	}
	nlen = htonl((uint32_t)len);
	memcpy(frame, &nlen, sizeof(nlen));
	memcpy(frame + sizeof(uint32_t), buf, len);
	iov.iov_base = frame;
	iov.iov_len = sizeof(uint32_t) + len;
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	sent = uib_zerocopy_sendmsg(ctx, p->fd, &message, MSG_NOSIGNAL,
				    p->zerocopy_enabled,
				    &p->zerocopy_sequence);
	if (sent < 0) {
		rc = (int)sent;
	} else if ((size_t)sent == iov.iov_len) {
		rc = 0;
	} else {
		iov.iov_base = frame + sent;
		iov.iov_len -= (size_t)sent;
		rc = write_all(p->fd, &iov, 1);
		if (!rc)
			uib_zerocopy_account_copy(ctx, iov.iov_len);
	}
	if (rc < 0)
		peer_drop(be, p);
	free(frame);
	pthread_mutex_unlock(&be->lock);
	return rc;
}

/* Only the worker thread accesses the receive buffer. */
static int peer_parse(struct uib_context *ctx, struct tcp_peer *p)
{
	struct uib_tcp_backend *be = ctx->backend;
	size_t off = 0;

	if (p->hs_expect) {
		struct uib_tcp_hs hs;
		int i;

		if (p->rx_len < sizeof(hs))
			return 0;
		memcpy(&hs, p->rx, sizeof(hs));
		memset(&p->laddr, 0, sizeof(p->laddr));
		p->laddr.sin_family = AF_INET;
		p->laddr.sin_addr.s_addr = hs.ip;
		p->laddr.sin_port = hs.port;
		p->hs_expect = 0;
		off = sizeof(hs);

		/*
		 * Connect-race resolution: if we already have a connection to this
		 * peer (one we initiated), keep exactly one. The endpoint with the
		 * lower address keeps the connection it initiated and drops this
		 * inbound one; the higher keeps this inbound and drops its outbound.
		 */
		pthread_mutex_lock(&be->lock);
		for (i = 0; i < UIB_TCP_MAX_PEERS; i++) {
			struct tcp_peer *q = &be->peers[i];

			if (q == p || !q->used || q->hs_expect)
				continue;
			if (addr_cmp(&q->laddr, &p->laddr) != 0)
				continue;
			if (addr_cmp(&be->local, &p->laddr) < 0) {
				pthread_mutex_unlock(&be->lock);
				return -1;
			}
			peer_drop(be, q);
			break;
		}
		pthread_mutex_unlock(&be->lock);
	}

	while (p->rx_len - off >= 4) {
		uint32_t flen;

		memcpy(&flen, p->rx + off, 4);
		flen = ntohl(flen);
		if (flen == 0 || flen > UIB_TCP_MAX_FRAME)
			return -1;
		if (p->rx_len - off < 4 + flen)
			break;
		uib_transport_deliver(ctx, &p->laddr, p->rx + off + 4, flen);
		off += 4 + flen;
	}

	if (off) {
		memmove(p->rx, p->rx + off, p->rx_len - off);
		p->rx_len -= off;
	}
	return 0;
}

static int peer_readable(struct uib_context *ctx, struct tcp_peer *p)
{
	uint8_t chunk[UIB_TCP_RXCHUNK];
	ssize_t n;

	n = recv(p->fd, chunk, sizeof(chunk), 0);
	if (n <= 0)
		return -1;

	if (p->rx_len + (size_t)n > p->rx_cap) {
		size_t nc = p->rx_cap ? p->rx_cap * 2 : UIB_TCP_RXCHUNK;

		while (nc < p->rx_len + (size_t)n)
			nc *= 2;
		uint8_t *nb = realloc(p->rx, nc);

		if (!nb)
			return -1;
		p->rx = nb;
		p->rx_cap = nc;
	}
	memcpy(p->rx + p->rx_len, chunk, (size_t)n);
	p->rx_len += (size_t)n;
	return peer_parse(ctx, p);
}

static void tcp_accept(struct uib_context *ctx)
{
	struct uib_tcp_backend *be = ctx->backend;
	struct epoll_event ev;
	struct tcp_peer *p;
	int zc;
	int fd;
	int one = 1;

	fd = accept(be->listen_fd, NULL, NULL);
	if (fd < 0)
		return;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	zc = uib_zerocopy_enable_socket(ctx, fd);
	if (zc < 0) {
		close(fd);
		return;
	}

	pthread_mutex_lock(&be->lock);
	p = peer_alloc(be);
	pthread_mutex_unlock(&be->lock);
	if (!p) {
		close(fd);
		return;
	}
	p->fd = fd;
	p->zerocopy_enabled = zc;
	p->hs_expect = 1;
	ev.events = EPOLLIN;
	ev.data.ptr = p;
	epoll_ctl(be->epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void *worker_loop(void *arg)
{
	struct uib_context *ctx = arg;
	struct uib_tcp_backend *be = ctx->backend;
	struct epoll_event evs[16];

	while (__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE)) {
		int n = epoll_wait(be->epfd, evs, 16, 200);
		int i;

		if (!__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE))
			break;
		for (i = 0; i < n; i++) {
			void *tag = evs[i].data.ptr;

			if (tag == EP_TAG_LISTEN) {
				tcp_accept(ctx);
			} else if (tag == EP_TAG_TIMER) {
				uint64_t exp;

				if (read(be->timer_fd, &exp, sizeof(exp)) == sizeof(exp))
					uib_transport_retransmit_walk(ctx);
			} else {
				struct tcp_peer *p = tag;

				if (peer_readable(ctx, p) < 0) {
					pthread_mutex_lock(&be->lock);
					peer_drop(be, p);
					pthread_mutex_unlock(&be->lock);
				}
			}
		}
	}
	return NULL;
}

static int tcp_probe(void)
{
	return 1;
}

static int tcp_init(struct uib_context *ctx)
{
	struct uib_tcp_backend *be;
	struct epoll_event ev;
	int one = 1, rc;

	be = calloc(1, sizeof(*be));
	if (!be)
		return -ENOMEM;
	be->listen_fd = be->epfd = be->timer_fd = -1;
	pthread_mutex_init(&be->lock, NULL);
	ctx->backend = be;

	be->local.sin_family = AF_INET;
	be->local.sin_port =
		htons(ctx->bind_port ? ctx->bind_port : ctx->pops->default_port);
	if (ctx->bind_ip[0]) {
		if (inet_pton(AF_INET, ctx->bind_ip, &be->local.sin_addr) != 1) {
			rc = -EINVAL;
			goto err;
		}
	} else {
		be->local.sin_addr.s_addr = htonl(INADDR_ANY);
	}

	be->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (be->listen_fd < 0) {
		rc = -errno;
		goto err;
	}
	setsockopt(be->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(be->listen_fd, (struct sockaddr *)&be->local,
		 sizeof(be->local)) < 0) {
		rc = -errno;
		goto err;
	}

	socklen_t sl = sizeof(be->local);

	getsockname(be->listen_fd, (struct sockaddr *)&be->local, &sl);
	ctx->local_addr = be->local;

	if (listen(be->listen_fd, UIB_TCP_BACKLOG) < 0) {
		rc = -errno;
		goto err;
	}

	be->epfd = epoll_create1(0);
	if (be->epfd < 0) {
		rc = -errno;
		goto err;
	}

	ev.events = EPOLLIN;
	ev.data.ptr = EP_TAG_LISTEN;
	epoll_ctl(be->epfd, EPOLL_CTL_ADD, be->listen_fd, &ev);

	be->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (be->timer_fd >= 0) {
		struct itimerspec its;

		memset(&its, 0, sizeof(its));
		its.it_interval.tv_nsec = UIB_TIMER_TICK_MS * 1000000L;
		its.it_value = its.it_interval;
		timerfd_settime(be->timer_fd, 0, &its, NULL);
		ev.events = EPOLLIN;
		ev.data.ptr = EP_TAG_TIMER;
		epoll_ctl(be->epfd, EPOLL_CTL_ADD, be->timer_fd, &ev);
	}

	__atomic_store_n(&be->worker_run, 1, __ATOMIC_RELEASE);
	if (pthread_create(&be->worker, NULL, worker_loop, ctx) != 0) {
		__atomic_store_n(&be->worker_run, 0, __ATOMIC_RELEASE);
		rc = -errno;
		goto err;
	}
	return 0;

err:
	if (be->epfd >= 0)
		close(be->epfd);
	if (be->listen_fd >= 0)
		close(be->listen_fd);
	if (be->timer_fd >= 0)
		close(be->timer_fd);
	pthread_mutex_destroy(&be->lock);
	free(be);
	ctx->backend = NULL;
	return rc;
}

static void tcp_fini(struct uib_context *ctx)
{
	struct uib_tcp_backend *be = ctx->backend;
	int i;

	if (!be)
		return;
	if (__atomic_exchange_n(&be->worker_run, 0, __ATOMIC_ACQ_REL))
		pthread_join(be->worker, NULL);

	for (i = 0; i < UIB_TCP_MAX_PEERS; i++) {
		if (be->peers[i].used) {
			close(be->peers[i].fd);
			free(be->peers[i].rx);
		}
	}
	if (be->epfd >= 0)
		close(be->epfd);
	if (be->listen_fd >= 0)
		close(be->listen_fd);
	if (be->timer_fd >= 0)
		close(be->timer_fd);
	pthread_mutex_destroy(&be->lock);
	free(be);
	ctx->backend = NULL;
}

static int tcp_mr_register(struct uib_context *ctx, struct uib_mr *mr)
{
	const char *ifn = getenv(UIB_ENV_DEVMEM_IFACE);
	const char *qs  = getenv(UIB_ENV_DEVMEM_QUEUE);
	struct uib_devmem_binding *b;
	int rc;

	(void)ctx;
	if (!ifn || !*ifn)
		return -ENODEV;
	b = calloc(1, sizeof(*b));
	if (!b)
		return -ENOMEM;
	rc = uib_devmem_bind_rx(ifn, qs ? (uint32_t)atoi(qs) : 0, mr->dmabuf_fd, b);
	if (rc < 0) {
		free(b);
		return rc;
	}
	mr->backend_mr = b;
	return 0;
}

static void tcp_mr_deregister(struct uib_context *ctx, struct uib_mr *mr)
{
	struct uib_devmem_binding *b = mr->backend_mr;

	(void)ctx;
	if (b) {
		uib_devmem_unbind(b);
		free(b);
		mr->backend_mr = NULL;
	}
}

const struct uib_transport_ops uib_tcp_devmem_ops = {
	.name          = "tcp-devmem",
	.probe         = tcp_probe,
	.init          = tcp_init,
	.fini          = tcp_fini,
	.send          = tcp_send,
	.mr_register   = tcp_mr_register,
	.mr_deregister = tcp_mr_deregister,
};
