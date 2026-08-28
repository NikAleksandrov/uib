#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-sim-cli-test.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

sh -n "$project_dir/simulation/runtime/with-device-anchor"
DEVICE_ANCHOR_PATH=/dev/null \
	"$project_dir/simulation/runtime/with-device-anchor" true
if DEVICE_ANCHOR_SOURCE="$test_dir/missing-device" \
	DEVICE_ANCHOR_PATH=/dev/null \
	"$project_dir/simulation/runtime/with-device-anchor" true \
	> /dev/null 2>&1; then
	echo "a non-character device anchor source was accepted" >&2
	exit 1
fi

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
"$cli" render --profile virtme-kubernetes --overlay network/asymmetric \
	--format json > "$test_dir/asymmetric.json"
python3 - "$test_dir/asymmetric.json" <<'PY'
import json
import sys

config = json.load(open(sys.argv[1], encoding="utf-8"))
assert config["network"]["shaping"]["latency"] == "250us"
assert config["network"]["endpointShaping"]["worker-01/data"]["latency"] == \
    "100us"
assert config["network"]["endpointShaping"]["worker-02/data"]["bandwidth"] == \
    "1Gbit"
PY
if "$cli" render --profile virtme-kubernetes \
	--set 'network.endpointShaping={worker-03/data: {enabled: true, latency: 1ms}}' \
	> /dev/null 2>&1; then
	echo "endpoint shaping accepted a nonexistent worker" >&2
	exit 1
fi
if "$cli" render --profile virtme-kubernetes \
	--set 'network.endpointShaping={worker-01/data: {enabled: true, jitter: 1ms}}' \
	> /dev/null 2>&1; then
	echo "endpoint shaping bypassed impairment validation" >&2
	exit 1
fi
if "$cli" render --profile kind-kubernetes --overlay network/asymmetric \
	> /dev/null 2>&1; then
	echo "Kind accepted unimplemented per-worker shaping" >&2
	exit 1
fi
"$cli" render --profile virtme-kubernetes --overlay network/asymmetric \
	--overlay network/clean --format json > "$test_dir/replaced-network.json"
python3 - "$test_dir/replaced-network.json" <<'PY'
import json
import sys

config = json.load(open(sys.argv[1], encoding="utf-8"))
assert config["network"]["shaping"] == {"enabled": False}
assert "endpointShaping" not in config["network"]
PY
"$cli" render --profile virtme-kubernetes --overlay network/minimum-mtu \
	--overlay network/datacenter --format json > "$test_dir/minimum-mtu.json"
python3 - "$test_dir/minimum-mtu.json" <<'PY'
import json
import sys

config = json.load(open(sys.argv[1], encoding="utf-8"))
assert config["network"]["mtu"] == 576
assert config["network"]["shaping"]["enabled"] is True
PY
"$cli" render --profile virtme-kubernetes --overlay network/jumbo \
	--overlay network/minimum-mtu --format json > "$test_dir/replaced-mtu.json"
python3 - "$test_dir/replaced-mtu.json" <<'PY'
import json
import sys

config = json.load(open(sys.argv[1], encoding="utf-8"))
assert config["network"]["mtu"] == 576
PY

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

"$cli" render --profile kind-kubernetes --format json \
	--overlay protocol/rocev2-nscc-experimental \
	--set uib.congestionControl.parameters.maxWindowBytes=262144 \
	> "$test_dir/rocev2-nscc.json"
"$cli" render --profile kind-kubernetes --format json \
	--set uib.protocol=uet \
	--set uib.congestionControl.algorithm=nscc \
	--set uib.congestionControl.parameters.maxWindowBytes=262144 \
	> "$test_dir/uet-nscc.json"
if "$cli" render --profile kind-kubernetes \
	--set uib.congestionControl.parameters.maxWindowBytes=65536 \
	> /dev/null 2>&1; then
	echo "NSCC parameters with the none algorithm were accepted" >&2
	exit 1
fi
if "$cli" render --profile kind-kubernetes \
	--overlay protocol/rocev2-nscc-experimental \
	--set uib.congestionControl.parameters.unknown=1 \
	> /dev/null 2>&1; then
	echo "an unknown NSCC parameter was accepted" >&2
	exit 1
