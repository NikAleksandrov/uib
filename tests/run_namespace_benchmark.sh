#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
keep_on_failure=${UIB_NAMESPACE_KEEP_ON_FAILURE:-0}
namespace_prefix=uib-bench-$$
server_namespace=${namespace_prefix}-server
client_namespace=${namespace_prefix}-client
server_interface=uibs$$
client_interface=uibc$$
server_log=/tmp/uib-namespace-server-$$.log
client_log=/tmp/uib-namespace-client-$$.log
allowed_cpus=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/self/status)
first_allowed_cpu=$(printf '%s\n' "$allowed_cpus" | sed 's/[-,].*//')
pin_cpu=${UIB_BENCH_PIN_CPU:-$first_allowed_cpu}
client_pin_cpu=$pin_cpu
if [ -z "${UIB_BENCH_PIN_CPU:-}" ]; then
	client_pin_cpu=$(printf '%s\n' "$allowed_cpus" | awk -F, '
		{
			split($1, range, "-")
			if (range[2] > range[1])
				print range[1] + 1
			else if (NF > 1) {
				split($2, range, "-")
				print range[1]
			} else
				print range[1]
		}')
fi
control_port=19390
data_port=19300

case ${1:-} in
'')
	;;
--keep-on-failure)
	keep_on_failure=1
	;;
*)
	printf 'usage: %s [--keep-on-failure]\n' "$0" >&2
	exit 2
	;;
esac

cleanup()
{
	exit_status=$?

	if [ "$exit_status" -ne 0 ] && [ "$keep_on_failure" = 1 ]; then
		printf 'Preserved failed namespace topology:\n' >&2
		printf '  server namespace: %s\n' "$server_namespace" >&2
		printf '  client namespace: %s\n' "$client_namespace" >&2
		printf '  server PID/log: %s / %s\n' \
			"${server_pid:-none}" "$server_log" >&2
		printf '  client log: %s\n' "$client_log" >&2
		return
	fi
	if [ -n "${server_pid:-}" ]; then
		kill "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	ip netns del "$server_namespace" 2>/dev/null || true
	ip netns del "$client_namespace" 2>/dev/null || true
	rm -f "$server_log" "$client_log"
}
trap cleanup EXIT INT TERM

skip_or_fail()
{
	if [ "${UIB_NAMESPACE_REQUIRED:-0}" = 1 ]; then
		printf 'namespace benchmark: %s\n' "$1" >&2
		exit 1
	fi
	printf 'SKIP namespace benchmark: %s\n' "$1"
	exit 0
}

if ! command -v ip >/dev/null 2>&1; then
	skip_or_fail 'iproute2 is unavailable'
fi
if ! command -v taskset >/dev/null 2>&1; then
	skip_or_fail 'taskset is unavailable'
fi
if ! ip netns add "$server_namespace" 2>/dev/null; then
	skip_or_fail 'network-namespace creation needs CAP_SYS_ADMIN and CAP_NET_ADMIN'
fi
if ! ip netns add "$client_namespace" 2>/dev/null; then
	skip_or_fail 'second network namespace could not be created'
fi
ip link add "$server_interface" type veth peer name "$client_interface"
ip link set "$server_interface" netns "$server_namespace"
ip link set "$client_interface" netns "$client_namespace"
ip -n "$server_namespace" link set lo up
ip -n "$client_namespace" link set lo up
ip -n "$server_namespace" address add 192.0.2.1/30 dev "$server_interface"
ip -n "$client_namespace" address add 192.0.2.2/30 dev "$client_interface"
ip -n "$server_namespace" link set "$server_interface" mtu 9000
ip -n "$client_namespace" link set "$client_interface" mtu 9000
ip -n "$server_namespace" link set "$server_interface" up
ip -n "$client_namespace" link set "$client_interface" up

