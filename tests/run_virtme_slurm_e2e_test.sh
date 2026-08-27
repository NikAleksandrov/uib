#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
run_id=uib-vslurm-$$
state_dir=${SIMULATION_STATE_DIR:-$project_dir/simulation/.artifacts/runs}
workload=${VIRTME_SLURM_WORKLOAD:-lifecycle}
case $workload in
lifecycle)
	request=$project_dir/simulation/requests/slurm-lifecycle-smoke.yaml
	image='docker.io/library/ubuntu@sha256:2edbbc5dc405e9612ba3584ce95480277e3eb374407b5505fe26f17df77c7dbc'
	memory=${VIRTME_SLURM_MEMORY:-2Gi}
	disk=${VIRTME_SLURM_DISK:-32Gi}
	job_timeout=${VIRTME_SLURM_JOB_WAIT:-120}
	;;
training)
	request=$project_dir/simulation/requests/training-smoke.yaml
	image=${VIRTME_SLURM_IMAGE:-uib-training:test}
	memory=${VIRTME_SLURM_MEMORY:-4Gi}
	disk=${VIRTME_SLURM_DISK:-32Gi}
	job_timeout=${VIRTME_SLURM_JOB_WAIT:-900}
	;;
inference)
	request=$project_dir/simulation/requests/inference-smoke.yaml
	image=${VIRTME_SLURM_IMAGE:-uib-inference:test}
	cache_source=${VIRTME_SLURM_CACHE_SOURCE:-$project_dir/simulation/.artifacts/huggingface}
	memory=${VIRTME_SLURM_MEMORY:-8Gi}
	disk=${VIRTME_SLURM_DISK:-64Gi}
	job_timeout=${VIRTME_SLURM_JOB_WAIT:-1200}
	;;
*)
	echo "VIRTME_SLURM_WORKLOAD must be lifecycle, training, or inference" >&2
	exit 2
	;;
esac
profile_args="--set nodes.cpu=2 --set nodes.memory=$memory --set nodes.disk=$disk"
result_dir=${VIRTME_SLURM_RESULT_DIR:-$state_dir/$run_id-results}
case $job_timeout in
''|*[!0-9]*)
	echo "VIRTME_SLURM_JOB_WAIT must be a positive integer" >&2
	exit 2
	;;
esac
if test "$job_timeout" -lt 2; then
	echo "VIRTME_SLURM_JOB_WAIT must be at least 2" >&2
	exit 2
fi
network_condition=${VIRTME_NETWORK_CONDITION:-}
case $network_condition in
'') ;;
clean|datacenter|constrained|lossy|bursty|reordered|ecn|asymmetric|minimum-mtu|jumbo)
	profile_args="$profile_args --overlay network/$network_condition"
	;;
*)
	echo "unsupported virtme network condition: $network_condition" >&2
	exit 2
	;;
esac
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

python3 - "$state_dir/$run_id" "$network_condition" "$disk" <<'PY'
import json
import os
import re
import sys


state = sys.argv[1]
condition = sys.argv[2]
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
assert ledger["enrootVersion"] == "4.2.1"
assert ledger["pyxisVersion"] == "0.24.0"
assert ledger["uibTraffic"] == "passed-direct-and-pyxis"
assert ledger["networkEvidence"] == "network.json"
assert os.path.isfile(os.path.join(ledger["slurmRuntime"], "usr/bin/srun"))
assert os.path.isfile(os.path.join(
    ledger["slurmRuntime"], "usr/lib/x86_64-linux-gnu/slurm-wlm",
    "spank_pyxis.so"))
with open(os.path.join(infrastructure, "topology.json"),
          encoding="utf-8") as source:
    topology = json.load(source)
storage = topology["sharedStorage"]
assert storage["uibRuntime"]["guest"] == "/var/lib/uib/runtime"
assert storage["uibRuntime"]["mode"] == "read-only"
assert os.path.isfile(os.path.join(
    storage["uibRuntime"]["host"], "bin", "ibv_rc_pingpong"))
assert storage["images"]["guest"] == "/var/cache/uib/images"
assert storage["images"]["mode"] == "read-write"
assert os.path.isdir(storage["images"]["host"])
assert storage["modelCache"]["guest"] == "/var/cache/uib/models/huggingface"
assert storage["modelCache"]["mode"] == "read-write"
assert os.path.isdir(storage["modelCache"]["host"])
assert storage["results"]["guest"] == "/var/lib/uib/results"
assert storage["results"]["mode"] == "read-write"
assert os.path.isdir(storage["results"]["host"])
enroot = topology["nodeStorage"]["enroot"]
assert enroot["guest"] == "/var/lib/uib/enroot"
assert enroot["device"] == "/dev/vda"
assert enroot["filesystem"] == "ext4"
assert enroot["mode"] == "node-local"
assert enroot["size"] == sys.argv[3]
assert sorted(enroot["images"]) == ["worker-01", "worker-02"]
assert all(os.path.isfile(path) for path in enroot["images"].values())
with open(os.path.join(infrastructure, "network.json"),
          encoding="utf-8") as source:
    network = json.load(source)
