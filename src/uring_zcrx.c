/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "uring_zcrx.h"
#include "utils.h"

#ifdef UIB_HAVE_URING_ZCRX
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <net/if.h>
#include <liburing.h>

struct uib_zcrx_buffer {
	struct uib_zcrx_buffer *next;
	struct uib_zcrx        *engine;
	uint64_t                 offset;
	uint32_t                 length;
	uint32_t                 refs;
};

struct uib_zcrx {
	struct io_uring_zcrx_rqe *rqes;
	struct uib_zcrx_buffer   *buffers;
	struct uib_zcrx_buffer   *free_buffers;
	struct io_uring           ring;
	pthread_mutex_t           lock;
	void                     *refill;
	void                     *area;
	size_t                    refill_size;
	size_t                    area_size;
	uint64_t                  area_token;
	uint32_t                 *rq_tail_kernel;
	uint32_t                  rq_tail;
	uint32_t                  rq_mask;
	uint32_t                  zcrx_id;
	uint32_t                  outstanding;
	int                       event_fd;
	int                       closing;
};

static void zcrx_free(struct uib_zcrx *engine)
{
	if (engine->refill && engine->refill != MAP_FAILED)
		munmap(engine->refill, engine->refill_size);
	if (engine->area && engine->area != MAP_FAILED)
		munmap(engine->area, engine->area_size);
	free(engine->buffers);
	pthread_mutex_destroy(&engine->lock);
	free(engine);
}