fi
"$cli" render --profile kind-kubernetes --format json \
	--overlay protocol/rocev2-dcqcn \
	--set uib.congestionControl.parameters.minimumRateBytesPerSecond=25000000 \
	--set uib.congestionControl.parameters.initialRateBytesPerSecond=1000000000 \
	--set uib.congestionControl.parameters.maximumRateBytesPerSecond=2000000000 \
	--set uib.congestionControl.parameters.alphaGainQ16=512 \
	> "$test_dir/rocev2-dcqcn.json"
"$cli" render --profile kind-kubernetes --format json \
	--overlay protocol/uet-rccc \
	--set uib.congestionControl.parameters.maxWindowBytes=262144 \
	--set uib.congestionControl.parameters.creditQuantumBytes=16384 \
	--set uib.congestionControl.parameters.linkRateBytesPerSecond=50000000000 \
	--set uib.congestionControl.parameters.baseRttNs=20000 \
	> "$test_dir/uet-rccc.json"
"$cli" render --profile kind-kubernetes --format json \
	--overlay protocol/uet-dcqcn-experimental \
	> "$test_dir/uet-dcqcn-experimental.json"
if "$cli" render --profile kind-kubernetes \
	--overlay protocol/rocev2-dcqcn \
	--set uib.congestionControl.parameters.minimumRateBytesPerSecond=200 \
	--set uib.congestionControl.parameters.initialRateBytesPerSecond=100 \
	> /dev/null 2>&1; then
	echo "invalid DCQCN rate ordering was accepted" >&2
	exit 1
fi
if "$cli" render --profile kind-kubernetes \
	--set uib.protocol=rocev2 \
	--set uib.congestionControl.algorithm=rccc > /dev/null 2>&1; then
	echo "RCCC with RoCEv2 was accepted" >&2
	exit 1
fi
if "$cli" render --profile kind-kubernetes \
	--set uib.protocol=uet \
	--set uib.congestionControl.algorithm=dcqcn > /dev/null 2>&1; then
	echo "DCQCN with UET was accepted without its experimental gate" >&2
	exit 1
fi

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
if "$cli" render --profile libvirt-slurm \
	--set libvirt.sshPortBase=65534 > /dev/null 2>&1; then
	echo "libvirt accepted an overflowing SSH port range" >&2
	exit 1
fi
if "$cli" render --profile libvirt-kubernetes \
	--set cluster.version=v1.35 > /dev/null 2>&1; then
	echo "libvirt Kubernetes accepted an unpinned k3s version" >&2
	exit 1
fi
if "$cli" render --profile libvirt-kubernetes \
	--set kubernetes.podCIDR=198.51.100.0/24 > /dev/null 2>&1; then
	echo "libvirt Kubernetes accepted overlapping pod and data networks" >&2
	exit 1
fi
if "$cli" render --profile libvirt-kubernetes \
	--set libvirt.uri=qemu:///session \
	--set libvirt.networkMode=rootless > /dev/null 2>&1; then
	echo "libvirt Kubernetes accepted isolated rootless management networks" >&2
	exit 1
fi

printf '%s\n' \
	'faults:' \
	'  - at: 10s' \
	'    action: add-delay' \
	'    target: worker-01/data' \
	'    latency: 2ms' \
	'    duration: 30s' \
	'  - afterEvent: checkpoint-complete' \
	'    action: terminate-rank' \
	'    rank: 1' > "$test_dir/valid-faults.yaml"
"$cli" render --profile kind-kubernetes \
	--request "$test_dir/valid-faults.yaml" --format json \
	> "$test_dir/valid-faults.json"

for invalid_fault in \
	'[{at: 1s, action: add-delay, target: worker-01/data, duration: 1s}]' \
	'[{at: 1s, action: packet-loss, target: worker-01/data, percent: 0, duration: 1s}]' \
	'[{at: 1s, action: link-down, target: worker-01/data, duration: 0s}]' \
	'[{at: 1s, action: partition, target: worker-01/data, duration: 1s, percent: 100}]' \
	'[{at: 1s, action: terminate-rank, rank: 1, target: worker-01/data}]'; do
	if "$cli" render --profile kind-kubernetes \
		--set "faults=$invalid_fault" > /dev/null 2>&1; then
		echo "invalid fault schedule was accepted: $invalid_fault" >&2
		exit 1
	fi
done

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
