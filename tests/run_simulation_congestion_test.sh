#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
netem=$project_dir/simulation/network/netem
loopback=$project_dir/tests/loopback
plugin=$project_dir/tests/congestion_plugin.so
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-congestion-sim.XXXXXX")
matrix=${SIMULATION_CC_MATRIX:-}
if [ -z "$matrix" ]; then
	matrix='rocev2:nscc:clean rocev2:nscc:datacenter
rocev2:nscc:lossy rocev2:none:constrained rocev2:test-pass:datacenter
uet:nscc:clean uet:nscc:datacenter uet:nscc:constrained
uet:nscc:lossy uet:nscc:reordered uet:none:constrained
uet:test-pass:datacenter'
fi
timeout_seconds=${SIMULATION_CC_TIMEOUT:-60}
max_window=${SIMULATION_CC_MAX_WINDOW:-65536}
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

if [ "${1:-}" = inner ]; then
	config=$2
	protocol=$3
	algorithm=$4
	use_plugin=$5
	ip link set lo up
	"$netem" apply "$config" lo --replace-existing
	environment=(UIB_PROTOCOL="$protocol" \
		UIB_TRANSPORT_MODE=udp-sync UIB_ZEROCOPY=off)
	if [ "$protocol" = uet ]; then
		environment+=(UIB_UET_CC="$algorithm"
			UIB_UET_NSCC_MAX_WND="$max_window")
	else
		environment+=(UIB_ROCEV2_CC="$algorithm"
			UIB_ROCEV2_NSCC_MAX_WND="$max_window")
	fi
	[ "$use_plugin" != 1 ] || environment+=(LD_PRELOAD="$plugin"
		CONGESTION_PLUGIN_TEST=1)
	timeout -k 5 "$timeout_seconds" env "${environment[@]}" "$loopback"
	exit
fi

for command_name in ip jq timeout unshare; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required congestion simulation command not found: $command_name" >&2
		exit 1
	fi
done

if [ "$(id -u)" -eq 0 ]; then
	runner=(unshare --net)
elif unshare --user --map-root-user --net true >/dev/null 2>&1; then
	runner=(unshare --user --map-root-user --net)
elif [ -n "${SIMULATION_NETNS_PRIVILEGE:-}" ]; then
	read -r -a privilege <<<"$SIMULATION_NETNS_PRIVILEGE"
	runner=("${privilege[@]}" unshare --net)
else
	echo "network-namespace privilege is required; set SIMULATION_NETNS_PRIVILEGE" >&2
	exit 1
fi

for entry in $matrix; do
	protocol=${entry%%:*}
	remainder=${entry#*:}
	algorithm=${remainder%%:*}
	condition=${remainder#*:}
	if [ "$protocol" = "$remainder" ] || [ "$algorithm" = "$condition" ]; then
		echo "invalid SIMULATION_CC_MATRIX entry: $entry" >&2
		exit 2
	fi
	case $protocol in
	rocev2|uet) ;;
	*)
		echo "unsupported protocol in SIMULATION_CC_MATRIX: $protocol" >&2
		exit 2
		;;
	esac
	use_plugin=0
	case $algorithm in
	nscc)
		if [ "$protocol" = rocev2 ]; then
			protocol_overlay=protocol/rocev2-nscc-experimental
		else
			protocol_overlay=protocol/uet-nscc
		fi
		;;
	none|test-pass)
		if [ "$protocol" = uet ]; then
			protocol_overlay=protocol/uet-none
		else
			protocol_overlay=protocol/rocev2
		fi
		[ "$algorithm" != test-pass ] || use_plugin=1
		;;
	*)
		echo "unsupported test algorithm in SIMULATION_CC_MATRIX: $algorithm" >&2
		exit 2
		;;
	esac
	config=$test_dir/$protocol-$algorithm-$condition.json
	render=("$cli" render --profile kind-kubernetes \
		--overlay "$protocol_overlay" --overlay "network/$condition" \
		--format json)
	if [ "$algorithm" = test-pass ]; then
		render+=(--set uib.congestionControl.algorithm=test-pass)
	fi
	"${render[@]}" > "$config"
	label=
	[ "$protocol:$algorithm" != rocev2:nscc ] || label=' experimental=true'
	echo "congestion simulation: protocol=$protocol algorithm=$algorithm network=$condition$label"
	"${runner[@]}" "$0" inner "$config" "$protocol" "$algorithm" \
		"$use_plugin"
done

echo "congestion-control network simulation test: PASS"
