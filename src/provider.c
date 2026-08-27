/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/eventfd.h>

#include <infiniband/verbs.h>

#undef ibv_reg_mr
#undef ibv_reg_mr_iova
#undef ibv_reg_dmabuf_mr
#undef ibv_query_port

#include "uib/uib.h"

#define UIBV_DEV_NAME "uib0"
#define UIBV_WR_INLINE_COUNT 16U
#define UIBV_MAX_INLINE_DATA 4096U

struct uibv_dev_pub {
	struct ibv_device ibv;
};

static struct uibv_dev_pub g_uibv_dev = {
	.ibv = {
		.node_type = IBV_NODE_CA,
		.transport_type = IBV_TRANSPORT_IB,
		.name        = "uib0",
		.dev_name    = "uib0",
		.dev_path    = "/dev/null",
		.ibdev_path  = "/sys/class/uib/uib0",
	},
};

struct uibv_ctx_wrap {
	struct ibv_context  ibv;
	struct uib_context *core;
};

struct uibv_pd_wrap {
	struct ibv_pd   ibv;
	struct uib_pd  *core;
};

struct uibv_mr_wrap {
	struct ibv_mr   ibv;
	struct uib_mr  *core;
};

struct uibv_srq_wrap {
	struct ibv_srq  ibv;
	struct uib_srq *core;
};

struct uibv_channel_wrap {
	struct ibv_comp_channel  ibv;
	struct uib_comp_channel *core;
};

struct uibv_qp_wrap;

struct uibv_cq_wrap {
	struct ibv_cq   ibv;
	struct uib_cq  *core;
	struct uibv_qp_wrap *recv_qps;
};

struct uibv_qp_wrap {
	struct ibv_qp   ibv;
	struct uib_qp  *core;
	struct ibv_qp_cap cap;
	struct uibv_cq_wrap *recv_cq;
	struct uibv_qp_wrap *recv_next;
	int sq_sig_all;
};

struct uibv_ah_wrap {
	struct ibv_ah       ibv;
	struct sockaddr_in  remote_addr;
};

struct efadv_device_attr;

#define WRAP_OF(ptr, T, member) \
	((T *)((char *)(ptr) - offsetof(T, member)))

static struct ibv_device **(*real_get_device_list)(int *);
static void                (*real_free_device_list)(struct ibv_device **);
static const char         *(*real_get_device_name)(struct ibv_device *);
static struct ibv_context *(*real_open_device)(struct ibv_device *);
static int                 (*real_close_device)(struct ibv_context *);
static int                 (*real_get_async_event)(struct ibv_context *,
						   struct ibv_async_event *);
static void                (*real_ack_async_event)(struct ibv_async_event *);
static struct ibv_pd      *(*real_alloc_pd)(struct ibv_context *);
static int                 (*real_dealloc_pd)(struct ibv_pd *);
static struct ibv_mr      *(*real_reg_mr)(struct ibv_pd *, void *, size_t, int);
static struct ibv_mr      *(*real_reg_mr_iova)(struct ibv_pd *, void *, size_t,
					       uint64_t, int);
static struct ibv_mr      *(*real_reg_mr_iova2)(struct ibv_pd *, void *, size_t,
						uint64_t, unsigned int);
static struct ibv_mr      *(*real_reg_dmabuf_mr)(struct ibv_pd *, uint64_t,
						 size_t, uint64_t, int, int);
static int                 (*real_dereg_mr)(struct ibv_mr *);
static struct ibv_comp_channel *(*real_create_comp_channel)(
						struct ibv_context *);
static int                 (*real_destroy_comp_channel)(
						struct ibv_comp_channel *);
static int                 (*real_get_cq_event)(struct ibv_comp_channel *,
					       struct ibv_cq **, void **);
static void                (*real_ack_cq_events)(struct ibv_cq *,
						 unsigned int);
static struct ibv_cq      *(*real_create_cq)(struct ibv_context *, int, void *,
					      struct ibv_comp_channel *, int);
static int                 (*real_destroy_cq)(struct ibv_cq *);
static struct ibv_srq     *(*real_create_srq)(struct ibv_pd *,
					      struct ibv_srq_init_attr *);
static int                 (*real_modify_srq)(struct ibv_srq *,
					      struct ibv_srq_attr *, int);
static int                 (*real_query_srq)(struct ibv_srq *,
					     struct ibv_srq_attr *);
static int                 (*real_destroy_srq)(struct ibv_srq *);
static struct ibv_qp      *(*real_create_qp)(struct ibv_pd *,
					     struct ibv_qp_init_attr *);
static struct ibv_ah      *(*real_create_ah)(struct ibv_pd *,
					     struct ibv_ah_attr *);
static int                 (*real_destroy_ah)(struct ibv_ah *);
static int                 (*real_modify_qp)(struct ibv_qp *,
					     struct ibv_qp_attr *, int);
static int                 (*real_destroy_qp)(struct ibv_qp *);
static int                 (*real_query_qp)(struct ibv_qp *,
					    struct ibv_qp_attr *, int,
					    struct ibv_qp_init_attr *);
static int                 (*real_query_device)(struct ibv_context *,
						struct ibv_device_attr *);
static int                 (*real_query_port)(struct ibv_context *, uint8_t,
					       struct _compat_ibv_port_attr *);
static int                 (*real_query_gid)(struct ibv_context *, uint8_t, int,
					      union ibv_gid *);
static int                 (*real_query_pkey)(struct ibv_context *, uint8_t,
					      int, __be16 *);
static int                 (*real_query_ece)(struct ibv_qp *, struct ibv_ece *);
static int                 (*real_set_ece)(struct ibv_qp *, struct ibv_ece *);
static int                 (*real_fork_init)(void);
static int                 (*real_efadv_query_device)(struct ibv_context *,
					       struct efadv_device_attr *,
					       uint32_t);

static int ctx_is_ours(struct ibv_context *ctx)
{
	return ctx && ctx->device == &g_uibv_dev.ibv;
}

__attribute__((constructor))
static void uibv_init(void)
{
	real_get_device_list   = dlsym(RTLD_NEXT, "ibv_get_device_list");
	real_free_device_list  = dlsym(RTLD_NEXT, "ibv_free_device_list");
	real_get_device_name   = dlsym(RTLD_NEXT, "ibv_get_device_name");
	real_open_device       = dlsym(RTLD_NEXT, "ibv_open_device");
	real_close_device      = dlsym(RTLD_NEXT, "ibv_close_device");
	real_get_async_event   = dlsym(RTLD_NEXT, "ibv_get_async_event");
	real_ack_async_event   = dlsym(RTLD_NEXT, "ibv_ack_async_event");
	real_alloc_pd          = dlsym(RTLD_NEXT, "ibv_alloc_pd");
	real_dealloc_pd        = dlsym(RTLD_NEXT, "ibv_dealloc_pd");
	real_reg_mr            = dlsym(RTLD_NEXT, "ibv_reg_mr");
	real_reg_mr_iova       = dlsym(RTLD_NEXT, "ibv_reg_mr_iova");
	real_reg_mr_iova2      = dlsym(RTLD_NEXT, "ibv_reg_mr_iova2");
	real_reg_dmabuf_mr     = dlsym(RTLD_NEXT, "ibv_reg_dmabuf_mr");
	real_dereg_mr          = dlsym(RTLD_NEXT, "ibv_dereg_mr");
	real_create_comp_channel = dlsym(RTLD_NEXT, "ibv_create_comp_channel");
	real_destroy_comp_channel = dlsym(RTLD_NEXT,
					  "ibv_destroy_comp_channel");
	real_get_cq_event      = dlsym(RTLD_NEXT, "ibv_get_cq_event");
	real_ack_cq_events     = dlsym(RTLD_NEXT, "ibv_ack_cq_events");
	real_create_cq         = dlsym(RTLD_NEXT, "ibv_create_cq");
	real_destroy_cq        = dlsym(RTLD_NEXT, "ibv_destroy_cq");
	real_create_srq        = dlsym(RTLD_NEXT, "ibv_create_srq");
	real_modify_srq        = dlsym(RTLD_NEXT, "ibv_modify_srq");
	real_query_srq         = dlsym(RTLD_NEXT, "ibv_query_srq");
	real_destroy_srq       = dlsym(RTLD_NEXT, "ibv_destroy_srq");
	real_create_qp         = dlsym(RTLD_NEXT, "ibv_create_qp");
	real_create_ah         = dlsym(RTLD_NEXT, "ibv_create_ah");
	real_destroy_ah        = dlsym(RTLD_NEXT, "ibv_destroy_ah");
	real_modify_qp         = dlsym(RTLD_NEXT, "ibv_modify_qp");
	real_destroy_qp        = dlsym(RTLD_NEXT, "ibv_destroy_qp");
	real_query_qp          = dlsym(RTLD_NEXT, "ibv_query_qp");
	real_query_device      = dlsym(RTLD_NEXT, "ibv_query_device");
	real_query_port        = dlsym(RTLD_NEXT, "ibv_query_port");
	real_query_gid         = dlsym(RTLD_NEXT, "ibv_query_gid");
	real_query_pkey        = dlsym(RTLD_NEXT, "ibv_query_pkey");
	real_query_ece         = dlsym(RTLD_NEXT, "ibv_query_ece");
	real_set_ece           = dlsym(RTLD_NEXT, "ibv_set_ece");
	real_fork_init         = dlsym(RTLD_NEXT, "ibv_fork_init");
	real_efadv_query_device = dlsym(RTLD_NEXT, "efadv_query_device");
}

