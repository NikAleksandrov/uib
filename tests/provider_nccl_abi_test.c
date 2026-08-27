/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>

#include <infiniband/verbs.h>

struct required_symbol {
	const char *name;
	const char *version;
};

static const struct required_symbol required_symbols[] = {
	{ "ibv_get_device_list", "IBVERBS_1.1" },
	{ "ibv_free_device_list", "IBVERBS_1.1" },
	{ "ibv_get_device_name", "IBVERBS_1.1" },
	{ "ibv_open_device", "IBVERBS_1.1" },
	{ "ibv_close_device", "IBVERBS_1.1" },
	{ "ibv_get_async_event", "IBVERBS_1.1" },
	{ "ibv_ack_async_event", "IBVERBS_1.1" },
	{ "ibv_query_device", "IBVERBS_1.1" },
	{ "ibv_query_port", "IBVERBS_1.1" },
	{ "ibv_query_gid", "IBVERBS_1.1" },
	{ "ibv_query_pkey", "IBVERBS_1.1" },
	{ "ibv_query_qp", "IBVERBS_1.1" },
	{ "ibv_alloc_pd", "IBVERBS_1.1" },
	{ "ibv_dealloc_pd", "IBVERBS_1.1" },
	{ "ibv_reg_mr", "IBVERBS_1.1" },
	{ "ibv_reg_mr_iova2", "IBVERBS_1.8" },
	{ "ibv_query_ece", "IBVERBS_1.10" },
	{ "ibv_set_ece", "IBVERBS_1.10" },
	{ "ibv_reg_dmabuf_mr", "IBVERBS_1.12" },
	{ "ibv_dereg_mr", "IBVERBS_1.1" },
	{ "ibv_create_cq", "IBVERBS_1.1" },
	{ "ibv_destroy_cq", "IBVERBS_1.1" },
	{ "ibv_create_ah", "IBVERBS_1.1" },
	{ "ibv_destroy_ah", "IBVERBS_1.1" },
	{ "ibv_create_qp", "IBVERBS_1.1" },
	{ "ibv_create_srq", "IBVERBS_1.1" },
	{ "ibv_modify_qp", "IBVERBS_1.1" },
	{ "ibv_modify_srq", "IBVERBS_1.1" },
	{ "ibv_query_srq", "IBVERBS_1.1" },
	{ "ibv_destroy_qp", "IBVERBS_1.1" },
	{ "ibv_destroy_srq", "IBVERBS_1.1" },
	{ "ibv_fork_init", "IBVERBS_1.1" },
	{ "ibv_event_type_str", "IBVERBS_1.1" },
};

static void *load_symbol(void *handle, const char *name, const char *version)
{
	void *symbol;
	const char *error;

	dlerror();
	symbol = dlvsym(handle, name, version);
	error = dlerror();
	if (error)
		fprintf(stderr, "%s@%s: %s\n", name, version, error);
	return symbol;
}

