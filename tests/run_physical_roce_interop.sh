#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

root=${UIB_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
endpoint=${UIB_INTEROP_ENDPOINT:-}
role=${UIB_INTEROP_ROLE:-}
transport=${UIB_INTEROP_TRANSPORT:-raw}
bind=${UIB_INTEROP_BIND:-}
server=${UIB_INTEROP_SERVER:-}
device=${UIB_INTEROP_DEVICE:-}
gid_index=${UIB_INTEROP_GID_INDEX:-}
size=${UIB_INTEROP_SIZE:-2051}
iterations=${UIB_INTEROP_ITERATIONS:-4096}
control_port=${UIB_INTEROP_CONTROL_PORT:-19801}
interface=${UIB_INTEROP_IFACE:-}
queue=${UIB_INTEROP_QUEUE:-0}
peer_mac=${UIB_INTEROP_PEER_MAC:-}
program=${UIB_INTEROP_AFXDP_PROGRAM:-$root/bpf/uib_afxdp_kern.o}
privilege=${UIB_INTEROP_PRIVILEGE:-}
native_libdir=${UIB_INTEROP_NATIVE_LIBDIR:-}
privilege_args=()
endpoint_env=()
command_args=()

usage()
{
	cat >&2 <<'EOF'
Required environment:
  UIB_INTEROP_ENDPOINT=uib|native
  UIB_INTEROP_ROLE=server|client
Client also requires UIB_INTEROP_SERVER=<server IPv4>.
UIB requires UIB_INTEROP_BIND=<local IPv4> and
UIB_INTEROP_TRANSPORT=raw|afxdp.
Native endpoints require UIB_INTEROP_DEVICE and UIB_INTEROP_GID_INDEX.
AF_XDP also requires UIB_INTEROP_IFACE and UIB_INTEROP_PEER_MAC.
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
if [[ $role == client && -z $server ]]; then
	usage
fi
if ! command -v ibv_rc_pingpong >/dev/null 2>&1; then
	echo "ibv_rc_pingpong is required" >&2
	exit 1
fi

case "$endpoint" in
native)
	if [[ -z $device || -z $gid_index ]]; then
		usage
	fi
	endpoint_env=(env -u LD_PRELOAD)
	if [[ -n $native_libdir ]]; then
		endpoint_env+=("LD_LIBRARY_PATH=$native_libdir")
	fi
	;;
uib)
	if [[ -z $bind || ! -r $root/libuibv.so ]]; then
		usage
	fi
	device=${device:-uib0}
	gid_index=${gid_index:-0}
	endpoint_env=(env
		"LD_PRELOAD=$root/libuibv.so"
		UIB_PROTOCOL=rocev2
		"UIB_BIND_IP=$bind"
		UIB_BIND_PORT=4791
		UIB_ACTIVE_MTU=1024
		UIB_STATS_OUTPUT=stderr)
	case "$transport" in
	raw)
		endpoint_env+=(UIB_TRANSPORT_MODE=udp-sync UIB_UDP_MODE=raw)
		;;
	afxdp)
		if [[ -z $interface || -z $peer_mac || ! -r $program ]]; then
			usage
		fi
		endpoint_env+=(UIB_TRANSPORT_MODE=udp-zc-afxdp
			"UIB_AFXDP_IFACE=$interface"
			"UIB_AFXDP_QUEUE=$queue"
			"UIB_AFXDP_PEER_MAC=$peer_mac"
			"UIB_AFXDP_PROGRAM=$program"
			UIB_ZEROCOPY=required)
		;;
	*)
		usage
		;;
	esac
	if [[ -n $privilege ]]; then
		read -r -a privilege_args <<<"$privilege"
	fi
	;;
*)
	usage
	;;
esac

command_args=(ibv_rc_pingpong -d "$device" -g "$gid_index" -s "$size"
	-n "$iterations" -p "$control_port")
if [[ $role == client ]]; then
	command_args+=("$server")
fi

exec "${privilege_args[@]}" "${endpoint_env[@]}" "${command_args[@]}"