static uint32_t mtu_to_bytes(enum ibv_mtu mtu)
{
	switch (mtu) {
	case IBV_MTU_256:
		return 256;
	case IBV_MTU_512:
		return 512;
	case IBV_MTU_1024:
		return 1024;
	case IBV_MTU_2048:
		return 2048;
	case IBV_MTU_4096:
		return 4096;
	default:
		return 0;
	}
}

static enum ibv_mtu bytes_to_mtu(uint32_t mtu)
{
	if (mtu <= 256)
		return IBV_MTU_256;
	if (mtu <= 512)
		return IBV_MTU_512;
	if (mtu <= 1024)
		return IBV_MTU_1024;
	if (mtu <= 2048)
		return IBV_MTU_2048;
	return IBV_MTU_4096;
}

static enum ibv_mtu active_mtu_from_env(void)
{
	const char *value = getenv(UIB_ENV_ACTIVE_MTU);

	if (!value || !*value || !strcmp(value, "4096"))
		return IBV_MTU_4096;
	if (!strcmp(value, "2048"))
		return IBV_MTU_2048;
	if (!strcmp(value, "1024"))
		return IBV_MTU_1024;
	if (!strcmp(value, "512"))
		return IBV_MTU_512;
	if (!strcmp(value, "256"))
		return IBV_MTU_256;
	return IBV_MTU_4096;
}

static enum uib_wr_opcode map_wr_op_in(enum ibv_wr_opcode op)
{
	switch (op) {
	case IBV_WR_SEND:
		return UIB_WR_SEND;
	case IBV_WR_SEND_WITH_IMM:
		return UIB_WR_SEND_WITH_IMM;
	case IBV_WR_RDMA_WRITE:
		return UIB_WR_RDMA_WRITE;
	case IBV_WR_RDMA_WRITE_WITH_IMM:
		return UIB_WR_RDMA_WRITE_WITH_IMM;
	case IBV_WR_RDMA_READ:
		return UIB_WR_RDMA_READ;
	case IBV_WR_ATOMIC_FETCH_AND_ADD:
		return UIB_WR_ATOMIC_FETCH_ADD;
	case IBV_WR_ATOMIC_CMP_AND_SWP:
		return UIB_WR_ATOMIC_CMP_SWAP;
	default:
		return UIB_WR_SEND;
	}
}

static int map_send_flags_in(unsigned int f)
{
	int o = 0;

	if (f & IBV_SEND_SIGNALED)
		o |= UIB_SEND_SIGNALED;
	if (f & IBV_SEND_SOLICITED)
		o |= UIB_SEND_SOLICITED;
	if (f & IBV_SEND_INLINE)
		o |= UIB_SEND_INLINE;
	return o;
}

static enum ibv_wc_status map_wc_status_out(enum uib_wc_status s)
{
	switch (s) {
	case UIB_WC_SUCCESS:
		return IBV_WC_SUCCESS;
	case UIB_WC_LOC_LEN_ERR:
		return IBV_WC_LOC_LEN_ERR;
	case UIB_WC_LOC_PROT_ERR:
		return IBV_WC_LOC_PROT_ERR;
	case UIB_WC_WR_FLUSH_ERR:
		return IBV_WC_WR_FLUSH_ERR;
	case UIB_WC_REM_ACCESS_ERR:
		return IBV_WC_REM_ACCESS_ERR;
	case UIB_WC_REM_OP_ERR:
		return IBV_WC_REM_OP_ERR;
	case UIB_WC_RETRY_EXC_ERR:
		return IBV_WC_RETRY_EXC_ERR;
	case UIB_WC_RESP_TIMEOUT_ERR:
		return IBV_WC_RESP_TIMEOUT_ERR;
	default:
		return IBV_WC_GENERAL_ERR;
	}
}

static enum ibv_wc_opcode map_wc_op_out(enum uib_wc_opcode op)
{
	switch (op) {
	case UIB_WC_SEND:
		return IBV_WC_SEND;
	case UIB_WC_RDMA_WRITE:
		return IBV_WC_RDMA_WRITE;
	case UIB_WC_RDMA_READ:
		return IBV_WC_RDMA_READ;
	case UIB_WC_FETCH_ADD:
		return IBV_WC_FETCH_ADD;
	case UIB_WC_COMP_SWAP:
		return IBV_WC_COMP_SWAP;
	case UIB_WC_UET_ATOMIC:
		return IBV_WC_FETCH_ADD;
	case UIB_WC_RECV:
		return IBV_WC_RECV;
	case UIB_WC_RECV_RDMA_WITH_IMM:
		return IBV_WC_RECV_RDMA_WITH_IMM;
	}
	return IBV_WC_SEND;
}

static enum uib_qp_type map_qpt_in(enum ibv_qp_type t)
{
	switch (t) {
	case IBV_QPT_RC:
		return UIB_QPT_RC;
	case IBV_QPT_UC:
		return UIB_QPT_UC;
	case IBV_QPT_UD:
		return UIB_QPT_UD;
	default:
		return UIB_QPT_RC;
	}
}

static enum uib_qp_state map_qps_in(enum ibv_qp_state s)
{
	switch (s) {
	case IBV_QPS_RESET:
		return UIB_QPS_RESET;
	case IBV_QPS_INIT:
		return UIB_QPS_INIT;
	case IBV_QPS_RTR:
		return UIB_QPS_RTR;
	case IBV_QPS_RTS:
		return UIB_QPS_RTS;
	case IBV_QPS_SQE:
		return UIB_QPS_SQE;
	case IBV_QPS_ERR:
		return UIB_QPS_ERR;
	default:
		return UIB_QPS_RESET;
	}
}

static int gid_to_sockaddr(const union ibv_gid *gid, struct sockaddr_in *out,
			   uint16_t port)
{
	const uint8_t *r = gid->raw;

	if (!(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0 &&
	      r[4] == 0 && r[5] == 0 && r[6] == 0 && r[7] == 0 &&
	      r[8] == 0 && r[9] == 0 && r[10] == 0xff && r[11] == 0xff))
		return -EAFNOSUPPORT;
	out->sin_family = AF_INET;
	out->sin_port = htons(port);
	memcpy(&out->sin_addr.s_addr, r + 12, 4);
	return 0;
}