int main(void)
{
	struct ibv_device **(*get_device_list)(int *);
	void (*free_device_list)(struct ibv_device **);
	const char *(*get_device_name)(struct ibv_device *);
	struct ibv_context *(*open_device)(struct ibv_device *);
	int (*close_device)(struct ibv_context *);
	int (*get_async_event)(struct ibv_context *, struct ibv_async_event *);
	int (*query_device)(struct ibv_context *, struct ibv_device_attr *);
	struct ibv_pd *(*alloc_pd)(struct ibv_context *);
	int (*dealloc_pd)(struct ibv_pd *);
	struct ibv_cq *(*create_cq)(struct ibv_context *, int, void *,
				    struct ibv_comp_channel *, int);
	int (*destroy_cq)(struct ibv_cq *);
	struct ibv_qp *(*create_qp)(struct ibv_pd *,
				    struct ibv_qp_init_attr *);
	int (*destroy_qp)(struct ibv_qp *);
	struct ibv_srq *(*create_srq)(struct ibv_pd *,
				     struct ibv_srq_init_attr *);
	int (*modify_srq)(struct ibv_srq *, struct ibv_srq_attr *, int);
	int (*query_srq)(struct ibv_srq *, struct ibv_srq_attr *);
	int (*destroy_srq)(struct ibv_srq *);
	int (*query_qp)(struct ibv_qp *, struct ibv_qp_attr *, int,
			struct ibv_qp_init_attr *);
	int (*fork_init)(void);
	const char *(*event_type_str)(enum ibv_event_type);
	struct ibv_qp_init_attr query_init_attr;
	struct ibv_qp_init_attr qp_init_attr;
	struct ibv_qp_attr qp_attr;
	struct ibv_recv_wr recv_wr;
	struct ibv_recv_wr *bad_recv;
	struct ibv_srq_init_attr srq_init_attr;
	struct ibv_srq_attr srq_attr;
	struct ibv_async_event event;
	struct ibv_device_attr device_attr;
	struct ibv_device **devices = NULL;
	struct ibv_context *context = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_srq *srq = NULL;
	void *handle;
	size_t i;
	int flags;
	int count;
	int rc = EXIT_FAILURE;

	handle = dlopen("./libuibv.so", RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return EXIT_FAILURE;
	}
	for (i = 0; i < sizeof(required_symbols) / sizeof(required_symbols[0]);
	     i++) {
		if (!load_symbol(handle, required_symbols[i].name,
				 required_symbols[i].version))
			goto out;
	}

	get_device_list = load_symbol(handle, "ibv_get_device_list",
				      "IBVERBS_1.1");
	free_device_list = load_symbol(handle, "ibv_free_device_list",
				       "IBVERBS_1.1");
	get_device_name = load_symbol(handle, "ibv_get_device_name",
				      "IBVERBS_1.1");
	open_device = load_symbol(handle, "ibv_open_device", "IBVERBS_1.1");
	close_device = load_symbol(handle, "ibv_close_device", "IBVERBS_1.1");
	get_async_event = load_symbol(handle, "ibv_get_async_event",
				      "IBVERBS_1.1");
	query_device = load_symbol(handle, "ibv_query_device", "IBVERBS_1.1");
	alloc_pd = load_symbol(handle, "ibv_alloc_pd", "IBVERBS_1.1");
	dealloc_pd = load_symbol(handle, "ibv_dealloc_pd", "IBVERBS_1.1");
	create_cq = load_symbol(handle, "ibv_create_cq", "IBVERBS_1.1");
	destroy_cq = load_symbol(handle, "ibv_destroy_cq", "IBVERBS_1.1");
	create_qp = load_symbol(handle, "ibv_create_qp", "IBVERBS_1.1");
	destroy_qp = load_symbol(handle, "ibv_destroy_qp", "IBVERBS_1.1");
	create_srq = load_symbol(handle, "ibv_create_srq", "IBVERBS_1.1");
	modify_srq = load_symbol(handle, "ibv_modify_srq", "IBVERBS_1.1");
	query_srq = load_symbol(handle, "ibv_query_srq", "IBVERBS_1.1");
	destroy_srq = load_symbol(handle, "ibv_destroy_srq", "IBVERBS_1.1");
	query_qp = load_symbol(handle, "ibv_query_qp", "IBVERBS_1.1");
	fork_init = load_symbol(handle, "ibv_fork_init", "IBVERBS_1.1");
	event_type_str = load_symbol(handle, "ibv_event_type_str",
				     "IBVERBS_1.1");

	if (fork_init()) {
		fprintf(stderr, "ibv_fork_init failed\n");
		goto out;
	}
	if (strcmp(event_type_str(IBV_EVENT_CQ_ERR), "CQ error") ||
	    strcmp(event_type_str((enum ibv_event_type)-1), "unknown")) {
		fprintf(stderr, "ibv_event_type_str returned an invalid name\n");
		goto out;
	}

	setenv("UIB_BIND_IP", "127.0.0.1", 1);
	devices = get_device_list(&count);
	if (!devices || count < 1 || !devices[0] ||
	    strcmp(get_device_name(devices[0]), "uib0")) {
		fprintf(stderr, "uib0 was not returned by device discovery\n");
		goto out;
	}
	context = open_device(devices[0]);
	if (!context) {
		perror("ibv_open_device");
		goto out;
	}
	if (context->async_fd < 0) {
		fprintf(stderr, "uib0 did not provide an async event fd\n");
		goto out;
	}
	if (!context->ops.post_send || !context->ops.post_recv ||
	    !context->ops.post_srq_recv ||
	    !context->ops.poll_cq || !context->ops.req_notify_cq) {
		fprintf(stderr, "uib0 did not install the NCCL data-path operations\n");
		goto out;
	}
	memset(&device_attr, 0, sizeof(device_attr));
	if (query_device(context, &device_attr) ||
	    device_attr.atomic_cap != IBV_ATOMIC_HCA) {
		fprintf(stderr, "uib0 did not advertise host atomic support\n");
		goto out;
	}
	flags = fcntl(context->async_fd, F_GETFL);
	if (flags < 0 || fcntl(context->async_fd, F_SETFL,
				     flags | O_NONBLOCK) < 0) {
		perror("fcntl");
		goto out;
	}
	errno = 0;
	if (get_async_event(context, &event) != -1 || errno != EAGAIN) {
		fprintf(stderr, "empty async event queue did not return EAGAIN\n");
		goto out;
	}
	pd = alloc_pd(context);
	cq = create_cq(context, 8, NULL, NULL, 0);
	if (!pd || !cq) {
		fprintf(stderr, "provider QP test setup failed\n");
		goto out;
	}
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
	qp_init_attr.send_cq = cq;
	qp_init_attr.recv_cq = cq;
	qp_init_attr.cap.max_send_wr = 8;
	qp_init_attr.cap.max_recv_wr = 8;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.qp_type = IBV_QPT_UD;
	qp_init_attr.sq_sig_all = 1;
	qp = create_qp(pd, &qp_init_attr);
	if (!qp) {
		fprintf(stderr, "provider UD QP creation failed\n");
		goto out;
	}
	memset(&qp_attr, 0, sizeof(qp_attr));
	memset(&query_init_attr, 0, sizeof(query_init_attr));
	if (query_qp(qp, &qp_attr, 0, &query_init_attr) ||
	    !query_init_attr.sq_sig_all) {
		fprintf(stderr, "provider discarded sq_sig_all\n");
		goto out;
	}
	if (destroy_qp(qp)) {
		fprintf(stderr, "provider UD QP destruction failed\n");
		goto out;
	}
	qp = NULL;
	memset(&srq_init_attr, 0, sizeof(srq_init_attr));
	srq_init_attr.attr.max_wr = 8;
	srq_init_attr.attr.max_sge = 1;
	srq = create_srq(pd, &srq_init_attr);
	if (!srq) {
		fprintf(stderr, "provider SRQ creation failed\n");
		goto out;
	}
	memset(&srq_attr, 0, sizeof(srq_attr));
	if (query_srq(srq, &srq_attr) || srq_attr.max_wr < 8 ||
	    srq_attr.max_sge < 1) {
		fprintf(stderr, "provider SRQ query failed\n");
		goto out;
	}
	srq_attr.srq_limit = 2;
	if (modify_srq(srq, &srq_attr, IBV_SRQ_LIMIT)) {
		fprintf(stderr, "provider SRQ modification failed\n");
		goto out;
	}
	memset(&recv_wr, 0, sizeof(recv_wr));
	recv_wr.wr_id = 0x51;
	bad_recv = NULL;
	if (ibv_post_srq_recv(srq, &recv_wr, &bad_recv) || bad_recv) {
		fprintf(stderr, "provider SRQ receive posting failed\n");
		goto out;
	}
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
	qp_init_attr.send_cq = cq;
	qp_init_attr.recv_cq = cq;
	qp_init_attr.srq = srq;
	qp_init_attr.cap.max_send_wr = 8;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.qp_type = IBV_QPT_RC;
	qp = create_qp(pd, &qp_init_attr);
	if (!qp) {
		fprintf(stderr, "provider SRQ-attached RC QP creation failed\n");
		goto out;
	}
	if (destroy_srq(srq) != EBUSY) {
		fprintf(stderr, "provider destroyed an attached SRQ\n");
		goto out;
	}
	rc = EXIT_SUCCESS;
out:
	if (qp && destroy_qp(qp))
		rc = EXIT_FAILURE;
	if (srq && destroy_srq(srq))
		rc = EXIT_FAILURE;
	if (cq && destroy_cq(cq))
		rc = EXIT_FAILURE;
	if (pd && dealloc_pd(pd))
		rc = EXIT_FAILURE;
	if (context && close_device(context))
		rc = EXIT_FAILURE;
	if (devices)
		free_device_list(devices);
	dlclose(handle);
	if (rc == EXIT_SUCCESS)
		printf("provider NCCL ABI test: PASS\n");
	return rc;
}
