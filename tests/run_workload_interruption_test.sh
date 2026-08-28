#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/training-smoke.yaml
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-workload-interruption.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT HUP INT TERM
mkdir -p "$test_dir/state" "$test_dir/results" "$test_dir/shared/runtime" \
	"$test_dir/shared/cache" "$test_dir/shared/results"
: > "$test_dir/kubeconfig"

wait_for_file()
{
	file=$1
	count=0
	while [ ! -f "$file" ]; do
		count=$((count + 1))
		[ "$count" -lt 200 ] || exit 1
		sleep 0.05
	done
}

interrupt()
{
	pid=$1
	kill -TERM "$pid"
	set +e
	wait "$pid"
	status=$?
	set -e
	[ "$status" -eq 143 ] || {
		echo "interrupted command exited with $status" >&2
		exit 1
	}
}

cat > "$test_dir/kubectl" <<'PY'
#!/usr/bin/env python3
import json
import os
import pathlib
import signal
import sys


arguments = sys.argv[1:]
mode = os.environ.get("KUBE_INTERRUPT_MODE", "normal")
root = pathlib.Path(os.environ["INTERRUPT_ROOT"])
command = next((item for item in arguments
                if item in {"apply", "delete", "get", "logs", "version"}),
               None)

if command == "apply":
    if mode == "apply":
        (root / "apply-started").touch()
        signal.pause()
    print("namespace/uib-workload created")
elif command == "delete":
    with (root / "deletes.log").open("a", encoding="utf-8") as output:
        output.write(" ".join(arguments) + "\n")
    if mode == "delete":
        (root / "delete-started").touch()
        signal.pause()
    print("namespace deleted")
elif command == "get" and "trainjob" in arguments:
    print(json.dumps({
        "apiVersion": "trainer.kubeflow.org/v1alpha1",
        "kind": "TrainJob",
        "metadata": {"name": arguments[arguments.index("trainjob") + 1]},
        "status": {"conditions": [{"type": "Complete", "status": "True"}]},
    }))
elif command == "get" and "pods" in arguments:
    print(json.dumps({
        "apiVersion": "v1", "kind": "List", "items": [{
            "metadata": {"name": "uib-workload-node-1"},
            "spec": {
                "nodeName": "worker-01",
                "initContainers": [{"name": "uib-runtime-check"}],
                "containers": [{"name": "node"}],
            },
            "status": {"phase": "Succeeded"},
        }, {
            "metadata": {"name": "uib-workload-node-2"},
            "spec": {
                "nodeName": "worker-02",
                "initContainers": [{"name": "uib-runtime-check"}],
                "containers": [{"name": "node"}],
            },
            "status": {"phase": "Succeeded"},
        }],
    }))
elif command == "get" and "namespace" in arguments:
    print("apiVersion: v1\nkind: Namespace")
elif command == "get" and "events" in arguments:
    print("apiVersion: v1\nkind: EventList\nitems: []")
elif command == "get" and "nodes" in arguments:
    print(json.dumps({"apiVersion": "v1", "kind": "NodeList", "items": []}))
elif command == "version":
    print(json.dumps({"clientVersion": {}, "serverVersion": {}}))
elif command == "logs":
    if mode == "logs":
        (root / "logs-started").touch()
        signal.pause()
    container = arguments[arguments.index("--container") + 1]
    if container == "uib-runtime-check":
        print("UIB runtime preflight: PASS")
    else:
        print(json.dumps({
            "schemaVersion": 1,
            "runId": "uib-workload",
            "status": "passed",
            "worldSize": 2,
            "device": "cpu",
            "strategy": "ddp",
            "completedSteps": 1,
            "resumedFromStep": 0,
            "durationSeconds": 1.0,
            "samplesPerSecond": 1.0,
            "loss": {"first": 1.0, "last": 1.0},
            "transport": {"distributedBackend": "gloo"},
        }))
