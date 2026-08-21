/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "direct_common.h"

#define BENCH_RECV_ID (1ULL << 63)
#define BENCH_TIMEOUT_NS (10ULL * 1000 * 1000 * 1000)
#define BENCH_CONTROL_MAGIC 0x55494242U
#define BENCH_CONTROL_PORT 19190
#define BENCH_DATA_PORT 19100

enum benchmark_role {
	BENCH_ROLE_LOCAL = 0,
	BENCH_ROLE_SERVER,
	BENCH_ROLE_CLIENT,
};

struct options {
	const char *protocol;
	const char *memory;
	const char *bind_address;
	const char *peer_address;
	uint32_t iterations;
	uint32_t message_size;
	uint32_t window;
	uint32_t mtu;
	uint16_t control_port;
	uint16_t data_port;
	enum benchmark_role role;
	int csv;
};

struct benchmark_peer {
	uint32_t magic;
	uint32_t qpn;
	uint32_t address;
	uint32_t iterations;
	uint32_t message_size;
	uint32_t window;
	uint32_t mtu;
	uint16_t port;
	uint8_t protocol;
	uint8_t version;
	uint16_t reserved;
};

static int parse_role(const char *value, enum benchmark_role *role)
{
	if (!strcmp(value, "local"))
		*role = BENCH_ROLE_LOCAL;
	else if (!strcmp(value, "server"))
		*role = BENCH_ROLE_SERVER;
	else if (!strcmp(value, "client"))
		*role = BENCH_ROLE_CLIENT;
	else
		return -EINVAL;
	return 0;
}

