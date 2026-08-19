#!/bin/sh
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
protocol=${UIB_TEST_PROTOCOL:-rocev2}
protocol_port=${UIB_TEST_PORT:-4791}
iterations=${UIB_TEST_ITERS:-64}
server_log=/tmp/uib-pingpong-server.log
client_log=/tmp/uib-pingpong-client.log
server_pid=

cleanup()
{
	if [ -n "$server_pid" ]; then
		kill "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	ip netns delete uib-tools-a 2>/dev/null || true
	ip netns delete uib-tools-b 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cleanup
ip netns add uib-tools-a
ip netns add uib-tools-b
ip link add uiba0 type veth peer name uibb0
ip link set uiba0 netns uib-tools-a
ip link set uibb0 netns uib-tools-b
ip netns exec uib-tools-a ip link set lo up
ip netns exec uib-tools-a ip addr add 198.51.100.1/24 dev uiba0
ip netns exec uib-tools-a ip link set uiba0 up
ip netns exec uib-tools-b ip link set lo up
ip netns exec uib-tools-b ip addr add 198.51.100.2/24 dev uibb0
ip netns exec uib-tools-b ip link set uibb0 up
ip netns exec uib-tools-a env \
	LD_PRELOAD="$project_dir/libuibv.so" \
	UIB_PROTOCOL="$protocol" \
	UIB_BIND_IP=198.51.100.1 \
	UIB_BIND_PORT="$protocol_port" \
	timeout -k 1 30 ibv_rc_pingpong \
	-d uib0 -g 0 -m 1024 -s 2051 -n "$iterations" -c \
	>"$server_log" 2>&1 &
server_pid=$!
sleep 1

ip netns exec uib-tools-b env \
	LD_PRELOAD="$project_dir/libuibv.so" \
	UIB_PROTOCOL="$protocol" \
	UIB_BIND_IP=198.51.100.2 \
	UIB_BIND_PORT="$protocol_port" \
	timeout -k 1 30 ibv_rc_pingpong \
	-d uib0 -g 0 -m 1024 -s 2051 -n "$iterations" -c 198.51.100.1 \
	>"$client_log" 2>&1
wait "$server_pid"
server_pid=

echo "UIB $protocol ibv_rc_pingpong client:"
cat "$client_log"
echo "UIB $protocol ibv_rc_pingpong server:"
cat "$server_log"
