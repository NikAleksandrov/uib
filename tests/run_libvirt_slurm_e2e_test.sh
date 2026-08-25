#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
workload=${LIBVIRT_E2E_WORKLOAD:-lifecycle}
case $workload in
lifecycle)
	request=$project_dir/simulation/requests/slurm-lifecycle-smoke.yaml
	image=
	job_timeout=${LIBVIRT_E2E_JOB_TIMEOUT:-180}
	;;
training)
	request=$project_dir/simulation/requests/training-smoke.yaml
	image=${LIBVIRT_E2E_IMAGE:-uib-training:test}
	job_timeout=${LIBVIRT_E2E_JOB_TIMEOUT:-900}
	;;
inference)
	request=$project_dir/simulation/requests/inference-smoke.yaml
	image=${LIBVIRT_E2E_IMAGE:-uib-inference:test}
	cache_source=${LIBVIRT_E2E_CACHE_SOURCE:-$project_dir/simulation/.artifacts/huggingface}
	job_timeout=${LIBVIRT_E2E_JOB_TIMEOUT:-1200}
	;;
*)
	echo "LIBVIRT_E2E_WORKLOAD must be lifecycle, training, or inference" >&2
	exit 2
	;;
esac
case $job_timeout in
''|*[!0-9]*|0|1)
	echo "LIBVIRT_E2E_JOB_TIMEOUT must be an integer of at least 2 seconds" >&2
	exit 2
	;;
