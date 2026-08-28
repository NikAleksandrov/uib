#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:?usage: congestion_benchmark_matrix.sh OUTPUT_DIRECTORY}
cli=$project_dir/simulation/bin/uib-sim
netem=$project_dir/simulation/network/netem
benchmark=$project_dir/examples/stress_bench
results=$output/congestion-comparison.csv
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-cc-benchmark.XXXXXX")
variants=${UIB_CC_BENCH_VARIANTS:-'rocev2:dcqcn rocev2:none uet:rccc uet:nscc uet:none'}
conditions=${UIB_CC_BENCH_CONDITIONS:-'clean latency drops reordered drops-reordered'}
matrix=${UIB_CC_BENCH_MATRIX:-}
iterations=${UIB_CC_BENCH_ITERATIONS:-1000}
message_size=${UIB_CC_BENCH_SIZE:-16384}
window=${UIB_CC_BENCH_WINDOW:-16}
mtu=${UIB_CC_BENCH_MTU:-1500}
timeout_seconds=${UIB_CC_BENCH_TIMEOUT:-120}
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

for numeric in "$iterations" "$message_size" "$window" "$mtu" \
	"$timeout_seconds"; do
	case $numeric in
	''|*[!0-9]*|0)
		echo "congestion benchmark numeric values must be positive integers" >&2
		exit 2
		;;
	esac
done
if [ -n "$matrix" ] &&
   { [ "${UIB_CC_BENCH_VARIANTS+x}" = x ] ||
     [ "${UIB_CC_BENCH_CONDITIONS+x}" = x ]; }; then
	echo "UIB_CC_BENCH_MATRIX cannot be combined with variant or condition selectors" >&2
	exit 2
fi

write_header()
{
	printf '%s' 'status,protocol,algorithm,condition,experimental,iterations,'
	printf '%s' 'message_size,window,wall_seconds,cpu_seconds,user_seconds,'
	printf '%s' 'system_seconds,gbit_per_second,mib_per_second,'
	printf '%s' 'cpu_utilization_percent,latency_min_ns,latency_avg_ns,'
	printf '%s' 'latency_p50_ns,latency_p95_ns,latency_p99_ns,latency_max_ns,'
	printf '%s\n' 'mtu,reason'
}

algorithm_environment()
{
	case $1:$2 in
	rocev2:dcqcn)
		printf '%s\n' UIB_ROCEV2_CC=dcqcn
		;;
	rocev2:none)
		printf '%s\n' UIB_ROCEV2_CC=none
		;;
	uet:rccc)
		printf '%s\n' UIB_UET_CC=rccc
		;;
	uet:nscc)
		printf '%s\n' UIB_UET_CC=nscc
		;;
	uet:none)
		printf '%s\n' UIB_UET_CC=none
		;;
	uet:dcqcn-experimental)
		printf '%s\n' UIB_UET_CC=dcqcn UIB_UET_CC_EXPERIMENTAL=on
		;;
	*)
		return 1
		;;
	esac
}

protocol_overlay()
{
	case $1:$2 in
	rocev2:dcqcn)
		printf '%s\n' protocol/rocev2-dcqcn
		;;
	rocev2:none)
		printf '%s\n' protocol/rocev2
		;;
	uet:rccc)
		printf '%s\n' protocol/uet-rccc
		;;
	uet:nscc)
		printf '%s\n' protocol/uet-nscc
		;;
	uet:none)
		printf '%s\n' protocol/uet-none
		;;
	uet:dcqcn-experimental)
		printf '%s\n' protocol/uet-dcqcn-experimental
		;;
	*)
		return 1
		;;
	esac
}

if [ "${1:-}" = inner ]; then
	config=$2
	protocol=$3
	algorithm=$4
	ip link set lo up
	"$netem" apply "$config" lo --replace-existing
	read -r -a cc_environment <<<"$(algorithm_environment \
		"$protocol" "$algorithm")"
	timeout -k 5 "$timeout_seconds" env \
		UIB_PROTOCOL="$protocol" UIB_TRANSPORT_MODE=udp-sync \
		UIB_ZEROCOPY=off "${cc_environment[@]}" "$benchmark" \
		--protocol "$protocol" --memory host --iterations "$iterations" \
		--size "$message_size" --window "$window" --mtu "$mtu" --csv
	exit
fi

mkdir -p "$output"
write_header >"$results"
for command_name in ip jq timeout unshare; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		printf 'SKIP,,,,,,,,,,,,,,,,,,,,,,missing command: %s\n' \
			"$command_name" >>"$results"
		exit 0
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
	printf '%s\n' 'SKIP,,,,,,,,,,,,,,,,,,,,,,network-namespace privilege unavailable' \
		>>"$results"
	exit 0
fi

if [ -z "$matrix" ]; then
	for variant in $variants; do
		for condition in $conditions; do
			matrix="$matrix $variant:$condition"
		done
	done
fi

for entry in $matrix; do
	protocol=${entry%%:*}
	remainder=${entry#*:}
	algorithm=${remainder%%:*}
	condition=${remainder#*:}
	experimental=0
	log=$test_dir/$protocol-$algorithm-$condition.log
	config=$test_dir/$protocol-$algorithm-$condition.json
	if ! overlay=$(protocol_overlay "$protocol" "$algorithm"); then
		echo "unsupported congestion benchmark variant: $protocol:$algorithm" >&2
		exit 2
	fi
	[ "$algorithm" != dcqcn-experimental ] || experimental=1
	"$cli" render --profile kind-kubernetes --overlay "$overlay" \
		--overlay "network/$condition" --format json >"$config"
	if "${runner[@]}" "$0" inner "$config" "$protocol" "$algorithm" \
		>"$log" 2>&1; then
		row=$(sed -n 's/^CSV,//p' "$log" | tail -1)
		if [ -z "$row" ]; then
			printf 'FAIL,%s,%s,%s,%s,,,,,,,,,,,,,,,,,,no CSV result\n' \
				"$protocol" "$algorithm" "$condition" "$experimental" \
				>>"$results"
			continue
		fi
		printf 'PASS,%s,%s,%s,%s' "$protocol" "$algorithm" \
			"$condition" "$experimental" >>"$results"
		printf ',%s' "$(printf '%s\n' "$row" | cut -d, -f5-20)" \
			>>"$results"
		printf ',%s,\n' "$(printf '%s\n' "$row" | cut -d, -f24)" \
			>>"$results"
		printf 'PASS %-7s %-20s %-17s\n' "$protocol" "$algorithm" \
			"$condition"
	else
		reason=$(grep -v '^/sbin/tc ' "$log" | tail -1 | tr ',' ';')
		printf 'FAIL,%s,%s,%s,%s,,,,,,,,,,,,,,,,,,%s\n' "$protocol" \
			"$algorithm" "$condition" "$experimental" "$reason" \
			>>"$results"
	fi
done

printf 'Congestion comparison CSV: %s\n' "$results"
