#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rxe_log=/tmp/uib-rxe-server.log
uib_log=/tmp/uib-rxe-client.log

cleanup()
{
	if [ -n "${server_pid:-}" ]; then
		kill "$server_pid" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

modprobe rdma_rxe
ip netns add uibns
ip link add rxeeth type veth peer name uibeth
ip link set uibeth netns uibns
ip addr add 192.0.2.1/24 dev rxeeth
ip link set rxeeth up
ip netns exec uibns ip link set lo up
ip netns exec uibns ip addr add 192.0.2.2/24 dev uibeth
ip netns exec uibns ip link set uibeth up
rdma link add rxe0 type rxe netdev rxeeth

"$project_dir/tests/rxe_interop" server rxe0 1 >"$rxe_log" 2>&1 &
server_pid=$!
sleep 1

ip netns exec uibns env \
	LD_PRELOAD="$project_dir/libuibv.so" \
	UIB_BIND_IP=192.0.2.2 \
	UIB_BIND_PORT=4791 \
	"$project_dir/tests/rxe_interop" client uib0 0 192.0.2.1 \
	>"$uib_log" 2>&1
wait "$server_pid"
server_pid=

echo "UIB client:"
cat "$uib_log"
echo "RXE server:"
cat "$rxe_log"
