#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

root=${UIB_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
role=${UIB_AFXDP_ROLE:-}
bind=${UIB_AFXDP_BIND:-}
peer=${UIB_AFXDP_PEER:-}
interface=${UIB_AFXDP_IFACE:-}
queue=${UIB_AFXDP_QUEUE:-0}
peer_mac=${UIB_AFXDP_PEER_MAC:-}
protocol=${UIB_AFXDP_PROTOCOL:-uet}
iterations=${UIB_AFXDP_ITERATIONS:-10000}
size=${UIB_AFXDP_SIZE:-60000}
window=${UIB_AFXDP_WINDOW:-128}
mtu=${UIB_AFXDP_MTU:-3000}
control_port=${UIB_AFXDP_CONTROL_PORT:-19781}
data_port=${UIB_AFXDP_DATA_PORT:-19681}
zerocopy=${UIB_AFXDP_ZEROCOPY:-required}
program=${UIB_AFXDP_PROGRAM:-$root/bpf/uib_afxdp_kern.o}
privilege=${UIB_AFXDP_PRIVILEGE:-}
privilege_args=()
program_args=()

usage()
{
	cat >&2 <<'EOF'
Required environment:
  UIB_AFXDP_ROLE=server|client
  UIB_AFXDP_BIND=<local IPv4>
  UIB_AFXDP_PEER=<peer IPv4>
  UIB_AFXDP_IFACE=<netdev>
  UIB_AFXDP_PEER_MAC=<peer MAC>
Optional: UIB_AFXDP_QUEUE, UIB_AFXDP_PROTOCOL, UIB_AFXDP_ITERATIONS,
UIB_AFXDP_SIZE, UIB_AFXDP_WINDOW, UIB_AFXDP_MTU,
UIB_AFXDP_CONTROL_PORT, UIB_AFXDP_DATA_PORT, UIB_AFXDP_ZEROCOPY,
UIB_AFXDP_PROGRAM (or "off"), UIB_AFXDP_PRIVILEGE.
EOF
	exit 2
}

case "$role" in
server|client)
	;;
*)
	usage
	;;
esac

if [[ -z $bind || -z $peer || -z $interface || -z $peer_mac ]]; then
	usage
fi
if [[ ! -x $root/examples/stress_bench || ! -r $root/libuibv.so ]]; then
	echo "build libuibv.so and examples/stress_bench first" >&2
	exit 1
fi
if [[ $program != off ]]; then
	if [[ ! -r $program ]]; then
		echo "build the AF_XDP program with: make afxdp-program" >&2
		exit 1
	fi
	program_args=("UIB_AFXDP_PROGRAM=$program")
fi
if [[ -n $privilege ]]; then
	read -r -a privilege_args <<<"$privilege"
fi

exec "${privilege_args[@]}" env \
	LD_PRELOAD="$root/libuibv.so" \
	UIB_PROTOCOL="$protocol" \
	UIB_TRANSPORT_MODE=udp-zc-afxdp \
	UIB_BIND_IP="$bind" \
	UIB_BIND_PORT="$data_port" \
	UIB_AFXDP_IFACE="$interface" \
	UIB_AFXDP_QUEUE="$queue" \
	UIB_AFXDP_PEER_MAC="$peer_mac" \
	UIB_ZEROCOPY="$zerocopy" \
	UIB_STATS_OUTPUT=stderr \
	"${program_args[@]}" \
	"$root/examples/stress_bench" --protocol "$protocol" \
	--iterations "$iterations" --size "$size" --window "$window" \
	--mtu "$mtu" --role "$role" --bind "$bind" --peer "$peer" \
	--control-port "$control_port" --data-port "$data_port"
