#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-sim-cli-test.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

for profile in "$project_dir"/simulation/profiles/*.yaml; do
	[ "$(basename "$profile")" != defaults.yaml ] || continue
	"$cli" render --profile "$profile" --format json \
		> "$test_dir/resolved.json"
	python3 - "$test_dir/resolved.json" <<'PY'
import json
import sys

config = json.load(open(sys.argv[1], encoding="utf-8"))
assert config["communication"]["fallbackPolicy"] == "fail"
assert config["uib"]["congestionControl"]["abi"] == 1
PY
done

for frontend in nccl ucx libfabric; do
	"$cli" render --profile kind-kubernetes \
		--overlay "communication/$frontend" --format json \
		> "$test_dir/frontend.json"
	python3 - "$test_dir/frontend.json" "$frontend" <<'PY'
import json
import sys

config = json.load(open(sys.argv[1], encoding="utf-8"))
frontend = sys.argv[2]
assert config["communication"]["frontend"] == frontend
assert frontend in config["communication"]
assert all(name == frontend or name not in config["communication"]
           for name in ("nccl", "rccl", "ucx", "libfabric"))
PY
done

"$cli" render --profile real-kubernetes --overlay communication/rccl \
	--set nodes.gpus.vendor=amd --format json > "$test_dir/rccl.json"
"$cli" render --profile kind-kubernetes --overlay protocol/uet-nscc \
	--format json > "$test_dir/uet-overlay.json"

printf 'unknownTopLevel: true\n' > "$test_dir/unknown.yaml"
if "$cli" render --profile kind-kubernetes \
	--request "$test_dir/unknown.yaml" > /dev/null 2>&1; then
	echo "unknown configuration key was accepted" >&2
	exit 1
fi

printf '%s\n' \
	'workload:' \
	'  kind: training' \
	'  image: example.invalid/training@sha256:deadbeef' \
	'  entrypoint: /opt/workload/train' \
	'  parallelism:' \
	'    nodes: 2' \
	'    processesPerNode: 2' \
	'    data: 1' \
	'    tensor: 1' \
	'    pipeline: 1' \
	'    expert: 1' > "$test_dir/bad-parallelism.yaml"
if "$cli" render --profile kind-kubernetes \
	--request "$test_dir/bad-parallelism.yaml" > /dev/null 2>&1; then
	echo "impossible parallelism was accepted" >&2
	exit 1
fi

if "$cli" render --profile kind-kubernetes \
	--set uib.congestionControl.algorithm=nscc > /dev/null 2>&1; then
	echo "unimplemented RoCEv2 congestion control was accepted" >&2
	exit 1
fi
"$cli" render --profile kind-kubernetes --format json \
	--set uib.protocol=uet \
	--set uib.congestionControl.algorithm=nscc \
	--set uib.congestionControl.parameters.maxWindowBytes=262144 \
	> "$test_dir/uet-nscc.json"

if "$cli" render --profile libvirt-slurm \
	--set nodes.memory=512Mi > /dev/null 2>&1; then
	echo "libvirt accepted no memory above its system reserve" >&2
	exit 1
fi
if "$cli" render --profile libvirt-slurm \
	--set nodes.memory=4Gi --set slurm.memory=4Gi \
	> /dev/null 2>&1; then
	echo "Slurm accepted more memory than a libvirt worker advertises" >&2
	exit 1
fi
"$cli" render --profile libvirt-slurm \
	--set nodes.memory=4Gi --set slurm.memory=3584Mi \
	> "$test_dir/libvirt-memory.yaml"

set +e
"$cli" doctor --profile kind-kubernetes > "$test_dir/doctor.json"
doctor_status=$?
set -e
if [ "$doctor_status" -gt 1 ]; then
	echo "doctor failed to produce a capability report" >&2
	exit 1
fi
python3 - "$test_dir/doctor.json" <<'PY'
import json
import sys

report = json.load(open(sys.argv[1], encoding="utf-8"))
assert report["readOnly"] is True
assert isinstance(report["missing"], list)
assert report["provider"] == "kind"
PY

echo "simulation CLI test: PASS"
