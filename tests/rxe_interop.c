#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CTRL_PORT 18555
#define BUF_SIZE  16384
#define MSG_LEN   2049
#define SEND_OFF  0
#define RECV_OFF  4096
#define TARGET_OFF 8192
#define READ_OFF  12288

struct endpoint {
	uint32_t qpn;
	uint32_t psn;
	uint32_t rkey;
	uint64_t addr;
	union ibv_gid gid;
};

struct peer {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	struct ibv_mr *mr;
	uint8_t *buf;
	int ctrl;
};

static int full_io(int fd, void *buf, size_t len, int write_it)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = write_it ? write(fd, p, len) : read(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int send_byte(int fd, char value)
{
	return full_io(fd, &value, 1, 1);
}

static int recv_byte(int fd, char expected)
{
	char value;

	if (full_io(fd, &value, 1, 0) || value != expected)
		return -1;
	return 0;
}

static int open_control(int server, const char *host)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr = { .sin_family = AF_INET,
		.sin_port = htons(CTRL_PORT) };
	int one = 1;

	if (fd < 0)
		return -1;
	if (server) {
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 1)) {
			close(fd);
			return -1;
		}
		int accepted = accept(fd, NULL, NULL);

		close(fd);
		return accepted;
	}
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		close(fd);
		return -1;
	}
	for (int i = 0; i < 100; i++) {
		if (!connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
			return fd;
		if (errno != ECONNREFUSED)
			break;
		struct timespec delay = { .tv_nsec = 20000000 };

		nanosleep(&delay, NULL);
	}
	close(fd);
	return -1;
}

static struct ibv_device *find_device(struct ibv_device **list, int count,
				      const char *name)
{
	for (int i = 0; i < count; i++)
		if (!strcmp(ibv_get_device_name(list[i]), name))
			return list[i];
	return NULL;
}

static void fill_pattern(uint8_t *buf, uint8_t seed)
{
	for (size_t i = 0; i < MSG_LEN; i++)
		buf[i] = (uint8_t)(seed + i * 13u);
}

static int check_pattern(const char *name, const uint8_t *buf, uint8_t seed)
{
	for (size_t i = 0; i < MSG_LEN; i++) {
		uint8_t expected = (uint8_t)(seed + i * 13u);

		if (buf[i] != expected) {
			fprintf(stderr, "%s mismatch at %zu: got=%02x expected=%02x\n",
				name, i, buf[i], expected);
			return -1;
		}
	}
	printf("%-24s MATCH (%d bytes)\n", name, MSG_LEN);
	return 0;
}

static int wait_wc(struct peer *p, uint64_t wr_id)
{
	for (int i = 0; i < 10000; i++) {
		struct ibv_wc wc;
		int n = ibv_poll_cq(p->cq, 1, &wc);

		if (n < 0)
			return -1;
		if (n == 1) {
			printf("WC wr_id=%llu status=%s opcode=%d len=%u\n",
				(unsigned long long)wc.wr_id,
				ibv_wc_status_str(wc.status), wc.opcode, wc.byte_len);
			return wc.wr_id == wr_id && wc.status == IBV_WC_SUCCESS ? 0 : -1;
		}
		usleep(1000);
	}
	fprintf(stderr, "completion timeout for wr_id=%llu\n",
		(unsigned long long)wr_id);
	return -1;
}

static int post_recv(struct peer *p, uint64_t wr_id)
{
	struct ibv_sge sge = { .addr = (uintptr_t)(p->buf + RECV_OFF),
		.length = MSG_LEN, .lkey = p->mr->lkey };
	struct ibv_recv_wr wr = { .wr_id = wr_id, .sg_list = &sge, .num_sge = 1 };
	struct ibv_recv_wr *bad;

	return ibv_post_recv(p->qp, &wr, &bad);
}

