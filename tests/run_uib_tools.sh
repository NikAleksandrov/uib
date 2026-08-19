#!/bin/sh
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
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
	UIB_BIND_IP=198.51.100.1 \
	UIB_BIND_PORT=4791 \
	timeout -k 1 30 ibv_rc_pingpong \
	-d uib0 -g 0 -m 1024 -s 2051 -n 64 -c \
	>"$server_log" 2>&1 &
server_pid=$!
sleep 1

ip netns exec uib-tools-b env \
	LD_PRELOAD="$project_dir/libuibv.so" \
	UIB_BIND_IP=198.51.100.2 \
	UIB_BIND_PORT=4791 \
	timeout -k 1 30 ibv_rc_pingpong \
	-d uib0 -g 0 -m 1024 -s 2051 -n 64 -c 198.51.100.1 \
	>"$client_log" 2>&1
wait "$server_pid"
server_pid=

echo "UIB ibv_rc_pingpong client:"
cat "$client_log"
echo "UIB ibv_rc_pingpong server:"
cat "$server_log"
