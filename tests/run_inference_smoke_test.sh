#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-inference-test.XXXXXX")
image=${INFERENCE_IMAGE:-uib-inference:test}
engine=${INFERENCE_CONTAINER_ENGINE:-docker}
cache=${INFERENCE_HF_CACHE:-$project_dir/simulation/.artifacts/huggingface}
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

container()
{
	# The make variable may include a privilege helper before the engine.
	# shellcheck disable=SC2086
	$engine "$@"
}

run_container()
{
	container run --user "$(id -u):$(id -g)" \
		-e HOME=/tmp -e USER=uib -e LOGNAME=uib \
		-e XDG_CACHE_HOME=/tmp/.cache "$@"
}

if [ "${INFERENCE_SKIP_BUILD:-0}" != 1 ]; then
	container build --progress=plain \
		-f "$project_dir/simulation/inference/Containerfile" \
		-t "$image" "$project_dir/simulation/inference"
fi

run_container --rm --entrypoint /usr/bin/python3 "$image" -c \
	'import importlib.metadata as m; assert m.version("vllm") == "0.27.1"; assert m.version("ray") == "2.58.0"; assert m.version("nvidia-nccl-cu13") == "2.30.7"'

run_container --rm --shm-size=256m --entrypoint /usr/bin/python3 "$image" -c \
	'import ray; ray.init(num_cpus=1, include_dashboard=False); f = ray.remote(lambda: "ray-ok"); assert ray.get(f.remote()) == "ray-ok"; ray.shutdown()'

# KubeRay 1.6 probes the Ray 2.58 dashboard agent over this HTTP endpoint.
# shellcheck disable=SC2016
run_container --rm --shm-size=256m --entrypoint /bin/sh "$image" -c '
	set -eu
	ray start --head --dashboard-host=0.0.0.0 --block >/tmp/ray.log 2>&1 &
	server=$!
	cleanup() { kill "$server" >/dev/null 2>&1 || true; }
	trap cleanup EXIT INT TERM
	for unused in $(seq 1 30); do
		if python3 -c '\''import urllib.request; urllib.request.urlopen("http://127.0.0.1:52365/api/local_raylet_healthz", timeout=2).read()'\'' \
			>/dev/null 2>&1; then
			exit 0
		fi
		sleep 1
	done
	cat /tmp/ray.log >&2
	exit 1
'

mkdir -p "$cache" "$test_dir/results"
sed -e 's/"runId": "tiny-public-inference-cpu"/"runId": "tiny-public-inference-streaming-cpu"/' \
	-e 's/"streaming": false/"streaming": true/' \
	"$project_dir/simulation/inference/configs/tiny-public.json" \
	> "$test_dir/streaming.json"
# The command string expands $server inside the container shell.
# shellcheck disable=SC2016
run_container --rm --shm-size=1g \
	-e HF_HOME=/cache/huggingface \
	-v "$cache:/cache/huggingface" \
	-v "$test_dir/results:/results" \
	-v "$test_dir/streaming.json:/streaming.json:ro" \
	--entrypoint /bin/sh "$image" -c '
		set -eu
		inference serve --config /opt/uib-inference/configs/tiny-public.json &
		server=$!
		cleanup() { kill "$server" >/dev/null 2>&1 || true; }
		trap cleanup EXIT INT TERM
		inference probe --config /opt/uib-inference/configs/tiny-public.json
		inference probe --config /streaming.json
		kill "$server" >/dev/null 2>&1 || true
		wait "$server" || true
		trap - EXIT INT TERM
	'

python3 - "$test_dir/results/tiny-public-inference-cpu" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
result = json.loads((root / "result.json").read_text())
trace = json.loads((root / "request-trace.json").read_text())
assert result["status"] == "passed"
assert result["backend"] == "transformers"
assert result["requests"] == 4
assert result["outputTokens"] == 16
assert result["stack"]["vllm"] == "0.27.1"
assert result["stack"]["ray"] == "2.58.0"
assert result["stack"]["ncclLibrary"] == "2.30.7"
assert result["transport"]["uibEnabled"] is False
assert len(trace["requests"]) == 4
streaming = json.loads(
	(root.parent / "tiny-public-inference-streaming-cpu/result.json").read_text()
)
assert streaming["status"] == "passed"
assert streaming["requests"] == 4
PY

if [ "${INFERENCE_PRODUCTION_CPU_TEST:-0}" = 1 ]; then
	production=$project_dir/simulation/inference/configs/production-cpu.json
	# The command string expands $server inside the container shell.
	# shellcheck disable=SC2016
	run_container --rm --shm-size=1g \
		-e HF_HOME=/cache/huggingface \
		-v "$cache:/cache/huggingface" \
		-v "$test_dir/results:/results" \
		-v "$production:/production.json:ro" \
		--entrypoint /bin/sh "$image" -c '
			set -eu
			inference serve --config /production.json &
			server=$!
			cleanup() { kill "$server" >/dev/null 2>&1 || true; }
			trap cleanup EXIT INT TERM
			inference probe --config /production.json
			kill "$server" >/dev/null 2>&1 || true
			wait "$server" || true
			trap - EXIT INT TERM
		'
	python3 - "$test_dir/results/production-shaped-inference-cpu" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