static int provider_post_send(struct ibv_qp *qp_ibv, struct ibv_send_wr *wr_ibv,
			      struct ibv_send_wr **bad_ibv)
{
	struct uib_send_wr inline_items[UIBV_WR_INLINE_COUNT];
	struct uibv_qp_wrap *qp = WRAP_OF(qp_ibv, struct uibv_qp_wrap, ibv);
	struct uib_send_wr *new_items;
	struct uib_send_wr *bad_u = NULL;
	struct ibv_send_wr *p;
	struct uib_send_wr *items;
	struct uib_send_wr *item;
	size_t capacity = UIBV_WR_INLINE_COUNT;
	size_t count = 0;
	size_t i;
	int rc;

	items = inline_items;
	for (p = wr_ibv; p; p = p->next) {
		if (count == capacity) {
			size_t new_capacity = qp->cap.max_send_wr;

			if (new_capacity <= capacity)
				new_capacity = capacity * 2U;
			if (new_capacity < capacity ||
			    new_capacity > SIZE_MAX / sizeof(*items)) {
				if (bad_ibv)
					*bad_ibv = p;
				rc = ENOMEM;
				goto out;
			}
			new_items = calloc(new_capacity, sizeof(*new_items));
			if (new_items) {
				memcpy(new_items, items, count * sizeof(*items));
				for (i = 0; i + 1U < count; i++)
					new_items[i].next = &new_items[i + 1U];
				if (count)
					new_items[count - 1U].next = NULL;
			}
			if (!new_items) {
				if (bad_ibv)
					*bad_ibv = p;
				rc = ENOMEM;
				goto out;
			}
			if (items != inline_items)
				free(items);
			items = new_items;
			capacity = new_capacity;
		}
		item = &items[count];
		if (items == inline_items)
			memset(item, 0, sizeof(*item));
		item->wr_id = p->wr_id;
		item->opcode = map_wr_op_in(p->opcode);
		item->send_flags = map_send_flags_in(p->send_flags);
		if (qp->sq_sig_all)
			item->send_flags |= UIB_SEND_SIGNALED;
		item->imm_data = p->imm_data;
		item->sg_list = (struct uib_sge *)p->sg_list;
		item->num_sge = p->num_sge;
		item->rdma.remote_addr = p->wr.rdma.remote_addr;
		item->rdma.rkey = p->wr.rdma.rkey;
		if (qp_ibv->qp_type == IBV_QPT_UD) {
			struct uibv_ah_wrap *ah;

			if (!p->wr.ud.ah ||
			    !ctx_is_ours(p->wr.ud.ah->context) ||
			    p->wr.ud.ah->pd != qp_ibv->pd) {
				if (bad_ibv)
					*bad_ibv = p;
				rc = EINVAL;
				goto out;
			}
			ah = WRAP_OF(p->wr.ud.ah, struct uibv_ah_wrap, ibv);
			item->ud.remote_addr = ah->remote_addr;
			item->ud.remote_qpn = p->wr.ud.remote_qpn;
			item->ud.qkey = p->wr.ud.remote_qkey;
		}
		if (p->opcode == IBV_WR_ATOMIC_FETCH_AND_ADD ||
		    p->opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
			item->atomic.remote_addr = p->wr.atomic.remote_addr;
			item->atomic.rkey = p->wr.atomic.rkey;
			item->atomic.compare_add = p->wr.atomic.compare_add;
			item->atomic.swap = p->wr.atomic.swap;
		}
		if (count)
			items[count - 1U].next = item;
		count++;
	}
	if (!count)
		return 0;

	rc = uib_post_send(qp->core, items, &bad_u);

	if (rc < 0 && bad_u && bad_ibv &&
	    bad_u >= items && bad_u < items + count) {
		i = (size_t)(bad_u - items);
		for (p = wr_ibv; i && p; i--)
			p = p->next;
		if (p)
			*bad_ibv = p;
	}
out:
	if (items != inline_items)
		free(items);
	return rc < 0 ? -rc : rc;
}

static int provider_post_recv_ud(struct uibv_qp_wrap *qp,
				 struct ibv_recv_wr *wr,
				 struct ibv_recv_wr **bad)
{
	for (; wr; wr = wr->next) {
		struct uib_sge inline_sges[UIBV_WR_INLINE_COUNT];
		struct uib_sge *sges = inline_sges;
		struct uib_recv_wr request = {0};
		struct ibv_grh *grh;
		uint32_t ip;
		int rc;

		if (wr->num_sge < 1 || wr->sg_list[0].length < sizeof(*grh)) {
			if (bad)
				*bad = wr;
			return EINVAL;
		}
		if (wr->num_sge > (int)UIBV_WR_INLINE_COUNT) {
			sges = calloc((size_t)wr->num_sge, sizeof(*sges));
			if (!sges) {
				if (bad)
					*bad = wr;
				return ENOMEM;
			}
		}
		memcpy(sges, wr->sg_list, (size_t)wr->num_sge * sizeof(*sges));
		grh = (struct ibv_grh *)(uintptr_t)sges[0].addr;
		memset(grh, 0, sizeof(*grh));
		grh->version_tclass_flow = htonl(6U << 28);
		grh->hop_limit = 64;
		grh->dgid.raw[10] = 0xff;
		grh->dgid.raw[11] = 0xff;
		ip = qp->core->ctx->local_addr.sin_addr.s_addr;
		memcpy(grh->dgid.raw + 12, &ip, sizeof(ip));
		sges[0].addr += sizeof(*grh);
		sges[0].length -= sizeof(*grh);

		request.wr_id = wr->wr_id;
		request.sg_list = sges;
		request.num_sge = wr->num_sge;
		rc = uib_post_recv(qp->core, &request, NULL);
		if (sges != inline_sges)
			free(sges);
		if (rc < 0) {
			if (bad)
				*bad = wr;
			return -rc;
		}
	}
	return 0;
}

static int provider_post_recv_queue(struct uib_qp *qp, struct uib_srq *srq,
				    uint32_t max_wr,
				    struct ibv_recv_wr *wr,
				    struct ibv_recv_wr **bad)
{
	struct uib_recv_wr inline_items[UIBV_WR_INLINE_COUNT];
	struct uib_recv_wr *new_items;
	struct uib_recv_wr *bad_u = NULL;
	struct ibv_recv_wr *p;
	struct uib_recv_wr *items;
	struct uib_recv_wr *item;
	size_t capacity = UIBV_WR_INLINE_COUNT;
	size_t count = 0;
	size_t i;
	int rc;

	items = inline_items;
	for (p = wr; p; p = p->next) {
		if (count == capacity) {
			size_t new_capacity = max_wr;

			if (new_capacity <= capacity)
				new_capacity = capacity * 2U;
			if (new_capacity < capacity ||
			    new_capacity > SIZE_MAX / sizeof(*items)) {
				if (bad)
					*bad = p;
				rc = ENOMEM;
				goto out;
			}
			new_items = calloc(new_capacity, sizeof(*new_items));
			if (new_items) {
				memcpy(new_items, items, count * sizeof(*items));
				for (i = 0; i + 1U < count; i++)
					new_items[i].next = &new_items[i + 1U];
				if (count)
					new_items[count - 1U].next = NULL;
			}
			if (!new_items) {
				if (bad)
					*bad = p;
				rc = ENOMEM;
				goto out;
			}
			if (items != inline_items)
				free(items);
			items = new_items;
			capacity = new_capacity;
		}
		item = &items[count];
		if (items == inline_items)
			memset(item, 0, sizeof(*item));
		item->wr_id = p->wr_id;
		item->sg_list = (struct uib_sge *)p->sg_list;
		item->num_sge = p->num_sge;
		if (count)
			items[count - 1U].next = item;
		count++;
	}
	if (!count)
		return 0;

