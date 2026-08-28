#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
rank_wrapper=$project_dir/tests/nccl_rank_wrapper.sh
nccl_test=${NCCL_TESTS_BIN:-}
collectives=${NCCL_TEST_COLLECTIVES:-all_reduce all_gather broadcast reduce reduce_scatter alltoall sendrecv}
mpiexec=${MPIEXEC:-mpirun}
max_bytes=${NCCL_TEST_MAX_BYTES:-64K}
iterations=${NCCL_TEST_ITERATIONS:-3}
warmups=${NCCL_TEST_WARMUPS:-1}
test_timeout=${NCCL_TEST_TIMEOUT:-90}
bind_port=${NCCL_TEST_BIND_PORT:-19700}
test_dir=$(mktemp -d /tmp/uib-nccl-test.XXXXXX)
mps_pipe=$test_dir/mps-pipe
mps_log=$test_dir/mps-log
mps_started=0

cleanup()
{
	if [ "$mps_started" = 1 ]; then
		printf 'quit\n' | env \
			CUDA_MPS_PIPE_DIRECTORY="$mps_pipe" \
			CUDA_MPS_LOG_DIRECTORY="$mps_log" \
			nvidia-cuda-mps-control >/dev/null 2>&1 || true
	fi
	rm -rf -- "$test_dir"
}
trap cleanup EXIT INT TERM

if [ -z "$nccl_test" ]; then
	nccl_test=$(command -v all_reduce_perf || true)
fi
if [ -z "$nccl_test" ] || [ ! -x "$nccl_test" ]; then
	echo "set NCCL_TESTS_BIN to the nccl-tests all_reduce_perf binary" >&2
	exit 1
fi
nccl_dir=$(CDPATH='' cd -- "$(dirname -- "$nccl_test")" && pwd)
for collective in $collectives; do
	case $collective in
	*[!a-z0-9_]*|'')
		echo "invalid NCCL_TEST_COLLECTIVES entry: $collective" >&2
		exit 2
		;;
	esac
	if [ ! -x "$nccl_dir/${collective}_perf" ]; then
		echo "NCCL collective binary not found: $nccl_dir/${collective}_perf" >&2
		exit 1
	fi
done
for command_name in "$mpiexec" nvidia-cuda-mps-control nvidia-smi \
	setpriv unshare timeout; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required command not found: $command_name" >&2
		exit 1
	fi
done
if ! nvidia-smi -L >/dev/null 2>&1; then
	echo "an accessible NVIDIA GPU and driver are required" >&2
	exit 1
fi

mkdir -p "$mps_pipe" "$mps_log"
env PATH=/usr/sbin:/usr/bin:/bin \
	CUDA_MPS_PIPE_DIRECTORY="$mps_pipe" \
	CUDA_MPS_LOG_DIRECTORY="$mps_log" \
	nvidia-cuda-mps-control -d
mps_started=1

index=0
for collective in $collectives; do
	output=$test_dir/$collective.log
	port=$((bind_port + index))
	if [ "$port" -gt 65535 ]; then
		echo "NCCL collective bind-port range exceeds 65535" >&2
		exit 2
	fi
	set +e
	timeout -k 5 "$test_timeout" env \
		CUDA_MPS_PIPE_DIRECTORY="$mps_pipe" \
		CUDA_MPS_LOG_DIRECTORY="$mps_log" \
		CUDA_VISIBLE_DEVICES=0 \
		NCCL_TESTS_DEVICE=0 \
		NCCL_MULTI_RANK_GPU_ENABLE=1 \
		NCCL_MAX_CTAS=1 \
		NCCL_MIN_NCHANNELS=1 \
		NCCL_MAX_NCHANNELS=1 \
		NCCL_NVLS_ENABLE=0 \
		NCCL_P2P_DISABLE=1 \
		NCCL_SHM_DISABLE=1 \
		NCCL_NET_GDR_LEVEL=LOC \
		NCCL_IBVERBS_LIB="$project_dir/libuibv.so" \
		NCCL_NET=IB \
		NCCL_IB_DISABLE=0 \
		NCCL_IB_HCA==uib0 \
		NCCL_SOCKET_IFNAME=lo \
		NCCL_DEBUG=INFO \
		NCCL_DEBUG_SUBSYS=INIT,NET \
		UIB_BIND_PORT="$port" \
		UIB_PROTOCOL=rocev2 \
		UIB_TRANSPORT_MODE=udp-sync \
		UIB_ZEROCOPY=off \
		"$mpiexec" -np 2 --bind-to none "$rank_wrapper" \
		"$nccl_dir/${collective}_perf" \
		-b 8 -e "$max_bytes" -f 2 -g 1 -n "$iterations" \
		-w "$warmups" >"$output" 2>&1
	status=$?
	set -e
	cat "$output"
	if [ "$status" -ne 0 ]; then
		echo "NCCL $collective failed with status $status" >&2
		exit "$status"
	fi

	grep -F "NCCL_IBVERBS_LIB set by environment to $project_dir/libuibv.so" \
		"$output" >/dev/null
	grep -F "Initialized NET plugin IB" "$output" >/dev/null
	grep -F "Using network IB" "$output" >/dev/null
	grep -E "via NET/IB/[0-9]+" "$output" >/dev/null
	grep -F "Out of bounds values : 0 OK" "$output" >/dev/null
	grep -F "Collective test concluded: ${collective}_perf" \
		"$output" >/dev/null
	if grep -E \
		"Initialized NET plugin Socket|Using network Socket|via NET/Socket" \
		"$output" >/dev/null; then
		echo "NCCL $collective silently selected its Socket network" >&2
		exit 1
	fi
	index=$((index + 1))
done

echo "actual NCCL/UIB collective matrix test: PASS"
