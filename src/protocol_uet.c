/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Wire formats and transport behavior follow the Ultra Ethernet
 * Specification 1.0.2 published by the Ultra Ethernet Consortium.
 */
#include "protocol_uet_internal.h"

UIB_UET_INTERNAL void uet_debug(struct uib_qp *qp, const char *format, ...)
{
	struct uib_uet_qp *uet = qp->protocol_private;
	va_list args;

	if (!uet || !uet->debug)
		return;
	fprintf(stderr, "uib: uet qpn %u: ", qp->qp_num);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

UIB_UET_INTERNAL int uet_psn_before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

static int uet_congestion_init(struct uib_qp *qp, struct uib_uet_qp *uet)
{
	struct uib_uet_nscc_config nscc_config;
	struct uib_uet_rccc_config rccc_config;
	struct uib_dcqcn_config dcqcn_config;
	const char *cc;
	const void *configuration = NULL;
	uint32_t mtu = qp->path_mtu + 40;
	int rc;

	cc = getenv(UIB_ENV_UET_CC);
	if (!cc)
		cc = "nscc";
	if (!strcmp(cc, "nscc")) {
		const char *setting;
		uint64_t max_wnd;

		uib_uet_nscc_config_default(&nscc_config, mtu);
		setting = getenv(UIB_ENV_UET_NSCC_MAX_WND);
		if (setting) {
			if (uib_parse_u64(setting, 0, &max_wnd) < 0 ||
			    max_wnd < nscc_config.mtu || max_wnd > UINT32_MAX)
				return -EINVAL;
			nscc_config.max_wnd = (uint32_t)max_wnd;
			nscc_config.bdp = nscc_config.max_wnd;
		}
		configuration = &nscc_config;
	} else if (!strcmp(cc, "dcqcn")) {
		const char *experimental = getenv(UIB_ENV_UET_CC_EXPERIMENTAL);

		if (!experimental || strcmp(experimental, "on"))
			return -EPERM;
		if (uib_dcqcn_config_from_env(&dcqcn_config, mtu))
			return -EINVAL;
		configuration = &dcqcn_config;
	} else if (!strcmp(cc, "rccc")) {
		if (uib_uet_rccc_config_from_env(&rccc_config, mtu))
			return -EINVAL;
		configuration = &rccc_config;
	}
	rc = uib_congestion_init(&uet->congestion, cc, configuration, mtu,
				 uib_now_ns());
	if (rc)
		return rc;
	uet->rccc_enabled = !strcmp(cc, "rccc");
	if (uet->rccc_enabled)
		return uib_uet_rccc_destination_init(&uet->rccc_destination,
						     &rccc_config,
						     uib_now_ns());
	return 0;
}

static void *uet_prep_worker(void *argument)
{
	struct uib_uet_prep_worker *worker;
	struct uib_uet_context *context;
	struct uib_uet_prep_task *task;

	worker = argument;
	context = worker->context;
	for (;;) {
		task = uib_ptr_ring_pop(&worker->ready);
		if (task) {
			uet_process_refill_task(task);
			continue;
		}
		pthread_mutex_lock(&worker->lock);
		while (__atomic_load_n(&context->run, __ATOMIC_ACQUIRE) &&
		       !uib_ptr_ring_count(&worker->ready))
			pthread_cond_wait(&worker->cond, &worker->lock);
		if (!__atomic_load_n(&context->run, __ATOMIC_ACQUIRE) &&
		    !uib_ptr_ring_count(&worker->ready)) {
			pthread_mutex_unlock(&worker->lock);
			break;
		}
		pthread_mutex_unlock(&worker->lock);
	}
	return NULL;
}

UIB_UET_INTERNAL void uet_sequence_notify(struct uib_qp *qp)
{
	struct uib_uet_context *context;
	struct uib_uet_qp *uet;
	int rc;

	context = qp->ctx->protocol_private;
	uet = qp->protocol_private;
	__atomic_store_n(&uet->sequence_pending, 1, __ATOMIC_RELEASE);
	if (__atomic_exchange_n(&uet->sequence_queued, 1,
				__ATOMIC_ACQ_REL))
		return;
	for (;;) {
		rc = uib_ptr_ring_push(&context->sequence_ready, qp);
		if (!rc)
			break;
		sched_yield();
	}
	pthread_mutex_lock(&context->sequence_lock);
	pthread_cond_signal(&context->sequence_cond);
	pthread_mutex_unlock(&context->sequence_lock);
}

static void *uet_sequence_worker(void *argument)
{
	struct uib_uet_context *context;
	struct uib_qp *qp;
	int run;

	context = argument;
	for (;;) {
		qp = uib_ptr_ring_pop(&context->sequence_ready);
		if (qp) {
			uet_sequence_progress(qp);
			continue;
		}
		pthread_mutex_lock(&context->sequence_lock);
		while (__atomic_load_n(&context->sequence_run,
					       __ATOMIC_ACQUIRE) &&
		       !uib_ptr_ring_count(&context->sequence_ready))
			pthread_cond_wait(&context->sequence_cond,
					  &context->sequence_lock);
		run = __atomic_load_n(&context->sequence_run,
				      __ATOMIC_ACQUIRE);
		pthread_mutex_unlock(&context->sequence_lock);
		if (!run && !uib_ptr_ring_count(&context->sequence_ready))
			break;
	}
	return NULL;
}

static void uet_context_cleanup(struct uib_uet_context *context)
{
	struct uib_uet_prep_worker *worker;
	unsigned int i;

	if (!context)
		return;
	__atomic_store_n(&context->run, 0, __ATOMIC_RELEASE);
	for (i = 0; i < context->worker_count; i++) {
		worker = &context->workers[i];
		if (!worker->initialized)
			continue;
		pthread_mutex_lock(&worker->lock);
		pthread_cond_broadcast(&worker->cond);
		pthread_mutex_unlock(&worker->lock);
	}
	for (i = 0; i < context->worker_count; i++) {
		worker = &context->workers[i];
		if (worker->started)
			pthread_join(worker->thread, NULL);
	}
	__atomic_store_n(&context->sequence_run, 0, __ATOMIC_RELEASE);
	if (context->sequence_sync_initialized) {
		pthread_mutex_lock(&context->sequence_lock);
		pthread_cond_broadcast(&context->sequence_cond);
		pthread_mutex_unlock(&context->sequence_lock);
	}
	if (context->sequence_started)
		pthread_join(context->sequence_thread, NULL);
	for (i = 0; i < context->worker_count; i++) {
		worker = &context->workers[i];
		if (worker->free_initialized)
			uib_ptr_ring_destroy(&worker->free_tasks);
		if (worker->ready_initialized)
			uib_ptr_ring_destroy(&worker->ready);
		if (worker->initialized) {
			pthread_cond_destroy(&worker->cond);
			pthread_mutex_destroy(&worker->lock);
		}
		free(worker->tasks);
	}
	if (context->sequence_ready_initialized)
		uib_ptr_ring_destroy(&context->sequence_ready);
	if (context->sequence_sync_initialized) {
		pthread_cond_destroy(&context->sequence_cond);
		pthread_mutex_destroy(&context->sequence_lock);
	}
	free(context->workers);
	free(context);
}

static int uet_prep_worker_init(struct uib_uet_context *context,
				struct uib_uet_prep_worker *worker)
{
	unsigned int i;
	int rc;

	worker->context = context;
	rc = pthread_mutex_init(&worker->lock, NULL);
	if (rc)
		return -rc;
	rc = pthread_cond_init(&worker->cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&worker->lock);
		return -rc;
	}
	worker->initialized = 1;
	rc = uib_ptr_ring_init(&worker->ready,
			       UIB_UET_PREP_WORKER_QUEUE_SIZE);
	if (rc < 0)
		return rc;
	worker->ready_initialized = 1;
	rc = uib_ptr_ring_init(&worker->free_tasks,
			       UIB_UET_PREP_WORKER_QUEUE_SIZE);
	if (rc < 0)
		return rc;
	worker->free_initialized = 1;
	worker->tasks = calloc(UIB_UET_PREP_WORKER_QUEUE_SIZE,
			       sizeof(*worker->tasks));
	if (!worker->tasks)
		return -ENOMEM;
	for (i = 0; i < UIB_UET_PREP_WORKER_QUEUE_SIZE; i++) {
		worker->tasks[i].worker = worker;
		uib_ptr_ring_push(&worker->free_tasks, &worker->tasks[i]);
	}
	return 0;
}

