#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d /tmp/uib-ucx-libfabric-test.XXXXXX)
test_timeout=${COMM_TEST_TIMEOUT:-30}
iterations=${COMM_TEST_ITERATIONS:-100}
bind_port=${COMM_TEST_BIND_PORT:-19800}
server_pid=

cleanup()
{
	if [ -n "$server_pid" ]; then
		kill "$server_pid" >/dev/null 2>&1 || true
		wait "$server_pid" >/dev/null 2>&1 || true
	fi
	rm -rf -- "$test_dir"
}
trap cleanup EXIT INT TERM

for command_name in ucx_info ucx_perftest fi_info fi_pingpong \
	fi_rdm_pingpong timeout; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required UCX/libfabric command not found: $command_name" >&2
		exit 1
	fi
done

if [ ! -r "$project_dir/libuibv.so" ]; then
	echo "build libuibv.so before running the UCX/libfabric gate" >&2
	exit 1
fi

env LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.1 \
	UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
	UIB_ZEROCOPY=off UCX_TLS=rc,ud UCX_NET_DEVICES=uib0:1 \
	ucx_info -d > "$test_dir/ucx-info.log" 2>&1
grep -F uib0 "$test_dir/ucx-info.log" >/dev/null

env LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.1 \
	UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
	UIB_ZEROCOPY=off FI_PROVIDER=verbs \
	fi_info -p verbs > "$test_dir/fi-info.log" 2>&1
grep -E 'provider:[[:space:]]*verbs|prov_name:[[:space:]]*verbs' \
	"$test_dir/fi-info.log" >/dev/null
grep -F uib0 "$test_dir/fi-info.log" >/dev/null

timeout -k 2 "$test_timeout" env \
	LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.1 \
	UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
	UIB_ZEROCOPY=off UCX_TLS=rc,ud UCX_NET_DEVICES=uib0:1 \
	UCX_LOG_LEVEL=info ucx_perftest -t tag_lat -n "$iterations" \
	-p "$bind_port" > "$test_dir/ucx-server.log" 2>&1 &
server_pid=$!
sleep 1
timeout -k 2 "$test_timeout" env \
	LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.2 \
	UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
	UIB_ZEROCOPY=off UCX_TLS=rc,ud UCX_NET_DEVICES=uib0:1 \
	UCX_LOG_LEVEL=info ucx_perftest -t tag_lat -n "$iterations" \
	-p "$bind_port" 127.0.0.1 > "$test_dir/ucx-client.log" 2>&1
wait "$server_pid"
server_pid=
grep -F uib0 "$test_dir/ucx-client.log" >/dev/null
if grep -Ei 'transport.*(tcp|self|posix|sysv|cma)' \
	"$test_dir/ucx-client.log" >/dev/null; then
	echo "UCX selected a forbidden non-verbs data transport" >&2
	exit 1
fi

for test_name in fi_pingpong fi_rdm_pingpong; do
	log_name=${test_name#fi_}
	provider=verbs
	if [ "$test_name" = fi_rdm_pingpong ]; then
		provider='ofi_rxm;verbs'
	fi
	timeout -k 2 "$test_timeout" env \
		LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.1 \
		UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
		UIB_ZEROCOPY=off FI_PROVIDER="$provider" FI_LOG_LEVEL=info \
		"$test_name" -p "$provider" > "$test_dir/$log_name-server.log" \
		2>&1 &
	server_pid=$!
	sleep 1
	timeout -k 2 "$test_timeout" env \
		LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.2 \
		UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
		UIB_ZEROCOPY=off FI_PROVIDER="$provider" FI_LOG_LEVEL=info \
		"$test_name" -p "$provider" 127.0.0.1 \
		> "$test_dir/$log_name-client.log" 2>&1
	wait "$server_pid"
	server_pid=
	grep -Ei 'provider.*verbs|prov_name.*verbs' \
		"$test_dir/$log_name-client.log" >/dev/null
	if grep -Ei 'provider.*(tcp|sockets)' \
		"$test_dir/$log_name-client.log" >/dev/null; then
		echo "libfabric selected a forbidden software provider" >&2
		exit 1
	fi
done

echo "actual UCX/libfabric UIB compatibility test: PASS"