	rc = srq ? uib_post_srq_recv(srq, items, &bad_u) :
		uib_post_recv(qp, items, &bad_u);

	if (rc < 0 && bad_u && bad &&
	    bad_u >= items && bad_u < items + count) {
		i = (size_t)(bad_u - items);
		for (p = wr; i && p; i--)
			p = p->next;
		if (p)
			*bad = p;
	}
out:
	if (items != inline_items)
		free(items);
	return rc < 0 ? -rc : rc;
}

static int provider_post_recv_connected(struct uibv_qp_wrap *qp,
					struct ibv_recv_wr *wr,
					struct ibv_recv_wr **bad)
{
	return provider_post_recv_queue(qp->core, NULL,
					qp->cap.max_recv_wr, wr, bad);
}

static int provider_post_srq_recv(struct ibv_srq *srq_ibv,
				  struct ibv_recv_wr *wr,
				  struct ibv_recv_wr **bad)
{
	struct uibv_srq_wrap *srq;
	struct uib_srq_attr attr;

	if (!srq_ibv || !ctx_is_ours(srq_ibv->context)) {
		if (bad)
			*bad = wr;
		return EOPNOTSUPP;
	}
	srq = WRAP_OF(srq_ibv, struct uibv_srq_wrap, ibv);
	if (uib_query_srq(srq->core, &attr)) {
		if (bad)
			*bad = wr;
		return EINVAL;
	}
	return provider_post_recv_queue(NULL, srq->core, attr.max_wr, wr, bad);
}

static int provider_post_recv(struct ibv_qp *qp_ibv, struct ibv_recv_wr *wr,
			      struct ibv_recv_wr **bad)
{
	struct uibv_qp_wrap *qp;

	qp = WRAP_OF(qp_ibv, struct uibv_qp_wrap, ibv);
	switch (qp_ibv->qp_type) {
	case IBV_QPT_RC:
	case IBV_QPT_UC:
		return provider_post_recv_connected(qp, wr, bad);
	case IBV_QPT_UD:
		return provider_post_recv_ud(qp, wr, bad);
	default:
		if (bad)
			*bad = wr;
		return EOPNOTSUPP;
	}
}

static int provider_poll_cq(struct ibv_cq *cq_ibv, int n, struct ibv_wc *wc)
{
	struct uibv_cq_wrap *cq = WRAP_OF(cq_ibv, struct uibv_cq_wrap, ibv);
	struct uib_wc tmp[n];
	int got = uib_poll_cq(cq->core, n, tmp);
	int i;

	for (i = 0; i < got; i++) {
		memset(&wc[i], 0, sizeof(wc[i]));
		wc[i].wr_id    = tmp[i].wr_id;
		wc[i].status   = map_wc_status_out(tmp[i].status);
		wc[i].opcode   = map_wc_op_out(tmp[i].opcode);
		wc[i].byte_len = tmp[i].byte_len;
		wc[i].qp_num   = tmp[i].qp_num;
		wc[i].src_qp   = tmp[i].src_qp;
		wc[i].imm_data = tmp[i].imm_data;
		if (tmp[i].opcode == UIB_WC_RECV) {
			struct uibv_qp_wrap *qp;

			pthread_mutex_lock(&cq->ibv.mutex);
			for (qp = cq->recv_qps; qp; qp = qp->recv_next) {
				if (qp->ibv.qp_num == tmp[i].qp_num)
					break;
			}
			if (qp && qp->ibv.qp_type == IBV_QPT_UD) {
				wc[i].byte_len += sizeof(struct ibv_grh);
				wc[i].wc_flags |= IBV_WC_GRH;
				wc[i].sl = 0xa;
			}
			pthread_mutex_unlock(&cq->ibv.mutex);
		}
		if (tmp[i].opcode == UIB_WC_RECV_RDMA_WITH_IMM)
			wc[i].wc_flags |= IBV_WC_WITH_IMM;
	}
	return got;
}

static int provider_req_notify_cq(struct ibv_cq *cq, int solicited_only)
{
	struct uibv_cq_wrap *wrapper;
	int rc;

	wrapper = WRAP_OF(cq, struct uibv_cq_wrap, ibv);
	rc = uib_req_notify_cq(wrapper->core, solicited_only);
	return rc < 0 ? -rc : 0;
}

struct ibv_device **ibv_get_device_list(int *num)
{
	struct ibv_device **real;
	struct ibv_device **list;
	int real_n = 0;
	int i;

	real = real_get_device_list ? real_get_device_list(NULL) : NULL;
	if (real) {
		while (real[real_n])
			real_n++;
	}

	list = calloc((size_t)real_n + 2, sizeof(*list));

	if (!list) {
		if (real && real_free_device_list)
			real_free_device_list(real);
		return NULL;
	}
	list[0] = &g_uibv_dev.ibv;
	for (i = 0; i < real_n; i++)
		list[i + 1] = real[i];
	if (real && real_free_device_list)
		real_free_device_list(real);
	if (num)
		*num = real_n + 1;
	return list;
}

void ibv_free_device_list(struct ibv_device **list)
{
	free(list);
}

const char *ibv_get_device_name(struct ibv_device *dev)
{
	if (dev == &g_uibv_dev.ibv)
		return UIBV_DEV_NAME;
	return real_get_device_name ? real_get_device_name(dev) : NULL;
}

static const char *bind_ip_from_env(void)
{
	const char *ip = getenv(UIB_ENV_BIND_IP);

	return ip && *ip ? ip : "0.0.0.0";
}

static uint16_t bind_port_from_env(void)
{
	const char *p = getenv(UIB_ENV_BIND_PORT);
	uint16_t default_port =
		uib_protocol_default_port(getenv(UIB_ENV_PROTOCOL));

	if (!p || !*p)
		return default_port;
	int v = atoi(p);

	return v > 0 ? (uint16_t)v : default_port;
}

struct ibv_context *ibv_open_device(struct ibv_device *dev)
{
	if (dev != &g_uibv_dev.ibv)
		return real_open_device ? real_open_device(dev) : NULL;

	struct uib_context *core = uib_open_device(bind_ip_from_env(),
						   bind_port_from_env());
	if (!core)
		return NULL;

	struct uibv_ctx_wrap *w = calloc(1, sizeof(*w));

	if (!w) {
		uib_close_device(core);
		return NULL;
	}
	w->core              = core;
	w->ibv.device        = dev;
	w->ibv.cmd_fd        = -1;
	w->ibv.async_fd      = eventfd(0, EFD_CLOEXEC);
	if (w->ibv.async_fd < 0) {
		free(w);
		uib_close_device(core);
		return NULL;
	}
	w->ibv.num_comp_vectors = 1;
	pthread_mutex_init(&w->ibv.mutex, NULL);
	w->ibv.ops.post_send       = provider_post_send;
	w->ibv.ops.post_recv       = provider_post_recv;
	w->ibv.ops.post_srq_recv   = provider_post_srq_recv;
	w->ibv.ops.poll_cq         = provider_poll_cq;
	w->ibv.ops.req_notify_cq   = provider_req_notify_cq;

	return &w->ibv;
}

int ibv_close_device(struct ibv_context *ctx)
{
	if (!ctx_is_ours(ctx))
		return real_close_device ? real_close_device(ctx) : -1;

	struct uibv_ctx_wrap *w = WRAP_OF(ctx, struct uibv_ctx_wrap, ibv);

	close(w->ibv.async_fd);
	uib_close_device(w->core);
	pthread_mutex_destroy(&w->ibv.mutex);
	free(w);
	return 0;
}

int ibv_get_async_event(struct ibv_context *ctx,
			struct ibv_async_event *event)
{
	uint64_t value;
	ssize_t ret;

