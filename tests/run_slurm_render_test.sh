#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
training=$project_dir/simulation/requests/training-smoke.yaml
inference=$project_dir/simulation/requests/inference-smoke.yaml
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-slurm-render.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

mkdir -p "$test_dir/runtime" "$test_dir/cache" "$test_dir/results" \
	"$test_dir/fake-bin" "$test_dir/fake-output"

python3 - "$project_dir/simulation/adapters/slurm/render" <<'PY'
import importlib.machinery
import importlib.util
import sys


loader = importlib.machinery.SourceFileLoader("uib_slurm_render", sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
renderer = importlib.util.module_from_spec(spec)
loader.exec_module(renderer)
config = {
    "communication": {"frontend": "libfabric"},
    "storage": {
        "modelCachePath": "/cache",
        "uibRuntimePath": "/runtime",
    },
    "uib": {"enabled": True},
}
assert renderer.container_mounts(config) == [
    "/runtime:/opt/uib:ro",
    "/cache:/cache",
    "/dev/null:/dev/infiniband/uib0",
]
config["uib"]["enabled"] = False
assert renderer.container_mounts(config) == ["/cache:/cache"]
PY

render_training()
{
	run_id=$1
	shift
	"$cli" render-workload --profile libvirt-slurm \
		--request "$training" --run-id "$run_id" --allow-mutable-tags \
		--set "storage.uibRuntimePath=$test_dir/runtime" \
		--set "storage.modelCachePath=$test_dir/cache" \
		--set "storage.resultsPath=$test_dir/results" "$@"
}

render_training uib-slurm-smoke > "$test_dir/training.sbatch"
sh -n "$test_dir/training.sbatch"

python3 - "$test_dir/training.sbatch" "$test_dir" <<'PY'
import json
import pathlib
import sys


batch = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
root = pathlib.Path(sys.argv[2])


def embedded(start, end):
    return batch.split(start, 1)[1].split(end, 1)[0].strip()


assert "#SBATCH --nodes=2" in batch
assert "#SBATCH --ntasks=2" in batch
assert "#SBATCH --ntasks-per-node=1" in batch
assert "#SBATCH --cpus-per-task=1" in batch
assert "#SBATCH --mem=1G" in batch
assert "#SBATCH --exclusive" in batch
assert "--container-image=uib-training:test" in batch
assert "--no-container-entrypoint" in batch
assert f"{root}/runtime:/opt/uib:ro" in batch
assert f"{root}/cache:/cache" in batch
assert "/opt/uib/lib/libuibv.so" in batch
assert "LD_LIBRARY_PATH=/opt/uib/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" in batch
assert "NCCL_IB_HCA='=uib0'" in batch
assert "GLOO_SOCKET_IFNAME=eth1" in batch
assert "UIB_ACTIVE_MTU=1024" in batch
assert "UIB_STATS_OUTPUT=stderr" in batch
assert "SLURM_UIB_NETWORK_MTU=1500" in batch
assert 'actual_mtu=$(/opt/uib/bin/ip -o link show dev ' in batch
assert "UIB_BIND_PORT=$((SLURM_UIB_BIND_PORT_BASE + LOCAL_RANK))" in batch
assert 'UIB_BIND_IP=$(/opt/uib/bin/ip -o -4 addr show dev "$SLURM_UIB_DATA_INTERFACE"' in batch
assert "/opt/uib/bin/uib-preflight --library-only" in batch
assert 'srun --version; scontrol --version' in batch
assert '--ntasks=2 --ntasks-per-node=1 /bin/sh -c' in batch
assert 'host-probes.psv' in batch
assert "export RANK=${SLURM_PROCID:-0}" in batch
assert "/opt/uib-training/bin/train" in batch

workload = json.loads(embedded(
    "<<'UIB_WORKLOAD_JSON_EOF'", "UIB_WORKLOAD_JSON_EOF"
))
assert workload["runId"] == "uib-slurm-smoke"
assert workload["training"]["steps"] == 4
resolved = json.loads(embedded(
    "<<'UIB_RESOLVED_JSON_EOF'", "UIB_RESOLVED_JSON_EOF"
))
assert resolved["cluster"]["manager"] == "slurm"
assert resolved["workload"]["entrypoint"] == "/opt/uib-training/bin/train"
assert resolved["workload"]["parallelism"]["nodes"] == 2
PY

render_training uib-slurm-resources \
	--set workload.resources.cpu=2 \
	--set workload.resources.memory=1Gi \
	> "$test_dir/resources.sbatch"
grep -F '#SBATCH --cpus-per-task=2' "$test_dir/resources.sbatch" \
	> /dev/null
grep -F '#SBATCH --mem=1G' "$test_dir/resources.sbatch" > /dev/null
test "$(grep -c -F -- '--cpus-per-task=2' \
	"$test_dir/resources.sbatch")" -eq 2

cat > "$test_dir/fake-bin/scontrol" <<'EOF'
#!/bin/sh
set -eu
if [ "$1" = show ] && [ "$2" = job ]; then
	echo "JobId=$3 JobName=uib-slurm-smoke"
	exit 0
fi
if [ "$1" = show ] && [ "$2" = hostnames ]; then
	printf '%s\n' node-a node-b
	exit 0
fi
if [ "$1" = show ] && [ "$2" = node ]; then
	echo "NodeName=$3 NodeAddr=198.51.100.21 State=ALLOCATED"
	exit 0
fi
exit 2
EOF
cat > "$test_dir/fake-bin/srun" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$@" > "$TEST_OUTPUT/srun.args"
env | sort > "$TEST_OUTPUT/srun.env"
EOF
chmod +x "$test_dir/fake-bin/scontrol" "$test_dir/fake-bin/srun"

PATH="$test_dir/fake-bin:$PATH" TEST_OUTPUT="$test_dir/fake-output" \
	SLURM_JOB_ID=4242 SLURM_JOB_NODELIST='node-[a-b]' \
	sh "$test_dir/training.sbatch"

if PATH="$test_dir/fake-bin:$PATH" TEST_OUTPUT="$test_dir/fake-output" \
	SLURM_JOB_ID=4243 SLURM_JOB_NODELIST='node-[a-b]' \
	sh "$test_dir/training.sbatch" > /dev/null 2>&1; then
	echo "an existing Slurm result directory was reused" >&2
	exit 1
fi

grep -Fx -- '--nodes=2' "$test_dir/fake-output/srun.args" > /dev/null
grep -Fx -- '--ntasks=2' "$test_dir/fake-output/srun.args" > /dev/null
grep -Fx -- '--container-image=uib-training:test' \
	"$test_dir/fake-output/srun.args" > /dev/null
grep -Fx 'NCCL_IBVERBS_LIB=/opt/uib/lib/libuibv.so' \
	"$test_dir/fake-output/srun.env" > /dev/null
grep -Fx 'NCCL_IB_HCA==uib0' "$test_dir/fake-output/srun.env" > /dev/null

python3 - "$test_dir/results/uib-slurm-smoke/status.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    status = json.load(source)
assert status == {
    "runId": "uib-slurm-smoke",
    "scheduler": "slurm",
    "jobId": "4242",
    "exitCode": 0,
    "workloadExitCode": 0,
    "expectedFaultOutcome": False,
}
PY

render_training uib-slurm-uet --overlay protocol/uet-nscc \
	> "$test_dir/uet.sbatch"
grep -F 'export UIB_PROTOCOL=uet' "$test_dir/uet.sbatch" > /dev/null
grep -F 'export UIB_UET_CC=nscc' "$test_dir/uet.sbatch" > /dev/null

render_training uib-slurm-minimum-mtu --overlay network/minimum-mtu \
	> "$test_dir/minimum-mtu.sbatch"
grep -F 'export UIB_ACTIVE_MTU=512' "$test_dir/minimum-mtu.sbatch" \
	> /dev/null
grep -F 'export SLURM_UIB_NETWORK_MTU=576' \
	"$test_dir/minimum-mtu.sbatch" > /dev/null

render_training uib-slurm-fault \
	--set 'faults=[{"at":"1s","action":"add-delay","target":"worker-01/data","latency":"2ms","duration":"1s"}]' \
	> "$test_dir/fault.sbatch"
sh -n "$test_dir/fault.sbatch"
grep -F -- '--overlap' "$test_dir/fault.sbatch" > /dev/null
grep -F -- '/run/uib-fault-target' "$test_dir/fault.sbatch" > /dev/null
grep -F -- '/run/uib-network-schedule' "$test_dir/fault.sbatch" > /dev/null
grep -F -- '--netem /usr/local/libexec/uib-netem' \
	"$test_dir/fault.sbatch" > /dev/null
grep -F -- '--privilege-command sudo' \
	"$test_dir/fault.sbatch" > /dev/null
grep -F -- 'FAULT_EXPECTED_EVENTS=' "$test_dir/fault.sbatch" > /dev/null
grep -F -- 'FAULT_RECOVERED=' "$test_dir/fault.sbatch" > /dev/null
grep -F -- "faults-\${SLURMD_NODENAME}.jsonl" \
	"$test_dir/fault.sbatch" > /dev/null

render_training uib-slurm-partition \
	--set 'faults=[{"at":"1s","action":"partition","target":"worker-01/data","duration":"1s"}]' \
	> "$test_dir/partition.sbatch"
sh -n "$test_dir/partition.sbatch"
grep -F -- '.action == "partition"' "$test_dir/partition.sbatch" \
	> /dev/null

"$cli" render-workload --profile virtme-slurm --request "$training" \
	--run-id uib-virtme-fault --allow-mutable-tags \
	--set "storage.uibRuntimePath=$test_dir/runtime" \
	--set "storage.modelCachePath=$test_dir/cache" \
	--set "storage.resultsPath=$test_dir/results" \
	--set 'faults=[{"at":"1s","action":"packet-loss","target":"worker-02/data","percent":1,"duration":"1s"}]' \
	> "$test_dir/virtme-fault.sbatch"
grep -F -- '--netem /run/uib-netem' \
	"$test_dir/virtme-fault.sbatch" > /dev/null
if grep -F -- '--privilege-command' "$test_dir/virtme-fault.sbatch" \
	> /dev/null; then
	echo "virtme fault scheduling requested unnecessary elevation" >&2
	exit 1
fi

render_training uib-slurm-rocev2-nscc \
	--overlay protocol/rocev2-nscc-experimental \
	--set uib.congestionControl.parameters.maxWindowBytes=65536 \
	> "$test_dir/rocev2-nscc.sbatch"
grep -F 'export UIB_PROTOCOL=rocev2' "$test_dir/rocev2-nscc.sbatch" \
	> /dev/null
grep -F 'export UIB_ROCEV2_CC=nscc' "$test_dir/rocev2-nscc.sbatch" \
	> /dev/null
grep -F 'export UIB_ROCEV2_NSCC_MAX_WND=65536' \
	"$test_dir/rocev2-nscc.sbatch" > /dev/null

render_training uib-slurm-memory --set slurm.memory=12Gi \
	> "$test_dir/memory.sbatch"
grep -F '#SBATCH --mem=12G' "$test_dir/memory.sbatch" > /dev/null

render_training uib-slurm-local \
	--set workload.parallelism.processesPerNode=2 \
	--set workload.parallelism.data=4 \
	--set workload.resources.cpu=2 > "$test_dir/local-ranks.sbatch"
grep -F '#SBATCH --ntasks-per-node=2' "$test_dir/local-ranks.sbatch" > /dev/null
grep -F '#SBATCH --cpus-per-task=1' "$test_dir/local-ranks.sbatch" > /dev/null
grep -F 'UIB_BIND_PORT=$((SLURM_UIB_BIND_PORT_BASE + LOCAL_RANK))' \
	"$test_dir/local-ranks.sbatch" > /dev/null

render_training uib-slurm-gpus \
	--set workload.parallelism.processesPerNode=2 \
	--set workload.parallelism.data=4 \
	--set workload.resources.cpu=2 \
	--set nodes.gpus.vendor=nvidia --set nodes.gpus.perWorker=2 \
	> "$test_dir/gpus.sbatch"
grep -F '#SBATCH --gpus-per-node=2' "$test_dir/gpus.sbatch" > /dev/null
grep -F '#SBATCH --gpus-per-task=1' "$test_dir/gpus.sbatch" > /dev/null
grep -F -- '--gpus-per-task=1' "$test_dir/gpus.sbatch" > /dev/null

render_training uib-slurm-native --set uib.enabled=false \
	--set communication.nccl.hca==mlx5_0 > "$test_dir/native.sbatch"
if grep -Eq 'libuibv|UIB_BIND|/opt/uib:ro' "$test_dir/native.sbatch"; then
	echo "native Slurm rendering retained UIB injection" >&2
	exit 1
fi

"$cli" render-workload --profile libvirt-slurm --request "$inference" \
	--run-id uib-slurm-inference --allow-mutable-tags \
	--set "storage.uibRuntimePath=$test_dir/runtime" \
	--set "storage.modelCachePath=$test_dir/cache" \
	--set "storage.resultsPath=$test_dir/results" \
	> "$test_dir/inference.sbatch"
sh -n "$test_dir/inference.sbatch"
grep -F 'inference serve' "$test_dir/inference.sbatch" > /dev/null
grep -F 'inference probe' "$test_dir/inference.sbatch" > /dev/null

if "$cli" render-workload --profile libvirt-slurm --request "$training" \
	--run-id uib-slurm-tags \
	--set "storage.uibRuntimePath=$test_dir/runtime" \
	--set "storage.modelCachePath=$test_dir/cache" \
	--set "storage.resultsPath=$test_dir/results" > /dev/null 2>&1; then
	echo "mutable Slurm images were accepted without opt-in" >&2
	exit 1
fi

digest=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
"$cli" render-workload --profile libvirt-slurm --request "$training" \
	--run-id uib-slurm-digest \
	--set "storage.uibRuntimePath=$test_dir/runtime" \
	--set "storage.modelCachePath=$test_dir/cache" \
	--set "storage.resultsPath=$test_dir/results" \
	--set "workload.image=example.invalid/training@sha256:$digest" \
	--set "uib.bundle=example.invalid/runtime@sha256:$digest" \
	> "$test_dir/digest.sbatch"
grep -F -- "--container-image=example.invalid/training@sha256:$digest" \
	"$test_dir/digest.sbatch" > /dev/null

if render_training uib-slurm-overflow --set uib.bindPort=65535 \
	--set workload.parallelism.processesPerNode=2 \
	--set workload.parallelism.data=4 \
	--set workload.resources.cpu=2 > /dev/null 2>&1; then
	echo "overflowing rank-specific UIB ports were accepted" >&2
	exit 1
fi

if render_training uib-slurm-gpu-split \
	--set workload.parallelism.processesPerNode=2 \
	--set workload.parallelism.data=4 \
	--set workload.resources.cpu=2 \
	--set nodes.gpus.vendor=nvidia --set nodes.gpus.perWorker=1 \
	> /dev/null 2>&1; then
	echo "a fractional per-task GPU allocation was accepted" >&2
	exit 1
fi

if render_training uib-slurm-ucx --overlay communication/ucx \
	> /dev/null 2>&1; then
	echo "an unsupported Slurm communication front end was accepted" >&2
	exit 1
fi

render_training uib-slurm-rank-fault \
	--set 'faults=[{"at":"1s","action":"terminate-rank","rank":0}]' \
	> "$test_dir/rank-fault.sbatch"
sh -n "$test_dir/rank-fault.sbatch"
grep -F '/usr/local/libexec/uib-rank-schedule' \
	"$test_dir/rank-fault.sbatch" > /dev/null
grep -F 'marker=/results/.uib-terminate-rank-$RANK' \
	"$test_dir/rank-fault.sbatch" > /dev/null
grep -F '"expectedFaultOutcome": $RANK_FAULT_EXPECTED' \
	"$test_dir/rank-fault.sbatch" > /dev/null

render_training uib-slurm-combined-fault \
	--set 'faults=[{"at":"1s","action":"add-delay","target":"worker-01/data","latency":"2ms","duration":"1s"},{"at":"2s","action":"terminate-rank","rank":0}]' \
	> "$test_dir/combined-fault.sbatch"
sh -n "$test_dir/combined-fault.sbatch"
grep -F "trap 'stop_fault_step; stop_rank_fault' EXIT INT TERM" \
	"$test_dir/combined-fault.sbatch" > /dev/null

if render_training uib-slurm-invalid-rank \
	--set 'faults=[{"at":"1s","action":"terminate-rank","rank":2}]' \
	> /dev/null 2>&1; then
	echo "an out-of-range workload rank was accepted" >&2
	exit 1
fi

if render_training uib-slurm-event-fault \
	--set 'faults=[{"afterEvent":"ready","action":"add-delay","target":"worker-01/data","latency":"2ms","duration":"1s"}]' \
	> /dev/null 2>&1; then
	echo "an event-relative network fault was accepted" >&2
	exit 1
fi

echo "Slurm workload render test: PASS"
