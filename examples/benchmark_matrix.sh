#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -u

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-/tmp/uib-benchmark-$(date +%Y%m%d-%H%M%S)}
iterations=${UIB_BENCH_ITERATIONS:-10000}
message_size=${UIB_BENCH_SIZE:-32768}
window=${UIB_BENCH_WINDOW:-64}
results=$output/results.csv
skips=$output/skips.txt
failures=$output/failures.txt
ib_failures=$output/ib-write-bw-failures.txt
allowed_cpus=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/self/status)
pin_cpu=${UIB_BENCH_PIN_CPU:-$(printf '%s\n' "$allowed_cpus" | sed 's/[-,].*//')}

mkdir -p "$output"
{
	printf '%s' 'status,variant,protocol,transport,memory,zerocopy_mode,'
	printf '%s' 'iterations,message_size,window,wall_seconds,cpu_seconds,'
	printf '%s' 'user_seconds,system_seconds,gbit_per_second,mib_per_second,'
	printf '%s' 'cpu_utilization_percent,latency_min_ns,latency_avg_ns,'
	printf '%s' 'latency_p50_ns,latency_p95_ns,latency_p99_ns,latency_max_ns,'
	printf '%s\n' 'zerocopy_calls,zerocopy_bytes,deferred_copies,mtu,cpu_affinity'
} >"$results"
: >"$skips"
: >"$failures"

ib_status=0
"$project_dir/examples/ib_write_bw_matrix.sh" "$output" || ib_status=$?

run_one()
{
	variant=$1
	protocol=$2
	memory=$3
	transport=$4
	udp_mode=$5
	zerocopy=$6
	expected=$7
	affinity=$8
	mtu=$9
	run_window=$window
	log=$(mktemp)
	if [ "$protocol" = dummy ] && [ "$transport" != tcp-devmem ] &&
	   [ "$run_window" -gt 16 ]; then
		# Dummy has no ACK-based flow control; bound each UDP burst.
		run_window=16
	fi

	if [ "$protocol" = rocev2 ] && [ "$mtu" -gt 4096 ]; then
		printf '%s,%s,%s,%s,mtu-%s: RoCEv2 path MTU maximum is 4096\n' \
			"$variant" "$protocol" "$memory" "$affinity" "$mtu" \
			>>"$skips"
		rm -f "$log"
		return
	fi
	if [ "$transport" = udp-zc-afxdp ] &&
	   [ -z "${UIB_AFXDP_IFACE:-}" ]; then
		printf '%s,%s,%s,%s: UIB_AFXDP_IFACE is not set\n' \
			"$variant" "$protocol" "$memory" "$affinity" \
			>>"$skips"
		rm -f "$log"
		return
	fi
	if [ "$transport" = udp-zc-afxdp ] &&
	   [ "$(cat "/sys/class/net/$UIB_AFXDP_IFACE/mtu" 2>/dev/null ||
		echo 0)" -lt "$mtu" ]; then
		printf '%s,%s,%s,%s,mtu-%s: interface MTU is too small\n' \
			"$variant" "$protocol" "$memory" "$affinity" "$mtu" \
			>>"$skips"
		rm -f "$log"
		return
	fi
	if [ "$affinity" = pinned ] && ! command -v taskset >/dev/null 2>&1; then
		printf '%s,%s,%s,%s: taskset is unavailable\n' \
			"$variant" "$protocol" "$memory" "$affinity" >>"$skips"
		rm -f "$log"
		return
	fi
	if [ "$affinity" = pinned ]; then
		timeout 120 taskset -c "$pin_cpu" env \
			UIB_TRANSPORT_MODE="$transport" \
			UIB_UDP_MODE="$udp_mode" \
			UIB_ZEROCOPY="$zerocopy" \
			"$project_dir/examples/stress_bench" \
			--protocol "$protocol" --memory "$memory" \
			--iterations "$iterations" --size "$message_size" \
			--window "$run_window" --mtu "$mtu" --csv >"$log" 2>&1
		status=$?
	else
		timeout 120 env \
			UIB_TRANSPORT_MODE="$transport" \
			UIB_UDP_MODE="$udp_mode" \
			UIB_ZEROCOPY="$zerocopy" \
			"$project_dir/examples/stress_bench" \
			--protocol "$protocol" --memory "$memory" \
			--iterations "$iterations" --size "$message_size" \
			--window "$run_window" --mtu "$mtu" --csv >"$log" 2>&1
		status=$?
	fi
	if [ "$status" -eq 0 ]; then
		row=$(sed -n 's/^CSV,//p' "$log" | tail -1)
		actual=$(printf '%s\n' "$row" | cut -d, -f2)
		if [ -n "$row" ] && [ "$actual" = "$expected" ]; then
			printf 'PASS,%s,%s,%s\n' \
				"$variant" "$row" "$affinity" >>"$results"
			printf 'PASS %-18s %-7s %-6s %-8s mtu=%s\n' \
				"$variant" "$protocol" "$memory" "$affinity" "$mtu"
		else
			printf '%s,%s,%s,%s,mtu-%s: requested %s, got %s\n' \
				"$variant" "$protocol" "$memory" \
				"$affinity" "$mtu" "$expected" \
				"${actual:-no-result}" \
				>>"$failures"
		fi
	else
		printf '%s,%s,%s,%s,mtu-%s: failed: %s\n' \
			"$variant" "$protocol" "$memory" "$affinity" "$mtu" \
			"$(tail -1 "$log")" >>"$failures"
	fi
	rm -f "$log"
}


