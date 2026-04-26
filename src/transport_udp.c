#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <liburing.h>

#include "uib/transport.h"
#include "uib/context.h"
#include "uib/bth.h"
#include "uib/afxdp_frame.h"

#define UIB_RING_DEPTH 64

struct uib_udp_backend {
	int                 fd;
	struct io_uring     ring;
	void               *recv_buf;
	size_t              recv_buf_len;
	struct sockaddr_in  recv_src;
	struct iovec        recv_iov;
	struct msghdr       recv_msg;
	struct __kernel_timespec ts;
	pthread_t           worker;
	int                 worker_run;
	int                 raw_mode;
};

static int raw_decode(struct uib_context *ctx, struct uib_udp_backend *be,
		      size_t frame_len, struct sockaddr_in *src,
		      void **rdma_out, size_t *rdma_len_out)
{
	uint8_t *frame = be->recv_buf;
	struct iphdr *ip;
	struct udphdr *udp;
	size_t ip_len, udp_len, rdma_len;
	uint8_t *rdma;
	uint32_t want_le, want, got, internal;
	uint32_t src_ip;
	uint16_t src_port;

	if (frame_len < sizeof(*ip) + sizeof(*udp))
		return -1;
	ip = (struct iphdr *)frame;
	if (ip->version != 4 || ip->protocol != IPPROTO_UDP)
		return -1;
	ip_len = (size_t)ip->ihl * 4;
	if (ip_len < sizeof(*ip) || ip_len + sizeof(*udp) > frame_len)
		return -1;
	udp = (struct udphdr *)(frame + ip_len);
	if (udp->dest != ctx->local_addr.sin_port)
		return -1;
	if (ctx->local_addr.sin_addr.s_addr != htonl(INADDR_ANY) &&
	    ip->daddr != ctx->local_addr.sin_addr.s_addr)
		return -1;
	udp_len = ntohs(udp->len);
	if (udp_len < sizeof(*udp) + UIB_BTH_BYTES + UIB_ICRC_BYTES ||
	    ip_len + udp_len > frame_len)
		return -1;
	rdma = (uint8_t *)(udp + 1);
	rdma_len = udp_len - sizeof(*udp);
	memcpy(&want_le, rdma + rdma_len - UIB_ICRC_BYTES, sizeof(want_le));
	want = le32toh(want_le);
	got = uib_rocev2_icrc_ipv4(ip->saddr, ip->daddr, udp->source, udp->dest,
				  ntohs(ip->id), rdma,
				  rdma_len - UIB_ICRC_BYTES);
	if (got != want)
		return -1;

	src_ip = ip->saddr;
	src_port = udp->source;
	memmove(frame, rdma, rdma_len);
	internal = htonl(uib_icrc(frame, rdma_len - UIB_ICRC_BYTES));
	memcpy(frame + rdma_len - UIB_ICRC_BYTES, &internal, sizeof(internal));
	memset(src, 0, sizeof(*src));
	src->sin_family = AF_INET;
	src->sin_addr.s_addr = src_ip;
	src->sin_port = src_port;
	*rdma_out = frame;
	*rdma_len_out = rdma_len;
	return 0;
}

static int arm_recv(struct uib_udp_backend *be)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&be->ring);

	if (!sqe)
		return -EAGAIN;

	be->recv_iov.iov_base       = be->recv_buf;
	be->recv_iov.iov_len        = be->recv_buf_len;
	be->recv_msg.msg_name       = &be->recv_src;
	be->recv_msg.msg_namelen    = sizeof(be->recv_src);
	be->recv_msg.msg_iov        = &be->recv_iov;
	be->recv_msg.msg_iovlen     = 1;
	be->recv_msg.msg_control    = NULL;
	be->recv_msg.msg_controllen = 0;
	be->recv_msg.msg_flags      = 0;

	io_uring_prep_recvmsg(sqe, be->fd, &be->recv_msg, 0);
	io_uring_sqe_set_data(sqe, (void *)(uintptr_t)UIB_UD_TAG_RECV);
	return 0;
}