static int uet_trimming_config(struct uib_uet_context *context)
{
	const char *trimmable;
	const char *trimmed;
	const char *last_hop;
	uint64_t trimmable_value;
	uint64_t trimmed_value;
	uint64_t last_hop_value;

	trimmable = getenv(UIB_ENV_UET_DSCP_TRIMMABLE);
	trimmed = getenv(UIB_ENV_UET_DSCP_TRIMMED);
	last_hop = getenv(UIB_ENV_UET_DSCP_TRIMMED_LASTHOP);
	if (!!trimmable != !!trimmed || !!trimmed != !!last_hop)
		return -EINVAL;
	if (!trimmable)
		return 0;
	if (uib_parse_u64(trimmable, 0, &trimmable_value) < 0 ||
	    uib_parse_u64(trimmed, 0, &trimmed_value) < 0 ||
	    uib_parse_u64(last_hop, 0, &last_hop_value) < 0 ||
	    trimmable_value > 63 || trimmed_value > 63 ||
	    last_hop_value > 63 || trimmable_value == trimmed_value ||
	    trimmable_value == last_hop_value ||
	    trimmed_value == last_hop_value)
		return -EINVAL;
	context->trimmable_dscp = (uint8_t)trimmable_value;
	context->trimmed_dscp = (uint8_t)trimmed_value;
	context->trimmed_last_hop_dscp = (uint8_t)last_hop_value;
	context->trimming_enabled = 1;
	return 0;
}

