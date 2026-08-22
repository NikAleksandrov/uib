#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -u

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:?usage: ib_write_bw_matrix.sh OUTPUT_DIRECTORY}
iterations=${UIB_BENCH_ITERATIONS:-10000}
message_size=${UIB_BENCH_SIZE:-32768}
results=$output/ib-write-bw.csv
failures=$output/ib-write-bw-failures.txt
server_pid=
server_log=
client_log=
row_number=0

cleanup()
{
	if [ -n "$server_pid" ]; then
		kill "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	if [ -n "$server_log" ]; then
		rm -f "$server_log"
	fi
	if [ -n "$client_log" ]; then
		rm -f "$client_log"
	fi
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$output"
printf '%s\n' 'status,protocol,transport,iterations,message_size,peak_gbit_per_second,average_gbit_per_second,message_rate_mpps,wall_seconds,user_seconds,system_seconds,cpu_seconds,cpu_utilization_percent,reason' >"$results"
: >"$failures"

skip_one()
{
	protocol=$1
	transport=$2
	reason=$3

	printf 'SKIP,%s,%s,%s,%s,n/a,n/a,n/a,n/a,n/a,n/a,n/a,n/a,%s\n' \
		"$protocol" "$transport" "$iterations" "$message_size" \
		"$reason" >>"$results"
	printf 'SKIP ib_write_bw %-7s %-14s %s\n' \
		"$protocol" "$transport" "$reason"
}

time_field()
{
	log=$1
	field=$2

	sed -n 's/^UIB_TIME,//p' "$log" | tail -1 | cut -d, -f"$field"
}

run_one()
{
	protocol=$1
	transport=$2
	row_number=$((row_number + 1))
	data_port=$((19500 + row_number))
	control_port=$((18600 + row_number))
	server_log=$output/ib-write-bw-$protocol-$transport-server.log
	client_log=$output/ib-write-bw-$protocol-$transport-client.log
	: >"$server_log"
	: >"$client_log"

	env LD_PRELOAD="$project_dir/libuibv.so" \
		UIB_PROTOCOL="$protocol" \
		UIB_TRANSPORT_MODE="$transport" \
		UIB_UDP_MODE=socket UIB_BIND_IP=127.0.0.1 \
		UIB_BIND_PORT="$data_port" \
		timeout 120 bash -c \
		'TIMEFORMAT="UIB_TIME,%R,%U,%S"; time "$@"' uib-time \
		ib_write_bw -p "$control_port" -i 1 -x 0 \
		--connection=RC --size="$message_size" --iters="$iterations" \
		--report_gbits -F >"$server_log" 2>&1 &
	server_pid=$!
	sleep 1
	if ! kill -0 "$server_pid" 2>/dev/null; then
		wait "$server_pid" 2>/dev/null
		status=$?
		printf '%s,%s: server failed with status %s: %s\n' \
			"$protocol" "$transport" "$status" \
			"$(tail -1 "$server_log")" >>"$failures"
		printf 'FAIL ib_write_bw %-7s %-14s server\n' \
			"$protocol" "$transport"
		tail -20 "$server_log" >&2
		server_pid=
		server_log=
		client_log=
		return
	fi

	env LD_PRELOAD="$project_dir/libuibv.so" \
		UIB_PROTOCOL="$protocol" \
		UIB_TRANSPORT_MODE="$transport" \
		UIB_UDP_MODE=socket UIB_BIND_IP=127.0.0.2 \
		UIB_BIND_PORT="$data_port" \
		timeout 120 bash -c \
		'TIMEFORMAT="UIB_TIME,%R,%U,%S"; time "$@"' uib-time \
		ib_write_bw -p "$control_port" -i 1 -x 0 --connection=RC \
		--size="$message_size" --iters="$iterations" \
		--report_gbits -F 127.0.0.1 >"$client_log" 2>&1
	client_status=$?
	wait "$server_pid" 2>/dev/null
	server_status=$?
	server_pid=
	if [ "$client_status" -ne 0 ] || [ "$server_status" -ne 0 ]; then
		printf '%s,%s: server=%s client=%s: %s\n' \
			"$protocol" "$transport" "$server_status" \
			"$client_status" "$(tail -1 "$client_log")" \
			>>"$failures"
		printf 'FAIL ib_write_bw %-7s %-14s server=%s client=%s\n' \
			"$protocol" "$transport" "$server_status" \
			"$client_status"
		tail -20 "$server_log" >&2
		tail -20 "$client_log" >&2
		server_log=
		client_log=
		return
	fi

	measurements=$(awk '
		/^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+/ {
			peak = $3;
			average = $4;
			rate = $5;
		}
		END {
			if (average != "")
				printf "%s,%s,%s", peak, average, rate;
		}' "$client_log")
	if [ -z "$measurements" ]; then
		printf '%s,%s: no perftest result row: %s\n' \
			"$protocol" "$transport" "$(tail -1 "$client_log")" \
			>>"$failures"
		printf 'FAIL ib_write_bw %-7s %-14s no result row\n' \
			"$protocol" "$transport"
		tail -20 "$client_log" >&2
		server_log=
		client_log=
		return
	fi

	client_wall=$(time_field "$client_log" 1)
	client_user=$(time_field "$client_log" 2)
	client_system=$(time_field "$client_log" 3)
	server_user=$(time_field "$server_log" 2)
	server_system=$(time_field "$server_log" 3)
	combined=$(awk -v cu="$client_user" -v cs="$client_system" \
		-v su="$server_user" -v ss="$server_system" -v wall="$client_wall" \
		'BEGIN {
			user_total = cu + su;
			system_total = cs + ss;
			cpu = user_total + system_total;
			util = wall ? 100 * cpu / wall : 0;
			printf "%.6f,%.6f,%.6f,%.3f", user_total,
				system_total, cpu, util;
		}')
	printf 'PASS,%s,%s,%s,%s,%s,%s,%s,\n' \
		"$protocol" "$transport" "$iterations" "$message_size" \
		"$measurements" "$client_wall" "$combined" >>"$results"
	printf 'PASS ib_write_bw %-7s %-14s average=%s Gbit/s\n' \
		"$protocol" "$transport" \
		"$(printf '%s\n' "$measurements" | cut -d, -f2)"
	rm -f "$server_log" "$client_log"
	server_log=
	client_log=
}

for protocol in ${UIB_BENCH_PROTOCOLS:-rocev2 uet dummy}; do
	for transport in udp-io-uring udp-sync tcp-devmem udp-zc-afxdp; do
		if [ "$protocol" = dummy ]; then
			skip_one "$protocol" "$transport" \
				'dummy protocol does not implement RDMA WRITE'
			continue
		fi
		if ! command -v ib_write_bw >/dev/null 2>&1; then
			skip_one "$protocol" "$transport" \
				'ib_write_bw is unavailable (install perftest)'
			continue
		fi
		if [ "$transport" = udp-zc-afxdp ]; then
			if [ -z "${UIB_AFXDP_IFACE:-}" ] || \
			   [ -z "${UIB_AFXDP_PEER_MAC:-}" ]; then
				skip_one "$protocol" "$transport" \
					'AF_XDP interface and peer MAC are not configured'
				continue
			fi
			skip_one "$protocol" "$transport" \
				'local loopback runner cannot provide an AF_XDP peer'
			continue
		fi
		run_one "$protocol" "$transport"
	done
done

if [ -s "$failures" ]; then
	exit 1
fi