static int arm_timer(struct uib_udp_backend *be)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&be->ring);

	if (!sqe)
		return -EAGAIN;
	be->ts.tv_sec  = UIB_TIMER_TICK_MS / 1000;
	be->ts.tv_nsec = (UIB_TIMER_TICK_MS % 1000) * 1000000;
	io_uring_prep_timeout(sqe, &be->ts, 0, 0);
	io_uring_sqe_set_data(sqe, (void *)(uintptr_t)UIB_UD_TAG_TIMER);
	return 0;
}

static void *worker_loop(void *arg)
{
	struct uib_context *ctx = arg;
	struct uib_udp_backend *be = ctx->backend;
	int recv_armed = 0, timer_armed = 0;

	while (__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE)) {
		if (!recv_armed && arm_recv(be) == 0)
			recv_armed = 1;
		if (!timer_armed && arm_timer(be) == 0)
			timer_armed = 1;
		io_uring_submit(&be->ring);

		struct io_uring_cqe *cqe;
		int ret = io_uring_wait_cqe(&be->ring, &cqe);

		if (ret < 0)
			continue;

		uintptr_t tag = (uintptr_t)io_uring_cqe_get_data(cqe);
		int res       = cqe->res;

		io_uring_cqe_seen(&be->ring, cqe);

		if (!__atomic_load_n(&be->worker_run, __ATOMIC_ACQUIRE))
			break;

		if (tag == UIB_UD_TAG_RECV) {
			recv_armed = 0;
			if (res > 0) {
				void *rdma = be->recv_buf;
				size_t rdma_len = (size_t)res;
				struct sockaddr_in src = be->recv_src;

				if (!be->raw_mode ||
				    raw_decode(ctx, be, (size_t)res, &src, &rdma,
					       &rdma_len) == 0)
					uib_transport_deliver(ctx, &src, rdma, rdma_len);
			}
		} else if (tag == UIB_UD_TAG_TIMER) {
			timer_armed = 0;
			uib_transport_retransmit_walk(ctx);
		}
	}
	return NULL;
}

static int udp_probe(void)
{
	return 1;
}

