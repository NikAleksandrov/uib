#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-$project_dir/simulation/.artifacts/single-qp-cpu}
worker_counts=${UIB_SINGLE_QP_CPU_COUNTS:-1 2 4 8}
trials=${UIB_SINGLE_QP_CPU_TRIALS:-11}
test_timeout=${UIB_SINGLE_QP_CPU_TIMEOUT:-120}
protocol=${UIB_BENCH_PROTOCOL:-uet}
transport=${UIB_TRANSPORT_MODE:-udp-sync}
iterations=${UIB_BENCH_ITERATIONS:-600}
message_size=${UIB_BENCH_SIZE:-60000}
window=${UIB_BENCH_WINDOW:-64}
mtu=${UIB_BENCH_MTU:-8192}
port=${UIB_SINGLE_QP_CPU_PORT:-22650}
results=$output/single-qp-cpu.csv
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
reverse_counts=
for workers in $worker_counts; do
	if ! valid_positive "$workers" || [ "$workers" -gt 256 ]; then
		echo "invalid worker count: $workers" >&2
		exit 1
	fi
	reverse_counts="$workers $reverse_counts"
done
if [ -z "$reverse_counts" ]; then
	echo "at least one worker count is required" >&2
	exit 1
fi
if [ "$port" -gt 65534 ]; then
	echo "UIB_SINGLE_QP_CPU_PORT must not exceed 65534" >&2
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
printf '%s\n' 'trial,tx_workers,protocol,qps,posting,iterations,'\
'message_size,window,mtu,gbit_per_second,elapsed_seconds,cpu_seconds' \
	> "$results"
{
	printf 'commit=%s\n' "$commit"
	printf 'kernel=%s\n' "$(uname -r)"
	printf 'allowed_cpus=%s\n' \
		"$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' \
		/proc/self/status)"
	printf 'worker_counts=%s\n' "$worker_counts"
	printf 'trials=%s\n' "$trials"
	printf 'protocol=%s\n' "$protocol"
	printf 'transport=%s\n' "$transport"
	printf 'iterations=%s\n' "$iterations"
	printf 'message_size=%s\n' "$message_size"
	printf 'window=%s\n' "$window"
	printf 'mtu=%s\n' "$mtu"
	printf 'port=%s\n' "$port"
	echo
	echo 'inherited_uib_environment:'
	env | LC_ALL=C sort | sed -n '/^UIB_/p'
} > "$output/configuration.txt"

trial=1
while [ "$trial" -le "$trials" ]; do
	if [ $((trial % 2)) -eq 1 ]; then
		order=$worker_counts
	else
		order=$reverse_counts
	fi
	for workers in $order; do
		log=$output/workers-$workers-trial-$trial.log
		timeout -k 2 "$test_timeout" env \
			UIB_TRANSPORT_MODE="$transport" \
			UIB_TX_WORKERS="$workers" \
			UIB_QP_SCALE_QPS=1 \
			UIB_QP_SCALE_PORT="$port" \
			UIB_QP_SCALE_POSTING=serial \
			UIB_BENCH_PROTOCOL="$protocol" \
			UIB_BENCH_ITERATIONS="$iterations" \
			UIB_BENCH_SIZE="$message_size" \
			UIB_BENCH_WINDOW="$window" \
			UIB_BENCH_MTU="$mtu" \
			"$project_dir/examples/qp_scaling_bench" > "$log" 2>&1
		row=$(sed -n 's/^CSV,//p' "$log")
		if [ -z "$row" ]; then
			echo "missing result: workers=$workers trial=$trial" >&2
			exit 1
		fi
		printf '%s,%s,%s\n' "$trial" "$workers" "$row" >> "$results"
	done
	trial=$((trial + 1))
done

echo "single-QP CPU scaling benchmark: PASS"
printf 'results: %s\n' "$results"
