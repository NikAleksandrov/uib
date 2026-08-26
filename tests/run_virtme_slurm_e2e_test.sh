#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
run_id=uib-vslurm-$$
state_dir=${SIMULATION_STATE_DIR:-$project_dir/simulation/.artifacts/runs}
profile_args='--set nodes.cpu=2 --set nodes.memory=2Gi'
created=false

cleanup()
{
	if test "$created" = true; then
		# shellcheck disable=SC2086
		"$cli" down --profile virtme-slurm --run-id "$run_id" \
			--state-dir "$state_dir" $profile_args || true
	fi
}
trap cleanup EXIT INT TERM

created=true
# shellcheck disable=SC2086
"$cli" up --profile virtme-slurm --run-id "$run_id" \
	--state-dir "$state_dir" $profile_args
# shellcheck disable=SC2086
"$cli" status --profile virtme-slurm --run-id "$run_id" \
	--state-dir "$state_dir" $profile_args > "$state_dir/$run_id/status.json"

python3 - "$state_dir/$run_id" <<'PY'
import json
import os
import sys


state = sys.argv[1]
with open(os.path.join(state, "status.json"), encoding="utf-8") as source:
    status = json.load(source)
assert status["state"] == "ready"
assert status["slurmController"] is True
assert status["liveGuests"] == status["guests"] == 3
infrastructure = os.path.join(state, "infrastructure")
with open(os.path.join(infrastructure, "ledger.json"), encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "ready"
assert ledger["clusterManager"] == "slurm"
assert ledger["slurmVersion"] == "24.11.5-4"
assert os.path.isfile(os.path.join(ledger["slurmRuntime"], "usr/bin/srun"))
with open(os.path.join(infrastructure, "inventory.json"),
          encoding="utf-8") as source:
    inventory = json.load(source)
assert len(inventory["all"]["children"]["slurm_workers"]["hosts"]) == 2
PY

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
for process in topology["ownership"]["processes"]:
    try:
        with open(f"/proc/{process['pid']}/stat", encoding="utf-8") as source:
            value = source.read()
    except OSError:
        continue
    fields = value.rpartition(") ")[2].split()
    assert len(fields) <= 19 or fields[19] != process["startTime"]
PY

echo "virtme Slurm end-to-end test: PASS"
