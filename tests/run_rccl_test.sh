#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
rank_wrapper=$project_dir/tests/nccl_rank_wrapper.sh
rccl_test=${RCCL_TESTS_BIN:-}
mpiexec=${MPIEXEC:-mpirun}
ranks=${RCCL_TEST_RANKS:-2}
max_bytes=${RCCL_TEST_MAX_BYTES:-64K}
iterations=${RCCL_TEST_ITERATIONS:-3}
warmups=${RCCL_TEST_WARMUPS:-1}
test_timeout=${RCCL_TEST_TIMEOUT:-90}
bind_port=${RCCL_TEST_BIND_PORT:-19900}
test_dir=$(mktemp -d /tmp/uib-rccl-test.XXXXXX)
output=$test_dir/rccl.log
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

if [ -z "$rccl_test" ]; then
	rccl_test=$(command -v all_reduce_perf || true)
fi
if [ -z "$rccl_test" ] || [ ! -x "$rccl_test" ]; then
	echo "set RCCL_TESTS_BIN to the MPI-enabled rccl-tests all_reduce_perf binary" >&2
	exit 1
fi
for command_name in "$mpiexec" rocminfo setpriv unshare timeout; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required RCCL command not found: $command_name" >&2
		exit 1
	fi
done
if ! rocminfo 2>/dev/null | grep -q 'Agent.*GPU'; then
	echo "at least two accessible AMD GPU ranks are required" >&2
	exit 1
fi
if [ "$ranks" -lt 2 ]; then
	echo "RCCL_TEST_RANKS must be at least 2 to validate UIB traffic" >&2
	exit 1
fi

set +e
timeout -k 5 "$test_timeout" env \
	HIP_VISIBLE_DEVICES="${HIP_VISIBLE_DEVICES:-0,1}" \
	NCCL_NET_GDR_LEVEL=LOC \
	NCCL_IBVERBS_LIB="$project_dir/libuibv.so" \
	NCCL_NET=IB NCCL_IB_DISABLE=0 NCCL_IB_HCA==uib0 \
	NCCL_SOCKET_IFNAME=lo NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=INIT,NET \
	UIB_BIND_PORT="$bind_port" UIB_PROTOCOL=rocev2 \
	UIB_TRANSPORT_MODE=udp-sync UIB_ZEROCOPY=off \
	"$mpiexec" -np "$ranks" --bind-to none "$rank_wrapper" \
	"$rccl_test" -b 8 -e "$max_bytes" -f 2 -g 1 \
	-n "$iterations" -w "$warmups" > "$output" 2>&1
status=$?
set -e
cat "$output"
if [ "$status" -ne 0 ]; then
	echo "RCCL collective failed with status $status" >&2
	exit "$status"
fi

grep -E 'Initialized NET plugin IB|Using network IB|NET/IB' "$output" \
	>/dev/null
grep -F 'Out of bounds values : 0 OK' "$output" >/dev/null
if grep -E 'Initialized NET plugin Socket|Using network Socket|NET/Socket' \
	"$output" >/dev/null; then
	echo "RCCL silently selected its Socket network" >&2
	exit 1
fi

echo "actual RCCL/UIB collective test: PASS"