run_endpoint()
{
	namespace=$1
	role=$2
	bind_address=$3
	peer_address=$4
	affinity=$5

	if [ "$affinity" = pinned ]; then
		endpoint_cpu=$pin_cpu
		if [ "$role" = client ]; then
			endpoint_cpu=$client_pin_cpu
		fi
		ip netns exec "$namespace" taskset -c "$endpoint_cpu" env \
			UIB_BENCH_ROLE="$role" \
			UIB_BENCH_BIND="$bind_address" \
			UIB_BENCH_PEER="$peer_address" \
			UIB_BENCH_PROTOCOL="$protocol" \
			UIB_BENCH_MEMORY="$memory" \
			UIB_BENCH_ITERATIONS=32 \
			UIB_BENCH_SIZE=16384 \
			UIB_BENCH_WINDOW=8 \
			UIB_BENCH_MTU="$mtu" \
			UIB_BENCH_CONTROL_PORT="$control_port" \
			UIB_BENCH_DATA_PORT="$data_port" \
			UIB_TRANSPORT_MODE="$transport" \
			UIB_UDP_MODE=socket \
			UIB_ZEROCOPY="$zerocopy" \
			timeout 30 "$project_dir/examples/stress_bench"
		return
	fi
	ip netns exec "$namespace" env \
		UIB_BENCH_ROLE="$role" \
		UIB_BENCH_BIND="$bind_address" \
		UIB_BENCH_PEER="$peer_address" \
		UIB_BENCH_PROTOCOL="$protocol" \
		UIB_BENCH_MEMORY="$memory" \
		UIB_BENCH_ITERATIONS=32 \
		UIB_BENCH_SIZE=16384 \
		UIB_BENCH_WINDOW=8 \
		UIB_BENCH_MTU="$mtu" \
		UIB_BENCH_CONTROL_PORT="$control_port" \
		UIB_BENCH_DATA_PORT="$data_port" \
		UIB_TRANSPORT_MODE="$transport" \
		UIB_UDP_MODE=socket \
		UIB_ZEROCOPY="$zerocopy" \
		timeout 30 "$project_dir/examples/stress_bench"
}

run_pair()
{
	protocol=$1
	transport=$2
	zerocopy=$3
	memory=$4
	affinity=$5

	if [ "$protocol" = rocev2 ] && [ "$mtu" -gt 4096 ]; then
		printf 'namespace benchmark %-7s %-11s %-8s %-6s %-8s mtu=%s SKIP\n' \
			"$protocol" "$transport" "$zerocopy" "$memory" \
			"$affinity" "$mtu"
		printf '%s\n' '  reason: RoCEv2 path MTU maximum is 4096'
		return
	fi
	run_endpoint "$server_namespace" server 192.0.2.1 '' "$affinity" \
		>"$server_log" 2>&1 &
	server_pid=$!
	sleep 0.1
	if ! run_endpoint "$client_namespace" client 192.0.2.2 192.0.2.1 \
		"$affinity" >"$client_log" 2>&1; then
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
	printf 'namespace benchmark %-7s %-11s %-8s %-6s %-8s mtu=%s PASS\n' \
		"$protocol" "$transport" "$zerocopy" "$memory" "$affinity" \
		"$mtu"
	control_port=$((control_port + 1))
	data_port=$((data_port + 1))
}

for affinity in ${UIB_NAMESPACE_AFFINITIES:-unpinned pinned}; do
	for mtu in ${UIB_NAMESPACE_MTUS:-1500 4096 8192}; do
		for protocol in ${UIB_NAMESPACE_PROTOCOLS:-rocev2 uet dummy}; do
			for memory in ${UIB_NAMESPACE_MEMORY:-host dmabuf}; do
				run_pair "$protocol" udp-io-uring off "$memory" \
					"$affinity"
				run_pair "$protocol" udp-io-uring required "$memory" \
					"$affinity"
				run_pair "$protocol" udp-sync off "$memory" \
					"$affinity"
				run_pair "$protocol" udp-sync required "$memory" \
					"$affinity"
				run_pair "$protocol" tcp-devmem off "$memory" \
					"$affinity"
				run_pair "$protocol" tcp-devmem required "$memory" \
					"$affinity"
			done
		done
	done
done

printf 'namespace benchmark matrix: PASS\n'
