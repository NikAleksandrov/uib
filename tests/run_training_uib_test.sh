#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
rank_wrapper=$project_dir/tests/training_uib_rank_wrapper.sh
runtime=${TRAINING_UIB_RUNTIME:-$project_dir/simulation/.artifacts/uib-runtime}
image=${TRAINING_IMAGE:-uib-training:test}
engine=${TRAINING_CONTAINER_ENGINE:-docker}
mpiexec=${MPIEXEC:-mpirun}
test_timeout=${TRAINING_UIB_TIMEOUT:-180}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-training-net.XXXXXX")
mps_pipe=$test_dir/mps-pipe
mps_log=$test_dir/mps-log
results=$test_dir/results
config=$test_dir/workload.json
output=$test_dir/training.log
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

# The make variable may include a privilege helper before the engine.
# shellcheck disable=SC2086
set -- $engine
engine_command=$1
for command_name in "$engine_command" "$mpiexec" nvidia-cuda-mps-control \
	nvidia-smi timeout; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required command not found: $command_name" >&2
		exit 1
	fi
done
if [ ! -f "$runtime/lib/libuibv.so" ]; then
	echo "UIB runtime artifact not found: $runtime" >&2
	exit 1
fi
if ! nvidia-smi -L >/dev/null 2>&1; then
	echo "an accessible NVIDIA GPU and driver are required" >&2
	exit 1
fi

mkdir -p "$mps_pipe" "$mps_log" "$results"
sed -e 's/"runId": "local-ddp-smoke"/"runId": "local-uib-ddp-smoke"/' \
	-e 's/"device": "cpu"/"device": "cuda"/' \
	"$project_dir/simulation/training/configs/smoke.json" > "$config"

env PATH=/usr/sbin:/usr/bin:/bin \
	CUDA_MPS_PIPE_DIRECTORY="$mps_pipe" \
	CUDA_MPS_LOG_DIRECTORY="$mps_log" \
	nvidia-cuda-mps-control -d
mps_started=1

set +e
timeout -k 10 "$test_timeout" env \
	TRAINING_CONTAINER_ENGINE="$engine" \
	TRAINING_IMAGE="$image" \
	TRAINING_UIB_RUNTIME="$runtime" \
	TRAINING_UIB_CONFIG="$config" \
	TRAINING_UIB_RESULTS="$results" \
	TRAINING_MPS_PIPE="$mps_pipe" \
	TRAINING_MPS_LOG="$mps_log" \
	TRAINING_GPU_DEVICE="${TRAINING_GPU_DEVICE:-nvidia.com/gpu=all}" \
	TRAINING_UIB_BIND_PORT="${TRAINING_UIB_BIND_PORT:-19720}" \
	TRAINING_MASTER_PORT="${TRAINING_MASTER_PORT:-29720}" \
	"$mpiexec" -np 2 --bind-to none "$rank_wrapper" >"$output" 2>&1
status=$?
set -e
cat "$output"
if [ "$status" -ne 0 ]; then
	echo "two-rank training over UIB failed with status $status" >&2
	exit "$status"
fi

grep -F "NCCL_IBVERBS_LIB set by environment to /opt/uib/libuibv.so" \
	"$output" >/dev/null
grep -F "Initialized NET plugin IB" "$output" >/dev/null
grep -F "Using network IB" "$output" >/dev/null
grep -E "via NET/IB/[0-9]+" "$output" >/dev/null
if grep -E "Initialized NET plugin Socket|Using network Socket|via NET/Socket" \
	"$output" >/dev/null; then
	echo "NCCL silently selected its Socket network" >&2
	exit 1
fi

python3 - "$results/local-uib-ddp-smoke/result.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
	result = json.load(source)
assert result["status"] == "passed"
assert result["device"] == "cuda"
assert result["strategy"] == "ddp"
assert result["worldSize"] == 2
assert result["stack"]["ncclLibrary"] == "2.30.4"
assert result["transport"]["distributedBackend"] == "nccl"
assert result["transport"]["uibEnabled"] is True
assert result["transport"]["ncclNetwork"] == "IB"
assert result["transport"]["ncclIbverbsLibrary"] == "/opt/uib/libuibv.so"
PY

echo "two-rank training over UIB: PASS"