static uint64_t now_ns(clockid_t clock_id)
{
	struct timespec now;

	clock_gettime(clock_id, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static uint64_t timeval_ns(const struct timeval *time)
{
	return (uint64_t)time->tv_sec * 1000000000ULL +
		(uint64_t)time->tv_usec * 1000;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return a > b ? 1 : a < b ? -1 : 0;
}

static uint64_t percentile(const uint64_t *values, uint32_t count,
			   uint32_t numerator)
{
	uint64_t index = ((uint64_t)(count - 1) * numerator + 99) / 100;

	return values[index];
}

static int parse_u32(const char *text, uint32_t *value)
{
	char *end;
	unsigned long parsed = strtoul(text, &end, 0);

	if (!*text || *end || parsed > UINT32_MAX)
		return -EINVAL;
	*value = (uint32_t)parsed;
	return 0;
}

static int parse_options(int argc, char **argv, struct options *options)
{
	static const struct option long_options[] = {
		{ "protocol", required_argument, NULL, 'p' },
		{ "iterations", required_argument, NULL, 'n' },
		{ "size", required_argument, NULL, 's' },
		{ "window", required_argument, NULL, 'w' },
		{ "mtu", required_argument, NULL, 'M' },
		{ "memory", required_argument, NULL, 'm' },
		{ "csv", no_argument, NULL, 'c' },
		{ "role", required_argument, NULL, 'r' },
		{ "bind", required_argument, NULL, 'b' },
		{ "peer", required_argument, NULL, 'P' },
		{ "control-port", required_argument, NULL, 'C' },
		{ "data-port", required_argument, NULL, 'D' },
		{ NULL, 0, NULL, 0 },
	};
	const char *value;
	uint32_t parsed;
	int option;

	*options = (struct options) {
		.protocol = "uet",
		.memory = "host",
		.iterations = 10000,
		.message_size = 4096,
		.window = 64,
		.mtu = 1500,
		.control_port = BENCH_CONTROL_PORT,
		.data_port = BENCH_DATA_PORT,
	};
	value = getenv("UIB_BENCH_PROTOCOL");
	if (value && *value)
		options->protocol = value;
	value = getenv("UIB_BENCH_MEMORY");
	if (value && *value)
		options->memory = value;
	value = getenv("UIB_BENCH_ROLE");
	if (value && *value && parse_role(value, &options->role) < 0)
		return -EINVAL;
	value = getenv("UIB_BENCH_BIND");
	if (value && *value)
		options->bind_address = value;
	value = getenv("UIB_BENCH_PEER");
	if (value && *value)
		options->peer_address = value;
	value = getenv("UIB_BENCH_ITERATIONS");
	if (value && *value && parse_u32(value, &options->iterations) < 0)
		return -EINVAL;
	value = getenv("UIB_BENCH_SIZE");
	if (value && *value && parse_u32(value, &options->message_size) < 0)
		return -EINVAL;
	value = getenv("UIB_BENCH_WINDOW");
	if (value && *value && parse_u32(value, &options->window) < 0)
		return -EINVAL;
	value = getenv("UIB_BENCH_MTU");
	if (value && *value && parse_u32(value, &options->mtu) < 0)
		return -EINVAL;
	value = getenv("UIB_BENCH_CONTROL_PORT");
	if (value && *value) {
		if (parse_u32(value, &parsed) < 0 || parsed > UINT16_MAX)
			return -EINVAL;
		options->control_port = (uint16_t)parsed;
	}
	value = getenv("UIB_BENCH_DATA_PORT");
	if (value && *value) {
		if (parse_u32(value, &parsed) < 0 || parsed > UINT16_MAX)
			return -EINVAL;
		options->data_port = (uint16_t)parsed;
	}
	while ((option = getopt_long(argc, argv, "p:n:s:w:M:m:cr:b:P:C:D:",
				     long_options,
				     NULL)) != -1) {
		switch (option) {
		case 'p':
			options->protocol = optarg;
			break;
		case 'n':
			if (parse_u32(optarg, &options->iterations) < 0)
				return -EINVAL;
			break;
		case 's':
			if (parse_u32(optarg, &options->message_size) < 0)
				return -EINVAL;
			break;
		case 'w':
			if (parse_u32(optarg, &options->window) < 0)
				return -EINVAL;
			break;
		case 'M':
			if (parse_u32(optarg, &options->mtu) < 0)
				return -EINVAL;
			break;
		case 'm':
			options->memory = optarg;
			break;
		case 'c':
			options->csv = 1;
			break;
		case 'r':
			if (parse_role(optarg, &options->role) < 0)
				return -EINVAL;
			break;
		case 'b':
			options->bind_address = optarg;
			break;
		case 'P':
			options->peer_address = optarg;
			break;
		case 'C': {
			uint32_t port;

			if (parse_u32(optarg, &port) < 0 || port > UINT16_MAX)
				return -EINVAL;
			options->control_port = (uint16_t)port;
			break;
		}
		case 'D': {
			uint32_t port;

			if (parse_u32(optarg, &port) < 0 || port > UINT16_MAX)
				return -EINVAL;
			options->data_port = (uint16_t)port;
			break;
		}
		default:
			return -EINVAL;
		}
	}
	if (!options->iterations || !options->message_size ||
	    !options->window || options->window > UIB_UET_PDC_MPR ||
	    options->message_size > 60000 || options->mtu < 256 ||
	    options->mtu > UINT16_MAX)
		return -EINVAL;
	if (strcmp(options->protocol, "rocev2") &&
	    strcmp(options->protocol, "uet") &&
	    strcmp(options->protocol, "dummy"))
		return -EINVAL;
	if (!strcmp(options->protocol, "rocev2") && options->mtu > UIB_PMTU)
		return -EINVAL;
	if (strcmp(options->memory, "host") && strcmp(options->memory, "dmabuf") &&
	    strcmp(options->memory, "gpu-dmabuf"))
		return -EINVAL;
	if (options->role == BENCH_ROLE_LOCAL &&
	    !strcmp(options->memory, "gpu-dmabuf"))
		return -EINVAL;
	if (options->role == BENCH_ROLE_CLIENT && !options->peer_address)
		return -EINVAL;
	if (options->role == BENCH_ROLE_LOCAL &&
	    options->data_port == UINT16_MAX)
		return -EINVAL;
	return 0;
}

static void fill_message(uint8_t *buffer, uint32_t length, uint64_t sequence)
{
	memset(buffer, (int)sequence, length);
	if (length >= sizeof(sequence))
		memcpy(buffer, &sequence, sizeof(sequence));
}

static int verify_message(const uint8_t *buffer, uint32_t length,
			  uint64_t sequence)
{
	uint64_t found = 0;

	if (length < sizeof(found))
		return buffer[0] == (uint8_t)sequence ? 0 : -EIO;
	memcpy(&found, buffer, sizeof(found));
	return found == sequence ? 0 : -EIO;
}

static void poll_pause(void)
{
	struct timespec delay = { .tv_nsec = 1000 };

	nanosleep(&delay, NULL);
}

static int run_batch(struct direct_endpoint *sender,
		     struct direct_endpoint *receiver,
		     const struct options *options, uint32_t first,
		     uint32_t count, uint64_t *posted_ns, uint64_t *latency_ns)
{
	uint32_t receive_completions = 0;
	uint32_t send_completions = 0;
	uint64_t deadline;
	uint32_t slot;
	int i;

	for (slot = 0; slot < count; slot++) {
		uint32_t sequence = first + slot;
		uint8_t *source = sender->buffer +
			(size_t)slot * options->message_size;
		uint8_t *target = receiver->buffer +
			(size_t)slot * options->message_size;

		if (sender->dmabuf_memory &&
		    uib_mr_cpu_begin(sender->mr, UIB_MR_CPU_WRITE) < 0)
			return -EIO;
		fill_message(source, options->message_size, sequence);
		if (sender->dmabuf_memory &&
		    uib_mr_cpu_end(sender->mr, UIB_MR_CPU_WRITE) < 0)
			return -EIO;
		if (receiver->dmabuf_memory &&
		    uib_mr_cpu_begin(receiver->mr, UIB_MR_CPU_WRITE) < 0)
			return -EIO;
		memset(target, 0, options->message_size);
		if (receiver->dmabuf_memory &&
		    uib_mr_cpu_end(receiver->mr, UIB_MR_CPU_WRITE) < 0)
			return -EIO;
		if (direct_post_receive(receiver, BENCH_RECV_ID | (sequence + 1),
					target, options->message_size) < 0)
			return -EIO;
		posted_ns[sequence] = now_ns(CLOCK_MONOTONIC);
		if (direct_post_send(sender, sequence + 1, UIB_WR_SEND, source,
				     options->message_size, 0, 0) < 0)
			return -EIO;
	}

	deadline = now_ns(CLOCK_MONOTONIC) + BENCH_TIMEOUT_NS;
	while (send_completions < count || receive_completions < count) {
		struct uib_wc completions[32];
		int completed;
		int progress = 0;

		completed = uib_poll_cq(sender->cq, 32, completions);
		if (completed)
			progress = 1;
		for (i = 0; i < completed; i++) {
			uint64_t sequence;

			if (completions[i].status != UIB_WC_SUCCESS ||
			    !completions[i].wr_id ||
			    completions[i].wr_id & BENCH_RECV_ID) {
				fprintf(stderr,
					"unexpected send completion: wr_id=%llu status=%u opcode=%u\n",
					(unsigned long long)completions[i].wr_id,
					completions[i].status,
					completions[i].opcode);
				return -EIO;
			}
			sequence = completions[i].wr_id - 1;
			latency_ns[sequence] = now_ns(CLOCK_MONOTONIC) -
				posted_ns[sequence];
			send_completions++;
		}
		completed = uib_poll_cq(receiver->cq, 32, completions);
		if (completed)
			progress = 1;
		for (i = 0; i < completed; i++) {
			uint64_t sequence;
			uint64_t found;
			uint32_t slot;
			int verify_rc;

			if (completions[i].status != UIB_WC_SUCCESS ||
			    !(completions[i].wr_id & BENCH_RECV_ID)) {
				fprintf(stderr,
					"unexpected receive completion: wr_id=%llu status=%u opcode=%u\n",
					(unsigned long long)completions[i].wr_id,
					completions[i].status,
					completions[i].opcode);
				return -EIO;
			}
			sequence = (completions[i].wr_id & ~BENCH_RECV_ID) - 1;
			slot = (uint32_t)sequence - first;
			if (slot >= count) {
				fprintf(stderr,
					"receive completion outside batch: sequence=%llu first=%u count=%u\n",
					(unsigned long long)sequence, first, count);
				return -EIO;
			}
			if (receiver->dmabuf_memory &&
			    uib_mr_cpu_begin(receiver->mr, UIB_MR_CPU_READ) < 0)
				return -EIO;
			verify_rc = verify_message(receiver->buffer +
						   (size_t)slot *
						   options->message_size,
						   options->message_size,
						   sequence);
			if (receiver->dmabuf_memory &&
			    uib_mr_cpu_end(receiver->mr, UIB_MR_CPU_READ) < 0)
				return -EIO;
			if (verify_rc < 0) {
				memcpy(&found, receiver->buffer +
				       (size_t)slot * options->message_size,
				       sizeof(found));
				fprintf(stderr,
					"receive payload mismatch: expected=%llu found=%llu slot=%u\n",
					(unsigned long long)sequence,
					(unsigned long long)found, slot);
				return -EIO;
			}
			receive_completions++;
		}
		if (!progress)
			poll_pause();
		if (now_ns(CLOCK_MONOTONIC) >= deadline)
			return -ETIMEDOUT;
	}
	return 0;
}

static int control_write(int fd, const void *buffer, size_t length)
{
	const uint8_t *position = buffer;

	while (length) {
		ssize_t written = send(fd, position, length, MSG_NOSIGNAL);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -EIO;
		position += written;
		length -= (size_t)written;
	}
	return 0;
}

static int control_read(int fd, void *buffer, size_t length)
{
	uint8_t *position = buffer;

	while (length) {
		ssize_t received = recv(fd, position, length, 0);

		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0)
			return -EIO;
		position += received;
		length -= (size_t)received;
	}
	return 0;
}

static int open_control(const struct options *options,
			char local_ip[INET_ADDRSTRLEN])
{
	struct sockaddr_in address = {0};
	struct sockaddr_in local = {0};
	struct sockaddr_in source = {0};
	struct timespec delay = { .tv_nsec = 20000000 };
	socklen_t local_length = sizeof(local);
	const char *bind_address;
	int listen_fd = -1;
	int one = 1;
	int fd = -1;
	int attempt;

	address.sin_family = AF_INET;
	address.sin_port = htons(options->control_port);
	if (options->role == BENCH_ROLE_CLIENT) {
		if (inet_pton(AF_INET, options->peer_address,
			      &address.sin_addr) != 1)
			return -EINVAL;
		if (options->bind_address) {
			source.sin_family = AF_INET;
			if (inet_pton(AF_INET, options->bind_address,
				      &source.sin_addr) != 1)
				return -EINVAL;
		}
		for (attempt = 0; attempt < 100; attempt++) {
			fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0)
				return -errno;
			if (options->bind_address &&
			    bind(fd, (struct sockaddr *)&source, sizeof(source)) < 0)
				goto err;
			if (!connect(fd, (struct sockaddr *)&address,
				     sizeof(address)))
				break;
			if (errno != ECONNREFUSED)
				goto err;
			close(fd);
			fd = -1;
			nanosleep(&delay, NULL);
		}
		if (fd < 0)
			goto err;
	} else {
		bind_address = options->bind_address ? options->bind_address :
			"0.0.0.0";
		if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1)
			return -EINVAL;
		listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (listen_fd < 0)
			return -errno;
		setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
			   sizeof(one));
		if (bind(listen_fd, (struct sockaddr *)&address,
			 sizeof(address)) < 0 || listen(listen_fd, 1) < 0)
			goto err;
		fd = accept(listen_fd, NULL, NULL);
		if (fd < 0)
			goto err;
		close(listen_fd);
		listen_fd = -1;
	}
	if (getsockname(fd, (struct sockaddr *)&local, &local_length) < 0)
		goto err;
	if (!inet_ntop(AF_INET, &local.sin_addr, local_ip, INET_ADDRSTRLEN))
		goto err;
	return fd;
