#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/training-smoke.yaml
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-slurm-collect.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

mkdir -p "$test_dir/runtime" "$test_dir/cache" "$test_dir/results" \
	"$test_dir/state" "$test_dir/fake-bin" "$test_dir/log"

cat > "$test_dir/fake-bin/sbatch" <<'EOF'
#!/bin/sh
set -eu
echo '7654;test-cluster'
EOF
cat > "$test_dir/fake-bin/sacct" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$@" > "$TEST_LOG/sacct.args"
echo "${SACCT_ROW:-7654|COMPLETED|0:0|00:00:19|node-[01-02]}"
EOF
chmod +x "$test_dir/fake-bin/sbatch" "$test_dir/fake-bin/sacct"

TEST_LOG="$test_dir/log" \
SBATCH_COMMAND="$test_dir/fake-bin/sbatch" \
"$cli" run --profile libvirt-slurm --request "$request" \
	--overlay protocol/uet-nscc --run-id uib-collect \
	--state-dir "$test_dir/state" --allow-mutable-tags \
	--set "storage.uibRuntimePath=$test_dir/runtime" \
	--set "storage.modelCachePath=$test_dir/cache" \
	--set "storage.resultsPath=$test_dir/results" > /dev/null

run_results=$test_dir/results/uib-collect
mkdir "$run_results"
cat > "$run_results/status.json" <<'EOF'
{
  "runId": "uib-collect",
  "scheduler": "slurm",
  "jobId": "7654",
  "exitCode": 0
}
EOF
printf '%s\n' node-01 node-02 > "$run_results/nodes.txt"
printf '%s\n' 'JobId=7654 JobState=COMPLETED' > "$run_results/slurm-job.txt"
printf '%s\n' '{"correctness":"passed"}' > "$run_results/result.json"
printf '%s\n' 'UIB preflight passed' > "$test_dir/results/uib-collect-7654.out"
: > "$test_dir/results/uib-collect-7654.err"
printf '%s\n' 'must not be collected' > "$test_dir/results/unrelated.out"

bundle=$test_dir/bundles/uib-collect
reported=$(TEST_LOG="$test_dir/log" \
	SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	"$cli" collect --run-id uib-collect --state-dir "$test_dir/state" \
	--output-dir "$bundle")
[ "$reported" = "$bundle" ]
[ -f "$bundle/request.yaml" ]
[ -f "$bundle/profile.yaml" ]
[ -f "$bundle/overlays/00.yaml" ]
[ -f "$bundle/resolved-values.json" ]
[ -f "$bundle/compatibility-lock.json" ]
[ -f "$bundle/scheduler/workload.sbatch" ]
[ -f "$bundle/scheduler/sacct.psv" ]
[ -f "$bundle/logs/batch.out" ]
[ -f "$bundle/workload/result.json" ]
[ ! -e "$bundle/unrelated.out" ]
grep -Fx -- '--jobs=7654' "$test_dir/log/sacct.args" > /dev/null

python3 - "$bundle" <<'PY'
import hashlib
import json
import pathlib
import sys


root = pathlib.Path(sys.argv[1])
with (root / "summary.json").open(encoding="utf-8") as source:
    summary = json.load(source)
assert summary["runId"] == "uib-collect"
assert summary["schedulerState"] == "COMPLETED"
assert summary["completeSchedulerSnapshot"] is True
assert summary["workloadStatusRecorded"] is True
assert summary["workloadExitCode"] == 0
assert summary["uibEnabled"] is True
assert summary["protocol"] == "uet"
assert summary["congestionControl"]["algorithm"] == "nscc"
assert summary["validationLevel"] == "not-assigned"
assert summary["uibTrafficEvidence"] == "not-evaluated"
assert summary["promotionEligible"] is False
with (root / "topology.json").open(encoding="utf-8") as source:
    topology = json.load(source)
assert topology["allocatedNodes"] == ["node-01", "node-02"]
with (root / "request-provenance.json").open(encoding="utf-8") as source:
    provenance = json.load(source)
assert provenance["overrides"]
assert provenance["overlays"][0]["name"] == "protocol/uet-nscc"
with (root / "manifest.json").open(encoding="utf-8") as source:
    manifest = json.load(source)
paths = {item["path"] for item in manifest["files"]}
assert "manifest.json" not in paths
assert "workload/result.json" in paths
for item in manifest["files"]:
    data = (root / item["path"]).read_bytes()
    assert len(data) == item["size"]
    assert hashlib.sha256(data).hexdigest() == item["sha256"]
PY

if TEST_LOG="$test_dir/log" SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	"$cli" collect --run-id uib-collect --state-dir "$test_dir/state" \
	--output-dir "$bundle" > /dev/null 2>&1; then
	echo "an existing result bundle was overwritten" >&2
	exit 1
fi

if TEST_LOG="$test_dir/log" SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	SACCT_ROW='7654|RUNNING|0:0|00:00:03|node-01' \
	"$cli" collect --run-id uib-collect --state-dir "$test_dir/state" \
	--output-dir "$test_dir/running" > /dev/null 2>&1; then
	echo "a nonterminal result was collected without explicit opt-in" >&2
	exit 1
fi
TEST_LOG="$test_dir/log" SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	SACCT_ROW='7654|RUNNING|0:0|00:00:03|node-01' \
	"$cli" collect --run-id uib-collect --state-dir "$test_dir/state" \
	--output-dir "$test_dir/running" --allow-incomplete > /dev/null
python3 - "$test_dir/running/summary.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    summary = json.load(source)
assert summary["completeSchedulerSnapshot"] is False
assert summary["promotionEligible"] is False
PY

ln -s "$test_dir/results/unrelated.out" "$run_results/unsafe-link"
if TEST_LOG="$test_dir/log" SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	"$cli" collect --run-id uib-collect --state-dir "$test_dir/state" \
	--output-dir "$test_dir/unsafe" > /dev/null 2>&1; then
	echo "a symbolic link in workload results was followed" >&2
	exit 1
fi
[ ! -e "$test_dir/unsafe" ]
rm "$run_results/unsafe-link"

python3 - "$test_dir/state/uib-collect/ledger.json" <<'PY'
import json
import sys


path = sys.argv[1]
with open(path, encoding="utf-8") as source:
    ledger = json.load(source)
ledger["batchFile"] = "../../outside.sbatch"
with open(path, "w", encoding="utf-8") as output:
    json.dump(ledger, output)
PY
printf '%s\n' '# external file' > "$test_dir/outside.sbatch"
if TEST_LOG="$test_dir/log" SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	"$cli" collect --run-id uib-collect --state-dir "$test_dir/state" \
	--output-dir "$test_dir/escaped" > /dev/null 2>&1; then
	echo "an escaping run-ledger path was accepted" >&2
	exit 1
fi
[ ! -e "$test_dir/escaped" ]

echo "Slurm result collection test: PASS"
