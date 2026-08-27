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
if [ "${SACCT_TEST_MODE:-available}" = disabled ]; then
	echo 'Slurm accounting storage is disabled' >&2
	exit 1
fi
echo "${SACCT_ROW:-7654|COMPLETED|0:0|00:00:19|node-[01-02]}"
EOF
cat > "$test_dir/fake-bin/scontrol" <<'EOF'
#!/bin/sh
set -eu
echo 'JobId=7654 JobState=COMPLETED ExitCode=0:0 RunTime=00:00:19 NodeList=node-[01-02]'
EOF
chmod +x "$test_dir/fake-bin/sbatch" "$test_dir/fake-bin/sacct" \
	"$test_dir/fake-bin/scontrol"

TEST_LOG="$test_dir/log" \
SBATCH_COMMAND="$test_dir/fake-bin/sbatch" \
"$cli" run --profile libvirt-slurm --request "$request" \
	--overlay protocol/uet-nscc --run-id uib-collect \
	--state-dir "$test_dir/state" --allow-mutable-tags \
	--set "storage.uibRuntimePath=$test_dir/runtime" \
	--set "storage.modelCachePath=$test_dir/cache" \
	--set "storage.resultsPath=$test_dir/results" \
	--set 'faults=[{"at":"1s","action":"packet-loss","target":"worker-01/data","percent":1,"duration":"1s"},{"at":"2s","action":"link-down","target":"worker-02/data","duration":"1s"},{"at":"3s","action":"terminate-rank","rank":0}]' \
	> /dev/null

run_results=$test_dir/results/uib-collect
mkdir "$run_results"
cat > "$run_results/status.json" <<'EOF'
{
  "runId": "uib-collect",
  "scheduler": "slurm",
  "jobId": "7654",
  "exitCode": 0,
  "workloadExitCode": 143,
  "expectedFaultOutcome": true
}
EOF
printf '%s\n' node-01 node-02 > "$run_results/nodes.txt"
printf '%s\n' 'JobId=7654 JobState=COMPLETED' > "$run_results/slurm-job.txt"
cat > "$run_results/result.json" <<'EOF'
{
  "schemaVersion": 1,
  "runId": "uib-collect",
  "status": "passed",
  "worldSize": 2,
  "device": "cpu",
  "strategy": "ddp",
  "completedSteps": 4,
  "resumedFromStep": 0,
  "durationSeconds": 2.0,
  "samplesPerSecond": 8.0,
  "loss": {"first": 4.0, "last": 2.0}
}
EOF
printf '%s\n' \
	'{"schemaVersion":1,"phase":"apply","target":"worker-01/data","result":"ok"}' \
	'{"schemaVersion":1,"phase":"recover","target":"worker-01/data","result":"ok"}' \
	> "$run_results/faults-node-01.jsonl"
printf '%s\n' \
	'{"schemaVersion":1,"phase":"apply","target":"worker-02/data","result":"ok"}' \
	'{"schemaVersion":1,"phase":"recover","target":"worker-02/data","result":"ok"}' \
	> "$run_results/faults-node-02.jsonl"
printf '%s\n' \
	'{"schemaVersion":1,"phase":"apply","action":"terminate-rank","target":"rank-0","rank":0,"result":"applied"}' \
	> "$run_results/faults-ranks.jsonl"
printf '%s\n' \
	'UIB runtime preflight: PASS' \
	'NCCL_IBVERBS_LIB set by environment to /opt/uib/lib/libuibv.so' \
	'Using network IB' \
	'Connected all rings, use ring PXN 0 GDR 0 via NET/IB/0' \
	> "$test_dir/results/uib-collect-7654.out"
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
[ -f "$bundle/scheduler/status.psv" ]
[ -f "$bundle/scheduler/status-source.json" ]
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
assert summary["underlyingWorkloadExitCode"] == 143
assert summary["expectedFaultOutcome"] is True
assert summary["uibEnabled"] is True
assert summary["protocol"] == "uet"
assert summary["congestionControl"]["algorithm"] == "nscc"
assert summary["validationLevel"] == "not-assigned"
assert summary["uibTrafficEvidence"] == "confirmed"
assert summary["transportEvidence"] == "transport-evidence.json"
assert summary["faultEvidenceFiles"] == 3
assert summary["faultEvidenceRecords"] == 5
assert "fault-evidence/completed-targets" not in summary["missingArtifacts"]
assert "fault-evidence/terminated-ranks" not in summary["missingArtifacts"]
assert summary["promotionEligible"] is False
with (root / "transport-evidence.json").open(encoding="utf-8") as source:
    transport = json.load(source)
assert transport["classification"] == "confirmed"
assert transport["positiveMatches"] == ["uib-library", "ib-data-path"]
assert transport["forbiddenMatches"] == []
with (root / "metrics" / "workload.jsonl").open(encoding="utf-8") as source:
    metrics = [json.loads(line) for line in source]
assert len(metrics) == 1
assert metrics[0]["kind"] == "training"
assert metrics[0]["worldSize"] == 2
assert metrics[0]["samplesPerSecond"] == 8.0
assert metrics[0]["sourceFiles"] == ["workload/result.json"]
assert summary["workloadMetricRecords"] == 1
with (root / "faults.jsonl").open(encoding="utf-8") as source:
    faults = [json.loads(line) for line in source]
assert len(faults) == 5
assert {item["target"] for item in faults} == {
    "worker-01/data", "worker-02/data", "rank-0"}
assert {item["sourceFile"] for item in faults} == {
    "faults-node-01.jsonl", "faults-node-02.jsonl", "faults-ranks.jsonl"}
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

printf '%s\n' 'Connected via NET/Socket/0' \
	>> "$test_dir/results/uib-collect-7654.out"
fallback=$test_dir/fallback
TEST_LOG="$test_dir/log" SACCT_TEST_MODE=disabled \
	SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	SCONTROL_COMMAND="$test_dir/fake-bin/scontrol" \
	"$cli" collect --run-id uib-collect --state-dir "$test_dir/state" \
	--output-dir "$fallback" > /dev/null
[ -f "$fallback/scheduler/scontrol.txt" ]
[ ! -e "$fallback/scheduler/sacct.psv" ]
python3 - "$fallback/summary.json" \
	"$fallback/scheduler/status-source.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    summary = json.load(source)
with open(sys.argv[2], encoding="utf-8") as source:
    status = json.load(source)
assert summary["schedulerEvidenceSource"] == "scontrol"
assert summary["uibTrafficEvidence"] == "fallback-detected"
assert status["source"] == "scontrol"
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