static int uet_context_init(struct uib_context *ctx)
{
	struct uib_uet_context *context;
	struct uib_uet_prep_worker *worker;
	cpu_set_t allowed;
	unsigned int count = 0;
	unsigned int cpu_start;
	unsigned int i;
	int affinity;
	int rc;

	rc = uib_worker_configuration(&count, &allowed, &affinity,
				      UIB_ENV_TX_WORKERS,
				      UIB_ENV_TX_WORKER_AFFINITY,
				      UIB_UET_PREP_WORKERS_AUTO_MAX);
	if (rc < 0)
		return rc;
	cpu_start = affinity ? uib_worker_cpu_reserve(count + 1) : 0;
	context = calloc(1, sizeof(*context));
	if (!context)
		return -ENOMEM;
	rc = uet_trimming_config(context);
	if (rc < 0)
		goto error;
	context->sequence_run = 1;
	context->run = 1;
	rc = pthread_mutex_init(&context->sequence_lock, NULL);
	if (rc) {
		rc = -rc;
		goto error;
	}
	rc = pthread_cond_init(&context->sequence_cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&context->sequence_lock);
		rc = -rc;
		goto error;
	}
	context->sequence_sync_initialized = 1;
	rc = uib_ptr_ring_init(&context->sequence_ready,
			       UIB_UET_SEQUENCE_QUEUE_SIZE);
	if (rc < 0)
		goto error;
	context->sequence_ready_initialized = 1;
	context->workers = calloc(count, sizeof(*context->workers));
	if (!context->workers) {
		rc = -ENOMEM;
		goto error;
	}
	context->worker_count = count;
	rc = uib_worker_thread_start(&context->sequence_thread,
				     uet_sequence_worker, context,
				     UIB_UET_PREP_WORKER_STACK_SIZE, &allowed,
				     cpu_start, affinity);
	if (rc < 0)
		goto error;
	context->sequence_started = 1;
	for (i = 0; i < count; i++) {
		worker = &context->workers[i];
		rc = uet_prep_worker_init(context, worker);
		if (rc < 0)
			goto error;
		rc = uib_worker_thread_start(&worker->thread, uet_prep_worker,
					     worker,
					     UIB_UET_PREP_WORKER_STACK_SIZE,
					     &allowed, cpu_start + i + 1,
					     affinity);
		if (rc < 0)
			goto error;
		worker->started = 1;
	}
	ctx->protocol_private = context;
	return 0;

