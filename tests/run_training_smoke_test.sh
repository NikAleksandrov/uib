#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-training-test.XXXXXX")
image=${TRAINING_IMAGE:-uib-training:test}
engine=${TRAINING_CONTAINER_ENGINE:-docker}
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

container()
{
	$engine "$@"
}

run_container()
{
	container run --user "$(id -u):$(id -g)" \
		-e HOME=/tmp -e USER=uib -e LOGNAME=uib \
		-e XDG_CACHE_HOME=/tmp/.cache "$@"
}

if [ "${TRAINING_SKIP_BUILD:-0}" != 1 ]; then
	container build --progress=plain \
		-f "$project_dir/simulation/training/Containerfile" \
		-t "$image" "$project_dir/simulation/training"
fi

run_container --rm --entrypoint python "$image" -c \
	'import ctypes, pathlib, nvidia.nccl; p = pathlib.Path(nvidia.nccl.__path__[0]) / "lib/libnccl.so.2"; lib = ctypes.CDLL(str(p)); version = ctypes.c_int(); assert lib.ncclGetVersion(ctypes.byref(version)) == 0; assert version.value == 23004, version.value'

if [ -n "${TRAINING_UIB_RUNTIME:-}" ]; then
	run_container --rm -v "$TRAINING_UIB_RUNTIME:/opt/uib:ro" \
		--entrypoint /opt/uib/bin/uib-preflight "$image" --library-only
fi

mkdir "$test_dir/ddp"
run_container --rm -v "$test_dir/ddp:/results" \
	--entrypoint torchrun "$image" --standalone --nproc-per-node=2 \
	--no-python /opt/uib-training/bin/train \
	--config /opt/uib-training/configs/smoke.json

mkdir "$test_dir/full" "$test_dir/restart"
sed 's/"steps": 4/"steps": 2/' \
	"$project_dir/simulation/training/configs/smoke.json" \
	> "$test_dir/restart/first.json"
sed 's#"resume": null#"resume": "/results/checkpoints/step-00000002.pt"#' \
	"$project_dir/simulation/training/configs/smoke.json" \
	> "$test_dir/restart/second.json"
run_container --rm -v "$test_dir/full:/results" --entrypoint train \
	"$image" --config /opt/uib-training/configs/smoke.json
run_container --rm -v "$test_dir/restart:/results" \
	-v "$test_dir/restart/first.json:/config.json:ro" --entrypoint train \
	"$image" --config /config.json
run_container --rm -v "$test_dir/restart:/results" \
	-v "$test_dir/restart/second.json:/config.json:ro" --entrypoint train \
	"$image" --config /config.json

python3 - "$test_dir" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
ddp = json.loads((root / "ddp/local-ddp-smoke/result.json").read_text())
full = json.loads((root / "full/local-ddp-smoke/result.json").read_text())
restart = json.loads((root / "restart/local-ddp-smoke/result.json").read_text())
assert ddp["status"] == "passed" and ddp["worldSize"] == 2
assert ddp["transport"]["distributedBackend"] == "gloo"
assert ddp["stack"]["ncclPackage"] == "2.30.4"
assert ddp["stack"]["ncclLibrary"] == "2.30.4"
assert restart["resumedFromStep"] == 2
assert restart["modelChecksum"] == full["modelChecksum"]
PY

if [ "${TRAINING_PUBLIC_TEST:-0}" = 1 ]; then
	cache=${TRAINING_HF_CACHE:-$project_dir/simulation/.artifacts/huggingface}
	mkdir -p "$cache" "$test_dir/public"
	run_container --rm -e HF_HOME=/cache/huggingface \
		-v "$cache:/cache/huggingface" \
		-v "$test_dir/public:/results" --entrypoint train "$image" \
		--config /opt/uib-training/configs/tiny-public.json
	python3 -m json.tool \
		"$test_dir/public/tiny-public-cpu/result.json" >/dev/null
fi

if [ "${TRAINING_PRODUCTION_CPU_TEST:-0}" = 1 ]; then
	cache=${TRAINING_HF_CACHE:-$project_dir/simulation/.artifacts/huggingface}
	config=$project_dir/simulation/training/configs/production-cpu.json
	mkdir -p "$cache" "$test_dir/production-full" \
		"$test_dir/production-restart"
	sed 's/"steps": 8/"steps": 4/' "$config" \
		> "$test_dir/production-restart/first.json"
	sed 's#"resume": null#"resume": "/results/checkpoints/step-00000004.pt"#' \
		"$config" > "$test_dir/production-restart/second.json"
	for stage in full first second; do
		case $stage in
		full)
			results=$test_dir/production-full
			stage_config=$config
			;;
		*)
			results=$test_dir/production-restart
			stage_config=$test_dir/production-restart/$stage.json
			;;
		esac
		run_container --rm -e HF_HOME=/cache/huggingface \
			-v "$cache:/cache/huggingface" \
			-v "$results:/results" \
			-v "$stage_config:/config.json:ro" \
			--entrypoint torchrun "$image" --standalone \
			--nproc-per-node=2 --no-python \
			/opt/uib-training/bin/train --config /config.json
	done
	python3 - "$test_dir" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
