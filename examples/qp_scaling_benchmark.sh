#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-$project_dir/simulation/.artifacts/qp-scaling}
counts=${UIB_QP_SCALE_COUNTS:-1 2 4 8}
trials=${UIB_QP_SCALE_TRIALS:-3}
test_timeout=${UIB_QP_SCALE_TIMEOUT:-120}
protocol=${UIB_BENCH_PROTOCOL:-uet}
posting=${UIB_QP_SCALE_POSTING:-parallel}
iterations=${UIB_BENCH_ITERATIONS:-2000}
message_size=${UIB_BENCH_SIZE:-60000}
window=${UIB_BENCH_WINDOW:-128}
mtu=${UIB_BENCH_MTU:-1500}
port=${UIB_QP_SCALE_PORT:-22550}
results=$output/shared-qp-scaling.csv
commit=unknown

valid_positive()
{
	case $1 in
	''|*[!0-9]*|0)
		return 1
		;;
	esac
	return 0
}

for command_name in date env sed sort timeout uname; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required command not found: $command_name" >&2
		exit 1
	fi
done
for value in "$trials" "$test_timeout" "$iterations" "$message_size" \
	"$window" "$mtu" "$port"; do
	if ! valid_positive "$value"; then
		echo "invalid positive integer: $value" >&2
		exit 1
	fi
done
for count in $counts; do
	if ! valid_positive "$count" || [ "$count" -gt 256 ]; then
		echo "invalid QP count: $count" >&2
		exit 1
	fi
done
if [ "$port" -gt 65534 ]; then
	echo "UIB_QP_SCALE_PORT must not exceed 65534" >&2
	exit 1
fi
if [ ! -x "$project_dir/examples/qp_scaling_bench" ]; then
	echo "build examples/qp_scaling_bench before running this benchmark" >&2
	exit 1
fi
if command -v git >/dev/null 2>&1; then
	commit=$(git -C "$project_dir" rev-parse HEAD 2>/dev/null || echo unknown)
fi

mkdir -p -- "$output"
printf '%s\n' 'trial,protocol,qps,posting,iterations,message_size,window,'\
'mtu,gbit_per_second,elapsed_seconds,cpu_seconds' > "$results"
{
	printf 'commit=%s\n' "$commit"
	printf 'kernel=%s\n' "$(uname -r)"
	printf 'allowed_cpus=%s\n' \
		"$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' \
		/proc/self/status)"
	printf 'qp_counts=%s\n' "$counts"
	printf 'trials=%s\n' "$trials"
	printf 'protocol=%s\n' "$protocol"
	printf 'posting=%s\n' "$posting"
	printf 'iterations_per_qp=%s\n' "$iterations"
	printf 'message_size=%s\n' "$message_size"
	printf 'window_per_qp=%s\n' "$window"
	printf 'mtu=%s\n' "$mtu"
	printf 'port=%s\n' "$port"
	echo
	echo 'inherited_uib_environment:'
	env | LC_ALL=C sort | sed -n '/^UIB_/p'
} > "$output/configuration.txt"

for count in $counts; do
	trial=1
	while [ "$trial" -le "$trials" ]; do
		log=$output/qps-$count-trial-$trial.log
		timeout -k 2 "$test_timeout" env \
			UIB_QP_SCALE_QPS="$count" \
			UIB_QP_SCALE_PORT="$port" \
			UIB_QP_SCALE_POSTING="$posting" \
			UIB_BENCH_PROTOCOL="$protocol" \
			UIB_BENCH_ITERATIONS="$iterations" \
			UIB_BENCH_SIZE="$message_size" \
			UIB_BENCH_WINDOW="$window" \
			UIB_BENCH_MTU="$mtu" \
			"$project_dir/examples/qp_scaling_bench" > "$log" 2>&1
		row=$(sed -n 's/^CSV,//p' "$log")
		if [ -z "$row" ]; then
			echo "missing result: qps=$count trial=$trial" >&2
			exit 1
		fi
		printf '%s,%s\n' "$trial" "$row" >> "$results"
		trial=$((trial + 1))
	done
done

echo "shared-context QP scaling benchmark: PASS"
printf 'results: %s\n' "$results"