error:
	uet_context_cleanup(context);
	return rc;
}

static void uet_context_fini(struct uib_context *ctx)
{
	struct uib_uet_context *context;

	context = ctx->protocol_private;
	ctx->protocol_private = NULL;
	uet_context_cleanup(context);
}

static int uet_qp_init(struct uib_qp *qp)
{
	struct uib_uet_qp *uet = calloc(1, sizeof(*uet));
	struct uib_uet_context *context = qp->ctx->protocol_private;
	const char *optimized;
	const char *multipath;
	int rc;

	if (!uet)
		return -ENOMEM;
	uet->out_pool = uet_out_pool_create();
	if (!uet->out_pool) {
		free(uet);
		return -ENOMEM;
	}
	uet_out_pool_prewarm_gather(uet->out_pool);
	uet_out_pool_prewarm_mtu(uet->out_pool, qp->path_mtu);
	uet->track_pool = uet_track_pool_create(qp->max_send_wr,
					       qp->max_send_sge);
	if (!uet->track_pool) {
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return -ENOMEM;
	}
	if (uib_psn_index_init(&uet->out_index, UIB_UET_OUT_INDEX_MIN)) {
		uet_track_pool_destroy(uet->track_pool);
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return -ENOMEM;
	}
	rc = pthread_mutex_init(&uet->rx_lock, NULL);
	if (rc) {
		uib_psn_index_destroy(&uet->out_index);
		uet_track_pool_destroy(uet->track_pool);
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return -rc;
	}
	rc = pthread_cond_init(&uet->prep_cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&uet->rx_lock);
		uib_psn_index_destroy(&uet->out_index);
		uet_track_pool_destroy(uet->track_pool);
		uet_out_pool_destroy(uet->out_pool);
		free(uet);
		return -rc;
	}
	uet->spdcid = (uint16_t)qp->qp_num;
	uet->next_msg_id = 1;
	uet->session.pid_on_fep = (uint16_t)qp->qp_num;
	uet->optimized_mode = UIB_UET_OPTIMIZED_AUTO;
	optimized = getenv(UIB_ENV_UET_OPTIMIZED);
	if (optimized && !strcmp(optimized, "off"))
		uet->optimized_mode = UIB_UET_OPTIMIZED_OFF;
	else if (optimized && !strcmp(optimized, "small"))
		uet->optimized_mode = UIB_UET_OPTIMIZED_SMALL;
	else if (optimized && !strcmp(optimized, "medium"))
		uet->optimized_mode = UIB_UET_OPTIMIZED_MEDIUM;
	uet->multipath.entropy_base = UIB_UET_DEFAULT_ENTROPY_BASE;
	uet->multipath.path_count = UIB_UET_MAX_PATHS;
	uet->multipath.ecn_hold_ns = qp->ctx->retry_timeout_ns;
	uet->multipath.failure_hold_ns = UIB_UET_DEFAULT_FAILURE_HOLD_NS;
	multipath = getenv(UIB_ENV_UET_MULTIPATH);
	if (multipath && !strcmp(multipath, "oblivious"))
		uet->multipath.mode = UIB_UET_MULTIPATH_OBLIVIOUS;
	else if (multipath && !strcmp(multipath, "path-aware"))
		uet->multipath.mode = UIB_UET_MULTIPATH_PATH_AWARE;
	uet->path_cursor = (uint16_t)(qp->qp_num % UIB_UET_MAX_PATHS);
	uet->previous_path = UINT16_MAX;
	uet->debug = !!getenv(UIB_ENV_UET_DEBUG);
	if (context->trimming_enabled) {
		uet->trimmed_dscp = context->trimmed_dscp;
		uet->trimmed_last_hop_dscp = context->trimmed_last_hop_dscp;
		uet->trimming_enabled = 1;
	}
	rc = uet_congestion_init(qp, uet);
	if (rc)
		goto error;
	qp->protocol_private = uet;
	return 0;

