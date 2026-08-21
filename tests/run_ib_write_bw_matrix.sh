#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d)
fake_dir=$test_dir/bin
output=$test_dir/output

cleanup()
{
	rm -rf "$test_dir"
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$fake_dir"
cp "$project_dir/tests/ib_write_bw_mock.sh" "$fake_dir/ib_write_bw"
chmod +x "$fake_dir/ib_write_bw"

PATH="$fake_dir:$PATH" UIB_BENCH_PROTOCOLS='rocev2 uet dummy' \
	UIB_BENCH_ITERATIONS=8 UIB_BENCH_SIZE=4096 \
	"$project_dir/examples/ib_write_bw_matrix.sh" "$output"

test "$(sed -n '2p' "$output/ib-write-bw.csv" | cut -d, -f2-3)" = \
	'rocev2,udp-io-uring'
test "$(awk -F, '$1 == "PASS" { count++ } END { print count + 0 }' \
	"$output/ib-write-bw.csv")" -eq 6
test "$(awk -F, '$1 == "SKIP" { count++ } END { print count + 0 }' \
	"$output/ib-write-bw.csv")" -eq 6
awk -F, '$1 == "PASS" && ($6 != 12.50 || $7 != 12.00 || $8 != 0.183) {
	exit 1;
}' "$output/ib-write-bw.csv"
awk -F, '$1 == "PASS" && ($9 == "" || $10 == "" || $11 == "" ||
	$12 == "" || $13 == "") {
	exit 1;
}' "$output/ib-write-bw.csv"

printf 'ib_write_bw benchmark matrix: PASS\n'