for affinity in ${UIB_BENCH_AFFINITIES:-unpinned pinned}; do
	for mtu in ${UIB_BENCH_MTUS:-1500 4096 8192}; do
		for protocol in ${UIB_BENCH_PROTOCOLS:-rocev2 uet dummy}; do
			for memory in ${UIB_BENCH_MEMORY_MODES:-host dmabuf}; do
				run_one udp-async-copy "$protocol" "$memory" \
					udp-io-uring \
					socket off udp-io-uring "$affinity" "$mtu"
				run_one udp-async-zerocopy "$protocol" "$memory" \
					udp-io-uring socket required udp-io-uring \
					"$affinity" "$mtu"
				run_one udp-sync-copy "$protocol" "$memory" udp-sync \
					socket off udp-sync "$affinity" "$mtu"
				run_one udp-sync-zerocopy "$protocol" "$memory" \
					udp-sync socket required udp-sync \
					"$affinity" "$mtu"
				run_one tcp-copy "$protocol" "$memory" tcp-devmem \
					socket off tcp-devmem "$affinity" "$mtu"
				run_one tcp-zerocopy "$protocol" "$memory" tcp-devmem \
					socket required tcp-devmem "$affinity" "$mtu"
				run_one afxdp-copy "$protocol" "$memory" \
					udp-zc-afxdp socket off udp-zc-afxdp \
					"$affinity" "$mtu"
				run_one afxdp-zerocopy "$protocol" "$memory" \
					udp-zc-afxdp socket required udp-zc-afxdp \
					"$affinity" "$mtu"
			done
		done
	done
done

awk -F, 'BEGIN {
	OFS=",";
	print "protocol,memory,transport,mtu,cpu_affinity," \
		"copy_gbit_per_second,zerocopy_gbit_per_second," \
		"bandwidth_change_percent,copy_latency_avg_ns," \
		"zerocopy_latency_avg_ns,latency_reduction_percent," \
		"copy_cpu_seconds,zerocopy_cpu_seconds," \
		"cpu_time_reduction_percent,zerocopy_calls," \
		"deferred_copy_percent"
}
NR == 1 { next }
$2 ~ /-copy$/ {
	base=$2;
	sub(/-copy$/, "", base);
	key=$3 SUBSEP $5 SUBSEP base SUBSEP $26 SUBSEP $27;
	bw[key]=$14;
	lat[key]=$18;
	cpu[key]=$11;
}
$2 ~ /-zerocopy$/ {
	base=$2;
	sub(/-zerocopy$/, "", base);
	key=$3 SUBSEP $5 SUBSEP base SUBSEP $26 SUBSEP $27;
	if (key in bw) {
		bw_delta=bw[key] ? 100 * ($14 - bw[key]) / bw[key] : 0;
		lat_delta=lat[key] ? 100 * (lat[key] - $18) / lat[key] : 0;
		cpu_delta=cpu[key] ? 100 * (cpu[key] - $11) / cpu[key] : 0;
		deferred=$23 ? 100 * $25 / $23 : 0;
		printf "%s,%s,%s,%s,%s,%.6f,%.6f,%.3f,%s,%s,%.3f,%.9f,%.9f,%.3f,%s,%.3f\n",
			$3, $5, base, $26, $27, bw[key], $14, bw_delta,
			lat[key], $18, lat_delta, cpu[key], $11, cpu_delta,
			$23, deferred;
	}
}' "$results" >"$output/zerocopy-comparison.csv"