error:
	pthread_cond_destroy(&uet->prep_cond);
	pthread_mutex_destroy(&uet->rx_lock);
	uib_psn_index_destroy(&uet->out_index);
	uet_track_pool_destroy(uet->track_pool);
	uet_out_pool_destroy(uet->out_pool);
	free(uet);
	return rc;
}

static void uet_qp_modified(struct uib_qp *qp, int attr_mask)
{
	struct uib_uet_qp *uet = qp->protocol_private;

	if ((attr_mask & UIB_QP_PATH_MTU) && uet) {
		struct uib_congestion previous = uet->congestion;

		uet_out_pool_prewarm_mtu(uet->out_pool, qp->path_mtu);
		if (uet_congestion_init(qp, uet))
			uet->congestion = previous;
	}
}

/**
 * uib_uet_session_configure - Configure a QP's UET session parameters.
 * @qp: Queue pair
 * @config: Configuration to apply or fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_session_configure(struct uib_qp *qp,
			      const struct uib_uet_session_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops ||
	    config->job_id > UIB_UET_SES_JOB_ID_MASK ||
	    config->pid_on_fep > UIB_UET_SES_PID_MASK ||
	    config->resource_index > UIB_UET_SES_PID_MASK)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	uet->session = *config;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_session_query - Read a QP's UET session parameters.
 * @qp: Queue pair
 * @config: Session configuration structure to fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_session_query(struct uib_qp *qp,
			  struct uib_uet_session_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	*config = uet->session;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_optimized_configure - Select a QP's optimized-header policy.
 * @qp: Queue pair
 * @mode: Mode to apply or fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_optimized_configure(struct uib_qp *qp,
				enum uib_uet_optimized_mode mode)
{
	struct uib_uet_qp *uet;

	if (!qp || qp->ctx->pops != &uib_uet_protocol_ops ||
	    mode > UIB_UET_OPTIMIZED_MEDIUM)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (uet->out_count) {
		pthread_mutex_unlock(&qp->lock);
		return -EBUSY;
	}
	uet->optimized_mode = mode;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_optimized_query - Read optimized-header counters and policy.
 * @qp: Queue pair
 * @mode: Output for the optimized-header policy
 * @stats: Statistics structure to fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_optimized_query(struct uib_qp *qp,
			    enum uib_uet_optimized_mode *mode,
			    struct uib_uet_optimized_stats *stats)
{
	struct uib_uet_qp *uet;

	if (!qp || (!mode && !stats) ||
	    qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (mode)
		*mode = uet->optimized_mode;
	if (stats)
		*stats = uet->optimized_stats;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_multipath_configure - Configure a QP's entropy paths and policy.
 * @qp: Queue pair
 * @config: Configuration to apply or fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_multipath_configure(struct uib_qp *qp,
				const struct uib_uet_multipath_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops ||
	    config->mode > UIB_UET_MULTIPATH_PATH_AWARE ||
	    !config->path_count || config->path_count > UIB_UET_MAX_PATHS ||
	    (uint32_t)config->entropy_base + config->path_count > UINT16_MAX)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (uet->out_count) {
		pthread_mutex_unlock(&qp->lock);
		return -EBUSY;
	}
	uet->multipath = *config;
	memset(&uet->multipath_stats, 0, sizeof(uet->multipath_stats));
	memset(uet->paths, 0, sizeof(uet->paths));
	uet->path_cursor %= config->path_count;
	uet->previous_path = UINT16_MAX;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_multipath_query - Read a QP's multipath state and counters.
 * @qp: Queue pair
 * @config: Multipath configuration structure to fill
 * @stats: Statistics structure to fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_multipath_query(struct uib_qp *qp,
			    struct uib_uet_multipath_config *config,
			    struct uib_uet_multipath_stats *stats)
{
	struct uib_uet_qp *uet;

	if (!qp || (!config && !stats) ||
	    qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (config)
		*config = uet->multipath;
	if (stats)
		*stats = uet->multipath_stats;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_atomic_datatype_size - Return a UET atomic datatype's byte size.
 * @datatype: Ultra Ethernet atomic datatype code
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: The datatype width in bytes, or 0 if @datatype is unsupported.
 */
