// SPDX-License-Identifier: MIT
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#ifdef UIB_HAVE_ZEROCOPY
#include <linux/errqueue.h>
#endif

#include "uib/context.h"
#include "uib/zerocopy.h"

#define UIB_ZEROCOPY_DEFAULT_THRESHOLD (10U * 1024U)
#define UIB_ZEROCOPY_FORCED_THRESHOLD  1024U

static size_t message_length(const struct msghdr *message)
{
	size_t length = 0;
	size_t iov_count;
	size_t i;

	iov_count = (size_t)message->msg_iovlen;
	for (i = 0; i < iov_count; i++) {
		if (message->msg_iov[i].iov_len > SIZE_MAX - length)
			return SIZE_MAX;
		length += message->msg_iov[i].iov_len;
	}
	return length;
}

static enum uib_zerocopy_mode mode_from_env(void)
{
	const char *value = getenv("UIB_ZEROCOPY");

	if (!value || !*value || !strcmp(value, "auto"))
		return UIB_ZEROCOPY_AUTO;
	if (!strcmp(value, "off"))
		return UIB_ZEROCOPY_OFF;
	if (!strcmp(value, "on"))
		return UIB_ZEROCOPY_ON;
	if (!strcmp(value, "required"))
		return UIB_ZEROCOPY_REQUIRED;
	return UIB_ZEROCOPY_AUTO;
}

void uib_zerocopy_init(struct uib_context *ctx)
{
	const char *threshold = getenv("UIB_ZEROCOPY_THRESHOLD");
	unsigned long parsed;
	char *end = NULL;

	memset(&ctx->zerocopy, 0, sizeof(ctx->zerocopy));
	pthread_mutex_init(&ctx->zerocopy_lock, NULL);
	ctx->zerocopy.mode = mode_from_env();
	ctx->zerocopy.threshold = UIB_ZEROCOPY_DEFAULT_THRESHOLD;
	if (ctx->zerocopy.mode == UIB_ZEROCOPY_ON ||
	    ctx->zerocopy.mode == UIB_ZEROCOPY_REQUIRED)
		ctx->zerocopy.threshold = UIB_ZEROCOPY_FORCED_THRESHOLD;
	if (!threshold || !*threshold)
		return;
	errno = 0;
	parsed = strtoul(threshold, &end, 0);
	if (!errno && end && !*end && parsed <= SIZE_MAX)
		ctx->zerocopy.threshold = (size_t)parsed;
}

int uib_zerocopy_enable_socket(struct uib_context *ctx, int fd)
{
#ifdef UIB_HAVE_ZEROCOPY
	int one = 1;

	if (ctx->zerocopy.mode == UIB_ZEROCOPY_OFF)
		return 0;
	if (!setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one))) {
		__atomic_fetch_or(&ctx->zerocopy.capabilities,
				  UIB_ZEROCOPY_CAP_MSG, __ATOMIC_RELAXED);
		return 1;
	}
	if (ctx->zerocopy.mode == UIB_ZEROCOPY_REQUIRED)
		return -errno;
	return 0;
#else
	(void)fd;
	if (ctx->zerocopy.mode == UIB_ZEROCOPY_REQUIRED)
		return -EOPNOTSUPP;
	return 0;
#endif
}

#ifdef UIB_HAVE_ZEROCOPY
static int wait_for_completion(struct uib_context *ctx, int fd,
			       uint32_t target)
{
	struct sock_extended_err *error;
	struct cmsghdr *control_message;
	struct pollfd poll_fd = { .fd = fd, .events = POLLERR };
	struct msghdr message;
	char control[CMSG_SPACE(sizeof(struct sock_extended_err))];
	uint32_t count;
	int rc;

	for (;;) {
		rc = poll(&poll_fd, 1, -1);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0)
			return -errno;
		memset(&message, 0, sizeof(message));
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		if (recvmsg(fd, &message, MSG_ERRQUEUE) < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -errno;
		}
		for (control_message = CMSG_FIRSTHDR(&message); control_message;
		     control_message = CMSG_NXTHDR(&message, control_message)) {
			if (control_message->cmsg_level != SOL_IP &&
			    control_message->cmsg_level != SOL_IPV6)
				continue;
			error = (struct sock_extended_err *)
				CMSG_DATA(control_message);
			if (error->ee_origin != SO_EE_ORIGIN_ZEROCOPY)
				continue;
			count = error->ee_data - error->ee_info + 1;
			__atomic_fetch_add(&ctx->zerocopy.completions, count,
					   __ATOMIC_RELAXED);
			if (error->ee_code == SO_EE_CODE_ZEROCOPY_COPIED)
				__atomic_fetch_add(&ctx->zerocopy.deferred_copies,
						   count,
						   __ATOMIC_RELAXED);
			if ((uint32_t)(target - error->ee_info) < count)
				return 0;
		}
	}
}
#endif

