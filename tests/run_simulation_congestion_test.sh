#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
netem=$project_dir/simulation/network/netem
loopback=$project_dir/tests/loopback
plugin=$project_dir/tests/congestion_plugin.so
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-congestion-sim.XXXXXX")
matrix=${SIMULATION_CC_MATRIX:-'nscc:clean nscc:datacenter nscc:constrained nscc:lossy nscc:reordered none:constrained test-pass:datacenter'}
timeout_seconds=${SIMULATION_CC_TIMEOUT:-60}
max_window=${SIMULATION_CC_MAX_WINDOW:-65536}
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

if [ "${1:-}" = inner ]; then
	config=$2
	algorithm=$3
	use_plugin=$4
	ip link set lo up
	"$netem" apply "$config" lo --replace-existing
	environment=(UIB_PROTOCOL=uet \
		UIB_UET_CC="$algorithm" UIB_UET_NSCC_MAX_WND="$max_window" \
		UIB_TRANSPORT_MODE=udp-sync UIB_ZEROCOPY=off)
	[ "$use_plugin" != 1 ] || environment+=(LD_PRELOAD="$plugin")
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
elif [ -x sudo ]; then
	runner=(sudo unshare --net)
else
	echo "network-namespace privilege is required; set SIMULATION_NETNS_PRIVILEGE" >&2
	exit 1
fi

for entry in $matrix; do
	algorithm=${entry%%:*}
	condition=${entry#*:}
	if [ "$algorithm" = "$condition" ]; then
		echo "invalid SIMULATION_CC_MATRIX entry: $entry" >&2
		exit 2
	fi
	case $algorithm in
	nscc) protocol_overlay=protocol/uet-nscc; use_plugin=0 ;;
	none) protocol_overlay=protocol/uet-none; use_plugin=0 ;;
	test-pass) protocol_overlay=protocol/uet-none; use_plugin=1 ;;
	*)
		echo "unsupported test algorithm in SIMULATION_CC_MATRIX: $algorithm" >&2
		exit 2
		;;
	esac
	config=$test_dir/$algorithm-$condition.json
	render=("$cli" render --profile kind-kubernetes \
		--overlay "$protocol_overlay" --overlay "network/$condition" \
		--format json)
	if [ "$algorithm" = test-pass ]; then
		render+=(--set uib.congestionControl.algorithm=test-pass)
	fi
	"${render[@]}" > "$config"
	echo "congestion simulation: algorithm=$algorithm network=$condition"
	"${runner[@]}" "$0" inner "$config" "$algorithm" "$use_plugin"
done

echo "congestion-control network simulation test: PASS"