size_t uib_uet_atomic_datatype_size(uint8_t datatype)
{
	switch (datatype) {
	case UIB_UET_AMO_INT8:
	case UIB_UET_AMO_UINT8:
		return 1;
	case UIB_UET_AMO_INT16:
	case UIB_UET_AMO_UINT16:
		return 2;
	case UIB_UET_AMO_INT32:
	case UIB_UET_AMO_UINT32:
		return 4;
	case UIB_UET_AMO_INT64:
	case UIB_UET_AMO_UINT64:
		return 8;
	default:
		return 0;
	}
}

UIB_UET_INTERNAL int uet_atomic_has_two_operands(uint8_t opcode)
{
	return opcode >= UIB_UET_AMO_CSWAP && opcode <= UIB_UET_AMO_MSWAP;
}

/**
 * uib_uet_post_atomic - Post an extended Ultra Ethernet atomic operation.
 * @qp: Queue pair
 * @wr: First work request in the chain
 * @bad: Output pointer for the first unposted work request
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_post_atomic(struct uib_qp *qp, struct uib_uet_atomic_wr *wr,
			struct uib_uet_atomic_wr **bad)
{
	struct uib_uet_atomic_wr *current;

	for (current = wr; current; current = current->next) {
		struct uib_send_wr send = {0};
		struct uib_send_wr *send_bad;
		struct uib_sge result_sge;
		size_t width;
		int rc;

		width = uib_uet_atomic_datatype_size(current->datatype);
		if (!qp || qp->ctx->pops != &uib_uet_protocol_ops || !width ||
		    current->opcode >= UIB_UET_AMO_INVAL ||
		    (current->opcode != UIB_UET_AMO_READ &&
		     !current->operand &&
		     !uet_atomic_has_two_operands(current->opcode)) ||
		    (uet_atomic_has_two_operands(current->opcode) &&
		     (!current->compare || !current->swap)) ||
		    (current->fetching && !current->result)) {
			if (bad)
				*bad = current;
			return -EINVAL;
		}
		send.wr_id = current->wr_id;
		send.opcode = UIB_WR_UET_ATOMIC;
		send.send_flags = current->send_flags;
		send.atomic.remote_addr = current->remote_addr;
		send.atomic.rkey = current->rkey;
		send.atomic.uet_opcode = current->opcode;
		send.atomic.uet_datatype = current->datatype;
		send.atomic.uet_semantic_control = current->semantic_control;
		send.atomic.uet_fetching = !!current->fetching;
		if (current->operand)
			memcpy(send.atomic.uet_operand, current->operand, width);
		if (current->compare)
			memcpy(send.atomic.uet_compare, current->compare, width);
		if (current->swap)
			memcpy(send.atomic.uet_swap, current->swap, width);
		if (current->fetching) {
			result_sge.addr = (uintptr_t)current->result;
			result_sge.length = (uint32_t)width;
			result_sge.lkey = current->result_lkey;
			send.sg_list = &result_sge;
			send.num_sge = 1;
		}
		rc = uib_post_send(qp, &send, &send_bad);
		if (rc < 0) {
			if (bad)
				*bad = current;
			return rc;
		}
	}
	return 0;
}

/**
 * uib_uet_nscc_configure_qp - Configure NSCC for one UET QP.
 * @qp: Queue pair
 * @config: Configuration to apply or fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_nscc_configure_qp(struct uib_qp *qp,
			      const struct uib_uet_nscc_config *config)
{
	struct uib_uet_qp *uet;

	if (!qp || !config || qp->ctx->pops != &uib_uet_protocol_ops ||
	    !config->mtu || config->max_wnd < config->mtu)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (uet->out_count) {
		pthread_mutex_unlock(&qp->lock);
		return -EBUSY;
	}
	if (uib_congestion_init(&uet->congestion, "nscc", config,
				qp->path_mtu + 40, uib_now_ns())) {
		pthread_mutex_unlock(&qp->lock);
		return -EINVAL;
	}
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

/**
 * uib_uet_nscc_query_qp - Read NSCC state from one UET QP.
 * @qp: Queue pair
 * @state: NSCC state structure to fill
 * @uib-api: Ultra Ethernet extensions
 *
 * Return: 0 on success or a negative error code on failure.
 */