awk -F, 'BEGIN {
	OFS=",";
	print "protocol,variant,mtu,cpu_affinity,host_gbit_per_second," \
		"dmabuf_gbit_per_second,dmabuf_bandwidth_change_percent," \
		"host_latency_avg_ns,dmabuf_latency_avg_ns," \
		"dmabuf_latency_change_percent,host_cpu_seconds," \
		"dmabuf_cpu_seconds,dmabuf_cpu_time_change_percent"
}
NR == 1 { next }
$5 == "host" {
	key=$3 SUBSEP $2 SUBSEP $26 SUBSEP $27;
	bw[key]=$14;
	lat[key]=$18;
	cpu[key]=$11;
}
$5 == "dmabuf" {
	key=$3 SUBSEP $2 SUBSEP $26 SUBSEP $27;
	if (key in bw) {
		bw_delta=bw[key] ? 100 * ($14 - bw[key]) / bw[key] : 0;
		lat_delta=lat[key] ? 100 * ($18 - lat[key]) / lat[key] : 0;
		cpu_delta=cpu[key] ? 100 * ($11 - cpu[key]) / cpu[key] : 0;
		printf "%s,%s,%s,%s,%.6f,%.6f,%.3f,%s,%s,%.3f,%.9f,%.9f,%.3f\n",
			$3, $2, $26, $27, bw[key], $14, bw_delta, lat[key], $18,
			lat_delta, cpu[key], $11, cpu_delta;
	}
}' "$results" >"$output/dmabuf-comparison.csv"

awk -F, 'BEGIN {
	OFS=",";
	print "protocol,memory,send_mode,mtu,cpu_affinity," \
		"sync_gbit_per_second,async_gbit_per_second," \
		"async_bandwidth_change_percent,sync_latency_avg_ns," \
		"async_latency_avg_ns,async_latency_reduction_percent," \
		"sync_cpu_seconds,async_cpu_seconds," \
		"async_cpu_time_reduction_percent"
}
NR == FNR {
	if (FNR == 1)
		next;
	if ($4 != "udp-sync")
		next;
	mode=$6 == "off" ? "copy" : "zerocopy";
	key=$3 SUBSEP $5 SUBSEP mode SUBSEP $26 SUBSEP $27;
	bw[key]=$14;
	lat[key]=$18;
	cpu[key]=$11;
	next;
}
FNR == 1 { next }
$4 == "udp-io-uring" {
	mode=$6 == "off" ? "copy" : "zerocopy";
	key=$3 SUBSEP $5 SUBSEP mode SUBSEP $26 SUBSEP $27;
	if (key in bw) {
		bw_delta=bw[key] ? 100 * ($14 - bw[key]) / bw[key] : 0;
		lat_delta=lat[key] ? 100 * (lat[key] - $18) / lat[key] : 0;
		cpu_delta=cpu[key] ? 100 * (cpu[key] - $11) / cpu[key] : 0;
		printf "%s,%s,%s,%s,%s,%.6f,%.6f,%+.3f,%s,%s,%+.3f," \
			"%.9f,%.9f,%+.3f\n", $3, $5, mode, $26, $27,
			bw[key], $14, bw_delta, lat[key], $18, lat_delta,
			cpu[key], $11, cpu_delta;
	}
}' "$results" "$results" >"$output/sync-async-comparison.csv"

printf '\nResults: %s\n' "$results"
printf 'Zero-copy comparison: %s\n' "$output/zerocopy-comparison.csv"
printf 'DMA-BUF comparison: %s\n' "$output/dmabuf-comparison.csv"
printf 'Sync/async comparison: %s\n' \
	"$output/sync-async-comparison.csv"
if [ -s "$skips" ]; then
	printf 'Unavailable combinations: %s\n' "$skips"
fi
if [ -s "$failures" ]; then
	printf 'Failed combinations: %s\n' "$failures" >&2
	exit 1
fi
if [ "$ib_status" -ne 0 ]; then
	printf 'Failed ib_write_bw combinations: %s\n' "$ib_failures" >&2
	exit 1
fi