shaped = condition not in ("", "clean", "minimum-mtu", "jumbo")
expected_mtu = {"minimum-mtu": 576, "jumbo": 9000}.get(condition, 1500)
expected_active_mtu = 512 if expected_mtu == 576 else (
    4096 if expected_mtu == 9000 else 1024)
assert network["scope"] == "worker-data-egress"
assert network["interface"] == "eth1"
assert network["mtu"] == expected_mtu
assert network["activeMtu"] == expected_active_mtu
assert network["shaping"]["enabled"] is shaped
assert len(network["endpoints"]) == 2
for endpoint in network["endpoints"].values():
    assert endpoint["mtu"] == expected_mtu
    assert ("qdisc netem " in endpoint["qdisc"]) is shaped
    assert endpoint["appliedCommand"]
    assert endpoint["iproute2"]
    assert endpoint["kernel"]
    if shaped:
        assert re.search(r"Sent [1-9][0-9]* bytes", endpoint["qdisc"])
if condition == "datacenter":
    assert network["shaping"]["bandwidth"] == "25Gbit"
    assert network["shaping"]["latency"] == "100us"
    assert network["shaping"]["jitter"] == "20us"
    assert network["shaping"]["queueLimit"] == 8192
if condition == "asymmetric":
    endpoints = {
        endpoint["target"]: endpoint
        for endpoint in network["endpoints"].values()
    }
    assert endpoints["worker-01/data"]["shaping"]["latency"] == "100us"
    assert endpoints["worker-02/data"]["shaping"]["latency"] == "5ms"
    assert endpoints["worker-02/data"]["shaping"]["bandwidth"] == "1Gbit"
with open(os.path.join(infrastructure, "inventory.json"),
          encoding="utf-8") as source:
    inventory = json.load(source)
assert len(inventory["all"]["children"]["slurm_workers"]["hosts"]) == 2
PY

if test "$workload" = inference; then
	# shellcheck disable=SC2086
	"$cli" load-cache --profile virtme-slurm --run-id "$run_id" \
		--state-dir "$state_dir" --source "$cache_source" \
		$profile_args >/dev/null
fi
# shellcheck disable=SC2086
staged_image=$("$cli" load-image --profile virtme-slurm \
	--run-id "$run_id" --state-dir "$state_dir" --image "$image" \
	--allow-mutable-tags $profile_args)
case $staged_image in
/var/cache/uib/images/*.sqsh) ;;
*)
	echo "virtme returned an invalid staged image path: $staged_image" >&2
	exit 1
	;;
esac

# shellcheck disable=SC2086
job_id=$("$cli" run --profile virtme-slurm \
	--request "$request" \
	--run-id "$run_id" --state-dir "$state_dir" --allow-mutable-tags \
	--set "workload.image=$staged_image" $profile_args)

python3 - "$state_dir/$run_id/ledger.json" "$job_id" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "submitted"
assert ledger["jobId"] == sys.argv[2]
assert ledger["schedulerTransport"] == "virtme-ssh"
PY

status_file=$state_dir/$run_id/slurm-status.psv
attempt=0
job_state=UNKNOWN
while test "$attempt" -lt $((job_timeout / 2)); do
	# shellcheck disable=SC2086
	"$cli" status --profile virtme-slurm --run-id "$run_id" \
		--state-dir "$state_dir" $profile_args > "$status_file"
	job_state=$(awk -F '|' -v job="$job_id" \
		'$1 == job { split($2, state, " "); sub(/[+]+$/, "", state[1]); print state[1]; exit }' \
		"$status_file")
	case $job_state in
	COMPLETED) break ;;
	FAILED|CANCELLED|TIMEOUT|NODE_FAIL|OUT_OF_MEMORY|PREEMPTED|BOOT_FAIL|DEADLINE)
		echo "virtme Slurm job $job_id ended in $job_state" >&2
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
if test "$job_state" != COMPLETED; then
	echo "virtme Slurm job $job_id did not complete in $job_timeout seconds" >&2
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
test "$reported" = "$result_dir"

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
assert summary["networkEmulation"] == "network-emulation.json"
with (root / "staged-images.json").open(encoding="utf-8") as source:
    staged = json.load(source)
assert staged["runId"] == sys.argv[2]
assert len(staged["images"]) == 1
with (root / "network-emulation.json").open(encoding="utf-8") as source:
    network = json.load(source)
assert network["runId"] == sys.argv[2]
assert len(network["endpoints"]) == 2
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
for image in topology["nodeStorage"]["enroot"]["images"].values():
    assert not os.path.exists(image)
PY

echo "virtme Slurm $workload end-to-end test: PASS"
