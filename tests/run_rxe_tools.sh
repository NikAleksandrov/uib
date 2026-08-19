#!/bin/sh
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
rxe_log=/tmp/uib-rxe-pingpong-server.log
uib_log=/tmp/uib-rxe-pingpong-client.log
server_pid=

cleanup()
{
	if [ -n "$server_pid" ]; then
		kill "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	rdma link delete rxe-tools0 2>/dev/null || true
	ip netns delete uib-rxe-tools 2>/dev/null || true
	ip link delete rxe-tools-eth 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cleanup
modprobe rdma_rxe
ip netns add uib-rxe-tools
ip link add rxe-tools-eth type veth peer name uib-tools-eth
ip link set uib-tools-eth netns uib-rxe-tools
ip addr add 192.0.2.1/24 dev rxe-tools-eth
ip link set rxe-tools-eth up
ip netns exec uib-rxe-tools ip link set lo up
ip netns exec uib-rxe-tools ip addr add 192.0.2.2/24 dev uib-tools-eth
ip netns exec uib-rxe-tools ip link set uib-tools-eth up
rdma link add rxe-tools0 type rxe netdev rxe-tools-eth

timeout -k 1 30 ibv_rc_pingpong \
	-d rxe-tools0 -g 1 -m 1024 -s 2051 -n 64 -c \
	>"$rxe_log" 2>&1 &
server_pid=$!
sleep 1

ip netns exec uib-rxe-tools env \
	LD_PRELOAD="$project_dir/libuibv.so" \
	UIB_BIND_IP=192.0.2.2 \
	UIB_BIND_PORT=4791 \
	timeout -k 1 30 ibv_rc_pingpong \
	-d uib0 -g 0 -m 1024 -s 2051 -n 64 -c 192.0.2.1 \
	>"$uib_log" 2>&1
wait "$server_pid"
server_pid=

echo "UIB ibv_rc_pingpong client:"
cat "$uib_log"
echo "RXE ibv_rc_pingpong server:"
cat "$rxe_log"