void uib_zerocopy_account_copy(struct uib_context *ctx, size_t length)
{
	__atomic_fetch_add(&ctx->zerocopy.copied_calls, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&ctx->zerocopy.copied_bytes, length,
			   __ATOMIC_RELAXED);
}

void uib_zerocopy_account_direct(struct uib_context *ctx, size_t length,
				 uint32_t capability)
{
	__atomic_fetch_or(&ctx->zerocopy.capabilities, capability,
			  __ATOMIC_RELAXED);
	__atomic_fetch_add(&ctx->zerocopy.zerocopy_calls, 1,
			   __ATOMIC_RELAXED);
	__atomic_fetch_add(&ctx->zerocopy.zerocopy_bytes, length,
			   __ATOMIC_RELAXED);
}

ssize_t uib_zerocopy_sendmsg(struct uib_context *ctx, int fd,
			     const struct msghdr *message, int flags,
			     int socket_enabled, uint32_t *sequence)
{
	size_t length = message_length(message);
	ssize_t sent;

	if (length == SIZE_MAX)
		return -EOVERFLOW;
#ifdef UIB_HAVE_ZEROCOPY
	if (socket_enabled && ctx->zerocopy.mode != UIB_ZEROCOPY_OFF &&
	    length >= ctx->zerocopy.threshold) {
		int serialize = ctx->mode != UIB_MODE_TCP_DEVMEM;

		/* TCP serializes sends with its peer-table lock. */
		if (serialize)
			pthread_mutex_lock(&ctx->zerocopy_lock);
		sent = sendmsg(fd, message, flags | MSG_ZEROCOPY);
		if (sent >= 0) {
			uint32_t target = (*sequence)++;

			uib_zerocopy_account_direct(ctx, (size_t)sent,
						    UIB_ZEROCOPY_CAP_MSG);
			if (wait_for_completion(ctx, fd, target) < 0)
				__atomic_fetch_add(&ctx->zerocopy.errors, 1,
						   __ATOMIC_RELAXED);
			if (serialize)
				pthread_mutex_unlock(&ctx->zerocopy_lock);
			return sent;
		}
		if (ctx->zerocopy.mode == UIB_ZEROCOPY_REQUIRED) {
			int error = errno;

			if (serialize)
				pthread_mutex_unlock(&ctx->zerocopy_lock);
			return -error;
		}
		if (serialize)
			pthread_mutex_unlock(&ctx->zerocopy_lock);
	}
#else
	(void)socket_enabled;
#endif
	sent = sendmsg(fd, message, flags);
	if (sent >= 0)
		uib_zerocopy_account_copy(ctx, (size_t)sent);
	return sent < 0 ? -errno : sent;
}

int uib_zerocopy_get_stats(const struct uib_context *ctx,
			   struct uib_zerocopy_stats *stats)
{
	if (!ctx || !stats)
		return -EINVAL;
	stats->copied_calls = __atomic_load_n(&ctx->zerocopy.copied_calls,
					      __ATOMIC_RELAXED);
	stats->copied_bytes = __atomic_load_n(&ctx->zerocopy.copied_bytes,
					      __ATOMIC_RELAXED);
	stats->zerocopy_calls = __atomic_load_n(&ctx->zerocopy.zerocopy_calls,
						__ATOMIC_RELAXED);
	stats->zerocopy_bytes = __atomic_load_n(&ctx->zerocopy.zerocopy_bytes,
						__ATOMIC_RELAXED);
	stats->completions = __atomic_load_n(&ctx->zerocopy.completions,
					     __ATOMIC_RELAXED);
	stats->deferred_copies = __atomic_load_n(&ctx->zerocopy.deferred_copies,
						 __ATOMIC_RELAXED);
	stats->errors = __atomic_load_n(&ctx->zerocopy.errors,
					__ATOMIC_RELAXED);
	stats->capabilities = __atomic_load_n(&ctx->zerocopy.capabilities,
					      __ATOMIC_RELAXED);
	stats->mode = ctx->zerocopy.mode;
	stats->threshold = ctx->zerocopy.threshold;
	return 0;
}
