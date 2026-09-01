#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/training-smoke.yaml
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kubernetes-fault.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

mkdir -p "$test_dir/state" "$test_dir/results"
: > "$test_dir/kubeconfig"

cat > "$test_dir/kubectl" <<'EOF'
#!/usr/bin/env python3
import json
import os
import pathlib
import sys


arguments = sys.argv[1:]
root = pathlib.Path(os.environ["KUBE_FAULT_ROOT"])
with (root / "commands.jsonl").open("a", encoding="utf-8") as output:
    output.write(json.dumps(arguments) + "\n")

command = next((value for value in arguments
                if value in {"api-resources", "apply", "cordon", "delete",
                             "get", "uncordon"}),
               None)
if command == "api-resources":
    print("iochaos.chaos-mesh.org")
elif command == "apply":
    if arguments[-2:] == ["-f", "-"]:
        value = json.load(sys.stdin)
        with (root / "chaos.json").open("w", encoding="utf-8") as output:
            json.dump(value, output)
    print("applied")
elif command == "cordon":
    (root / (arguments[-1] + ".cordoned")).write_text("yes\n", encoding="utf-8")
    print(f"node/{arguments[-1]} cordoned")
elif command == "uncordon":
    (root / (arguments[-1] + ".cordoned")).unlink(missing_ok=True)
    print(f"node/{arguments[-1]} uncordoned")
elif command == "delete":
    print("deleted")
elif command == "get" and "trainjob" in arguments:
    run_id = arguments[arguments.index("--namespace") + 1]
    print(json.dumps({
        "apiVersion": "trainer.kubeflow.org/v1alpha1",
        "kind": "TrainJob",
        "metadata": {"name": run_id},
        "status": {"conditions": [{"type": "Complete", "status": "True"}]},
    }))
elif command == "get" and "pods" in arguments:
    print(json.dumps({"items": [
        {
            "metadata": {"name": "workload-rank-0"},
            "spec": {
                "nodeName": "worker-01",
                "initContainers": [{"name": "uib-runtime-check"}],
                "containers": [{"name": "node"}],
            },
            "status": {"phase": "Succeeded"},
        },
        {
            "metadata": {"name": "workload-rank-1"},
            "spec": {
                "nodeName": "worker-02",
                "initContainers": [{"name": "uib-runtime-check"}],
                "containers": [{"name": "node"}],
            },
            "status": {"phase": "Succeeded"},
        },
    ]}))
else:
    print("unsupported invocation", arguments, file=sys.stderr)
    sys.exit(2)
EOF
chmod +x "$test_dir/kubectl"

common="--profile real-kubernetes --request $request --allow-mutable-tags --set infrastructure.kubernetesContext=test-context"
faults='[{"at":"0ms","action":"delay-worker","target":"worker-02","duration":"100ms"},{"at":"10ms","action":"checkpoint-storage-delay","target":"/results","latency":"5ms","duration":"60ms"},{"at":"20ms","action":"terminate-rank","rank":1}]'
run_id=uib-k8s-fault
KUBE_FAULT_ROOT=$test_dir KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" run $common --set "faults=$faults" --run-id "$run_id" \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig" \
	> /dev/null

python3 - "$test_dir/state/$run_id/ledger.json" <<'PY'
import json
import sys
import time


with open(sys.argv[1], encoding="utf-8") as source:
    process = json.load(source)["faultScheduler"]
deadline = time.monotonic() + 5
while time.monotonic() < deadline:
    try:
        with open(f"/proc/{process['pid']}/stat", encoding="utf-8") as source:
            fields = source.read().rpartition(") ")[2].split()
    except OSError:
        break
    if len(fields) <= 19 or fields[19] != process["startTime"]:
        break
    time.sleep(0.05)
else:
    raise SystemExit("Kubernetes fault scheduler did not complete")
PY

python3 - "$test_dir/state/$run_id/faults-kubernetes.jsonl" \
	"$test_dir/chaos.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    records = [json.loads(line) for line in source]
assert len(records) == 5
assert {(item["action"], item["phase"]) for item in records} == {
    ("delay-worker", "apply"),
    ("delay-worker", "recover"),
    ("checkpoint-storage-delay", "apply"),
    ("checkpoint-storage-delay", "recover"),
    ("terminate-rank", "apply"),
}
termination = next(item for item in records
                   if item["action"] == "terminate-rank")
assert termination["target"] == "rank-1"
assert termination["command"]["pod"] == "workload-rank-1"
with open(sys.argv[2], encoding="utf-8") as source:
    chaos = json.load(source)
assert chaos["kind"] == "IOChaos"
assert chaos["spec"]["volumePath"] == "/results"
assert chaos["spec"]["delay"] == "5ms"
PY
test ! -e "$test_dir/worker-02.cordoned"

KUBE_FAULT_ROOT=$test_dir KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" down --profile real-kubernetes --request "$request" \
	--set infrastructure.kubernetesContext=test-context \
	--set "faults=$faults" --run-id "$run_id" \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig"

interrupt_id=uib-k8s-cleanup
interrupt='[{"at":"0ms","action":"delay-worker","target":"worker-01","duration":"30s"}]'
KUBE_FAULT_ROOT=$test_dir KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" run $common --set "faults=$interrupt" --run-id "$interrupt_id" \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig" \
	> /dev/null
test -f "$test_dir/worker-01.cordoned"
KUBE_FAULT_ROOT=$test_dir KUBECTL_COMMAND=$test_dir/kubectl \
	"$cli" down --profile real-kubernetes --request "$request" \
	--set infrastructure.kubernetesContext=test-context \
	--set "faults=$interrupt" --run-id "$interrupt_id" \
	--state-dir "$test_dir/state" --kubeconfig "$test_dir/kubeconfig"
test ! -e "$test_dir/worker-01.cordoned"
grep -q '"phase": "cleanup"' \
	"$test_dir/state/$interrupt_id/faults-kubernetes.jsonl"

echo "Kubernetes scheduler fault test: PASS"
