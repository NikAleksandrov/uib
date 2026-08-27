#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d /tmp/uib-ucx-libfabric-test.XXXXXX)
test_timeout=${COMM_TEST_TIMEOUT:-30}
iterations=${COMM_TEST_ITERATIONS:-100}
warmups=${COMM_TEST_WARMUPS:-10}
bind_port=${COMM_TEST_BIND_PORT:-19800}
message_sizes=${COMM_TEST_MESSAGE_SIZES:-l:1,64,1024}
ucx_tests=${COMM_TEST_UCX_TESTS:-tag_lat stream_lat ucp_put_lat ucp_get ucp_fadd ucp_cswap}
uct_tests=${COMM_TEST_UCT_TESTS:-am_lat put_lat get}
server_pid=

if [ ! -c /dev/infiniband/uib0 ]; then
	echo "UCX/libfabric require the UIB character-device anchor at" \
		"/dev/infiniband/uib0; run this gate through" \
		"simulation/runtime/with-device-anchor" >&2
	exit 1
fi

cleanup()
{
	status=$?
	if [ -n "$server_pid" ]; then
		kill "$server_pid" >/dev/null 2>&1 || true
		wait "$server_pid" >/dev/null 2>&1 || true
	fi
	if [ "$status" -eq 0 ]; then
		rm -rf -- "$test_dir"
	else
		echo "UCX/libfabric logs retained in $test_dir" >&2
	fi
}
trap cleanup EXIT INT TERM

for command_name in ucx_info ucx_perftest fi_info fi_rdm_pingpong timeout; do
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

run_ucx_test()
{
	test_name=$1
	server_log=$test_dir/ucx-$test_name-server.log
	client_log=$test_dir/ucx-$test_name-client.log

	timeout -k 2 "$test_timeout" env \
		LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.1 \
		UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
		UIB_ZEROCOPY=off UCX_TLS=ud UCX_NET_DEVICES=uib0:1 \
		UCX_LOG_LEVEL=info ucx_perftest -t "$test_name" \
		-n "$iterations" -w "$warmups" -P 0 -p "$bind_port" \
		> "$server_log" 2>&1 &
	server_pid=$!
	sleep 1
	timeout -k 2 "$test_timeout" env \
		LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.2 \
		UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
		UIB_ZEROCOPY=off UCX_TLS=ud UCX_NET_DEVICES=uib0:1 \
		UCX_LOG_LEVEL=info ucx_perftest -t "$test_name" \
		-n "$iterations" -w "$warmups" -P 0 -p "$bind_port" \
		127.0.0.1 > "$client_log" 2>&1
	wait "$server_pid"
	server_pid=
	grep -F 'ud_verbs/uib0:1' "$client_log" >/dev/null
	if grep -Ei 'transport.*(tcp|self|posix|sysv|cma)' \
		"$client_log" >/dev/null; then
		echo "UCX selected a forbidden non-verbs data transport" >&2
		exit 1
	fi
}

for test_name in $ucx_tests; do
	run_ucx_test "$test_name"
done

run_uct_test()
{
	test_name=$1
	server_log=$test_dir/uct-$test_name-server.log
	client_log=$test_dir/uct-$test_name-client.log
	layout=short
	if [ "$test_name" = get ]; then
		layout=bcopy
	fi

	timeout -k 2 "$test_timeout" env \
		LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.1 \
		UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
		UIB_ZEROCOPY=off UCX_LOG_LEVEL=info UCX_RC_VERBS_FLUSH_MODE=fc \
		ucx_perftest -t "$test_name" -x rc_verbs -d uib0:1 \
		-D "$layout" -n "$iterations" -w "$warmups" -P 0 \
		-p "$bind_port" -I \
		> "$server_log" 2>&1 &
	server_pid=$!
	sleep 1
	timeout -k 2 "$test_timeout" env \
		LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.2 \
		UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
		UIB_ZEROCOPY=off UCX_LOG_LEVEL=info UCX_RC_VERBS_FLUSH_MODE=fc \
		ucx_perftest -t "$test_name" -x rc_verbs -d uib0:1 \
		-D "$layout" -n "$iterations" -w "$warmups" -P 0 \
		-p "$bind_port" -I \
		127.0.0.1 > "$client_log" 2>&1
	wait "$server_pid"
	server_pid=
	grep -F 'API:          transport layer' "$server_log" >/dev/null
	grep -F 'Final:' "$client_log" >/dev/null
}

for test_name in $uct_tests; do
	run_uct_test "$test_name"
done

test_name=fi_rdm_pingpong
log_name=${test_name#fi_}
provider='verbs;ofi_rxd'
timeout -k 2 "$test_timeout" env \
	LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.1 \
	UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
	UIB_ZEROCOPY=off FI_PROVIDER="$provider" FI_LOG_LEVEL=info \
	FI_VERBS_DGRAM_USE_NAME_SERVER=0 \
	"$test_name" -p "$provider" -d uib0-dgram \
	-I "$iterations" -S "$message_sizes" -v -b \
	> "$test_dir/$log_name-server.log" \
	2>&1 &
server_pid=$!
sleep 1
timeout -k 2 "$test_timeout" env \
	LD_PRELOAD="$project_dir/libuibv.so" UIB_BIND_IP=127.0.0.2 \
	UIB_BIND_PORT="$bind_port" UIB_TRANSPORT_MODE=udp-sync \
	UIB_ZEROCOPY=off FI_PROVIDER="$provider" FI_LOG_LEVEL=info \
	FI_VERBS_DGRAM_USE_NAME_SERVER=0 \
	"$test_name" -p "$provider" -d uib0-dgram \
	-I "$iterations" -S "$message_sizes" -v -b 127.0.0.1 \
	> "$test_dir/$log_name-client.log" 2>&1
wait "$server_pid"
server_pid=
grep -F 'Using verbs provider' \
	"$test_dir/$log_name-client.log" >/dev/null
grep -F 'Using ofi_rxd provider' \
	"$test_dir/$log_name-client.log" >/dev/null
grep -F uib0-dgram "$test_dir/$log_name-client.log" >/dev/null
if grep -Ei 'Using (tcp|sockets) provider' \
	"$test_dir/$log_name-client.log" >/dev/null; then
	echo "libfabric selected a forbidden software provider" >&2
	exit 1
fi

echo "actual UCX/libfabric UIB compatibility test: PASS"
