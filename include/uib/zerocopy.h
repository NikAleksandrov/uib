/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_ZEROCOPY_H
#define UIB_ZEROCOPY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

struct uib_context;

enum uib_zerocopy_mode {
	UIB_ZEROCOPY_AUTO = 0,
	UIB_ZEROCOPY_OFF,
	UIB_ZEROCOPY_ON,
	UIB_ZEROCOPY_REQUIRED,
};

enum uib_zerocopy_capability {
	UIB_ZEROCOPY_CAP_MSG		= 1U << 0,
	UIB_ZEROCOPY_CAP_AF_XDP		= 1U << 1,
	UIB_ZEROCOPY_CAP_DMABUF_RX	= 1U << 2,
	UIB_ZEROCOPY_CAP_DMABUF_TX	= 1U << 3,
};

struct uib_zerocopy_stats {
	uint64_t copied_calls;
	uint64_t copied_bytes;
	uint64_t zerocopy_calls;
	uint64_t zerocopy_bytes;
	uint64_t completions;
	uint64_t deferred_copies;
	uint64_t errors;
	uint32_t capabilities;
	enum uib_zerocopy_mode mode;
	size_t threshold;
};

int uib_zerocopy_get_stats(const struct uib_context *ctx,
			   struct uib_zerocopy_stats *stats);

void uib_zerocopy_init(struct uib_context *ctx);
int uib_zerocopy_enable_socket(struct uib_context *ctx, int fd);
ssize_t uib_zerocopy_sendmsg(struct uib_context *ctx, int fd,
			     const struct msghdr *message, int flags,
			     int socket_enabled, uint32_t *sequence);
void uib_zerocopy_account_copy(struct uib_context *ctx, size_t length);
void uib_zerocopy_account_direct(struct uib_context *ctx, size_t length,
				 uint32_t capability);

#endif