esac
state_dir=${SIMULATION_LIBVIRT_STATE_DIR:-$project_dir/simulation/.artifacts/runs}
run_id=${LIBVIRT_E2E_RUN_ID:-uib-libvirt-e2e-$$}
result_dir=${LIBVIRT_E2E_RESULT_DIR:-$project_dir/simulation/.artifacts/results/$run_id}
uri=${LIBVIRT_URI:-qemu:///session}
network_mode=${LIBVIRT_NETWORK_MODE:-rootless}
pool=${LIBVIRT_POOL:-$run_id}
pool_dir=${LIBVIRT_POOL_DIR:-$project_dir/simulation/.artifacts/libvirt-pools/$pool}
ansible_tmp=${ANSIBLE_LOCAL_TEMP:-$project_dir/simulation/.artifacts/ansible-tmp}
ssh_port_base=${LIBVIRT_SSH_PORT_BASE:-$((30000 + $$ % 20000))}
created_pool=0

settings="--set libvirt.uri=$uri --set libvirt.pool=$pool"
settings="$settings --set libvirt.networkMode=$network_mode"
settings="$settings --set nodes.workers=${LIBVIRT_WORKERS:-2}"
settings="$settings --set libvirt.sshPortBase=$ssh_port_base"
settings="$settings --set nodes.cpu=${LIBVIRT_CPUS:-2}"
settings="$settings --set nodes.memory=${LIBVIRT_MEMORY:-4Gi}"
settings="$settings --set nodes.disk=${LIBVIRT_DISK:-16Gi}"

cleanup()
{
	status=$?
	trap - EXIT INT TERM
	set +e
	if [ -f "$state_dir/$run_id/infrastructure/ledger.json" ]; then
		env TOFU_COMMAND="${TOFU_COMMAND:-tofu}" \
			"$cli" down --profile libvirt-slurm --run-id "$run_id" \
			--state-dir "$state_dir" $settings >/dev/null 2>&1
	fi
	if [ "$created_pool" -eq 1 ]; then
		virsh -c "$uri" pool-destroy "$pool" >/dev/null 2>&1
		virsh -c "$uri" pool-undefine "$pool" >/dev/null 2>&1
		rmdir "$pool_dir" >/dev/null 2>&1
	fi
	exit "$status"
}
trap cleanup EXIT INT TERM

mkdir -p "$ansible_tmp"

if [ "$uri" = qemu:///session ]; then
	if ! virsh -c "$uri" uri >/dev/null 2>&1; then
		libvirtd=${LIBVIRTD_COMMAND:-}
		if [ -z "$libvirtd" ]; then
			libvirtd=$(command -v libvirtd || true)
		fi
		if [ -z "$libvirtd" ] && [ -x /usr/sbin/libvirtd ]; then
			libvirtd=/usr/sbin/libvirtd
		fi
		if [ -z "$libvirtd" ]; then
			echo "libvirtd is required for qemu:///session" >&2
			exit 1
		fi
		"$libvirtd" --daemon --timeout 120 \
			--pid-file "$project_dir/simulation/.artifacts/$run_id-libvirtd.pid"
	fi

	if ! virsh -c "$uri" pool-info "$pool" >/dev/null 2>&1; then
		mkdir -p "$pool_dir"
		virsh -c "$uri" pool-define-as "$pool" dir \
			--target "$pool_dir" >/dev/null
		virsh -c "$uri" pool-start "$pool" >/dev/null
		created_pool=1
	fi
fi

env ANSIBLE_LOCAL_TEMP="$ansible_tmp" \
	TOFU_COMMAND="${TOFU_COMMAND:-tofu}" \
	ANSIBLE_PLAYBOOK_COMMAND="${ANSIBLE_PLAYBOOK_COMMAND:-ansible-playbook}" \
	"$cli" up --profile libvirt-slurm --run-id "$run_id" \
	--state-dir "$state_dir" $settings

infra=$state_dir/$run_id/infrastructure
python3 - "$infra/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["provider"] == "libvirt"
assert ledger["clusterManager"] == "slurm"
assert ledger["state"] == "ready"
PY

grep -F 'TASK [Run one ordinary Slurm task on every worker]' \
	"$infra/lifecycle.log" >/dev/null
grep -F 'TASK [Verify the private data path between workers]' \
	"$infra/lifecycle.log" >/dev/null
grep -F 'TASK [Run actual UIB verbs traffic between Slurm tasks]' \
	"$infra/lifecycle.log" >/dev/null
grep -F 'TASK [Require an actual verbs payload result]' \
	"$infra/lifecycle.log" >/dev/null
grep -F 'TASK [Run a pinned image through Pyxis and Enroot]' \
	"$infra/lifecycle.log" >/dev/null
grep -F 'TASK [Preflight the shared UIB runtime inside Pyxis]' \
	"$infra/lifecycle.log" >/dev/null

workload_settings=
if [ "$workload" = inference ]; then
	"$cli" load-cache --profile libvirt-slurm --run-id "$run_id" \
		--state-dir "$state_dir" --source "$cache_source" >/dev/null
fi
if [ -n "$image" ]; then
	staged_image=$("$cli" load-image --profile libvirt-slurm \
		--run-id "$run_id" --state-dir "$state_dir" \
		--image "$image" --allow-mutable-tags)
	workload_settings="--set workload.image=$staged_image"
fi

job_id=$("$cli" run --profile libvirt-slurm --request "$request" \
	--run-id "$run_id" --state-dir "$state_dir" --allow-mutable-tags \
	$settings $workload_settings)

python3 - "$state_dir/$run_id/ledger.json" "$job_id" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "submitted"
assert ledger["jobId"] == sys.argv[2]
assert ledger["schedulerTransport"] == "libvirt-ssh"
PY

status_file=$state_dir/$run_id/slurm-status.psv
attempt=0
max_attempts=$((job_timeout / 2))
while [ "$attempt" -lt "$max_attempts" ]; do
	"$cli" status --profile libvirt-slurm --run-id "$run_id" \
		--state-dir "$state_dir" $settings > "$status_file"
	job_state=$(awk -F '|' -v job="$job_id" \
		'$1 == job { split($2, state, " "); sub(/[+]+$/, "", state[1]); print state[1]; exit }' \
		"$status_file")
	case "$job_state" in
	COMPLETED)
		break
		;;
	FAILED|CANCELLED|TIMEOUT|NODE_FAIL|OUT_OF_MEMORY|PREEMPTED|BOOT_FAIL|DEADLINE)
		echo "Slurm job $job_id ended in $job_state" >&2
		cat "$status_file" >&2
		if "$cli" collect --run-id "$run_id" --state-dir "$state_dir" \
			--output-dir "$result_dir" >/dev/null 2>&1; then
			find "$result_dir/logs" -maxdepth 1 -type f -print \
				-exec tail -100 {} \; >&2
		fi
		exit 1
		;;
	esac
	attempt=$((attempt + 1))
	sleep 2