full = json.loads((
	root / "production-full/production-shaped-training-cpu/result.json"
).read_text())
restart = json.loads((
	root / "production-restart/production-shaped-training-cpu/result.json"
).read_text())
shape = full["trainingShape"]
assert full["status"] == "passed" and full["worldSize"] == 2
assert full["completedSteps"] == 8 and full["resumedFromStep"] == 0
assert shape["sequenceLength"] == 64
assert shape["microBatchSize"] == 1
assert shape["globalBatchSize"] == 8
assert shape["gradientAccumulationSteps"] == 4
assert shape["gradientCheckpointing"] is True
assert restart["resumedFromStep"] == 4
assert restart["modelChecksum"] == full["modelChecksum"]
PY
fi

if [ "${TRAINING_GPU_TEST:-0}" = 1 ]; then
	gpu_device=${TRAINING_GPU_DEVICE:-nvidia.com/gpu=all}
	gpu_processes=${TRAINING_GPU_PROCESSES:-1}
	gpu_strategy=${TRAINING_GPU_STRATEGY:-fsdp}
	case $gpu_processes in
		''|*[!0-9]*|0)
			echo "TRAINING_GPU_PROCESSES must be a positive integer" >&2
			exit 1
			;;
	esac
	case $gpu_strategy in
		ddp|fsdp) ;;
		*)
			echo "TRAINING_GPU_STRATEGY must be ddp or fsdp" >&2
			exit 1
			;;
	esac
	gpu_global_batch=$((2 * gpu_processes))
	mkdir "$test_dir/gpu" "$test_dir/gpu-full"
	sed -e "s/\"strategy\": \"ddp\"/\"strategy\": \"$gpu_strategy\"/" \
		-e 's/"device": "cpu"/"device": "cuda"/' \
		-e "s/\"globalBatchSize\": 4/\"globalBatchSize\": $gpu_global_batch/" \
		"$project_dir/simulation/training/configs/smoke.json" \
		> "$test_dir/gpu-full/config.json"
	sed -e "s/\"strategy\": \"ddp\"/\"strategy\": \"$gpu_strategy\"/" \
		-e 's/"device": "cpu"/"device": "cuda"/' \
		-e "s/\"globalBatchSize\": 4/\"globalBatchSize\": $gpu_global_batch/" \
		"$test_dir/restart/first.json" > "$test_dir/gpu/first.json"
	sed -e "s/\"strategy\": \"ddp\"/\"strategy\": \"$gpu_strategy\"/" \
		-e 's/"device": "cpu"/"device": "cuda"/' \
		-e "s/\"globalBatchSize\": 4/\"globalBatchSize\": $gpu_global_batch/" \
		"$test_dir/restart/second.json" > "$test_dir/gpu/second.json"
	run_container --rm --device "$gpu_device" \
		-v "$test_dir/gpu-full:/results" \
		-v "$test_dir/gpu-full/config.json:/config.json:ro" \
		--entrypoint torchrun "$image" --standalone \
		--nproc-per-node="$gpu_processes" \
		--no-python /opt/uib-training/bin/train --config /config.json
	run_container --rm --device "$gpu_device" \
		-v "$test_dir/gpu:/results" \
		-v "$test_dir/gpu/first.json:/config.json:ro" \
		--entrypoint torchrun "$image" --standalone \
		--nproc-per-node="$gpu_processes" \
		--no-python /opt/uib-training/bin/train --config /config.json
	run_container --rm --device "$gpu_device" \
		-v "$test_dir/gpu:/results" \
		-v "$test_dir/gpu/second.json:/config.json:ro" \
		--entrypoint torchrun "$image" --standalone \
		--nproc-per-node="$gpu_processes" \
		--no-python /opt/uib-training/bin/train --config /config.json
	python3 - "$test_dir/gpu/local-ddp-smoke/result.json" \
		"$test_dir/gpu-full/local-ddp-smoke/result.json" \
		"$gpu_processes" "$gpu_strategy" <<'PY'
import json
import sys

result = json.load(open(sys.argv[1]))
full = json.load(open(sys.argv[2]))
assert result["device"] == "cuda"
assert result["strategy"] == sys.argv[4]
assert result["resumedFromStep"] == 2
assert result["stack"]["ncclLibrary"] == "2.30.4"
assert result["transport"]["distributedBackend"] == "nccl"
assert result["modelChecksum"] == full["modelChecksum"]
assert result["worldSize"] == int(sys.argv[3])
PY
fi

echo "training image smoke test: PASS"