	if (!ctx_is_ours(ctx))
		return real_get_async_event ? real_get_async_event(ctx, event) : -1;
	if (!event) {
		errno = EINVAL;
		return -1;
	}
	do {
		ret = read(ctx->async_fd, &value, sizeof(value));
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		return -1;
	errno = EIO;
	return -1;
}

void ibv_ack_async_event(struct ibv_async_event *event)
{
	struct ibv_cq *cq;
	struct ibv_qp *qp;

	if (!event)
		return;
	switch (event->event_type) {
	case IBV_EVENT_CQ_ERR:
		cq = event->element.cq;
		if (!cq || !ctx_is_ours(cq->context))
			break;
		pthread_mutex_lock(&cq->mutex);
		cq->async_events_completed++;
		pthread_cond_signal(&cq->cond);
		pthread_mutex_unlock(&cq->mutex);
		return;
	case IBV_EVENT_QP_FATAL:
	case IBV_EVENT_QP_REQ_ERR:
	case IBV_EVENT_QP_ACCESS_ERR:
	case IBV_EVENT_COMM_EST:
	case IBV_EVENT_SQ_DRAINED:
	case IBV_EVENT_PATH_MIG:
	case IBV_EVENT_PATH_MIG_ERR:
	case IBV_EVENT_QP_LAST_WQE_REACHED:
		qp = event->element.qp;
		if (!qp || !ctx_is_ours(qp->context))
			break;
		pthread_mutex_lock(&qp->mutex);
		qp->events_completed++;
		pthread_cond_signal(&qp->cond);
		pthread_mutex_unlock(&qp->mutex);
		return;
	default:
		break;
	}
	if (real_ack_async_event)
		real_ack_async_event(event);
}

int ibv_fork_init(void)
{
	return real_fork_init ? real_fork_init() : 0;
}

const char *ibv_event_type_str(enum ibv_event_type event)
{
	static const char *const names[] = {
		[IBV_EVENT_CQ_ERR] = "CQ error",
		[IBV_EVENT_QP_FATAL] = "local work queue catastrophic error",
		[IBV_EVENT_QP_REQ_ERR] = "invalid request local work queue error",
		[IBV_EVENT_QP_ACCESS_ERR] =
			"local access violation work queue error",
		[IBV_EVENT_COMM_EST] = "communication established",
		[IBV_EVENT_SQ_DRAINED] = "send queue drained",
		[IBV_EVENT_PATH_MIG] = "path migrated",
		[IBV_EVENT_PATH_MIG_ERR] = "path migration request error",
		[IBV_EVENT_DEVICE_FATAL] = "local catastrophic error",
		[IBV_EVENT_PORT_ACTIVE] = "port active",
		[IBV_EVENT_PORT_ERR] = "port error",
		[IBV_EVENT_LID_CHANGE] = "LID change",
		[IBV_EVENT_PKEY_CHANGE] = "P_Key change",
		[IBV_EVENT_SM_CHANGE] = "SM change",
		[IBV_EVENT_SRQ_ERR] = "SRQ catastrophic error",
		[IBV_EVENT_SRQ_LIMIT_REACHED] = "SRQ limit reached",
		[IBV_EVENT_QP_LAST_WQE_REACHED] = "last WQE reached",
		[IBV_EVENT_CLIENT_REREGISTER] = "client reregistration",
		[IBV_EVENT_GID_CHANGE] = "GID table change",
		[IBV_EVENT_WQ_FATAL] = "WQ fatal",
	};

	if ((unsigned int)event < sizeof(names) / sizeof(names[0]) &&
	    names[event])
		return names[event];
	return "unknown";
}

struct ibv_comp_channel *ibv_create_comp_channel(struct ibv_context *ctx)
{
	struct uibv_channel_wrap *wrapper;
	struct uib_comp_channel *core;

	if (!ctx_is_ours(ctx))
		return real_create_comp_channel ? real_create_comp_channel(ctx) : NULL;
	core = uib_create_comp_channel(
		WRAP_OF(ctx, struct uibv_ctx_wrap, ibv)->core);
	if (!core)
		return NULL;
	wrapper = calloc(1, sizeof(*wrapper));
	if (!wrapper) {
		uib_destroy_comp_channel(core);
		return NULL;
	}
	wrapper->core = core;
	wrapper->ibv.context = ctx;
	wrapper->ibv.fd = uib_comp_channel_fd(core);
	return &wrapper->ibv;
}

int ibv_destroy_comp_channel(struct ibv_comp_channel *channel)
{
	struct uibv_channel_wrap *wrapper;
	int rc;

	if (!channel || !ctx_is_ours(channel->context))
		return real_destroy_comp_channel ?
			real_destroy_comp_channel(channel) : -1;
	wrapper = WRAP_OF(channel, struct uibv_channel_wrap, ibv);
	if (channel->refcnt)
		return EBUSY;
	rc = uib_destroy_comp_channel(wrapper->core);
	if (rc < 0)
		return -rc;
	free(wrapper);
	return 0;
}

int ibv_get_cq_event(struct ibv_comp_channel *channel, struct ibv_cq **cq,
		     void **cq_context)
{
	struct uibv_channel_wrap *channel_wrapper;
	struct uibv_cq_wrap *cq_wrapper;
	struct uib_cq *core_cq;
	void *event_context;
	int rc;

	if (!channel || !ctx_is_ours(channel->context))
		return real_get_cq_event ?
			real_get_cq_event(channel, cq, cq_context) : -1;
	channel_wrapper = WRAP_OF(channel, struct uibv_channel_wrap, ibv);
	rc = uib_get_cq_event(channel_wrapper->core, &core_cq,
			      &event_context);
	if (rc < 0)
		return -rc;
	cq_wrapper = event_context;
	*cq = &cq_wrapper->ibv;
	if (cq_context)
		*cq_context = cq_wrapper->ibv.cq_context;
	pthread_mutex_lock(&cq_wrapper->ibv.mutex);
	cq_wrapper->ibv.comp_events_completed++;
	pthread_mutex_unlock(&cq_wrapper->ibv.mutex);
	return 0;
}

void ibv_ack_cq_events(struct ibv_cq *cq, unsigned int events)
{
	struct uibv_cq_wrap *wrapper;

	if (!cq || !ctx_is_ours(cq->context)) {
		if (real_ack_cq_events)
			real_ack_cq_events(cq, events);
		return;
	}
	wrapper = WRAP_OF(cq, struct uibv_cq_wrap, ibv);
	uib_ack_cq_events(wrapper->core, events);
}

struct ibv_pd *ibv_alloc_pd(struct ibv_context *ctx)
{
	if (!ctx_is_ours(ctx))
		return real_alloc_pd ? real_alloc_pd(ctx) : NULL;
	struct uibv_ctx_wrap *cw = WRAP_OF(ctx, struct uibv_ctx_wrap, ibv);
	struct uib_pd *core = uib_alloc_pd(cw->core);

	if (!core)
		return NULL;
	struct uibv_pd_wrap *w = calloc(1, sizeof(*w));

	if (!w) {
		uib_dealloc_pd(core);
		return NULL;
	}
	w->core = core;
	w->ibv.context = ctx;
	w->ibv.handle  = core->handle;
	return &w->ibv;
}

int ibv_dealloc_pd(struct ibv_pd *pd)
{
	if (!ctx_is_ours(pd->context))
		return real_dealloc_pd ? real_dealloc_pd(pd) : -1;
	struct uibv_pd_wrap *w = WRAP_OF(pd, struct uibv_pd_wrap, ibv);
	int rc = uib_dealloc_pd(w->core);

	if (rc < 0)
		return -rc;
	free(w);
	return 0;
}

struct ibv_srq *ibv_create_srq(struct ibv_pd *pd,
			       struct ibv_srq_init_attr *attr)
{
	struct uib_srq_init_attr core_attr;
	struct uibv_srq_wrap *wrapper;
	struct uibv_pd_wrap *pd_wrapper;
	struct uib_srq *core;

