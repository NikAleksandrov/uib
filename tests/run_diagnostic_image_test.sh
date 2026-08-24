#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-diagnostic-test.XXXXXX")
image=${DIAGNOSTIC_IMAGE:-uib-diagnostics:test}
engine=${DIAGNOSTIC_CONTAINER_ENGINE:-docker}
cache=${DIAGNOSTIC_DOWNLOAD_CACHE:-$project_dir/simulation/.artifacts/downloads}
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

container()
{
	$engine "$@"
}

run_container()
{
	container run --user "$(id -u):$(id -g)" \
		-e HOME=/tmp -e USER=uib -e LOGNAME=uib "$@"
}

if [ "${DIAGNOSTIC_SKIP_BUILD:-0}" != 1 ]; then
	DIAGNOSTIC_DOWNLOAD_CACHE=$cache \
		"$project_dir/simulation/diagnostics/fetch-sources"
	container build --progress=plain \
		--build-context "diagnostic-sources=$cache" \
		-f "$project_dir/simulation/diagnostics/Containerfile" \
		-t "$image" "$project_dir/simulation/diagnostics"
fi

run_container --rm "$image" capabilities > "$test_dir/capabilities.json"
python3 - "$test_dir/capabilities.json" <<'PY'
import json
import sys

capabilities = json.load(open(sys.argv[1]))
assert capabilities["schemaVersion"] == 1
assert capabilities["stack"]["cuda"] == "13.2.0"
assert capabilities["stack"]["nccl"].startswith("2.30.4-")
assert all(capabilities["tools"].values())
PY
run_container --rm "$image" preflight

if [ -n "${DIAGNOSTIC_UIB_RUNTIME:-}" ]; then
	run_container --rm -v "$DIAGNOSTIC_UIB_RUNTIME:/opt/uib:ro" \
		"$image" preflight --uib-library-only
fi

if [ "${DIAGNOSTIC_GPU_TEST:-0}" = 1 ]; then
	gpu_device=${DIAGNOSTIC_GPU_DEVICE:-nvidia.com/gpu=all}
	if ! run_container --rm --device "$gpu_device" \
		--security-opt seccomp=unconfined \
		"$image" preflight --require-gpu > "$test_dir/gpu.log" 2>&1; then
		cat "$test_dir/gpu.log" >&2
		exit 1
	fi
	cat "$test_dir/gpu.log"
	grep -q 'Result = PASS' "$test_dir/gpu.log"
	grep -q 'Out of bounds values : 0 OK' "$test_dir/gpu.log"
fi

echo "diagnostic image smoke test: PASS"