static int udp_init(struct uib_context *ctx)
{
	struct uib_udp_backend *be = calloc(1, sizeof(*be));

	if (!be)
		return -ENOMEM;
	be->fd = -1;
	ctx->backend = be;

	ctx->local_addr.sin_family = AF_INET;
	ctx->local_addr.sin_port   =
		htons(ctx->bind_port ? ctx->bind_port : UIB_ROCE_V2_UDP_DPORT);
	if (ctx->bind_ip[0]) {
		if (inet_pton(AF_INET, ctx->bind_ip, &ctx->local_addr.sin_addr) != 1)
			goto err;
	} else {
		ctx->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	}

	be->fd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
	if (be->fd >= 0) {
		int one = 1;

		if (setsockopt(be->fd, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0)
			goto err;
		be->raw_mode = 1;
	} else {
		be->fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (be->fd < 0)
			goto err;
		int one = 1;

		setsockopt(be->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		if (bind(be->fd, (struct sockaddr *)&ctx->local_addr,
			 sizeof(ctx->local_addr)) < 0)
			goto err;

		socklen_t sl = sizeof(ctx->local_addr);

		getsockname(be->fd, (struct sockaddr *)&ctx->local_addr, &sl);
	}

	if (io_uring_queue_init(UIB_RING_DEPTH, &be->ring, 0) < 0)
		goto err;

	be->recv_buf_len = UIB_RECV_BUF_LEN;
	be->recv_buf     = malloc(be->recv_buf_len);
	if (!be->recv_buf)
		goto err_ring;

	__atomic_store_n(&be->worker_run, 1, __ATOMIC_RELEASE);
	if (pthread_create(&be->worker, NULL, worker_loop, ctx) != 0) {
		__atomic_store_n(&be->worker_run, 0, __ATOMIC_RELEASE);
		goto err_buf;
	}
	return 0;

err_buf:
	free(be->recv_buf);
err_ring:
	io_uring_queue_exit(&be->ring);
err:
	if (be->fd >= 0)
		close(be->fd);
	free(be);
	ctx->backend = NULL;
	return -errno;
}

static void udp_fini(struct uib_context *ctx)
{
	struct uib_udp_backend *be = ctx->backend;

	if (!be)
		return;

	if (__atomic_exchange_n(&be->worker_run, 0, __ATOMIC_ACQ_REL)) {
		if (be->fd >= 0)
			shutdown(be->fd, SHUT_RDWR);
		pthread_join(be->worker, NULL);
	}

	io_uring_queue_exit(&be->ring);
	free(be->recv_buf);
	if (be->fd >= 0)
		close(be->fd);
	free(be);
	ctx->backend = NULL;
}

static int udp_send(struct uib_context *ctx, const struct sockaddr_in *dst,
		    const void *buf, size_t len)
{
	struct uib_udp_backend *be = ctx->backend;

	if (be->raw_mode) {
		struct iphdr *ip;
		struct udphdr *udp;
		uint8_t *packet;
		size_t packet_len;
		uint32_t src_ip = ctx->local_addr.sin_addr.s_addr;
		uint32_t crc, wire_crc;

		if (len < UIB_BTH_BYTES + UIB_ICRC_BYTES ||
		    len > UINT16_MAX - sizeof(*ip) - sizeof(*udp))
			return -EMSGSIZE;
		if (src_ip == htonl(INADDR_ANY)) {
			int route_fd = socket(AF_INET, SOCK_DGRAM, 0);
			struct sockaddr_in local;
			socklen_t local_len = sizeof(local);

			if (route_fd < 0)
				return -errno;
			if (connect(route_fd, (const struct sockaddr *)dst, sizeof(*dst)) < 0 ||
			    getsockname(route_fd, (struct sockaddr *)&local, &local_len) < 0) {
				int saved = errno;

				close(route_fd);
				return -saved;
			}
			close(route_fd);
			src_ip = local.sin_addr.s_addr;
		}

		packet_len = sizeof(*ip) + sizeof(*udp) + len;
		packet = malloc(packet_len);
		if (!packet)
			return -ENOMEM;
		ip = (struct iphdr *)packet;
		udp = (struct udphdr *)(ip + 1);
		memset(ip, 0, sizeof(*ip));
		ip->version = 4;
		ip->ihl = 5;
		ip->tot_len = htons((uint16_t)packet_len);
		ip->id = 0;
		ip->frag_off = htons(0x4000);
		ip->ttl = 64;
		ip->protocol = IPPROTO_UDP;
		ip->saddr = src_ip;
		ip->daddr = dst->sin_addr.s_addr;
		ip->check = htons(uib_inet_csum(ip, sizeof(*ip)));
		memset(udp, 0, sizeof(*udp));
		udp->source = ctx->local_addr.sin_port;
		udp->dest = dst->sin_port ? dst->sin_port : htons(UIB_ROCE_V2_UDP_DPORT);
		udp->len = htons((uint16_t)(sizeof(*udp) + len));
		memcpy(udp + 1, buf, len);
		crc = uib_rocev2_icrc_ipv4(ip->saddr, ip->daddr, udp->source,
					      udp->dest, 0, udp + 1,
					      len - UIB_ICRC_BYTES);
		wire_crc = htole32(crc);
		memcpy(packet + packet_len - UIB_ICRC_BYTES, &wire_crc,
		       sizeof(wire_crc));
		ssize_t n = sendto(be->fd, packet, packet_len, 0,
				   (const struct sockaddr *)dst, sizeof(*dst));
		int saved = errno;

		free(packet);
		if (n < 0)
			return -saved;
		return (size_t)n == packet_len ? 0 : -EIO;
	}

	ssize_t n = sendto(be->fd, buf, len, 0,
			   (const struct sockaddr *)dst, sizeof(*dst));
	if (n < 0)
		return -errno;
	if ((size_t)n != len)
		return -EIO;
	return 0;
}

const struct uib_transport_ops uib_udp_ops = {
	.name  = "udp-blocking",
	.probe = udp_probe,
	.init  = udp_init,
	.fini  = udp_fini,
	.send  = udp_send,
};
