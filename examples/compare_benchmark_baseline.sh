#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

baseline=${1:?usage: compare_benchmark_baseline.sh BASELINE.md OUTPUT_DIRECTORY}
output=${2:?usage: compare_benchmark_baseline.sh BASELINE.md OUTPUT_DIRECTORY}
results=$output/results.csv
old_rows=$output/baseline-old.csv
new_rows=$output/baseline-new.csv
comparison=$output/baseline-comparison.csv
markdown=$output/baseline-comparison.md
summary=$output/baseline-summary.csv
limit=${UIB_BENCH_MAX_REGRESSION_PERCENT:-5}

if [ ! -r "$baseline" ] || [ ! -r "$results" ]; then
	printf 'benchmark comparison: missing baseline or results\n' >&2
	exit 1
fi

awk -F'|' '
function trim(value) {
	gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
	return value;
}
BEGIN {
	OFS=",";
	print "protocol,transport,memory,mtu,cpu_affinity,mode," \
		"gbit_per_second,cpu_seconds,latency_avg_us";
}
/^\| Protocol \| Transport \| Memory \| MTU \| Affinity \| Send mode / {
	in_table=1;
	next;
}
in_table && /^## / { exit }
in_table && /^\|---/ { next }
in_table && /^\|/ {
	protocol=trim($2);
	if (protocol != "rocev2" && protocol != "uet" && protocol != "dummy")
		next;
	transport=trim($3);
	memory=trim($4);
	mtu=trim($5);
	affinity=trim($6);
	mode=trim($7);
	mode=mode == "copy" ? "copy" : "zerocopy";
	printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", protocol, transport,
		memory, mtu, affinity, mode, trim($8), trim($10), trim($12);
}
' "$baseline" >"$old_rows"

awk -F, 'BEGIN {
	OFS=",";
	print "protocol,transport,memory,mtu,cpu_affinity,mode," \
		"gbit_per_second,cpu_seconds,latency_avg_us";
}
NR == 1 { next }
{
	if ($4 == "udp-io-uring")
		transport="UDP";
	else if ($4 == "tcp-devmem")
		transport="TCP";
	else
		transport="AF_XDP";
	mode=$6 == "off" ? "copy" : "zerocopy";
	printf "%s,%s,%s,%s,%s,%s,%.9f,%.9f,%.3f\n", $3, transport,
		$5, $26, $27, mode, $14, $11, $18 / 1000;
}
' "$results" >"$new_rows"

awk -F, 'BEGIN {
	OFS=",";
	print "protocol,transport,memory,mtu,cpu_affinity,mode," \
		"old_gbit_per_second,new_gbit_per_second,bandwidth_change_percent," \
		"old_latency_avg_us,new_latency_avg_us,latency_improvement_percent," \
		"old_cpu_seconds,new_cpu_seconds,cpu_improvement_percent";
}
NR == FNR {
	if (FNR == 1)
		next;
	key=$1 SUBSEP $2 SUBSEP $3 SUBSEP $4 SUBSEP $5 SUBSEP $6;
	bw[key]=$7;
	cpu[key]=$8;
	latency[key]=$9;
	next;
}
FNR == 1 { next }
{
	key=$1 SUBSEP $2 SUBSEP $3 SUBSEP $4 SUBSEP $5 SUBSEP $6;
	if (!(key in bw))
		next;
	bw_change=bw[key] ? 100 * ($7 - bw[key]) / bw[key] : 0;
	latency_change=latency[key] ? 100 * (latency[key] - $9) / latency[key] : 0;
	cpu_change=cpu[key] ? 100 * (cpu[key] - $8) / cpu[key] : 0;
	printf "%s,%s,%s,%s,%s,%s,%.9f,%.9f,%+.3f,%.3f,%.3f,%+.3f," \
		"%.9f,%.9f,%+.3f\n", $1, $2, $3, $4, $5, $6,
		bw[key], $7, bw_change, latency[key], $9, latency_change,
		cpu[key], $8, cpu_change;
}
' "$old_rows" "$new_rows" >"$comparison"

awk -F, 'BEGIN {
	OFS=",";
	print "protocol,transport,mode,rows,old_gbit_per_second_mean," \
		"new_gbit_per_second_mean,bandwidth_change_percent," \
		"old_latency_avg_us_mean,new_latency_avg_us_mean," \
		"latency_improvement_percent,old_cpu_seconds_mean," \
		"new_cpu_seconds_mean,cpu_improvement_percent";
}
NR == 1 { next }
{
	keys[0]="all" SUBSEP $2 SUBSEP $6;
	keys[1]=$1 SUBSEP $2 SUBSEP $6;
	for (i=0; i<2; i++) {
		key=keys[i];
		count[key]++;
		old_bw[key]+=$7;
		new_bw[key]+=$8;
		old_latency[key]+=$10;
		new_latency[key]+=$11;
		old_cpu[key]+=$13;
		new_cpu[key]+=$14;
	}
}
END {
	for (key in count) {
		split(key, fields, SUBSEP);
		bw_change=0;
		latency_change=0;
		cpu_change=0;
		if (old_bw[key])
			bw_change=100 * (new_bw[key] - old_bw[key]) / old_bw[key];
		if (old_latency[key]) {
			delta=old_latency[key] - new_latency[key];
			latency_change=100 * delta / old_latency[key];
		}
		if (old_cpu[key]) {
			delta=old_cpu[key] - new_cpu[key];
			cpu_change=100 * delta / old_cpu[key];
		}
		printf "%s,%s,%s,%u,%.9f,%.9f,%+.3f,%.3f,%.3f,%+.3f," \
			"%.9f,%.9f,%+.3f\n", fields[1], fields[2], fields[3],
			count[key], old_bw[key] / count[key],
			new_bw[key] / count[key], bw_change,
			old_latency[key] / count[key],
			new_latency[key] / count[key], latency_change,
			old_cpu[key] / count[key], new_cpu[key] / count[key],
			cpu_change;
	}
}
' "$comparison" | { read -r header; printf '%s\n' "$header"; sort; } >"$summary"

{
	printf '## Compared with previous reference\n\n'
	printf 'Positive percentages mean the new result improved. '
	printf 'All old and new values are shown.\n\n'
	printf '%s\n' '| Protocol | Transport | Mode | Rows | Old Gbit/s | New Gbit/s | Bandwidth | Old avg us | New avg us | Latency | Old CPU s | New CPU s | CPU |'
	printf '%s\n' '|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|'
	awk -F, 'NR > 1 {
		printf "| %s | %s | %s | %s | %.3f | %.3f | %+.1f%% | " \
			"%.1f | %.1f | %+.1f%% | %.3f | %.3f | %+.1f%% |\n",
			$1, $2, $3, $4, $5, $6, $7, $8, $9, $10,
			$11, $12, $13;
	}' "$summary"
	printf '\nDetailed per-row values: `baseline-comparison.csv`.\n'
} >"$markdown"

printf 'Baseline comparison CSV: %s\n' "$comparison"
printf 'Baseline comparison Markdown: %s\n' "$markdown"

if awk -F, -v limit="$limit" 'NR > 1 && $1 == "all" && $2 == "UDP" &&
    ($7 < -limit || $10 < -limit || $13 < -limit) { failed=1 }
    END { exit failed ? 0 : 1 }' "$summary"; then
	printf 'benchmark comparison: UDP regression exceeds %s%%\n' "$limit" >&2
	exit 2
fi