	if (!pd || !attr) {
		errno = EINVAL;
		return NULL;
	}
	if (!ctx_is_ours(pd->context))
		return real_create_srq ? real_create_srq(pd, attr) : NULL;
	core_attr.max_wr = attr->attr.max_wr;
	core_attr.max_sge = attr->attr.max_sge;
	core_attr.srq_limit = attr->attr.srq_limit;
	pd_wrapper = WRAP_OF(pd, struct uibv_pd_wrap, ibv);
	core = uib_create_srq(pd_wrapper->core, &core_attr);
	if (!core)
		return NULL;
	wrapper = calloc(1, sizeof(*wrapper));
	if (!wrapper) {
		uib_destroy_srq(core);
		return NULL;
	}
	wrapper->core = core;
	wrapper->ibv.context = pd->context;
	wrapper->ibv.srq_context = attr->srq_context;
	wrapper->ibv.pd = pd;
	pthread_mutex_init(&wrapper->ibv.mutex, NULL);
	pthread_cond_init(&wrapper->ibv.cond, NULL);
	return &wrapper->ibv;
}

int ibv_modify_srq(struct ibv_srq *srq, struct ibv_srq_attr *attr,
		   int attr_mask)
{
	struct uib_srq_attr core_attr;
	struct uibv_srq_wrap *wrapper;
	int core_mask = 0;
	int rc;

	if (!srq || !attr)
		return EINVAL;
	if (!ctx_is_ours(srq->context))
		return real_modify_srq ? real_modify_srq(srq, attr, attr_mask) :
			EOPNOTSUPP;
	if (attr_mask & ~(IBV_SRQ_MAX_WR | IBV_SRQ_LIMIT))
		return EINVAL;
	if (attr_mask & IBV_SRQ_MAX_WR)
		core_mask |= UIB_SRQ_MAX_WR;
	if (attr_mask & IBV_SRQ_LIMIT)
		core_mask |= UIB_SRQ_LIMIT;
	core_attr.max_wr = attr->max_wr;
	core_attr.max_sge = attr->max_sge;
	core_attr.srq_limit = attr->srq_limit;
	wrapper = WRAP_OF(srq, struct uibv_srq_wrap, ibv);
	rc = uib_modify_srq(wrapper->core, &core_attr, core_mask);
	return rc < 0 ? -rc : rc;
}

int ibv_query_srq(struct ibv_srq *srq, struct ibv_srq_attr *attr)
{
	struct uib_srq_attr core_attr;
	struct uibv_srq_wrap *wrapper;
	int rc;

	if (!srq || !attr)
		return EINVAL;
	if (!ctx_is_ours(srq->context))
		return real_query_srq ? real_query_srq(srq, attr) : EOPNOTSUPP;
	wrapper = WRAP_OF(srq, struct uibv_srq_wrap, ibv);
	rc = uib_query_srq(wrapper->core, &core_attr);
	if (rc < 0)
		return -rc;
	attr->max_wr = core_attr.max_wr;
	attr->max_sge = core_attr.max_sge;
	attr->srq_limit = core_attr.srq_limit;
	return 0;
}

int ibv_destroy_srq(struct ibv_srq *srq)
{
	struct uibv_srq_wrap *wrapper;
	int rc;

	if (!srq)
		return EINVAL;
	if (!ctx_is_ours(srq->context))
		return real_destroy_srq ? real_destroy_srq(srq) : EOPNOTSUPP;
	wrapper = WRAP_OF(srq, struct uibv_srq_wrap, ibv);
	rc = uib_destroy_srq(wrapper->core);
	if (rc < 0)
		return -rc;
	pthread_cond_destroy(&wrapper->ibv.cond);
	pthread_mutex_destroy(&wrapper->ibv.mutex);
	free(wrapper);
	return 0;
}

static struct ibv_mr *provider_reg_mr(struct ibv_pd *pd, void *addr,
				      size_t length, uint64_t iova,
				      unsigned int access)
{
	struct uibv_mr_wrap *wrapper;
	struct uibv_pd_wrap *pdw;
	struct uib_mr *core;

	/* UIB host MRs currently expose their process VA as the wire IOVA. */
	if (iova != (uint64_t)(uintptr_t)addr) {
		errno = EOPNOTSUPP;
		return NULL;
	}
	pdw = WRAP_OF(pd, struct uibv_pd_wrap, ibv);
	core = uib_reg_mr(pdw->core, addr, length, (int)access);

	if (!core)
		return NULL;
	wrapper = calloc(1, sizeof(*wrapper));
	if (!wrapper) {
		uib_dereg_mr(core);
		return NULL;
	}
	wrapper->core = core;
	wrapper->ibv.context = pd->context;
	wrapper->ibv.pd = pd;
	wrapper->ibv.addr = addr;
	wrapper->ibv.length = length;
	wrapper->ibv.handle = 0;
	wrapper->ibv.lkey = core->lkey;
	wrapper->ibv.rkey = core->rkey;
	return &wrapper->ibv;
}

struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
			  int access)
{
	if (!ctx_is_ours(pd->context))
		return real_reg_mr ? real_reg_mr(pd, addr, length, access) : NULL;
	return provider_reg_mr(pd, addr, length, (uint64_t)(uintptr_t)addr,
			       (unsigned int)access);
}

struct ibv_mr *ibv_reg_mr_iova(struct ibv_pd *pd, void *addr, size_t length,
			       uint64_t iova, int access)
{
	if (!ctx_is_ours(pd->context))
		return real_reg_mr_iova ?
			real_reg_mr_iova(pd, addr, length, iova, access) : NULL;
	return provider_reg_mr(pd, addr, length, iova, (unsigned int)access);
}

struct ibv_mr *ibv_reg_mr_iova2(struct ibv_pd *pd, void *addr, size_t length,
				uint64_t iova, unsigned int access)
{
	if (!ctx_is_ours(pd->context))
		return real_reg_mr_iova2 ?
			real_reg_mr_iova2(pd, addr, length, iova, access) : NULL;
	return provider_reg_mr(pd, addr, length, iova, access);
}

struct ibv_mr *ibv_reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset,
				 size_t length, uint64_t iova, int fd,
				 int access)
{
	struct uibv_pd_wrap *pdw;
	struct uibv_mr_wrap *wrapper;
	struct uib_mr *core;

	if (!ctx_is_ours(pd->context))
		return real_reg_dmabuf_mr ?
			real_reg_dmabuf_mr(pd, offset, length, iova, fd, access) :
			NULL;
	pdw = WRAP_OF(pd, struct uibv_pd_wrap, ibv);
	core = uib_reg_mr_dmabuf_iova(pdw->core, fd, (size_t)offset, length,
				      iova, access, UIB_DMABUF_ALLOW_MMAP |
				       UIB_DMABUF_ALLOW_DEVICE);
	if (!core)
		return NULL;
	wrapper = calloc(1, sizeof(*wrapper));
	if (!wrapper) {
		uib_dereg_mr(core);
		return NULL;
	}
	wrapper->core = core;
	wrapper->ibv.context = pd->context;
	wrapper->ibv.pd = pd;
	wrapper->ibv.addr = (void *)(uintptr_t)iova;
	wrapper->ibv.length = length;
	wrapper->ibv.lkey = core->lkey;
	wrapper->ibv.rkey = core->rkey;
	return &wrapper->ibv;
}

int ibv_dereg_mr(struct ibv_mr *mr)
{
	if (!ctx_is_ours(mr->context))
		return real_dereg_mr ? real_dereg_mr(mr) : -1;
	struct uibv_mr_wrap *w = WRAP_OF(mr, struct uibv_mr_wrap, ibv);
	int rc = uib_dereg_mr(w->core);

	free(w);
	return rc < 0 ? -rc : 0;
}