static int post_op(struct peer *p, uint64_t wr_id, enum ibv_wr_opcode opcode,
		   uint32_t local_off, const struct endpoint *remote)
{
	struct ibv_sge sge = { .addr = (uintptr_t)(p->buf + local_off),
		.length = MSG_LEN, .lkey = p->mr->lkey };
	struct ibv_send_wr wr = { .wr_id = wr_id, .sg_list = &sge, .num_sge = 1,
		.opcode = opcode, .send_flags = IBV_SEND_SIGNALED };
	struct ibv_send_wr *bad;

	if (opcode == IBV_WR_RDMA_WRITE || opcode == IBV_WR_RDMA_READ) {
		wr.wr.rdma.remote_addr = remote->addr;
		wr.wr.rdma.rkey = remote->rkey;
	}
	if (ibv_post_send(p->qp, &wr, &bad))
		return -1;
	return wait_wc(p, wr_id);
}

static int connect_qp(struct peer *p, const struct endpoint *local,
		      const struct endpoint *remote, int gid_index)
{
	struct ibv_qp_attr attr = {0};

	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = 1;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
		IBV_ACCESS_REMOTE_READ;
	if (ibv_modify_qp(p->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
		IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
		return -1;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_1024;
	attr.dest_qp_num = remote->qpn;
	attr.rq_psn = remote->psn;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 1;
	attr.ah_attr.port_num = 1;
	attr.ah_attr.grh.dgid = remote->gid;
	attr.ah_attr.grh.sgid_index = gid_index;
	attr.ah_attr.grh.hop_limit = 64;
	if (ibv_modify_qp(p->qp, &attr, IBV_QP_STATE | IBV_QP_AV |
		IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
		IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
		return -1;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.sq_psn = local->psn;
	attr.timeout = 12;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.max_rd_atomic = 1;
	return ibv_modify_qp(p->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN |
		IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
		IBV_QP_MAX_QP_RD_ATOMIC);
}

static int run_client(struct peer *p, const struct endpoint *remote)
{
	fill_pattern(p->buf + SEND_OFF, 0x21);
	if (post_op(p, 0x201, IBV_WR_SEND, SEND_OFF, remote) ||
	    send_byte(p->ctrl, 's'))
		return -1;
	fill_pattern(p->buf + SEND_OFF, 0x32);
	if (post_op(p, 0x202, IBV_WR_RDMA_WRITE, SEND_OFF, remote) ||
	    send_byte(p->ctrl, 'w'))
		return -1;
	memset(p->buf + READ_OFF, 0, MSG_LEN);
	if (post_op(p, 0x203, IBV_WR_RDMA_READ, READ_OFF, remote) ||
	    check_pattern("uib.read-from-rxe", p->buf + READ_OFF, 0x32) ||
	    send_byte(p->ctrl, 'r'))
		return -1;
	if (wait_wc(p, 0x204) ||
	    check_pattern("uib.recv-from-rxe", p->buf + RECV_OFF, 0x43) ||
	    recv_byte(p->ctrl, 'S') || recv_byte(p->ctrl, 'W') ||
	    check_pattern("uib.target-written", p->buf + TARGET_OFF, 0x54) ||
	    recv_byte(p->ctrl, 'R'))
		return -1;
	return 0;
}

static int run_server(struct peer *p, const struct endpoint *remote)
{
	if (wait_wc(p, 0x101) ||
	    check_pattern("rxe.recv-from-uib", p->buf + RECV_OFF, 0x21) ||
	    recv_byte(p->ctrl, 's') || recv_byte(p->ctrl, 'w') ||
	    check_pattern("rxe.target-written", p->buf + TARGET_OFF, 0x32) ||
	    recv_byte(p->ctrl, 'r'))
		return -1;
	fill_pattern(p->buf + SEND_OFF, 0x43);
	if (post_op(p, 0x102, IBV_WR_SEND, SEND_OFF, remote) ||
	    send_byte(p->ctrl, 'S'))
		return -1;
	fill_pattern(p->buf + SEND_OFF, 0x54);
	if (post_op(p, 0x103, IBV_WR_RDMA_WRITE, SEND_OFF, remote) ||
	    send_byte(p->ctrl, 'W'))
		return -1;
	memset(p->buf + READ_OFF, 0, MSG_LEN);
	if (post_op(p, 0x104, IBV_WR_RDMA_READ, READ_OFF, remote) ||
	    check_pattern("rxe.read-from-uib", p->buf + READ_OFF, 0x54) ||
	    send_byte(p->ctrl, 'R'))
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	int server, gid_index, count = 0, rc = 2;
	const char *host = NULL;
	struct ibv_device **list = NULL, *dev;
	struct peer p = { .ctrl = -1 };
	struct endpoint local = {0}, remote;
	struct ibv_qp_init_attr init = {0};

	if (argc < 4 || (strcmp(argv[1], "server") && strcmp(argv[1], "client"))) {
		fprintf(stderr, "usage: %s server|client DEVICE GID_INDEX [SERVER_IP]\n", argv[0]);
		return 2;
	}
	server = !strcmp(argv[1], "server");
	gid_index = atoi(argv[3]);
	if (!server) {
		if (argc != 5)
			return 2;
		host = argv[4];
	}
	list = ibv_get_device_list(&count);
	dev = find_device(list, count, argv[2]);
	if (!dev) {
		fprintf(stderr, "verbs setup failed for %s\n", argv[2]);
		goto out;
	}
	p.ctx = ibv_open_device(dev);
	if (!p.ctx) {
		fprintf(stderr, "verbs setup failed for %s\n", argv[2]);
		goto out;
	}
	p.pd = ibv_alloc_pd(p.ctx);
	if (!p.pd) {
		fprintf(stderr, "verbs setup failed for %s\n", argv[2]);
		goto out;
	}
	p.cq = ibv_create_cq(p.ctx, 32, NULL, NULL, 0);
	if (!p.cq) {
		fprintf(stderr, "verbs setup failed for %s\n", argv[2]);
		goto out;
	}
	if (posix_memalign((void **)&p.buf, 4096, BUF_SIZE))
		goto out;
	memset(p.buf, 0, BUF_SIZE);
	p.mr = ibv_reg_mr(p.pd, p.buf, BUF_SIZE, IBV_ACCESS_LOCAL_WRITE |
		IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (!p.mr)
		goto out;
	init.send_cq = p.cq;
	init.recv_cq = p.cq;
	init.qp_type = IBV_QPT_RC;
	init.cap.max_send_wr = init.cap.max_recv_wr = 16;
	init.cap.max_send_sge = init.cap.max_recv_sge = 1;
	p.qp = ibv_create_qp(p.pd, &init);
	if (!p.qp)
		goto out;
	local.qpn = p.qp->qp_num;
	local.psn = server ? 0x123456 : 0x654321;
	local.rkey = p.mr->rkey;
	local.addr = (uintptr_t)(p.buf + TARGET_OFF);
	if (ibv_query_gid(p.ctx, 1, gid_index, &local.gid)) {
		fprintf(stderr, "query_gid failed\n");
		goto out;
	}
	p.ctrl = open_control(server, host);
	if (p.ctrl < 0 || full_io(p.ctrl, &local, sizeof(local), 1) ||
	    full_io(p.ctrl, &remote, sizeof(remote), 0) ||
	    connect_qp(&p, &local, &remote, gid_index)) {
		fprintf(stderr, "connection setup failed\n");
		goto out;
	}
	if (post_recv(&p, server ? 0x101 : 0x204) ||
	    send_byte(p.ctrl, 'Q') || recv_byte(p.ctrl, 'Q'))
		goto out;
	rc = server ? run_server(&p, &remote) : run_client(&p, &remote);
	printf("RESULT %s\n", rc ? "FAIL" : "PASS");
out:
	if (p.ctrl >= 0)
		close(p.ctrl);
	if (p.qp)
		ibv_destroy_qp(p.qp);
	if (p.mr)
		ibv_dereg_mr(p.mr);
	if (p.cq)
		ibv_destroy_cq(p.cq);
	if (p.pd)
		ibv_dealloc_pd(p.pd);
	if (p.ctx)
		ibv_close_device(p.ctx);
	free(p.buf);
	if (list)
		ibv_free_device_list(list);
	return rc ? 1 : 0;
}