err:
	if (listen_fd >= 0)
		close(listen_fd);
	if (fd >= 0)
		close(fd);
	return -errno;
}

static int exchange_peer(int control_fd, const struct direct_endpoint *endpoint,
			 const struct options *options,
			 struct sockaddr_in *remote_address,
			 uint32_t *remote_qpn)
{
	struct benchmark_peer local = {0};
	struct benchmark_peer remote;

	local.magic = htonl(BENCH_CONTROL_MAGIC);
	local.qpn = htonl(endpoint->qp->qp_num);
	local.address = endpoint->context->local_addr.sin_addr.s_addr;
	local.iterations = htonl(options->iterations);
	local.message_size = htonl(options->message_size);
	local.window = htonl(options->window);
	local.mtu = htonl(options->mtu);
	local.port = endpoint->context->local_addr.sin_port;
	local.protocol = !strcmp(options->protocol, "rocev2") ? 1 :
		!strcmp(options->protocol, "uet") ? 2 : 3;
	local.version = 2;
	if (control_write(control_fd, &local, sizeof(local)) < 0 ||
	    control_read(control_fd, &remote, sizeof(remote)) < 0)
		return -EIO;
	if (ntohl(remote.magic) != BENCH_CONTROL_MAGIC || remote.version != 2 ||
	    remote.protocol != local.protocol ||
	    ntohl(remote.iterations) != options->iterations ||
	    ntohl(remote.message_size) != options->message_size ||
	    ntohl(remote.window) != options->window ||
	    ntohl(remote.mtu) != options->mtu)
		return -EPROTO;
	memset(remote_address, 0, sizeof(*remote_address));
	remote_address->sin_family = AF_INET;
	remote_address->sin_addr.s_addr = remote.address;
	remote_address->sin_port = remote.port;
	*remote_qpn = ntohl(remote.qpn);
	return 0;
}

