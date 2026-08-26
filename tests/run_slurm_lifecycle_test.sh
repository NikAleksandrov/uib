#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/training-smoke.yaml
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-slurm-lifecycle.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

mkdir -p "$test_dir/runtime" "$test_dir/cache" "$test_dir/results" \
	"$test_dir/state" "$test_dir/fake-bin" "$test_dir/log"

cat > "$test_dir/fake-bin/sbatch" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$@" > "$TEST_LOG/sbatch.args"
if [ "${SBATCH_TEST_MODE:-pass}" = fail ]; then
	echo "submission rejected" >&2
	exit 7
fi
if [ "${SBATCH_TEST_MODE:-pass}" = malformed ]; then
	echo "submitted"
	exit 0
fi
echo "7654;test-cluster"
EOF
cat > "$test_dir/fake-bin/sacct" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$@" > "$TEST_LOG/sacct.args"
if [ "${SACCT_TEST_MODE:-available}" = disabled ]; then
	echo 'Slurm accounting storage is disabled' >&2
	exit 1
fi
echo '7654|RUNNING|0:0|00:00:03|node-a'
EOF
cat > "$test_dir/fake-bin/scontrol" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$@" > "$TEST_LOG/scontrol.args"
echo "${SCONTROL_ROW:-JobId=7654 JobState=COMPLETED ExitCode=0:0 RunTime=00:00:19 NodeList=node-a}"
EOF
cat > "$test_dir/fake-bin/scancel" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$@" >> "$TEST_LOG/scancel.args"
EOF
chmod +x "$test_dir/fake-bin/sbatch" "$test_dir/fake-bin/sacct" \
	"$test_dir/fake-bin/scontrol" "$test_dir/fake-bin/scancel"

run_workload()
{
	run_id=$1
	shift
	TEST_LOG="$test_dir/log" \
	SBATCH_COMMAND="$test_dir/fake-bin/sbatch" \
	"$cli" run --profile libvirt-slurm --request "$request" \
		--run-id "$run_id" --state-dir "$test_dir/state" \
		--allow-mutable-tags \
		--set "storage.uibRuntimePath=$test_dir/runtime" \
		--set "storage.modelCachePath=$test_dir/cache" \
		--set "storage.resultsPath=$test_dir/results" "$@"
}

job_id=$(run_workload uib-lifecycle)
[ "$job_id" = 7654 ]
run_dir=$test_dir/state/uib-lifecycle
sh -n "$run_dir/workload.sbatch"
grep -Fx -- '--parsable' "$test_dir/log/sbatch.args" > /dev/null
grep -Fx -- "$run_dir/workload.sbatch" "$test_dir/log/sbatch.args" > /dev/null

python3 - "$run_dir/ledger.json" "$run_dir/resolved-simulation.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["schemaVersion"] == 1
assert ledger["runId"] == "uib-lifecycle"
assert ledger["scheduler"] == "slurm"
assert ledger["state"] == "submitted"
assert ledger["jobId"] == "7654"
assert ledger["batchFile"] == "workload.sbatch"
assert ledger["inputs"]["profile"] == "inputs/profile.yaml"
assert ledger["inputs"]["request"] == "inputs/request.yaml"
assert ledger["inputs"]["overrides"]
with open(sys.argv[2], encoding="utf-8") as source:
    resolved = json.load(source)
assert resolved["runId"] == "uib-lifecycle"
assert resolved["cluster"]["manager"] == "slurm"
PY
[ -f "$run_dir/inputs/profile.yaml" ]
[ -f "$run_dir/inputs/request.yaml" ]

ledger_before=$(sha256sum "$run_dir/ledger.json" | awk '{print $1}')
TEST_LOG="$test_dir/log" SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	"$cli" status --profile libvirt-slurm --run-id uib-lifecycle \
	--state-dir "$test_dir/state" > "$test_dir/status.out"
ledger_after=$(sha256sum "$run_dir/ledger.json" | awk '{print $1}')
[ "$ledger_before" = "$ledger_after" ]
grep -F '7654|RUNNING|0:0|' "$test_dir/status.out" > /dev/null
grep -Fx -- '--jobs=7654' "$test_dir/log/sacct.args" > /dev/null
grep -Fx -- '--format=JobIDRaw,State,ExitCode,Elapsed,NodeList' \
	"$test_dir/log/sacct.args" > /dev/null

TEST_LOG="$test_dir/log" SACCT_TEST_MODE=disabled \
	SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	SCONTROL_COMMAND="$test_dir/fake-bin/scontrol" \
	"$cli" status --profile libvirt-slurm --run-id uib-lifecycle \
	--state-dir "$test_dir/state" > "$test_dir/fallback-status.out"
grep -Fx '7654|COMPLETED|0:0|00:00:19|node-a' \
	"$test_dir/fallback-status.out" > /dev/null
grep -Fx -- '--oneliner' "$test_dir/log/scontrol.args" > /dev/null
grep -Fx 7654 "$test_dir/log/scontrol.args" > /dev/null

TEST_LOG="$test_dir/log" SACCT_TEST_MODE=disabled \
	SCONTROL_ROW='JobId=7654 JobState=PENDING ExitCode=0:0 RunTime=00:00:00 NodeList=' \
	SACCT_COMMAND="$test_dir/fake-bin/sacct" \
	SCONTROL_COMMAND="$test_dir/fake-bin/scontrol" \
	"$cli" status --profile libvirt-slurm --run-id uib-lifecycle \
	--state-dir "$test_dir/state" > "$test_dir/pending-status.out"
