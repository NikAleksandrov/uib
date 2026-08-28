#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

output=${1:?usage: benchmark_report.sh OUTPUT_DIRECTORY}
ib_write_bw=$output/ib-write-bw.csv
results=$output/results.csv
zerocopy=$output/zerocopy-comparison.csv
dmabuf=$output/dmabuf-comparison.csv
sync_async=$output/sync-async-comparison.csv
congestion=$output/congestion-comparison.csv
skips=$output/skips.txt
failures=$output/failures.txt
text_report=$output/benchmark.txt
markdown_report=$output/benchmark.md

protocol_label()
{
	case $1 in
	rocev2)
		printf '%s' 'RoCEv2'
		;;
	uet)
		printf '%s' 'UET'
		;;
	dummy)
		printf '%s' 'Dummy'
		;;
	esac
}

if [ ! -r "$ib_write_bw" ] || [ ! -r "$results" ] || [ ! -r "$zerocopy" ] ||
   [ ! -r "$dmabuf" ] || [ ! -r "$sync_async" ] ||
   [ ! -r "$congestion" ]; then
	printf 'benchmark_report: incomplete CSV input in %s\n' "$output" >&2
	exit 1
fi

{
	printf 'UIB benchmark matrix\n'
	printf '====================\n\n'
	printf 'ib_write_bw through the transparent provider\n'
	printf '%-7s %-14s %-6s %10s %10s %10s %9s %8s %7s %s\n' \
		'Proto' 'Transport' 'Status' 'Peak Gb/s' 'Avg Gb/s' 'Mpps' \
		'Wall(s)' 'CPU(s)' 'CPU%' 'Reason'
	printf '%-7s %-14s %-6s %10s %10s %10s %9s %8s %7s %s\n' \
		'-------' '--------------' '------' '----------' '----------' \
		'----------' '---------' '--------' '-------' '------'
	awk -F, 'NR > 1 {
		printf "%-7s %-14s %-6s %10s %10s %10s %9s %8s %7s %s\n",
			$2, $3, $1, $6, $7, $8, $9, $12, $13, $14;
	}' "$ib_write_bw"
	printf '\nNative UIB stress benchmark\n'
	printf '%-7s %-5s %-7s %5s %-8s %-12s %8s %9s %8s %7s %9s %9s %9s %9s %12s\n' \
		'Proto' 'Xport' 'Memory' 'MTU' 'Affinity' 'Mode' 'Gbit/s' 'MiB/s' 'CPU(s)' \
		'CPU%' 'Avg(us)' 'p50(us)' 'p95(us)' 'p99(us)' 'Deferred/Req'
	printf '%-7s %-5s %-7s %5s %-8s %-12s %8s %9s %8s %7s %9s %9s %9s %9s %12s\n' \
		'-------' '-----' '-------' '-----' '--------' '------------' \
		'--------' '---------' \
		'--------' '-------' '---------' '---------' '---------' \
		'---------' '------------'
	awk -F, 'NR > 1 {
		if ($4 == "udp-io-uring")
			transport = "UDP-A";
		else if ($4 == "udp-sync")
			transport = "UDP-S";
		else if ($4 == "tcp-devmem")
			transport = "TCP";
		else
			transport = "AF_XDP";
		mode = $6 == "off" ? "copy" : "zerocopy";
		format = "%-7s %-5s %-7s %5s %-8s %-12s %8.3f %9.1f " \
			"%8.3f %7.1f %9.1f %9.1f %9.1f %9.1f %5s/%-6s\n";
		printf format,
			$3, transport, $5, $26, $27, mode, $14, $15, $11, $16,
			$18 / 1000, $19 / 1000, $20 / 1000, $21 / 1000,
			$25, $23;
	}' "$results"

	printf '\nProtocol and congestion-control resilience\n'
	printf '%-7s %-20s %-17s %-6s %8s %9s %9s %7s %s\n' \
		'Proto' 'Algorithm' 'Condition' 'Status' 'Gbit/s' 'Avg(us)' \
		'p99(us)' 'CPU%' 'Notes'
	printf '%-7s %-20s %-17s %-6s %8s %9s %9s %7s %s\n' \
		'-------' '--------------------' '-----------------' '------' \
		'--------' '---------' '---------' '-------' '-----'
	awk -F, 'NR > 1 {
		note = $5 == 1 ? "experimental" : $23;
		printf "%-7s %-20s %-17s %-6s %8s %9s %9s %7s %s\n",
			$2, $3, $4, $1, $13, $17 / 1000, $20 / 1000, $15,
			note;
	}' "$congestion"

	printf '\nZero-copy compared with copy (positive means improvement)\n'
	printf '%-7s %-7s %-5s %5s %-8s %21s %24s %22s %10s\n' \
		'Proto' 'Memory' 'Xport' 'MTU' 'Affinity' 'Gbit/s copy->zc (%)' \
		'Latency us copy->zc (%)' 'CPU s copy->zc (%)' 'Deferred'
	printf '%-7s %-7s %-5s %5s %-8s %21s %24s %22s %10s\n' \
		'-------' '-------' '-----' '-----' '--------' '---------------------' \
		'------------------------' '----------------------' '----------'
	awk -F, 'NR > 1 {
		format = "%-7s %-7s %-5s %5s %-8s %6.3f->%-6.3f (%+5.1f%%) " \
			"%7.1f->%-7.1f (%+5.1f%%) " \
			"%6.3f->%-6.3f (%+5.1f%%) %9.1f%%\n";
		printf format,
			$1, $2, toupper($3), $4, $5, $6, $7, $8,
			$9 / 1000, $10 / 1000, $11, $12, $13, $14, $16;
	}' "$zerocopy"

	printf '\nDMA-BUF compared with host memory\n'
	printf '%-7s %-15s %5s %-8s %23s %26s %24s\n' \
		'Proto' 'Mode' 'MTU' 'Affinity' 'Gbit/s host->DMA (%)' \
		'Latency us host->DMA (%)' 'CPU s host->DMA (%)'
	printf '%-7s %-15s %5s %-8s %23s %26s %24s\n' \
		'-------' '---------------' '-----' '--------' '-----------------------' \
		'--------------------------' '------------------------'
	awk -F, 'NR > 1 {
		format = "%-7s %-15s %5s %-8s %6.3f->%-6.3f (%+5.1f%%) " \
			"%7.1f->%-7.1f (%+5.1f%%) " \
			"%6.3f->%-6.3f (%+5.1f%%)\n";
		printf format,
			$1, $2, $3, $4, $5, $6, $7, $8 / 1000, $9 / 1000,
			$10, $11, $12, $13;
	}' "$dmabuf"

	printf '\nAsynchronous UDP compared with synchronous UDP\n'
	printf '%-7s %-7s %-8s %5s %-8s %24s %27s %25s\n' \
		'Proto' 'Memory' 'Mode' 'MTU' 'Affinity' \
		'Gbit/s sync->async (%)' 'Latency us sync->async (%)' \
		'CPU s sync->async (%)'
	printf '%-7s %-7s %-8s %5s %-8s %24s %27s %25s\n' \
		'-------' '-------' '--------' '-----' '--------' \
		'------------------------' '---------------------------' \
		'-------------------------'
	awk -F, 'NR > 1 {
		format = "%-7s %-7s %-8s %5s %-8s " \
			"%6.3f->%-6.3f (%+5.1f%%) " \
			"%7.1f->%-7.1f (%+5.1f%%) " \
			"%6.3f->%-6.3f (%+5.1f%%)\n";
		printf format,
			$1, $2, $3, $4, $5, $6, $7, $8,
			$9 / 1000, $10 / 1000, $11, $12, $13, $14;
	}' "$sync_async"

	if [ -s "$skips" ]; then
		printf '\nSkipped combinations\n'
		sed 's/^/  - /' "$skips"
	fi
	if [ -s "$failures" ]; then
		printf '\nFailed combinations\n'
		sed 's/^/  - /' "$failures"
	fi
} >"$text_report"