static int run_client_batch(struct direct_endpoint *endpoint, int control_fd,
			    const struct options *options, uint32_t first,
			    uint32_t count, uint64_t *posted_ns,
			    uint64_t *latency_ns)
{
	uint32_t completed = 0;
	uint64_t deadline;
	uint32_t slot;
	uint8_t token;
	int i;

	if (control_read(control_fd, &token, sizeof(token)) < 0 || token != 'R') {
		fprintf(stderr, "client did not receive batch-ready token\n");
		return -EIO;
	}
	for (slot = 0; slot < count; slot++) {
		uint32_t sequence = first + slot;
		uint8_t *source = endpoint->buffer +
			(size_t)slot * options->message_size;

		if (endpoint->dmabuf_memory &&
		    uib_mr_cpu_begin(endpoint->mr, UIB_MR_CPU_WRITE) < 0)
			return -EIO;
		fill_message(source, options->message_size, sequence);
		if (endpoint->dmabuf_memory &&
		    uib_mr_cpu_end(endpoint->mr, UIB_MR_CPU_WRITE) < 0)
			return -EIO;
		posted_ns[sequence] = now_ns(CLOCK_MONOTONIC);
		if (direct_post_send(endpoint, sequence + 1, UIB_WR_SEND, source,
				     options->message_size, 0, 0) < 0) {
			fprintf(stderr, "client SEND posting failed: sequence=%u\n",
				sequence);
			return -EIO;
		}
	}
	deadline = now_ns(CLOCK_MONOTONIC) + BENCH_TIMEOUT_NS;
	while (completed < count) {
		struct uib_wc completions[32];
		int count_done;

		count_done = uib_poll_cq(endpoint->cq, 32, completions);
		for (i = 0; i < count_done; i++) {
			uint64_t sequence;

			if (completions[i].status != UIB_WC_SUCCESS ||
			    !completions[i].wr_id) {
				fprintf(stderr,
					"client send completion failed: wr_id=%llu status=%u opcode=%u\n",
					(unsigned long long)completions[i].wr_id,
					completions[i].status,
					completions[i].opcode);
				return -EIO;
			}
			sequence = completions[i].wr_id - 1;
			latency_ns[sequence] = now_ns(CLOCK_MONOTONIC) -
				posted_ns[sequence];
			completed++;
		}
		if (!count_done)
			poll_pause();
		if (now_ns(CLOCK_MONOTONIC) >= deadline)
			return -ETIMEDOUT;
	}
	if (control_read(control_fd, &token, sizeof(token)) < 0 || token != 'D') {
		fprintf(stderr, "client did not receive batch-done token\n");
		return -EIO;
	}
	return 0;
}

