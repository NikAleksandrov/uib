#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-$project_dir/simulation/.artifacts/context-scaling}
lane_sets=${UIB_CONTEXT_SCALE_LANES:-1 2 4 8}
trials=${UIB_CONTEXT_SCALE_TRIALS:-3}
port_base=${UIB_CONTEXT_SCALE_PORT_BASE:-22400}
test_timeout=${UIB_CONTEXT_SCALE_TIMEOUT:-120}
protocol=${UIB_BENCH_PROTOCOL:-uet}
memory=${UIB_BENCH_MEMORY:-host}
iterations=${UIB_BENCH_ITERATIONS:-10000}
message_size=${UIB_BENCH_SIZE:-4096}
window=${UIB_BENCH_WINDOW:-128}
mtu=${UIB_BENCH_MTU:-1500}
transport=${UIB_TRANSPORT_MODE:-udp-sync}
udp_workers=${UIB_UDP_WORKERS:-1}
results=$output/context-scaling.csv
server_pids=
client_pids=
commit=unknown

if command -v git >/dev/null 2>&1; then
	commit=$(git -C "$project_dir" rev-parse HEAD 2>/dev/null || echo unknown)
fi

cleanup()
{
	set +e
	for pid in $client_pids $server_pids; do
		kill "$pid" 2>/dev/null
	done
	for pid in $client_pids $server_pids; do
		wait "$pid" 2>/dev/null
	done
}
trap cleanup EXIT INT TERM

valid_positive()
{
	case $1 in
	''|*[!0-9]*|0)
		return 1
		;;
	esac
	return 0
}

for command_name in awk date env sed sort tail tee timeout uname; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required command not found: $command_name" >&2
		exit 1
	fi
done
for value in "$trials" "$port_base" "$test_timeout" "$iterations" \
	"$message_size" "$window" "$mtu"; do
	if ! valid_positive "$value"; then
		echo "invalid positive integer: $value" >&2
		exit 1
	fi
done
if [ "$udp_workers" != auto ] && ! valid_positive "$udp_workers"; then
	echo "UIB_UDP_WORKERS must be auto or a positive integer" >&2
	exit 1
fi
if [ "$port_base" -gt 65400 ]; then
	echo "UIB_CONTEXT_SCALE_PORT_BASE must not exceed 65400" >&2
	exit 1
fi
for lanes in $lane_sets; do
	if ! valid_positive "$lanes" || [ "$lanes" -gt 32 ]; then
		echo "invalid lane count: $lanes" >&2
		exit 1
	fi
done
if [ ! -x "$project_dir/examples/stress_bench" ]; then
	echo "build examples/stress_bench before running this benchmark" >&2
	exit 1
fi

mkdir -p -- "$output"
{
	printf '%s\n' 'lanes,trial,global_gbit_per_second,'\
'sum_lane_gbit_per_second,min_lane_gbit_per_second,'\
'max_lane_gbit_per_second,client_cpu_seconds,server_cpu_seconds,'\
'elapsed_seconds'
} > "$results"
{
	printf 'commit=%s\n' "$commit"
	printf 'kernel=%s\n' "$(uname -r)"
	printf 'allowed_cpus=%s\n' \
		"$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' \
		/proc/self/status)"
	printf 'protocol=%s\n' "$protocol"
	printf 'transport=%s\n' "$transport"
	printf 'memory=%s\n' "$memory"
	printf 'iterations=%s\n' "$iterations"
	printf 'message_size=%s\n' "$message_size"
	printf 'window=%s\n' "$window"
	printf 'mtu=%s\n' "$mtu"
	printf 'udp_workers_per_process=%s\n' "$udp_workers"
	printf 'uet_cc=%s\n' "${UIB_UET_CC:-library-default}"
	printf 'rocev2_cc=%s\n' "${UIB_ROCEV2_CC:-library-default}"
	echo
	echo 'inherited_uib_environment:'
	env | LC_ALL=C sort | sed -n '/^UIB_/p'
} > "$output/configuration.txt"