result = json.loads((root / "result.json").read_text())
trace = json.loads((root / "request-trace.json").read_text())
load = result["load"]
assert result["status"] == "passed"
assert result["backend"] == "transformers"
assert result["requests"] == load["configuredRequests"] == 16
assert result["outputTokens"] == 128
assert load["concurrency"] == 4
assert load["requestRate"] == 4
assert load["promptTokens"] == 16
assert load["maximumOutputTokens"] == 8
assert load["streaming"] is True
assert load["warmupRequests"] == 2
assert len(trace["requests"]) == 16
assert trace["requests"][-1]["scheduledOffsetSeconds"] == 3.75
assert result["latencySeconds"]["p95"] > 0
PY
fi

sed -e 's/"backend": "transformers"/"backend": "vllm"/' \
	-e 's/"distributedExecutor": null/"distributedExecutor": "ray"/' \
	"$project_dir/simulation/inference/configs/tiny-public.json" \
	> "$test_dir/vllm.json"
run_container --rm -v "$test_dir/vllm.json:/config.json:ro" "$image" \
	render-vllm --config /config.json > "$test_dir/vllm-arguments.json"
python3 - "$test_dir/vllm-arguments.json" <<'PY'
import json
import sys

arguments = json.load(open(sys.argv[1]))
assert arguments[:3] == [
	"vllm", "serve", "HuggingFaceTB/SmolLM2-135M-Instruct"
]
assert arguments[arguments.index("--revision") + 1] == \
	"12fd25f77366fa6b3b4b768ec3050bf629380bac"
assert arguments[arguments.index("--distributed-executor-backend") + 1] == "ray"
PY

if [ "${INFERENCE_GPU_TEST:-0}" = 1 ]; then
	gpu_device=${INFERENCE_GPU_DEVICE:-nvidia.com/gpu=all}
	tp=${INFERENCE_GPU_TENSOR_PARALLEL:-1}
	pp=${INFERENCE_GPU_PIPELINE_PARALLEL:-1}
	dp=${INFERENCE_GPU_DATA_PARALLEL:-1}
	for value in "$tp" "$pp" "$dp"; do
		case $value in
			''|*[!0-9]*|0)
				echo "inference parallel sizes must be positive integers" >&2
				exit 1
				;;
		esac
	done
	if [ -n "${INFERENCE_GPU_CONFIG:-}" ]; then
		gpu_config=$INFERENCE_GPU_CONFIG
	else
		gpu_config=$test_dir/gpu.json
		sed -e 's/"runId": "tiny-public-inference-cpu"/"runId": "tiny-public-inference-gpu"/' \
			-e 's/"backend": "transformers"/"backend": "vllm"/' \
			-e 's/"dtype": "float32"/"dtype": "auto"/' \
			-e "s/\"tensorParallel\": 1/\"tensorParallel\": $tp/" \
			-e "s/\"pipelineParallel\": 1/\"pipelineParallel\": $pp/" \
			-e "s/\"dataParallel\": 1/\"dataParallel\": $dp/" \
			"$project_dir/simulation/inference/configs/tiny-public.json" \
			> "$gpu_config"
	fi
	mkdir -p "$test_dir/gpu-results"
	# The command string expands $server inside the container shell.
	# shellcheck disable=SC2016
	run_container --rm --device "$gpu_device" --shm-size=4g \
		--security-opt seccomp=unconfined \
		-e HF_HOME=/cache/huggingface \
		-v "$cache:/cache/huggingface" \
		-v "$test_dir/gpu-results:/results" \
		-v "$gpu_config:/config.json:ro" \
		--entrypoint /bin/sh "$image" -c '
			set -eu
			inference serve --config /config.json &
			server=$!
			cleanup() { kill "$server" >/dev/null 2>&1 || true; }
			trap cleanup EXIT INT TERM
			inference probe --config /config.json
			vllm bench serve --backend openai \
				--base-url http://127.0.0.1:8000 \
				--model smollm2-135m \
				--tokenizer /cache/huggingface/hub/models--HuggingFaceTB--SmolLM2-135M-Instruct/snapshots/12fd25f77366fa6b3b4b768ec3050bf629380bac \
				--dataset-name random --num-prompts 4 \
				--random-input-len 8 --random-output-len 4 \
				--max-concurrency 2 --temperature 0 --seed 20260824 \
				--save-result --result-dir /results \
				--result-filename standard-vllm-benchmark.json
			kill "$server" >/dev/null 2>&1 || true
			wait "$server" || true
			trap - EXIT INT TERM
		'
	python3 - "$gpu_config" "$test_dir/gpu-results" "$tp" "$pp" "$dp" <<'PY'
import json
import pathlib
import sys

config = json.load(open(sys.argv[1]))
root = pathlib.Path(sys.argv[2]) / config["runId"]
result = json.loads((root / "result.json").read_text())
assert result["status"] == "passed"
assert result["backend"] == "vllm"
assert result["stack"]["cudaAvailable"] is True
assert result["stack"]["vllm"] == "0.27.1"
assert result["parallelism"]["tensorParallel"] == int(sys.argv[3])
assert result["parallelism"]["pipelineParallel"] == int(sys.argv[4])
assert result["parallelism"]["dataParallel"] == int(sys.argv[5])
benchmark = json.loads(
	(pathlib.Path(sys.argv[2]) / "standard-vllm-benchmark.json").read_text()
)
assert benchmark["completed"] == 4
PY
fi

echo "inference image smoke test: PASS"