static int run_server_batch(struct direct_endpoint *endpoint, int control_fd,
			    const struct options *options, uint32_t first,
			    uint32_t count, uint64_t *posted_ns,
			    uint64_t *latency_ns)
{
	uint32_t completed = 0;
	uint64_t deadline;
	uint32_t slot;
	uint8_t token = 'R';
	int i;

	for (slot = 0; slot < count; slot++) {
		uint32_t sequence = first + slot;
		uint8_t *target = endpoint->buffer +
			(size_t)slot * options->message_size;

		posted_ns[sequence] = now_ns(CLOCK_MONOTONIC);
		if (direct_post_receive(endpoint, BENCH_RECV_ID | (sequence + 1),
					target, options->message_size) < 0)
			return -EIO;
	}
	if (control_write(control_fd, &token, sizeof(token)) < 0)
		return -EIO;
	deadline = now_ns(CLOCK_MONOTONIC) + BENCH_TIMEOUT_NS;
	while (completed < count) {
		struct uib_wc completions[32];
		int count_done;

		count_done = uib_poll_cq(endpoint->cq, 32, completions);
		for (i = 0; i < count_done; i++) {
			uint64_t sequence;
			uint32_t index;
			int verify_rc;

			if (completions[i].status != UIB_WC_SUCCESS ||
			    !(completions[i].wr_id & BENCH_RECV_ID))
				return -EIO;
			sequence = (completions[i].wr_id & ~BENCH_RECV_ID) - 1;
			index = (uint32_t)sequence - first;
			if (index >= count)
				return -EIO;
			if (endpoint->dmabuf_memory &&
			    uib_mr_cpu_begin(endpoint->mr, UIB_MR_CPU_READ) < 0)
				return -EIO;
			verify_rc = verify_message(endpoint->buffer +
						   (size_t)index *
						   options->message_size,
						   options->message_size,
						   sequence);
			if (endpoint->dmabuf_memory &&
			    uib_mr_cpu_end(endpoint->mr, UIB_MR_CPU_READ) < 0)
				return -EIO;
			if (verify_rc < 0)
				return -EIO;
			latency_ns[sequence] = now_ns(CLOCK_MONOTONIC) -
				posted_ns[sequence];
			completed++;
		}
		if (!count_done)
			poll_pause();
		if (now_ns(CLOCK_MONOTONIC) >= deadline)
			return -ETIMEDOUT;
	}
	token = 'D';
	return control_write(control_fd, &token, sizeof(token));
}