done
if [ "$job_state" != COMPLETED ]; then
	echo "Slurm job $job_id did not complete in $job_timeout seconds" >&2
	cat "$status_file" >&2
	if "$cli" collect --run-id "$run_id" --state-dir "$state_dir" \
		--output-dir "$result_dir" >/dev/null 2>&1; then
		find "$result_dir/logs" -maxdepth 1 -type f -print \
			-exec tail -100 {} \; >&2
	fi
	exit 1
fi

reported=$("$cli" collect --run-id "$run_id" --state-dir "$state_dir" \
	--output-dir "$result_dir")
[ "$reported" = "$result_dir" ]

python3 - "$result_dir" "$run_id" "$job_id" "$workload" <<'PY'
import json
import pathlib
import sys


root = pathlib.Path(sys.argv[1])
with (root / "summary.json").open(encoding="utf-8") as source:
    summary = json.load(source)
assert summary["runId"] == sys.argv[2]
assert summary["jobId"] == sys.argv[3]
assert summary["schedulerState"] == "COMPLETED"
assert summary["schedulerExitCode"] == "0:0"
assert summary["workloadStatusRecorded"] is True
assert summary["workloadExitCode"] == 0
assert summary["uibEnabled"] is True
with (root / "topology.json").open(encoding="utf-8") as source:
    topology = json.load(source)
expected_nodes = 1 if sys.argv[4] == "inference" else 2
assert topology["requestedNodes"] == expected_nodes
assert len(topology["allocatedNodes"]) == expected_nodes
if sys.argv[4] == "lifecycle":
    for rank in range(2):
        with (root / "workload" / f"rank-{rank}.json").open(
                encoding="utf-8") as source:
            result = json.load(source)
        assert result == {"rank": rank, "worldSize": 2}
elif sys.argv[4] == "training":
    with (root / "workload" / sys.argv[2] / "result.json").open(
            encoding="utf-8") as source:
        result = json.load(source)
    assert result["status"] == "passed"
    assert result["runId"] == sys.argv[2]
    assert result["worldSize"] == 2
    assert result["device"] == "cpu"
    assert result["strategy"] == "ddp"
    assert result["completedSteps"] == 4
    assert result["transport"]["distributedBackend"] == "gloo"
    assert result["transport"]["uibEnabled"] is True
    assert result["stack"]["ncclLibrary"] == "2.30.4"
    with (root / "staged-images.json").open(encoding="utf-8") as source:
        staged = json.load(source)
    assert staged["runId"] == sys.argv[2]
else:
    with (root / "workload" / sys.argv[2] / "result.json").open(
            encoding="utf-8") as source:
        result = json.load(source)
    assert result["status"] == "passed"
    assert result["runId"] == sys.argv[2]
    assert result["backend"] == "transformers"
    assert result["requests"] == 4
    assert result["outputTokens"] == 16
    assert result["transport"]["uibEnabled"] is True
    assert result["stack"]["vllm"] == "0.27.1"
    assert result["stack"]["ray"] == "2.58.0"
    assert result["stack"]["ncclLibrary"] == "2.30.7"
    with (root / "workload" / sys.argv[2] / "request-trace.json").open(
            encoding="utf-8") as source:
        trace = json.load(source)
    assert len(trace["requests"]) == 4
    with (root / "staged-caches.json").open(encoding="utf-8") as source:
        staged_cache = json.load(source)
    assert staged_cache["runId"] == sys.argv[2]
PY

grep -F 'UIB runtime preflight: PASS' "$result_dir/logs/batch.out" >/dev/null

env TOFU_COMMAND="${TOFU_COMMAND:-tofu}" \
	"$cli" down --profile libvirt-slurm --run-id "$run_id" \
	--state-dir "$state_dir" $settings

python3 - "$infra/ledger.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "destroyed"
PY

if virsh -c "$uri" dominfo "$run_id-controller" >/dev/null 2>&1; then
	echo "controller domain survived cleanup" >&2
	exit 1
fi
worker=1
while [ "$worker" -le "${LIBVIRT_WORKERS:-2}" ]; do
	worker_name=$(printf '%s-worker-%02d' "$run_id" "$worker")
	if virsh -c "$uri" dominfo "$worker_name" >/dev/null 2>&1; then
		echo "$worker_name domain survived cleanup" >&2
		exit 1
	fi
	worker=$((worker + 1))
done

echo "libvirt Slurm $workload end-to-end test: PASS"
