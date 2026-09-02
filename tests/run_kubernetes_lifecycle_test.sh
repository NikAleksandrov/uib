#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/training-smoke.yaml
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kubernetes-lifecycle.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

mkdir -p "$test_dir/state" "$test_dir/results" "$test_dir/log"
: > "$test_dir/kubeconfig"

cat > "$test_dir/kubectl" <<'EOF'
#!/usr/bin/env python3
import json
import os
import pathlib
import sys


arguments = sys.argv[1:]
log = pathlib.Path(os.environ["TEST_LOG"])
with log.open("a", encoding="utf-8") as output:
    output.write(json.dumps(arguments) + "\n")

command = next((item for item in arguments
                if item in {"apply", "delete", "get", "logs", "version"}),
               None)
if command == "apply":
    print("namespace/uib-kubernetes created")
    print("trainjob.trainer.kubeflow.org/uib-kubernetes created")
elif command == "delete":
    print("namespace uib-kubernetes deleted")
elif command == "logs":
    container = arguments[arguments.index("--container") + 1]
    if container == "uib-runtime-check":
        print("UIB runtime preflight: PASS")
    else:
        print(json.dumps({
            "schemaVersion": 1,
            "runId": "uib-kubernetes",
            "status": "passed",
            "worldSize": 2,
            "device": "cpu",
            "strategy": "ddp",
            "completedSteps": 4,
            "resumedFromStep": 0,
            "durationSeconds": 2.0,
            "samplesPerSecond": 8.0,
            "loss": {"first": 4.0, "last": 2.0},
            "transport": {"distributedBackend": "gloo"},
        }))
elif command == "get" and "trainjob" in arguments:
    state = os.environ.get("KUBE_TEST_STATE", "complete")
    condition = ("Complete" if state == "complete" else "Running")
    print(json.dumps({
        "apiVersion": "trainer.kubeflow.org/v1alpha1",
        "kind": "TrainJob",
        "metadata": {"name": "uib-kubernetes"},
        "status": {"conditions": [{"type": condition, "status": "True"}]},
    }))
elif command == "get" and "pods" in arguments:
    items = []
    for index in (1, 2):
        items.append({
            "metadata": {"name": f"uib-kubernetes-node-{index}"},
            "spec": {
                "nodeName": f"worker-{index:02d}",
                "initContainers": [{"name": "uib-runtime-check"}],
                "containers": [{"name": "node"}],
            },
            "status": {"phase": "Succeeded"},
        })
    print(json.dumps({"apiVersion": "v1", "kind": "List", "items": items}))
elif command == "get" and "namespace" in arguments:
    print("apiVersion: v1\nkind: Namespace\nmetadata:\n  name: uib-kubernetes")
elif command == "get" and "events" in arguments:
    print("apiVersion: v1\nkind: EventList\nitems: []")
elif command == "get" and "nodes" in arguments:
    items = []
    for index in (1, 2):
        items.append({
            "metadata": {"name": f"worker-{index:02d}"},
            "status": {"nodeInfo": {
                "architecture": "amd64",
                "operatingSystem": "linux",
                "osImage": "Test Linux",
                "kernelVersion": "6.12.0-test",
                "containerRuntimeVersion": "containerd://2.1.0",
                "kubeletVersion": "v1.35.0",
            }},
        })
    print(json.dumps({"apiVersion": "v1", "kind": "NodeList", "items": items}))
elif command == "version":
    print(json.dumps({
        "clientVersion": {"gitVersion": "v1.35.0"},
        "serverVersion": {"gitVersion": "v1.35.0"},
    }))
else:
    print("unsupported fake kubectl invocation: " + repr(arguments),
          file=sys.stderr)
    sys.exit(2)
EOF
chmod +x "$test_dir/kubectl"

common_args="--profile real-kubernetes --set infrastructure.kubernetesContext=test-context"
run_id=uib-kubernetes
TEST_LOG=$test_dir/log/kubectl.jsonl KUBECTL_COMMAND=$test_dir/kubectl \
	$cli run $common_args --request "$request" --run-id "$run_id" \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig" \
	--allow-mutable-tags > "$test_dir/run.out"
grep -Fx "trainjob/$run_id" "$test_dir/run.out" > /dev/null

run_dir=$test_dir/state/$run_id
[ -f "$run_dir/workload.yaml" ]
[ -f "$run_dir/resolved-simulation.json" ]
python3 - "$run_dir/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["scheduler"] == "kubernetes"
assert ledger["state"] == "submitted"
assert ledger["namespace"] == "uib-kubernetes"
assert ledger["resourceKind"] == "trainjob"
assert ledger["inputs"]["request"] == "inputs/request.yaml"
PY

TEST_LOG=$test_dir/log/kubectl.jsonl KUBECTL_COMMAND=$test_dir/kubectl \
	$cli status $common_args --request "$request" --run-id "$run_id" \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig" \
	> "$test_dir/status.json"
python3 - "$test_dir/status.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    status = json.load(source)
assert status["state"] == "COMPLETED"
assert status["terminal"] is True
assert status["podPhases"] == {"Succeeded": 2}
PY

