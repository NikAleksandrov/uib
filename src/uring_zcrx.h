/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_URING_ZCRX_H
#define UIB_URING_ZCRX_H

#include <stddef.h>
#include <stdint.h>

struct uib_zcrx;
struct uib_zcrx_buffer;

typedef void (*uib_zcrx_receive_fn)(void *opaque, uint64_t user_data,
				    const void *data, size_t length,
				    struct uib_zcrx_buffer *buffer,
				    int more, int error);

int uib_zcrx_create(struct uib_zcrx **engine, const char *ifname,
		     uint32_t queue_id, uint32_t entries, size_t area_size);
void uib_zcrx_destroy(struct uib_zcrx *engine);
int uib_zcrx_event_fd(const struct uib_zcrx *engine);
int uib_zcrx_prep_recv(struct uib_zcrx *engine, int fd, uint64_t user_data);
int uib_zcrx_submit(struct uib_zcrx *engine);
int uib_zcrx_drain(struct uib_zcrx *engine, uib_zcrx_receive_fn receive,
		    void *opaque);
void uib_zcrx_release(void *opaque);
void uib_zcrx_retain(struct uib_zcrx_buffer *buffer);
int uib_zcrx_compiled(void);

#endif
