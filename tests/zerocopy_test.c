/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "uib/uib.h"

#define TEST_LENGTH (32U * 1024U)

static int failed;

static void check(const char *name, int condition)
{
	printf("%-30s %s\n", name, condition ? "OK" : "FAIL");
	if (!condition)
		failed = 1;
}

static int make_receiver(struct sockaddr_in *address)
{
	socklen_t length = sizeof(*address);
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	memset(address, 0, sizeof(*address));
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)address, sizeof(*address)) < 0 ||
	    getsockname(fd, (struct sockaddr *)address, &length) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(void)
{
	struct uib_zerocopy_stats stats;
	struct sockaddr_in receiver_address;
	struct uib_context context;
	struct msghdr message = {0};
	struct iovec vector;
	uint8_t *received;
	uint8_t *payload;
	uint32_t sequence = 0;
	ssize_t length;
	int receiver;
	int enabled;
	int sender;

	payload = malloc(TEST_LENGTH);
	received = malloc(TEST_LENGTH);
	receiver = make_receiver(&receiver_address);
	sender = socket(AF_INET, SOCK_DGRAM, 0);
	if (!payload || !received || receiver < 0 || sender < 0)
		return 2;
	memset(payload, 0xa5, TEST_LENGTH);
	memset(&context, 0, sizeof(context));
	setenv("UIB_ZEROCOPY", "off", 1);
	uib_zerocopy_init(&context);
	enabled = uib_zerocopy_enable_socket(&context, sender);
	check("disabled.socket", enabled == 0);
	vector.iov_base = payload;
	vector.iov_len = TEST_LENGTH;
	message.msg_name = &receiver_address;
	message.msg_namelen = sizeof(receiver_address);
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	length = uib_zerocopy_sendmsg(&context, sender, &message, 0, enabled,
				      &sequence);
	check("disabled.send", length == TEST_LENGTH);
	check("disabled.stats", !uib_zerocopy_get_stats(&context, &stats) &&
	      stats.copied_calls == 1 && stats.zerocopy_calls == 0);
	length = recv(receiver, received, TEST_LENGTH, 0);
	check("disabled.payload", length == TEST_LENGTH &&
	      !memcmp(received, payload, TEST_LENGTH));

	close(sender);
	pthread_mutex_destroy(&context.zerocopy_lock);
	sender = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&context, 0, sizeof(context));
	setenv("UIB_ZEROCOPY", "required", 1);
	setenv("UIB_ZEROCOPY_THRESHOLD", "1", 1);
	uib_zerocopy_init(&context);
	enabled = uib_zerocopy_enable_socket(&context, sender);
#ifdef UIB_HAVE_ZEROCOPY
	check("required.socket", enabled == 1);
	length = uib_zerocopy_sendmsg(&context, sender, &message, 0, enabled,
				      &sequence);
	check("required.send", length == TEST_LENGTH);
	check("required.stats", !uib_zerocopy_get_stats(&context, &stats) &&
	      stats.zerocopy_calls == 1 &&
	      stats.zerocopy_bytes == TEST_LENGTH && stats.completions == 1);
	length = recv(receiver, received, TEST_LENGTH, 0);
	check("required.payload", length == TEST_LENGTH &&
	      !memcmp(received, payload, TEST_LENGTH));
#else
	check("required.unavailable", enabled == -EOPNOTSUPP);
#endif

	close(sender);
	close(receiver);
	pthread_mutex_destroy(&context.zerocopy_lock);
	free(received);
	free(payload);
	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