int uib_zcrx_create(struct uib_zcrx **engine_out, const char *ifname,
		     uint32_t queue_id, uint32_t entries, size_t area_size)
{
	struct io_uring_zcrx_area_reg area = {0};
	struct io_uring_region_desc region = {0};
	struct io_uring_zcrx_ifq_reg registration = {0};
	struct uib_zcrx *engine;
	unsigned int ring_flags;
	unsigned int ifindex;
	long page_size;
	uint32_t index;
	int rc;

	if (!engine_out || !ifname || !*ifname || entries < 2 ||
	    (entries & (entries - 1)) || !area_size)
		return -EINVAL;
	ifindex = if_nametoindex(ifname);
	if (!ifindex)
		return -ENODEV;
	engine = calloc(1, sizeof(*engine));
	if (!engine)
		return -ENOMEM;
	engine->event_fd = -1;
	pthread_mutex_init(&engine->lock, NULL);
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		page_size = 4096;
	engine->area_size = uib_align_up(area_size, (size_t)page_size);
	engine->refill_size = uib_align_up(
		(size_t)entries * sizeof(*engine->rqes) + (size_t)page_size,
		(size_t)page_size);
	engine->area = mmap(NULL, engine->area_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	engine->refill = mmap(NULL, engine->refill_size,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	engine->buffers = calloc(entries, sizeof(*engine->buffers));
	if (engine->area == MAP_FAILED || engine->refill == MAP_FAILED ||
	    !engine->buffers) {
		rc = -ENOMEM;
		goto err_memory;
	}
	ring_flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
		IORING_SETUP_CQE32;
	rc = io_uring_queue_init(entries, &engine->ring, ring_flags);
	if (rc < 0)
		goto err_memory;
	area.addr = (uint64_t)(uintptr_t)engine->area;
	area.len = engine->area_size;
	region.user_addr = (uint64_t)(uintptr_t)engine->refill;
	region.size = engine->refill_size;
	region.flags = IORING_MEM_REGION_TYPE_USER;
	registration.if_idx = ifindex;
	registration.if_rxq = queue_id;
	registration.rq_entries = entries;
	registration.area_ptr = (uint64_t)(uintptr_t)&area;
	registration.region_ptr = (uint64_t)(uintptr_t)&region;
	rc = io_uring_register(engine->ring.ring_fd, IORING_REGISTER_ZCRX_IFQ,
			       &registration, 1);
	if (rc < 0)
		goto err_ring;
	engine->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (engine->event_fd < 0) {
		rc = -errno;
		goto err_ring;
	}
	rc = io_uring_register_eventfd(&engine->ring, engine->event_fd);
	if (rc < 0)
		goto err_event;
	engine->rqes = (struct io_uring_zcrx_rqe *)
		((uint8_t *)engine->refill + registration.offsets.rqes);
	engine->rq_tail_kernel = (uint32_t *)
		((uint8_t *)engine->refill + registration.offsets.tail);
	engine->rq_mask = entries - 1;
	engine->zcrx_id = registration.zcrx_id;
	engine->area_token = area.rq_area_token;
	for (index = 0; index < entries; index++) {
		engine->buffers[index].engine = engine;
		engine->buffers[index].next = engine->free_buffers;
		engine->free_buffers = &engine->buffers[index];
	}
	*engine_out = engine;
	return 0;

err_event:
	close(engine->event_fd);
	engine->event_fd = -1;
err_ring:
	io_uring_queue_exit(&engine->ring);
err_memory:
	zcrx_free(engine);
	return rc;
}

void uib_zcrx_destroy(struct uib_zcrx *engine)
{
	int release;

	if (!engine)
		return;
	if (engine->event_fd >= 0)
		close(engine->event_fd);
	io_uring_queue_exit(&engine->ring);
	pthread_mutex_lock(&engine->lock);
	engine->closing = 1;
	release = !engine->outstanding;
	pthread_mutex_unlock(&engine->lock);
	if (release)
		zcrx_free(engine);
}

int uib_zcrx_event_fd(const struct uib_zcrx *engine)
{
	return engine ? engine->event_fd : -1;
}

int uib_zcrx_prep_recv(struct uib_zcrx *engine, int fd, uint64_t user_data)
{
	struct io_uring_sqe *sqe;

	if (!engine || fd < 0)
		return -EINVAL;
	sqe = io_uring_get_sqe(&engine->ring);
	if (!sqe)
		return -ENOSPC;
	io_uring_prep_rw(IORING_OP_RECV_ZC, sqe, fd, NULL, 0, 0);
	sqe->ioprio |= IORING_RECV_MULTISHOT;
	sqe->zcrx_ifq_idx = engine->zcrx_id;
	sqe->user_data = user_data;
	return 0;
}

int uib_zcrx_submit(struct uib_zcrx *engine)
{
	return engine ? io_uring_submit(&engine->ring) : -EINVAL;
}

static struct uib_zcrx_buffer *zcrx_buffer_get(struct uib_zcrx *engine)
{
	struct uib_zcrx_buffer *buffer;

	pthread_mutex_lock(&engine->lock);
	buffer = engine->free_buffers;
	if (buffer) {
		engine->free_buffers = buffer->next;
		buffer->next = NULL;
		buffer->refs = 1;
		engine->outstanding++;
	}
	pthread_mutex_unlock(&engine->lock);
	return buffer;
}

static void zcrx_refill(struct uib_zcrx *engine, uint64_t offset,
			uint32_t length)
{
	struct io_uring_zcrx_rqe *rqe;

	pthread_mutex_lock(&engine->lock);
	if (!engine->closing) {
		rqe = &engine->rqes[engine->rq_tail & engine->rq_mask];
		rqe->off = offset | engine->area_token;
		rqe->len = length;
		engine->rq_tail++;
		__atomic_store_n(engine->rq_tail_kernel, engine->rq_tail,
				 __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&engine->lock);
}

void uib_zcrx_release(void *opaque)
{
	struct uib_zcrx_buffer *buffer = opaque;
	struct uib_zcrx *engine;
	struct io_uring_zcrx_rqe *rqe;
	int release;

	if (!buffer)
		return;
	engine = buffer->engine;
	pthread_mutex_lock(&engine->lock);
	if (--buffer->refs) {
		pthread_mutex_unlock(&engine->lock);
		return;
	}
	if (!engine->closing) {
		rqe = &engine->rqes[engine->rq_tail & engine->rq_mask];
		rqe->off = buffer->offset | engine->area_token;
		rqe->len = buffer->length;
		engine->rq_tail++;
		__atomic_store_n(engine->rq_tail_kernel, engine->rq_tail,
				 __ATOMIC_RELEASE);
	}
	buffer->next = engine->free_buffers;
	engine->free_buffers = buffer;
	engine->outstanding--;
	release = engine->closing && !engine->outstanding;
	pthread_mutex_unlock(&engine->lock);
	if (release)
		zcrx_free(engine);
}

void uib_zcrx_retain(struct uib_zcrx_buffer *buffer)
{
	struct uib_zcrx *engine;

	if (!buffer)
		return;
	engine = buffer->engine;
	pthread_mutex_lock(&engine->lock);
	buffer->refs++;
	pthread_mutex_unlock(&engine->lock);
}

int uib_zcrx_drain(struct uib_zcrx *engine, uib_zcrx_receive_fn receive,
		    void *opaque)
{
	struct io_uring_zcrx_cqe *zcqe;
	struct uib_zcrx_buffer *buffer;
	struct io_uring_cqe *cqe;
	uint64_t area_mask;
	unsigned int head;
	unsigned int count;
	int error;

	if (!engine || !receive)
		return -EINVAL;
	area_mask = ((uint64_t)1 << IORING_ZCRX_AREA_SHIFT) - 1;
	count = 0;
	io_uring_for_each_cqe(&engine->ring, head, cqe) {
		buffer = NULL;
		error = cqe->res < 0 ? cqe->res : 0;
		if (cqe->res > 0) {
			zcqe = (struct io_uring_zcrx_cqe *)(cqe + 1);
			buffer = zcrx_buffer_get(engine);
			if (!buffer) {
				zcrx_refill(engine,
					     zcqe->off & ~IORING_ZCRX_AREA_MASK,
					     (uint32_t)cqe->res);
				receive(opaque, cqe->user_data, NULL, 0, NULL,
					!!(cqe->flags & IORING_CQE_F_MORE),
					-ENOBUFS);
				count++;
				continue;
			}
			buffer->offset = zcqe->off & ~IORING_ZCRX_AREA_MASK;
			buffer->length = (uint32_t)cqe->res;
			receive(opaque, cqe->user_data,
				(uint8_t *)engine->area + (zcqe->off & area_mask),
				(size_t)cqe->res, buffer,
				!!(cqe->flags & IORING_CQE_F_MORE), 0);
		} else {
			receive(opaque, cqe->user_data, NULL, 0, NULL,
				!!(cqe->flags & IORING_CQE_F_MORE), error);
		}
		count++;
	}
	io_uring_cq_advance(&engine->ring, count);
	return (int)count;
}

int uib_zcrx_compiled(void)
{
	return 1;
}

#else

int uib_zcrx_create(struct uib_zcrx **engine, const char *ifname,
		     uint32_t queue_id, uint32_t entries, size_t area_size)
{
	return -EOPNOTSUPP;
}

void uib_zcrx_destroy(struct uib_zcrx *engine)
{
}

int uib_zcrx_event_fd(const struct uib_zcrx *engine)
{
	return -1;
}

int uib_zcrx_prep_recv(struct uib_zcrx *engine, int fd, uint64_t user_data)
{
	return -EOPNOTSUPP;
}

int uib_zcrx_submit(struct uib_zcrx *engine)
{
	return -EOPNOTSUPP;
}

int uib_zcrx_drain(struct uib_zcrx *engine, uib_zcrx_receive_fn receive,
		    void *opaque)
{
	return -EOPNOTSUPP;
}

void uib_zcrx_release(void *opaque)
{
}

void uib_zcrx_retain(struct uib_zcrx_buffer *buffer)
{
}

int uib_zcrx_compiled(void)
{
	return 0;
}

#endif