int uib_uet_nscc_query_qp(struct uib_qp *qp, struct uib_uet_nscc *state)
{
	struct uib_uet_qp *uet;

	if (!qp || !state || qp->ctx->pops != &uib_uet_protocol_ops)
		return -EINVAL;
	pthread_mutex_lock(&qp->lock);
	uet = qp->protocol_private;
	if (strcmp(uib_congestion_name(&uet->congestion), "nscc")) {
		pthread_mutex_unlock(&qp->lock);
		return -EOPNOTSUPP;
	}
	*state = *(struct uib_uet_nscc *)uet->congestion.state.bytes;
	pthread_mutex_unlock(&qp->lock);
	return 0;
}

static int uet_handle_packet(struct uib_qp *qp,
			     const struct sockaddr_in *src,
			     const void *packet, size_t len,
			     const struct uib_rx_meta *meta)
{
	const struct uib_uet_prologue *prologue = packet;
	struct uib_uet_qp *uet = qp->protocol_private;
	uint8_t nack_code;
	uint8_t type;

	if (!packet || len < sizeof(*prologue))
		return -EBADMSG;
	type = uib_uet_prologue_type(prologue);
	nack_code = uet_trim_code(uet, meta);
	if (nack_code)
		return uet_handle_trimmed(qp, src, packet, len, type,
					  (uint8_t)nack_code);
	switch (type) {
	case UIB_UET_PDS_TYPE_RUD_REQ:
	case UIB_UET_PDS_TYPE_RUD_CC_REQ:
		return uet_handle_request(qp, src, packet, len,
					  meta && meta->ecn == IPTOS_ECN_CE);
	case UIB_UET_PDS_TYPE_ACK:
	case UIB_UET_PDS_TYPE_ACK_CC:
		return uet_handle_ack(qp, packet, len,
				      meta ? meta->rx_time_ns : 0);
	case UIB_UET_PDS_TYPE_NACK:
		return uet_handle_nack(qp, packet, len);
	case UIB_UET_PDS_TYPE_CTRL_MSG:
		return uet_handle_control(qp, src, packet, len);
	default:
		return -EOPNOTSUPP;
	}
}

const struct uib_protocol_ops uib_uet_protocol_ops = {
	.name = "uet",
	.default_port = UIB_UET_DEFAULT_PORT,
	.multipath_ports = 1,
	.min_packet = sizeof(struct uib_uet_prologue),
	.init = uet_context_init,
	.fini = uet_context_fini,
	.qp_init = uet_qp_init,
	.qp_fini = uet_qp_fini,
	.qp_modified = uet_qp_modified,
	.packet_qpn = uet_packet_qpn,
	.post_send = uet_post_send,
	.handle_packet = uet_handle_packet,
	.retransmit_tick = uet_retransmit_tick,
	.ipv4_tos = uet_ipv4_tos,
};
