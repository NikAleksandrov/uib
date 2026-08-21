#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
server_log=/tmp/uib-benchmark-server.log
client_log=/tmp/uib-benchmark-client.log
control_port=19290
data_port=19200

cleanup()
{
	if [ -n "${server_pid:-}" ]; then
		kill "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

run_pair()
{
	protocol=$1
	transport=$2
	zerocopy=$3
	memory=$4

	env UIB_BENCH_ROLE=server \
		UIB_BENCH_BIND=127.0.0.1 \
		UIB_BENCH_PROTOCOL="$protocol" \
		UIB_BENCH_MEMORY="$memory" \
		UIB_BENCH_ITERATIONS=64 \
		UIB_BENCH_SIZE=16384 \
		UIB_BENCH_WINDOW=16 \
		UIB_BENCH_CONTROL_PORT="$control_port" \
		UIB_BENCH_DATA_PORT="$data_port" \
		UIB_TRANSPORT_MODE="$transport" \
		UIB_UDP_MODE=socket \
		UIB_ZEROCOPY="$zerocopy" \
		timeout 30 "$project_dir/examples/stress_bench" \
		>"$server_log" 2>&1 &
	server_pid=$!
	sleep 0.1
	if ! env UIB_BENCH_ROLE=client \
		UIB_BENCH_BIND=127.0.0.2 \
		UIB_BENCH_PEER=127.0.0.1 \
		UIB_BENCH_PROTOCOL="$protocol" \
		UIB_BENCH_MEMORY="$memory" \
		UIB_BENCH_ITERATIONS=64 \
		UIB_BENCH_SIZE=16384 \
		UIB_BENCH_WINDOW=16 \
		UIB_BENCH_CONTROL_PORT="$control_port" \
		UIB_BENCH_DATA_PORT="$data_port" \
		UIB_TRANSPORT_MODE="$transport" \
		UIB_UDP_MODE=socket \
		UIB_ZEROCOPY="$zerocopy" \
		timeout 30 "$project_dir/examples/stress_bench" \
		>"$client_log" 2>&1; then
		cat "$client_log"
		cat "$server_log"
		return 1
	fi
	if ! wait "$server_pid"; then
		server_pid=
		cat "$client_log"
		cat "$server_log"
		return 1
	fi
	server_pid=
	grep -q 'benchmark role        : client' "$client_log"
	grep -q 'benchmark role        : server' "$server_log"
	grep -q 'RESULT PASS' "$client_log"
	grep -q 'RESULT PASS' "$server_log"
	printf 'remote benchmark %-7s %-11s %-8s %-6s PASS\n' \
		"$protocol" "$transport" "$zerocopy" "$memory"
	control_port=$((control_port + 1))
	data_port=$((data_port + 1))
}

for protocol in rocev2 uet dummy; do
	run_pair "$protocol" udp-io-uring off host
	run_pair "$protocol" udp-io-uring required dmabuf
	run_pair "$protocol" udp-sync off host
	run_pair "$protocol" udp-sync required dmabuf
	run_pair "$protocol" tcp-devmem off host
	run_pair "$protocol" tcp-devmem required dmabuf
done