static void print_stats(const struct options *options, uint64_t wall_ns,
			uint64_t cpu_ns, const struct rusage *before,
			const struct rusage *after, uint64_t *latency_ns,
			const struct direct_endpoint *sender)
{
	long double seconds;
	long double average;
	long double bytes;
	struct uib_transport_stats transport;
	struct uib_zerocopy_stats zerocopy;
	const char *zerocopy_mode;
	uint64_t system_ns;
	uint64_t user_ns;
	uint32_t i;

	seconds = (long double)wall_ns / 1000000000.0L;
	bytes = (long double)options->iterations * options->message_size;
	average = 0;
	user_ns = timeval_ns(&after->ru_utime) -
		timeval_ns(&before->ru_utime);
	system_ns = timeval_ns(&after->ru_stime) -
		timeval_ns(&before->ru_stime);
	zerocopy_mode = getenv("UIB_ZEROCOPY");
	for (i = 0; i < options->iterations; i++)
		average += latency_ns[i];
	average /= options->iterations;
	qsort(latency_ns, options->iterations, sizeof(*latency_ns), compare_u64);
	if (!zerocopy_mode || !*zerocopy_mode)
		zerocopy_mode = "auto";
	if (options->csv) {
		uib_zerocopy_get_stats(sender->context, &zerocopy);
		printf("CSV,%s,%s,%s,%s,%u,%u,%u,%.9Lf,%.9Lf,%.9Lf,%.9Lf,",
		       options->protocol, sender->context->tops->name,
		       options->memory, zerocopy_mode, options->iterations,
		       options->message_size, options->window, seconds,
		       (long double)cpu_ns / 1000000000.0L,
		       (long double)user_ns / 1000000000.0L,
		       (long double)system_ns / 1000000000.0L);
		printf("%.6Lf,%.6Lf,%.3Lf,%llu,%.0Lf,%llu,%llu,%llu,%llu,",
		       bytes * 8.0L / wall_ns,
		       bytes / (1024.0L * 1024.0L) / seconds,
		       100.0L * cpu_ns / wall_ns,
		       (unsigned long long)latency_ns[0], average,
		       (unsigned long long)percentile(latency_ns,
						      options->iterations, 50),
		       (unsigned long long)percentile(latency_ns,
						      options->iterations, 95),
		       (unsigned long long)percentile(latency_ns,
						      options->iterations, 99),
		       (unsigned long long)latency_ns[options->iterations - 1]);
		printf("%llu,%llu,%llu,%u\n",
		       (unsigned long long)zerocopy.zerocopy_calls,
		       (unsigned long long)zerocopy.zerocopy_bytes,
		       (unsigned long long)zerocopy.deferred_copies,
		       options->mtu);
		return;
	}

	printf("\nUIB traffic stress results\n");
	printf("  protocol / transport : %s / %s\n", options->protocol,
	       sender->context->tops->name);
	printf("  registered memory     : %s\n", options->memory);
	printf("  protocol path MTU     : %u bytes\n", options->mtu);
	printf("  benchmark role        : %s\n",
	       options->role == BENCH_ROLE_CLIENT ? "client" :
	       options->role == BENCH_ROLE_SERVER ? "server" : "local");
	if (!uib_zerocopy_get_stats(sender->context, &zerocopy)) {
		printf("  wire copy calls       : %llu (%llu bytes)\n",
		       (unsigned long long)zerocopy.copied_calls,
		       (unsigned long long)zerocopy.copied_bytes);
		printf("  wire zero-copy calls  : %llu (%llu bytes; %llu deferred copies)\n",
		       (unsigned long long)zerocopy.zerocopy_calls,
		       (unsigned long long)zerocopy.zerocopy_bytes,
		       (unsigned long long)zerocopy.deferred_copies);
	}
	if (!uib_transport_get_stats(sender->context, &transport)) {
		printf("  UDP GSO / GRO         : %s / %s (%llu batches, "
		       "%llu packets; %llu RX batches)\n",
		       transport.gso_enabled ? "on" : "off",
		       transport.gro_enabled ? "on" : "off",
		       (unsigned long long)transport.tx_gso_batches,
		       (unsigned long long)transport.tx_gso_packets,
		       (unsigned long long)transport.rx_gro_batches);
	}
	printf("  operations            : %u SENDs (%u bytes each, window %u)\n",
	       options->iterations, options->message_size, options->window);
	printf("  payload transferred   : %.3Lf MiB\n", bytes / (1024.0L * 1024.0L));
	printf("  wall time             : %.6Lf seconds\n", seconds);
	printf("  payload bandwidth     : %.3Lf Gbit/s (%.3Lf MiB/s)\n",
	       bytes * 8.0L / wall_ns, bytes / (1024.0L * 1024.0L) / seconds);
	printf("  operation rate        : %.0Lf operations/s\n",
	       options->iterations / seconds);
	printf("  CPU time              : %.6Lf s (user %.6Lf, system %.6Lf)\n",
	       (long double)cpu_ns / 1000000000.0L,
	       (long double)user_ns / 1000000000.0L,
	       (long double)system_ns / 1000000000.0L);
	printf("  CPU utilization       : %.1Lf%%\n",
	       100.0L * cpu_ns / wall_ns);
	printf("  completion latency ns : min %llu, avg %.0Lf, p50 %llu, "
	       "p95 %llu, p99 %llu, max %llu\n",
	       (unsigned long long)latency_ns[0], average,
	       (unsigned long long)percentile(latency_ns,
					       options->iterations, 50),
	       (unsigned long long)percentile(latency_ns,
					       options->iterations, 95),
	       (unsigned long long)percentile(latency_ns,
					       options->iterations, 99),
	       (unsigned long long)latency_ns[options->iterations - 1]);
	if (!strcmp(options->protocol, "uet")) {
		struct uib_uet_nscc nscc;

		if (!uib_uet_nscc_query_qp(sender->qp, &nscc))
			printf("  NSCC final state      : cwnd %llu bytes, "
			       "byte-clock residual %lld, base RTT %llu ns\n",
			       (unsigned long long)nscc.cwnd,
			       (long long)nscc.inflight,
			       (unsigned long long)nscc.base_rtt_ns);
	}
}