struct ibv_cq *ibv_create_cq(struct ibv_context *ctx, int cqe, void *cq_context,
			      struct ibv_comp_channel *channel, int comp_vector)
{
	struct uibv_channel_wrap *channel_wrapper = NULL;
	struct uibv_ctx_wrap *context_wrapper;
	struct uibv_cq_wrap *wrapper;
	struct uib_cq *core;

	if (!ctx_is_ours(ctx))
		return real_create_cq ? real_create_cq(ctx, cqe, cq_context,
						       channel, comp_vector) : NULL;
	(void)comp_vector;
	if (channel) {
		if (channel->context != ctx) {
			errno = EINVAL;
			return NULL;
		}
		channel_wrapper = WRAP_OF(channel, struct uibv_channel_wrap, ibv);
	}
	context_wrapper = WRAP_OF(ctx, struct uibv_ctx_wrap, ibv);
	wrapper = calloc(1, sizeof(*wrapper));
	if (!wrapper)
		return NULL;
	core = uib_create_cq_channel(context_wrapper->core, cqe,
				     channel_wrapper ? channel_wrapper->core : NULL,
				     wrapper);
	if (!core) {
		free(wrapper);
		return NULL;
	}
	wrapper->core = core;
	wrapper->ibv.context = ctx;
	wrapper->ibv.channel = channel;
	wrapper->ibv.cqe = cqe;
	wrapper->ibv.cq_context = cq_context;
	if (channel)
		channel->refcnt++;
	pthread_mutex_init(&wrapper->ibv.mutex, NULL);
	pthread_cond_init(&wrapper->ibv.cond, NULL);
	return &wrapper->ibv;
}

int ibv_destroy_cq(struct ibv_cq *cq)
{
	if (!ctx_is_ours(cq->context))
		return real_destroy_cq ? real_destroy_cq(cq) : -1;
	struct uibv_cq_wrap *w = WRAP_OF(cq, struct uibv_cq_wrap, ibv);
	int rc = uib_destroy_cq(w->core);

	if (cq->channel)
		cq->channel->refcnt--;
	pthread_cond_destroy(&w->ibv.cond);
	pthread_mutex_destroy(&w->ibv.mutex);
	free(w);
	return rc < 0 ? -rc : 0;
}

struct ibv_qp *ibv_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *attr)
{
	struct uibv_srq_wrap *srq = NULL;

	if (!ctx_is_ours(pd->context))
		return real_create_qp ? real_create_qp(pd, attr) : NULL;
	if (attr->cap.max_inline_data > UIBV_MAX_INLINE_DATA) {
		errno = EINVAL;
		return NULL;
	}
	struct uibv_pd_wrap *pdw = WRAP_OF(pd, struct uibv_pd_wrap, ibv);
	struct uibv_cq_wrap *scw = attr->send_cq ?
		WRAP_OF(attr->send_cq, struct uibv_cq_wrap, ibv) : NULL;
	struct uibv_cq_wrap *rcw = attr->recv_cq ?
		WRAP_OF(attr->recv_cq, struct uibv_cq_wrap, ibv) : NULL;

	struct uib_qp_init_attr ia = {0};
	if (attr->srq) {
		if (!ctx_is_ours(attr->srq->context) || attr->srq->pd != pd) {
			errno = EINVAL;
			return NULL;
		}
		srq = WRAP_OF(attr->srq, struct uibv_srq_wrap, ibv);
	}

	ia.qp_type      = map_qpt_in(attr->qp_type);
	ia.send_cq      = scw ? scw->core : NULL;
	ia.recv_cq      = rcw ? rcw->core : NULL;
	ia.srq          = srq ? srq->core : NULL;
	ia.max_send_wr  = attr->cap.max_send_wr;
	ia.max_recv_wr  = attr->cap.max_recv_wr;
	ia.max_send_sge = attr->cap.max_send_sge;
	ia.max_recv_sge = attr->cap.max_recv_sge;

	struct uib_qp *core = uib_create_qp(pdw->core, &ia);

	if (!core)
		return NULL;
	struct uibv_qp_wrap *w = calloc(1, sizeof(*w));

	if (!w) {
		uib_destroy_qp(core);
		return NULL;
	}
	w->core = core;
	w->ibv.context = pd->context;
	w->ibv.pd      = pd;
	w->ibv.send_cq = attr->send_cq;
	w->ibv.recv_cq = attr->recv_cq;
	w->ibv.srq     = attr->srq;
	w->ibv.qp_num  = core->qp_num;
	w->ibv.qp_type = attr->qp_type;
	w->ibv.state   = IBV_QPS_RESET;
	w->cap         = attr->cap;
	w->sq_sig_all  = !!attr->sq_sig_all;
	attr->cap      = w->cap;
	w->recv_cq = rcw;
	if (rcw) {
		pthread_mutex_lock(&rcw->ibv.mutex);
		w->recv_next = rcw->recv_qps;
		rcw->recv_qps = w;
		pthread_mutex_unlock(&rcw->ibv.mutex);
	}
	pthread_mutex_init(&w->ibv.mutex, NULL);
	pthread_cond_init(&w->ibv.cond, NULL);
	return &w->ibv;
}

struct ibv_ah *ibv_create_ah(struct ibv_pd *pd, struct ibv_ah_attr *attr)
{
	struct uibv_ah_wrap *wrapper;
	int rc;

	if (!pd || !ctx_is_ours(pd->context))
		return real_create_ah ? real_create_ah(pd, attr) : NULL;
	if (!attr || !attr->is_global || attr->port_num != 1) {
		errno = EINVAL;
		return NULL;
	}
	wrapper = calloc(1, sizeof(*wrapper));
	if (!wrapper)
		return NULL;
	rc = gid_to_sockaddr(&attr->grh.dgid, &wrapper->remote_addr,
			     bind_port_from_env());
	if (rc < 0) {
		free(wrapper);
		errno = -rc;
		return NULL;
	}
	wrapper->ibv.context = pd->context;
	wrapper->ibv.pd = pd;
	return &wrapper->ibv;
}

int ibv_destroy_ah(struct ibv_ah *ah)
{
	if (!ah || !ctx_is_ours(ah->context))
		return real_destroy_ah ? real_destroy_ah(ah) : EINVAL;
	free(WRAP_OF(ah, struct uibv_ah_wrap, ibv));
	return 0;
}

int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int mask)
{
	if (!ctx_is_ours(qp->context))
		return real_modify_qp ? real_modify_qp(qp, attr, mask) : -1;
	struct uibv_qp_wrap *qw = WRAP_OF(qp, struct uibv_qp_wrap, ibv);

	struct uib_qp_attr ua = {0};
	int umask = 0;

	if (mask & IBV_QP_STATE) {
		ua.qp_state = map_qps_in(attr->qp_state);
		umask |= UIB_QP_STATE;
		qp->state = attr->qp_state;
	}
	if (mask & IBV_QP_DEST_QPN) {
		ua.dest_qp_num = attr->dest_qp_num;
		umask |= UIB_QP_DEST_QPN;
	}
	if (mask & IBV_QP_RQ_PSN) {
		ua.rq_psn = attr->rq_psn;
		umask |= UIB_QP_RQ_PSN;
	}
	if (mask & IBV_QP_SQ_PSN) {
		ua.sq_psn = attr->sq_psn;
		umask |= UIB_QP_SQ_PSN;
	}
	if (mask & IBV_QP_PATH_MTU) {
		ua.path_mtu = mtu_to_bytes(attr->path_mtu);
		umask |= UIB_QP_PATH_MTU;
	}
	if (mask & IBV_QP_QKEY) {
		ua.qkey = attr->qkey;
		umask |= UIB_QP_QKEY;
	}
	if (mask & IBV_QP_AV) {
		struct sockaddr_in sa = {0};

		if (gid_to_sockaddr(&attr->ah_attr.grh.dgid, &sa,
				    qw->core->ctx->bind_port) == 0) {
			ua.remote_addr = sa;
			umask |= UIB_QP_AV;
		}
	}
	if (mask & IBV_QP_ACCESS_FLAGS) {
		ua.qp_access_flags = attr->qp_access_flags;
		umask |= UIB_QP_ACCESS_FLAGS;
	}
	int rc = uib_modify_qp(qw->core, &ua, umask);

	return rc < 0 ? -rc : 0;
}