bundle=$test_dir/results/$run_id
reported=$(TEST_LOG=$test_dir/log/kubectl.jsonl \
	KUBECTL_COMMAND=$test_dir/kubectl \
	$cli collect --run-id "$run_id" --state-dir "$test_dir/state" \
	--output-dir "$bundle" --kubeconfig "$test_dir/kubeconfig")
[ "$reported" = "$bundle" ]
[ -f "$bundle/scheduler/workload.yaml" ]
[ -f "$bundle/scheduler/workload.json" ]
[ -f "$bundle/scheduler/pods.json" ]
[ -f "$bundle/scheduler/events.yaml" ]
[ -f "$bundle/logs/uib-kubernetes-node-1.uib-runtime-check.log" ]
[ -f "$bundle/manifest.json" ]
python3 - "$bundle" <<'PY'
import hashlib
import json
import pathlib
import sys


root = pathlib.Path(sys.argv[1])
with (root / "summary.json").open(encoding="utf-8") as source:
    summary = json.load(source)
assert summary["scheduler"] == "kubernetes"
assert summary["schedulerState"] == "COMPLETED"
assert summary["completeSchedulerSnapshot"] is True
assert summary["uibPreflightVerified"] is True
assert summary["uibTrafficEvidence"] == "not-exercised-non-rdma-backend"
assert summary["uibCounterEvidence"] == "insufficient"
assert summary["providerCounters"] == "provider-counters.json"
assert summary["normalizedResult"] == "normalized-result.json"
assert summary["promotionEligible"] is False
with (root / "normalized-result.json").open(encoding="utf-8") as source:
    normalized = json.load(source)
assert normalized["scheduler"] == {
    "kind": "kubernetes", "state": "COMPLETED", "terminal": True}
assert normalized["workload"]["requestedRanks"] == 2
assert normalized["communication"]["protocol"] == "rocev2"
assert normalized["evidence"]["uibCounters"] == "insufficient"
with (root / "transport-evidence.json").open(encoding="utf-8") as source:
    transport = json.load(source)
assert transport["classification"] == "not-exercised-non-rdma-backend"
assert transport["requestedRanks"] == 2
with (root / "provider-counters.json").open(encoding="utf-8") as source:
    counters = json.load(source)
assert counters["classification"] == "insufficient"
assert counters["recordCount"] == 0
with (root / "metrics" / "workload.jsonl").open(encoding="utf-8") as source:
    metrics = [json.loads(line) for line in source]
assert len(metrics) == 1
assert metrics[0]["kind"] == "training"
assert metrics[0]["samplesPerSecond"] == 8.0
assert len(metrics[0]["sourceFiles"]) == 2
assert "sourceFiles" not in normalized["workload"]["results"][0]
assert summary["workloadMetricRecords"] == 1
with (root / "host-probes.json").open(encoding="utf-8") as source:
    probes = json.load(source)
assert summary["hostProbeNodes"] == 2
assert [node["name"] for node in probes["nodes"]] == [
    "worker-01", "worker-02"]
assert probes["nodes"][0]["kernelVersion"] == "6.12.0-test"
assert probes["nodes"][0]["containerRuntimeVersion"] == "containerd://2.1.0"
assert probes["versions"]["serverVersion"]["gitVersion"] == "v1.35.0"
with (root / "preflight.json").open(encoding="utf-8") as source:
    preflight = json.load(source)
assert preflight["expectedPods"] == 2
assert preflight["recordedPods"] == 2
assert preflight["passedPods"] == 2
with (root / "topology.json").open(encoding="utf-8") as source:
    topology = json.load(source)
assert topology["allocatedNodes"] == ["worker-01", "worker-02"]
with (root / "manifest.json").open(encoding="utf-8") as source:
    manifest = json.load(source)
for item in manifest["files"]:
    content = (root / item["path"]).read_bytes()
    assert len(content) == item["size"]
    assert hashlib.sha256(content).hexdigest() == item["sha256"]
PY

if TEST_LOG=$test_dir/log/kubectl.jsonl KUBECTL_COMMAND=$test_dir/kubectl \
	KUBE_TEST_STATE=pending \
	$cli collect --run-id "$run_id" --state-dir "$test_dir/state" \
	--output-dir "$test_dir/results/pending-rejected" \
	--kubeconfig "$test_dir/kubeconfig" > /dev/null 2>&1; then
	echo "a nonterminal Kubernetes result was collected without opt-in" >&2
	exit 1
fi
TEST_LOG=$test_dir/log/kubectl.jsonl KUBECTL_COMMAND=$test_dir/kubectl \
	KUBE_TEST_STATE=pending \
	$cli collect --run-id "$run_id" --state-dir "$test_dir/state" \
	--output-dir "$test_dir/results/pending" --allow-incomplete \
	--kubeconfig "$test_dir/kubeconfig" > /dev/null

TEST_LOG=$test_dir/log/kubectl.jsonl KUBECTL_COMMAND=$test_dir/kubectl \
	$cli down $common_args --run-id "$run_id" \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig"
python3 - "$run_dir/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "cleanup-requested"
assert "cleanupRequestedAt" in ledger
PY

echo "Kubernetes workload lifecycle and collection test: PASS"