grep -Fx '7654|PENDING|0:0|00:00:00|' \
	"$test_dir/pending-status.out" > /dev/null

TEST_LOG="$test_dir/log" SCANCEL_COMMAND="$test_dir/fake-bin/scancel" \
	"$cli" down --profile libvirt-slurm --run-id uib-lifecycle \
	--state-dir "$test_dir/state"
grep -Fx -- '--quiet' "$test_dir/log/scancel.args" > /dev/null
grep -Fx 7654 "$test_dir/log/scancel.args" > /dev/null
python3 - "$run_dir/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "cancellation-requested"
assert "cancellationRequestedAt" in ledger
PY

TEST_LOG="$test_dir/log" SCANCEL_COMMAND="$test_dir/fake-bin/scancel" \
	"$cli" down --profile libvirt-slurm --run-id uib-lifecycle \
	--state-dir "$test_dir/state"
[ "$(wc -l < "$test_dir/log/scancel.args")" -eq 2 ]

if run_workload uib-lifecycle > /dev/null 2>&1; then
	echo "an existing run ledger was overwritten" >&2
	exit 1
fi

if SBATCH_TEST_MODE=fail run_workload uib-submit-fail \
	> /dev/null 2>&1; then
	echo "a failed sbatch submission was accepted" >&2
	exit 1
fi
python3 - "$test_dir/state/uib-submit-fail/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "submission-failed"
assert ledger["submissionExitCode"] == 7
assert "jobId" not in ledger
PY

if SBATCH_TEST_MODE=malformed run_workload uib-submit-unknown \
	> /dev/null 2>&1; then
	echo "an unparseable successful submission was accepted" >&2
	exit 1
fi
python3 - "$test_dir/state/uib-submit-unknown/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "submission-unknown"
assert ledger["submissionOutput"] == "submitted\n"
assert "jobId" not in ledger
PY

if TEST_LOG="$test_dir/log" SBATCH_COMMAND="$test_dir/fake-bin/sbatch" \
	"$cli" run --profile libvirt-slurm --request "$request" \
	--run-id uib-missing-path --state-dir "$test_dir/state" \
	--allow-mutable-tags \
	--set "storage.uibRuntimePath=$test_dir/missing-runtime" \
	--set "storage.modelCachePath=$test_dir/cache" \
	--set "storage.resultsPath=$test_dir/results" > /dev/null 2>&1; then
	echo "a missing shared runtime path was accepted" >&2
	exit 1
fi
python3 - "$test_dir/state/uib-missing-path/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "preflight-failed"
assert len(ledger["missingPaths"]) == 1
PY

cat > "$test_dir/fake-bin/ssh" <<'EOF'
#!/bin/sh
set -eu
command=
for argument in "$@"; do
	command=$argument
done
printf '%s\n' "$command" >> "$TEST_LOG/ssh.commands"
case "$command" in
*'sbatch --parsable')
	cat > "$TEST_LOG/remote.sbatch"
	echo '8765;virtme-test'
	;;
esac
EOF
chmod +x "$test_dir/fake-bin/ssh"

remote_id=uib-virtme-remote
remote_dir=$test_dir/state/$remote_id
infrastructure=$remote_dir/infrastructure
slurm_runtime=$test_dir/virtme-slurm-runtime
mkdir -p "$infrastructure" "$slurm_runtime/usr/bin"
: > "$infrastructure/id_ed25519"
: > "$slurm_runtime/usr/bin/sbatch"
"$cli" render --profile virtme-slurm --format json \
	> "$infrastructure/resolved-simulation.json"
python3 - "$infrastructure" "$remote_id" "$slurm_runtime" <<'PY'
import json
import os
import sys


infrastructure, run_id, runtime = sys.argv[1:]
key = os.path.join(infrastructure, "id_ed25519")
with open(os.path.join(infrastructure, "ledger.json"), "w",
          encoding="utf-8") as output:
    json.dump({
        "schemaVersion": 1,
        "runId": run_id,
        "provider": "virtme",
        "clusterManager": "slurm",
        "state": "ready",
        "slurmRuntime": runtime,
    }, output)
with open(os.path.join(infrastructure, "inventory.json"), "w",
          encoding="utf-8") as output:
    json.dump({
        "all": {
            "vars": {
                "ansible_user": "root",
                "ansible_ssh_private_key_file": key,
            },
            "children": {
                "slurm_controller": {"hosts": {"controller": {}}},
                "slurm_cluster": {"hosts": {
                    "controller": {
                        "ansible_host": "127.0.0.1",
                        "ansible_port": 23000,
                    },
                }},
            },
        },
    }, output)
PY

job_id=$(TEST_LOG="$test_dir/log" SSH_COMMAND="$test_dir/fake-bin/ssh" \
	"$cli" run --profile virtme-slurm --request "$request" \
	--run-id "$remote_id" --state-dir "$test_dir/state" \
	--allow-mutable-tags)
[ "$job_id" = 8765 ]
grep -F 'SLURM_CONF=/run/uib-slurm/slurm.conf' \
	"$test_dir/log/ssh.commands" > /dev/null
grep -F 'sbatch --parsable' "$test_dir/log/ssh.commands" > /dev/null
test -s "$test_dir/log/remote.sbatch"
python3 - "$remote_dir/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["jobId"] == "8765"
assert ledger["schedulerTransport"] == "virtme-ssh"
PY

echo "Slurm workload lifecycle test: PASS"