int ibv_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int mask,
		 struct ibv_qp_init_attr *init_attr)
{
	if (!ctx_is_ours(qp->context))
		return real_query_qp ? real_query_qp(qp, attr, mask, init_attr) : ENOSYS;
	struct uibv_qp_wrap *w = WRAP_OF(qp, struct uibv_qp_wrap, ibv);
	struct uib_qp *q = w->core;

	memset(attr, 0, sizeof(*attr));
	memset(init_attr, 0, sizeof(*init_attr));
	attr->qp_state = qp->state;
	attr->cur_qp_state = qp->state;
	attr->path_mtu = bytes_to_mtu(q->path_mtu);
	attr->path_mig_state = IBV_MIG_MIGRATED;
	attr->qkey = q->qkey;
	attr->rq_psn = q->rq_psn;
	attr->sq_psn = q->sq_psn;
	attr->dest_qp_num = q->dest_qp_num;
	attr->qp_access_flags = q->access_flags;
	attr->cap = w->cap;
	attr->port_num = 1;
	attr->pkey_index = 0;
	if (q->remote_addr.sin_family == AF_INET) {
		attr->ah_attr.is_global = 1;
		attr->ah_attr.port_num = 1;
		attr->ah_attr.grh.sgid_index = 0;
		attr->ah_attr.grh.hop_limit = 64;
		memset(attr->ah_attr.grh.dgid.raw, 0, 16);
		attr->ah_attr.grh.dgid.raw[10] = 0xff;
		attr->ah_attr.grh.dgid.raw[11] = 0xff;
		memcpy(attr->ah_attr.grh.dgid.raw + 12,
		       &q->remote_addr.sin_addr.s_addr, 4);
	}
	init_attr->send_cq = qp->send_cq;
	init_attr->recv_cq = qp->recv_cq;
	init_attr->srq = qp->srq;
	init_attr->qp_type = qp->qp_type;
	init_attr->cap = w->cap;
	init_attr->sq_sig_all = w->sq_sig_all;
	(void)mask;
	return 0;
}

int ibv_query_ece(struct ibv_qp *qp, struct ibv_ece *ece)
{
	if (!qp || !ctx_is_ours(qp->context))
		return real_query_ece ? real_query_ece(qp, ece) : EOPNOTSUPP;

	errno = EOPNOTSUPP;
	return EOPNOTSUPP;
}

int ibv_set_ece(struct ibv_qp *qp, struct ibv_ece *ece)
{
	if (!qp || !ctx_is_ours(qp->context))
		return real_set_ece ? real_set_ece(qp, ece) : EOPNOTSUPP;

	errno = EOPNOTSUPP;
	return EOPNOTSUPP;
}

int efadv_query_device(struct ibv_context *ctx,
		       struct efadv_device_attr *attr, uint32_t inlen)
{
	if (!ctx_is_ours(ctx))
		return real_efadv_query_device ?
			real_efadv_query_device(ctx, attr, inlen) : EOPNOTSUPP;
	return EOPNOTSUPP;
}

int ibv_destroy_qp(struct ibv_qp *qp)
{
	if (!ctx_is_ours(qp->context))
		return real_destroy_qp ? real_destroy_qp(qp) : -1;
	struct uibv_qp_wrap *w = WRAP_OF(qp, struct uibv_qp_wrap, ibv);
	struct uibv_qp_wrap **link;
	int rc = uib_destroy_qp(w->core);

	if (w->recv_cq) {
		pthread_mutex_lock(&w->recv_cq->ibv.mutex);
		for (link = &w->recv_cq->recv_qps; *link;
		     link = &(*link)->recv_next) {
			if (*link == w) {
				*link = w->recv_next;
				break;
			}
		}
		pthread_mutex_unlock(&w->recv_cq->ibv.mutex);
	}

	pthread_cond_destroy(&w->ibv.cond);
	pthread_mutex_destroy(&w->ibv.mutex);
	free(w);
	return rc < 0 ? -rc : 0;
}

int ibv_query_device(struct ibv_context *ctx, struct ibv_device_attr *attr)
{
	if (!ctx_is_ours(ctx))
		return real_query_device ? real_query_device(ctx, attr) : -1;
	memset(attr, 0, sizeof(*attr));
	snprintf(attr->fw_ver, sizeof(attr->fw_ver), "uib-1.0");
	attr->vendor_id        = 0xffff;
	attr->vendor_part_id   = 0x0001;
	attr->hw_ver           = 1;
	attr->max_mr_size      = (uint64_t)-1;
	attr->max_qp           = 1024;
	attr->max_qp_wr        = 1024;
	attr->max_sge          = 16;
	attr->max_cq           = 1024;
	attr->max_cqe          = 65536;
	attr->max_mr           = 1024;
	attr->max_pd           = 1024;
	attr->max_srq          = 1024;
	attr->max_srq_wr       = 4096;
	attr->max_srq_sge      = 16;
	attr->max_qp_rd_atom   = 16;
	attr->max_res_rd_atom  = 256;
	attr->atomic_cap       = IBV_ATOMIC_HCA;
	attr->phys_port_cnt    = 1;
	return 0;
}

int ibv_query_port(struct ibv_context *ctx, uint8_t port,
		   struct _compat_ibv_port_attr *attr)
{
	if (!ctx_is_ours(ctx))
		return real_query_port ? real_query_port(ctx, port, attr) : -1;
	struct ibv_port_attr *p = (struct ibv_port_attr *)attr;

	memset(p, 0, sizeof(*p));
	p->state          = IBV_PORT_ACTIVE;
	p->max_mtu        = IBV_MTU_4096;
	p->active_mtu     = active_mtu_from_env();
	p->max_msg_sz     = 4096;
	p->gid_tbl_len    = 1;
	p->pkey_tbl_len   = 1;
	p->lid            = 1;
	p->link_layer     = IBV_LINK_LAYER_ETHERNET;
	p->active_speed   = 16;
	p->active_width   = 1;
	return 0;
}

int ibv_query_gid(struct ibv_context *ctx, uint8_t port, int index,
		  union ibv_gid *gid)
{
	if (!ctx_is_ours(ctx))
		return real_query_gid ? real_query_gid(ctx, port, index, gid) : ENOSYS;
	if (!gid || port != 1 || index != 0)
		return EINVAL;
	struct uibv_ctx_wrap *w = WRAP_OF(ctx, struct uibv_ctx_wrap, ibv);
	uint32_t ip = w->core->local_addr.sin_addr.s_addr;

	if (ip == htonl(INADDR_ANY))
		return EADDRNOTAVAIL;
	memset(gid, 0, sizeof(*gid));
	gid->raw[10] = 0xff;
	gid->raw[11] = 0xff;
	memcpy(gid->raw + 12, &ip, 4);
	return 0;
}

int ibv_query_pkey(struct ibv_context *ctx, uint8_t port, int index,
		   __be16 *pkey)
{
	if (!ctx_is_ours(ctx))
		return real_query_pkey ? real_query_pkey(ctx, port, index, pkey) : ENOSYS;
	if (!pkey || port != 1 || index != 0)
		return EINVAL;
	*pkey = htons(UIB_DEFAULT_PKEY);
	return 0;
}