else:
    print("unsupported invocation", arguments, file=sys.stderr)
    sys.exit(2)
PY
chmod +x "$test_dir/kubectl"

common="--profile real-kubernetes --request $request --allow-mutable-tags --set infrastructure.kubernetesContext=test-context"
INTERRUPT_ROOT=$test_dir KUBE_INTERRUPT_MODE=apply \
KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" run $common --run-id interrupt-apply \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig" \
	> "$test_dir/apply.out" 2> "$test_dir/apply.err" &
pid=$!
wait_for_file "$test_dir/apply-started"
interrupt "$pid"
python3 - "$test_dir/state/interrupt-apply/ledger.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "interrupted"
assert ledger["interruptedStage"] == "submission"
assert ledger["cleanup"] == "requested"
PY
grep -F 'delete namespace interrupt-apply --ignore-not-found --wait=false' \
	"$test_dir/deletes.log" > /dev/null

INTERRUPT_ROOT=$test_dir KUBE_INTERRUPT_MODE=normal \
KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" run $common --run-id uib-workload \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig" \
	> /dev/null

bundle=$test_dir/results/uib-workload
INTERRUPT_ROOT=$test_dir KUBE_INTERRUPT_MODE=logs \
KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" collect --run-id uib-workload --state-dir "$test_dir/state" \
	--output-dir "$bundle" --kubeconfig "$test_dir/kubeconfig" \
	> "$test_dir/collect.out" 2> "$test_dir/collect.err" &
pid=$!
wait_for_file "$test_dir/logs-started"
interrupt "$pid"
[ ! -e "$bundle" ]
if find "$test_dir/results" -maxdepth 1 -name '.uib-workload.*' | grep .; then
	echo "interrupted collection retained a temporary bundle" >&2
	exit 1
fi
INTERRUPT_ROOT=$test_dir KUBE_INTERRUPT_MODE=normal \
KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" collect --run-id uib-workload --state-dir "$test_dir/state" \
	--output-dir "$bundle" --kubeconfig "$test_dir/kubeconfig" > /dev/null
[ -f "$bundle/manifest.json" ]

INTERRUPT_ROOT=$test_dir KUBE_INTERRUPT_MODE=delete \
KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" down --profile real-kubernetes --run-id uib-workload \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig" \
	--set infrastructure.kubernetesContext=test-context \
	> "$test_dir/down.out" 2> "$test_dir/down.err" &
pid=$!
wait_for_file "$test_dir/delete-started"
interrupt "$pid"
python3 - "$test_dir/state/uib-workload/ledger.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    assert json.load(source)["state"] == "submitted"
PY
INTERRUPT_ROOT=$test_dir KUBE_INTERRUPT_MODE=normal \
KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" down --profile real-kubernetes --run-id uib-workload \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig" \
	--set infrastructure.kubernetesContext=test-context

cat > "$test_dir/sbatch" <<'EOF'
#!/bin/sh
: > "$INTERRUPT_ROOT/sbatch-started"
trap 'exit 143' INT TERM
while :; do sleep 1; done
EOF
chmod +x "$test_dir/sbatch"
INTERRUPT_ROOT=$test_dir SBATCH_COMMAND=$test_dir/sbatch \
	"$cli" run --profile real-slurm --request "$request" \
	--run-id interrupt-slurm --state-dir "$test_dir/state" \
	--allow-mutable-tags --set uib.enabled=false \
	--set "storage.modelCachePath=$test_dir/shared/cache" \
	--set "storage.resultsPath=$test_dir/shared/results" \
	> "$test_dir/slurm.out" 2> "$test_dir/slurm.err" &
pid=$!
wait_for_file "$test_dir/sbatch-started"
interrupt "$pid"
python3 - "$test_dir/state/interrupt-slurm/ledger.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "submission-unknown"
assert "jobId" not in ledger
assert "SIGTERM" in ledger["error"]
PY

echo "workload lifecycle interruption test: PASS"