{
	printf '# UIB benchmark matrix\n\n'
	printf '## Contents\n\n'
	printf '%s\n' '- [`ib_write_bw` through the transparent provider](#ib_write_bw-through-the-transparent-provider)'
	printf '%s\n' '- [Native UIB stress benchmark](#native-uib-stress-benchmark)'
	printf '%s\n' '- [Protocol and congestion-control resilience](#protocol-and-congestion-control-resilience)'
	printf '%s\n' '- [Zero-copy compared with copy](#zero-copy-compared-with-copy)'
	printf '%s\n' '- [DMA-BUF compared with host memory](#dma-buf-compared-with-host-memory)'
	printf '%s\n\n' '- [Asynchronous UDP compared with synchronous UDP](#asynchronous-udp-compared-with-synchronous-udp)'
	printf '## `ib_write_bw` through the transparent provider\n\n'
	for report_protocol in rocev2 uet dummy; do
		report_label=$(protocol_label "$report_protocol")
		printf '### %s\n\n' "$report_label"
		printf '%s' '| Transport | Status | Peak Gbit/s | Average Gbit/s | '
		printf '%s' 'Message rate Mpps | Wall s | User CPU s | System CPU s | '
		printf '%s\n' 'Total CPU s | CPU % | Reason |'
		printf '%s' '|---|---|---:|---:|---:|---:|---:|---:|'
		printf '%s\n' '---:|---:|---|'
		awk -F, -v protocol="$report_protocol" 'NR > 1 && $2 == protocol {
			format = "| %s | %s | %s | %s | %s | %s | %s | %s | " \
				"%s | %s | %s |\n";
			printf format,
				$3, $1, $6, $7, $8, $9, $10, $11, $12, $13, $14;
		}' "$ib_write_bw"
		printf '\n'
	done

	printf '\n## Native UIB stress benchmark\n\n'
	printf 'Each table fixes protocol, CPU affinity, and MTU so transport, '
	printf 'memory, and send mode can be compared directly.\n\n'
	for report_protocol in rocev2 uet dummy; do
		report_label=$(protocol_label "$report_protocol")
		awk -F, -v protocol="$report_protocol" -v label="$report_label" '
		NR > 1 && $3 == protocol {
			key = $27 SUBSEP $26;
			if (key != previous) {
				if (previous != "")
					print "";
				printf "### %s: %s, MTU %s\n\n", label, $27, $26;
				print "| Transport | Memory | Window | Send mode | Gbit/s | " \
					"MiB/s | CPU s | CPU % | Average us | p50 us | " \
					"p95 us | p99 us | Deferred/requests |";
				print "|---|---|---:|---|---:|---:|---:|---:|---:|" \
					"---:|---:|---:|---:|";
				previous = key;
			}
			if ($4 == "udp-io-uring")
				transport = "UDP async";
			else if ($4 == "udp-sync")
				transport = "UDP sync";
			else if ($4 == "tcp-devmem")
				transport = "TCP";
			else
				transport = "AF_XDP";
			if ($6 == "off")
				mode = "copy";
			else if ($4 == "udp-io-uring")
				mode = "`SENDMSG_ZC`";
			else
				mode = "`MSG_ZEROCOPY`";
			format = "| %s | %s | %s | %s | %.3f | %.1f | %.3f | " \
				"%.1f | %.1f | %.1f | %.1f | %.1f | %s/%s |\n";
			printf format, transport, $5, $9, mode, $14, $15, $11, $16,
				$18 / 1000, $19 / 1000, $20 / 1000, $21 / 1000,
				$25, $23;
		}' "$results"
		printf '\n'
	done

	printf '\n## Protocol and congestion-control resilience\n\n'
	printf 'Each row uses the same native UIB workload, host memory, synchronous '
	printf 'UDP transport, MTU, message size, window, and iteration count. Only '
	printf 'the protocol, congestion-control algorithm, and named `tc netem` '
	printf 'condition change. UET with DCQCN is cross-protocol experimental '
	printf 'coverage; it is never selected by default.\n\n'
	printf '%s\n' '| Protocol | Algorithm | Condition | Status | Experimental | Gbit/s | CPU % | Average us | p50 us | p95 us | p99 us | Reason |'
	printf '%s\n' '|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|'
	awk -F, 'NR > 1 {
		average = $17 == "" ? "" : sprintf("%.1f", $17 / 1000);
		cpu = $15 == "" ? "" : $15;
		p50 = $18 == "" ? "" : sprintf("%.1f", $18 / 1000);
		p95 = $19 == "" ? "" : sprintf("%.1f", $19 / 1000);
		p99 = $20 == "" ? "" : sprintf("%.1f", $20 / 1000);
		format = "| %s | %s | %s | %s | %s | %s | %s | %s | " \
			"%s | %s | %s | %s |\n";
		printf format, $2, $3, $4, $1, $5 ? "yes" : "no", $13,
			cpu, average, p50, p95, p99, $23;
	}' "$congestion"
	printf '\nThe built-in conditions are reproducible named profiles:\n\n'
	printf '%s\n' '| Condition | Netem impairment |'
	printf '%s\n' '|---|---|'
	printf '%s\n' '| `clean` | No impairment |'
	printf '%s\n' '| `latency` | 2 ms delay |'
	printf '%s\n' '| `drops` | 1% random packet loss |'
	printf '%s\n' '| `reordered` | 2 ms delay, 500 us normal jitter, and 10% reordering |'
	printf '%s\n\n' '| `drops-reordered` | 2 ms delay, 500 us normal jitter, 1% loss, and 10% reordering |'
	printf 'Run only selected comparisons, for example:\n\n'
	printf '%s\n' '```sh'
	printf '%s %s\n' \
		"UIB_CC_BENCH_VARIANTS='rocev2:dcqcn uet:rccc uet:nscc'" '\'
	printf '%s\n' "UIB_CC_BENCH_CONDITIONS='clean drops reordered' make benchmark-all"
	printf '%s\n\n' '```'

	printf '\n## Zero-copy compared with copy\n\n'
	printf 'Positive values mean zero-copy improved. Each row compares the '
	printf 'same protocol, affinity, MTU, memory, and transport.\n\n'
	for report_protocol in rocev2 uet dummy; do
		report_label=$(protocol_label "$report_protocol")
		awk -F, -v protocol="$report_protocol" -v label="$report_label" '
		NR > 1 && $1 == protocol {
			key = $5 SUBSEP $4;
			if (key != previous) {
				if (previous != "")
					print "";
				printf "### %s: %s, MTU %s\n\n", label, $5, $4;
				print "| Memory | Transport | Copy Gbit/s | Zero-copy " \
					"Gbit/s | Change | Copy latency us | Zero-copy " \
					"latency us | Reduction | Copy CPU s | Zero-copy " \
					"CPU s | Reduction | Deferred |";
				print "|---|---|---:|---:|---:|---:|---:|---:|---:|" \
					"---:|---:|---:|";
				previous = key;
			}
			format = "| %s | %s | %.3f | %.3f | %+.1f%% | %.1f | " \
				"%.1f | %+.1f%% | %.3f | %.3f | %+.1f%% | %.1f%% |\n";
			printf format, $2, toupper($3), $6, $7, $8, $9 / 1000,
				$10 / 1000, $11, $12, $13, $14, $16;
		}' "$zerocopy"
		printf '\n'
	done

	printf '\n## DMA-BUF compared with host memory\n\n'
	printf 'Each row compares host and DMA-BUF memory with protocol, affinity, '
	printf 'MTU, transport, and send mode held constant.\n\n'
	for report_protocol in rocev2 uet dummy; do
		report_label=$(protocol_label "$report_protocol")
		awk -F, -v protocol="$report_protocol" -v label="$report_label" '
		NR > 1 && $1 == protocol {
			key = $4 SUBSEP $3;
			if (key != previous) {
				if (previous != "")
					print "";
				printf "### %s: %s, MTU %s\n\n", label, $4, $3;
				print "| Transport/mode | Host Gbit/s | DMA-BUF Gbit/s | " \
					"Change | Host latency us | DMA-BUF latency us | " \
					"Change | Host CPU s | DMA-BUF CPU s | Change |";
				print "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|";
				previous = key;
			}
			format = "| %s | %.3f | %.3f | %+.1f%% | %.1f | %.1f | " \
				"%+.1f%% | %.3f | %.3f | %+.1f%% |\n";
			printf format, $2, $5, $6, $7, $8 / 1000, $9 / 1000,
				$10, $11, $12, $13;
		}' "$dmabuf"
		printf '\n'
	done

	printf '\n## Asynchronous UDP compared with synchronous UDP\n\n'
	printf 'Positive values mean the asynchronous path improved. Each row '
	printf 'holds protocol, affinity, MTU, memory, and send mode constant.\n\n'
	for report_protocol in rocev2 uet dummy; do
		report_label=$(protocol_label "$report_protocol")
		awk -F, -v protocol="$report_protocol" -v label="$report_label" '
		NR > 1 && $1 == protocol {
			key = $5 SUBSEP $4;
			if (key != previous) {
				if (previous != "")
					print "";
				printf "### %s: %s, MTU %s\n\n", label, $5, $4;
				print "| Memory | Mode | Sync Gbit/s | Async Gbit/s | " \
					"Change | Sync latency us | Async latency us | " \
					"Reduction | Sync CPU s | Async CPU s | Reduction |";
				print "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|";
				previous = key;
			}
			format = "| %s | %s | %.3f | %.3f | %+.1f%% | %.1f | " \
				"%.1f | %+.1f%% | %.3f | %.3f | %+.1f%% |\n";
			printf format, $2, $3, $6, $7, $8, $9 / 1000,
				$10 / 1000, $11, $12, $13, $14;
		}' "$sync_async"
		printf '\n'
	done

	if [ -s "$skips" ]; then
		printf '\n## Skipped combinations\n\n'
		sed 's/^/- /' "$skips"
	fi
	if [ -s "$failures" ]; then
		printf '\n## Failed combinations\n\n'
		sed 's/^/- /' "$failures"
	fi
} >"$markdown_report"

printf 'ASCII report: %s\n' "$text_report"
printf 'GitHub Markdown report: %s\n' "$markdown_report"
printf 'Raw CSV: %s\n' "$results"
printf 'ib_write_bw CSV: %s\n' "$ib_write_bw"
printf 'Zero-copy comparison CSV: %s\n' "$zerocopy"
printf 'DMA-BUF comparison CSV: %s\n' "$dmabuf"
printf 'Sync/async comparison CSV: %s\n' "$sync_async"
printf 'Congestion comparison CSV: %s\n' "$congestion"
printf 'Skipped combinations: %s\n' "$skips"
printf 'Failed combinations: %s\n' "$failures"