int main(int argc, char **argv)
{
	struct direct_endpoint sender = {0}, receiver = {0};
	struct direct_endpoint *endpoint = NULL;
	struct sockaddr_in remote_address;
	struct rusage usage_before, usage_after;
	struct options options;
	uint64_t wall_start, wall_end, cpu_start, cpu_end;
	uint64_t *latency_ns = NULL;
	uint64_t *posted_ns = NULL;
	char local_ip[INET_ADDRSTRLEN];
	size_t buffer_length;
	uint32_t remote_qpn;
	uint32_t first;
	int control_fd = -1;
	int batch_rc;
	int rc = 1;

	if (parse_options(argc, argv, &options) < 0) {
		fprintf(stderr, "usage: %s", argv[0]);
		fputs(" [--protocol rocev2|uet|dummy]", stderr);
		fputs(" [--memory host|dmabuf|gpu-dmabuf] [--iterations N]",
		      stderr);
		fputs(" [--size BYTES] [--window N] [--mtu BYTES] [--csv]",
		      stderr);
		fputs(" [--role local|server|client] [--bind IPv4]", stderr);
		fputs(" [--peer IPv4] [--control-port N] [--data-port N]\n",
		      stderr);
		return 2;
	}
	buffer_length = (size_t)options.window * options.message_size;
	posted_ns = calloc(options.iterations, sizeof(*posted_ns));
	latency_ns = calloc(options.iterations, sizeof(*latency_ns));
	if (!posted_ns || !latency_ns)
		goto out_free;
	if (options.role == BENCH_ROLE_LOCAL) {
		if (direct_endpoint_open_memory(&sender, options.protocol,
						"127.0.0.1", options.data_port,
						buffer_length, options.window,
						options.memory) < 0 ||
		    direct_endpoint_open_memory(&receiver, options.protocol,
						"127.0.0.1",
						(uint16_t)(options.data_port + 1),
						buffer_length, options.window,
						options.memory) < 0)
			goto out_endpoints;
		if (direct_endpoint_connect(&sender, &receiver, options.mtu) < 0 ||
		    direct_endpoint_connect(&receiver, &sender, options.mtu) < 0)
			goto out_endpoints;
		endpoint = &sender;
	} else {
		control_fd = open_control(&options, local_ip);
		if (control_fd < 0) {
			fprintf(stderr, "control connection failed: %s\n",
				strerror(-control_fd));
			goto out_endpoints;
		}
		endpoint = options.role == BENCH_ROLE_CLIENT ? &sender : &receiver;
		if (direct_endpoint_open_memory(endpoint, options.protocol,
						local_ip, options.data_port,
						buffer_length, options.window,
						options.memory) < 0)
			goto out_endpoints;
		if (exchange_peer(control_fd, endpoint, &options,
				  &remote_address, &remote_qpn) < 0 ||
		    direct_endpoint_connect_address(endpoint, &remote_address,
						    remote_qpn,
						    options.mtu) < 0)
			goto out_endpoints;
	}

	getrusage(RUSAGE_SELF, &usage_before);
	wall_start = now_ns(CLOCK_MONOTONIC);
	cpu_start = now_ns(CLOCK_PROCESS_CPUTIME_ID);
	for (first = 0; first < options.iterations;
	     first += options.window) {
		uint32_t count = options.iterations - first;

		if (count > options.window)
			count = options.window;
		if (options.role == BENCH_ROLE_LOCAL)
			batch_rc = run_batch(&sender, &receiver, &options, first,
					     count, posted_ns, latency_ns);
		else if (options.role == BENCH_ROLE_CLIENT)
			batch_rc = run_client_batch(endpoint, control_fd, &options,
						    first, count, posted_ns,
						    latency_ns);
		else
			batch_rc = run_server_batch(endpoint, control_fd, &options,
						    first, count, posted_ns,
						    latency_ns);
		if (batch_rc < 0) {
			fprintf(stderr, "traffic failed in batch beginning at %u: %s\n",
				first, strerror(-batch_rc));
			goto out_endpoints;
		}
	}
	cpu_end = now_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_end = now_ns(CLOCK_MONOTONIC);
	getrusage(RUSAGE_SELF, &usage_after);
	print_stats(&options, wall_end - wall_start, cpu_end - cpu_start,
		    &usage_before, &usage_after, latency_ns, endpoint);
	printf("RESULT PASS\n");
	rc = 0;

out_endpoints:
	if (control_fd >= 0)
		close(control_fd);
	direct_endpoint_close(&receiver);
	direct_endpoint_close(&sender);
out_free:
	free(latency_ns);
	free(posted_ns);
	return rc;
}