for lanes in $lane_sets; do
	trial=1
	while [ "$trial" -le "$trials" ]; do
		run_dir=$output/lanes-$lanes-trial-$trial
		lane_results=$run_dir/lanes.csv
		server_pids=
		client_pids=
		status=0
		lane=0
		mkdir -p -- "$run_dir"
		while [ "$lane" -lt "$lanes" ]; do
			control_port=$((port_base + lane))
			data_port=$((port_base + 64 + lane))
			timeout -k 2 "$test_timeout" env \
				UIB_TRANSPORT_MODE="$transport" \
				UIB_UDP_WORKERS="$udp_workers" \
				"$project_dir/examples/stress_bench" \
				--protocol "$protocol" --memory "$memory" \
				--iterations "$iterations" --size "$message_size" \
				--window "$window" --mtu "$mtu" --csv \
				--role server --bind 127.0.0.1 \
				--control-port "$control_port" \
				--data-port "$data_port" \
				> "$run_dir/server-$lane.log" 2>&1 &
			server_pids="$server_pids $!"
			lane=$((lane + 1))
		done
		start_ns=$(date +%s%N)
		lane=0
		while [ "$lane" -lt "$lanes" ]; do
			control_port=$((port_base + lane))
			data_port=$((port_base + 64 + lane))
			timeout -k 2 "$test_timeout" env \
				UIB_TRANSPORT_MODE="$transport" \
				UIB_UDP_WORKERS="$udp_workers" \
				"$project_dir/examples/stress_bench" \
				--protocol "$protocol" --memory "$memory" \
				--iterations "$iterations" --size "$message_size" \
				--window "$window" --mtu "$mtu" --csv \
				--role client --bind 127.0.0.2 --peer 127.0.0.1 \
				--control-port "$control_port" \
				--data-port "$data_port" \
				> "$run_dir/client-$lane.log" 2>&1 &
			client_pids="$client_pids $!"
			lane=$((lane + 1))
		done
		for pid in $client_pids; do
			if ! wait "$pid"; then
				status=1
			fi
		done
		client_pids=
		for pid in $server_pids; do
			if ! wait "$pid"; then
				status=1
			fi
		done
		server_pids=
		end_ns=$(date +%s%N)
		if [ "$status" -ne 0 ]; then
			echo "context scaling failed: lanes=$lanes trial=$trial" >&2
			tail -n 40 "$run_dir"/*.log >&2
			exit 1
		fi
		printf '%s\n' 'lane,gbit_per_second,client_cpu_seconds,'\
'server_cpu_seconds' > "$lane_results"
		lane=0
		while [ "$lane" -lt "$lanes" ]; do
			client_row=$(sed -n 's/^CSV,//p' \
				"$run_dir/client-$lane.log" | tail -1)
			server_row=$(sed -n 's/^CSV,//p' \
				"$run_dir/server-$lane.log" | tail -1)
			if [ -z "$client_row" ] || [ -z "$server_row" ]; then
				echo "missing CSV result: lanes=$lanes lane=$lane" >&2
				exit 1
			fi
			awk -F, -v lane="$lane" -v client="$client_row" \
				-v server="$server_row" \
				'BEGIN {
					split(client, client_fields, ",");
					split(server, server_fields, ",");
					printf "%u,%.9f,%.9f,%.9f\n", lane,
					       client_fields[12], client_fields[8],
					       server_fields[8];
				}' >> "$lane_results"
			lane=$((lane + 1))
		done
		elapsed_ns=$((end_ns - start_ns))
		awk -F, -v lanes="$lanes" -v trial="$trial" \
			-v elapsed_ns="$elapsed_ns" -v iterations="$iterations" \
			-v size="$message_size" '
			NR == 1 { next }
			NR == 2 { minimum = $2; maximum = $2 }
			{
				bandwidth += $2;
				client_cpu += $3;
				server_cpu += $4;
				if ($2 < minimum)
					minimum = $2;
				if ($2 > maximum)
					maximum = $2;
			}
			END {
				global = lanes * iterations * size * 8 / elapsed_ns;
				printf "%u,%u,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n",
				       lanes, trial, global, bandwidth, minimum, maximum,
				       client_cpu, server_cpu, elapsed_ns / 1000000000;
			}' "$lane_results" | tee -a "$results"
		trial=$((trial + 1))
	done
done

echo "context scaling benchmark: PASS"
printf 'results: %s\n' "$results"
