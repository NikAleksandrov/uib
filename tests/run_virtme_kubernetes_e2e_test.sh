#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
kubectl_command=${KUBECTL_COMMAND:-kubectl}
run_id=uib-vk8s-$$
state_dir=${SIMULATION_STATE_DIR:-$project_dir/simulation/.artifacts/runs}
profile_args='--set nodes.workers=1 --set nodes.cpu=2 --set nodes.memory=2Gi'
created=false

cleanup()
{
	if test "$created" = true; then
		# shellcheck disable=SC2086
		"$cli" down --profile virtme-kubernetes --run-id "$run_id" \
			--state-dir "$state_dir" $profile_args || true
	fi
}
trap cleanup EXIT INT TERM

command -v "$kubectl_command" > /dev/null
created=true
# shellcheck disable=SC2086
"$cli" up --profile virtme-kubernetes --run-id "$run_id" \
	--state-dir "$state_dir" $profile_args

kubeconfig=$state_dir/$run_id/infrastructure/kubeconfig
nodes=$($kubectl_command --kubeconfig "$kubeconfig" get nodes \
	-o jsonpath='{range .items[*]}{.metadata.name}{"|"}{.status.conditions[?(@.type=="Ready")].status}{"\n"}{end}')
test "$(printf '%s\n' "$nodes" | sed '/^$/d' | wc -l)" -eq 2
if printf '%s\n' "$nodes" | grep -vq '|True$'; then
	printf '%s\n' "$nodes" >&2
	exit 1
fi

"$cli" status --profile virtme-kubernetes --run-id "$run_id" \
	--state-dir "$state_dir" $profile_args > /dev/null
cleanup
created=false

python3 - "$state_dir/$run_id/infrastructure" <<'PY'
import json
import os
import sys


state = sys.argv[1]
with open(os.path.join(state, "ledger.json"), encoding="utf-8") as source:
    assert json.load(source)["state"] == "destroyed"
with open(os.path.join(state, "topology.json"), encoding="utf-8") as source:
    topology = json.load(source)
assert len(topology["guests"]) == 2
assert topology["network"]["type"] == "qemu-socket"
assert topology["kubernetesApiPort"] > 1024
for process in topology["ownership"]["processes"]:
    try:
        with open(f"/proc/{process['pid']}/stat", encoding="utf-8") as source:
            value = source.read()
    except OSError:
        continue
    fields = value.rpartition(") ")[2].split()
    assert len(fields) <= 19 or fields[19] != process["startTime"]
PY

echo "virtme Kubernetes end-to-end test: PASS"
