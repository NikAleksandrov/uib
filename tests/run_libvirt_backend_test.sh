#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
backend=$project_dir/simulation/backends/libvirt/cluster
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-libvirt-test.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

mkdir -p "$test_dir/bin" "$test_dir/cache" "$test_dir/log" "$test_dir/state"
printf '%s\n' 'cached cloud image' > "$test_dir/base.img"
image_sha=$(sha256sum "$test_dir/base.img" | awk '{print $1}')
cached_image=$test_dir/cache/ubuntu-noble-$(printf '%.16s' "$image_sha").img
cp "$test_dir/base.img" "$cached_image"

cp "$project_dir/tests/fakes/libvirt/ssh-keygen" "$test_dir/bin/ssh-keygen"
cp "$project_dir/tests/fakes/libvirt/tofu" "$test_dir/bin/tofu"
cp "$project_dir/tests/fakes/libvirt/ansible-playbook" "$test_dir/bin/ansible-playbook"
cp "$project_dir/tests/fakes/libvirt/ssh" "$test_dir/bin/ssh"
cp "$project_dir/tests/fakes/libvirt/scp" "$test_dir/bin/scp"
cp "$project_dir/tests/fakes/libvirt/docker" "$test_dir/bin/docker"
chmod +x "$test_dir/bin/ssh-keygen" "$test_dir/bin/tofu" \
	"$test_dir/bin/ansible-playbook" "$test_dir/bin/ssh" \
	"$test_dir/bin/scp" "$test_dir/bin/docker"

"$cli" render --profile libvirt-slurm --format json \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha" \
	--set nodes.cpu=2 --set nodes.memory=4Gi --set nodes.disk=16Gi \
	> "$test_dir/config.json"

printf '%s\n' 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAITest uib-test' \
	> "$test_dir/test.pub"
"$backend" render "$test_dir/config.json" --name uib-libvirt-test \
	--base-image "$cached_image" --ssh-public-key "$test_dir/test.pub" \
	> "$test_dir/values.json"
python3 "$project_dir/tests/cases/libvirt_backend/ansible_extra_vars.py" "$test_dir/values.json"

"$cli" render --profile libvirt-slurm --format json \
	--set libvirt.uri=qemu:///session \
	--set libvirt.networkMode=rootless \
	--set libvirt.sshPortBase=31000 \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha" \
	--set nodes.cpu=2 --set nodes.memory=4Gi --set nodes.disk=16Gi \
	> "$test_dir/rootless-config.json"
"$backend" render "$test_dir/rootless-config.json" \
	--name uib-libvirt-rootless --base-image "$cached_image" \
	--ssh-public-key "$test_dir/test.pub" > "$test_dir/rootless-values.json"
python3 "$project_dir/tests/cases/libvirt_backend/topology.py" "$test_dir/rootless-values.json"

if "$cli" render --profile libvirt-slurm \
	--set libvirt.networkMode=rootless >/dev/null 2>&1; then
	echo "rootless libvirt accepted a system URI" >&2
	exit 1
fi
if "$cli" render --profile libvirt-slurm \
	--set cluster.version=24.11 >/dev/null 2>&1; then
	echo "libvirt provisioning accepted an unpinned Slurm version" >&2
	exit 1
fi
export TOFU_COMMAND="$test_dir/bin/tofu"
export ANSIBLE_PLAYBOOK_COMMAND="$test_dir/bin/ansible-playbook"
export SSH_KEYGEN_COMMAND="$test_dir/bin/ssh-keygen"
export LIBVIRT_DOWNLOAD_CACHE="$test_dir/cache"
export LIBVIRT_TEST_LOG="$test_dir/log"

"$cli" up --profile libvirt-kubernetes --run-id uib-kubernetes-test \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha" \
	--set nodes.cpu=2 --set nodes.memory=4Gi --set nodes.disk=16Gi \
	> "$test_dir/kubernetes-up.out"

kubernetes_infra=$test_dir/state/uib-kubernetes-test/infrastructure
[ -f "$kubernetes_infra/kubeconfig" ]
[ "$(stat -c %a "$kubernetes_infra/ansible-vars.json")" = 600 ]
if grep -F 'uib_k3s_token' "$kubernetes_infra/lifecycle.log" >/dev/null; then
	echo "the k3s token was written to the lifecycle log" >&2
	exit 1
fi
grep -F 'server: https://192.0.2.10:6443' \
	"$kubernetes_infra/kubeconfig" > /dev/null
"$cli" kubeconfig --profile libvirt-kubernetes \
	--run-id uib-kubernetes-test --state-dir "$test_dir/state" \
	> "$test_dir/kubeconfig"
cmp "$kubernetes_infra/kubeconfig" "$test_dir/kubeconfig"
"$cli" status --profile libvirt-kubernetes \
	--run-id uib-kubernetes-test --state-dir "$test_dir/state" \
	> "$test_dir/kubernetes-status.json"
python3 "$project_dir/tests/cases/libvirt_backend/rootless_topology.py" "$kubernetes_infra/ledger.json" \
	"$test_dir/kubernetes-status.json"
if "$cli" load-image --profile libvirt-kubernetes \
	--run-id uib-kubernetes-test --state-dir "$test_dir/state" \
	--image example.invalid/uib@sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
	> /dev/null 2>&1; then
	echo "the unimplemented libvirt Kubernetes image loader was accepted" >&2
	exit 1
fi
"$cli" down --profile libvirt-kubernetes --run-id uib-kubernetes-test \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha"

"$cli" up --profile libvirt-slurm --run-id uib-libvirt-test \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha" \
	--set nodes.cpu=2 --set nodes.memory=4Gi --set nodes.disk=16Gi \
	> "$test_dir/up.out"

infra=$test_dir/state/uib-libvirt-test/infrastructure
[ -f "$infra/ledger.json" ]
[ -f "$infra/resolved-simulation.json" ]
[ -f "$infra/terraform.tfvars.json" ]
[ -f "$infra/inventory.yaml" ]
[ -f "$infra/id_ed25519" ]
[ -f "$infra/opentofu/main.tf" ]
[ -f "$infra/network.json" ]
python3 "$project_dir/tests/cases/libvirt_backend/kubernetes_status.py" "$infra/ledger.json"
python3 "$project_dir/tests/cases/libvirt_backend/kubernetes_images.py" "$infra/network.json"
grep -F -- ' init -input=false' "$test_dir/log/tofu.args" > /dev/null
grep -F -- ' apply -auto-approve ' "$test_dir/log/tofu.args" > /dev/null

"$cli" status --profile libvirt-slurm --run-id uib-libvirt-test \
	--state-dir "$test_dir/state" > "$test_dir/status.json"
python3 "$project_dir/tests/cases/libvirt_backend/host_network.py" "$test_dir/status.json"

export SSH_COMMAND="$test_dir/bin/ssh"
export SCP_COMMAND="$test_dir/bin/scp"
export CONTAINER_ENGINE_COMMAND="$test_dir/bin/docker"
export SLURM_IMAGE_CACHE="$test_dir/slurm-images"
export SLURM_CACHE_INVENTORY="$test_dir/slurm-caches"
export LIBVIRT_REMOTE_ROOT="$test_dir/remote"
mkdir -p "$LIBVIRT_REMOTE_ROOT/results"

mkdir -p "$test_dir/huggingface/blobs" "$test_dir/huggingface/snapshots"
printf '%s\n' fake-model > "$test_dir/huggingface/blobs/model"
ln -s ../blobs/model "$test_dir/huggingface/snapshots/model"
mkdir "$test_dir/unsafe-huggingface"
ln -s /etc/passwd "$test_dir/unsafe-huggingface/passwd"
if "$cli" load-cache --profile libvirt-slurm \
	--run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--source "$test_dir/unsafe-huggingface" >/dev/null 2>&1; then
	echo "load-cache accepted an escaping symbolic link" >&2
	exit 1
fi
staged_cache=$("$cli" load-cache --profile libvirt-slurm \
	--run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--source "$test_dir/huggingface")
[ "$staged_cache" = /var/cache/uib/models/huggingface ]
python3 "$project_dir/tests/cases/libvirt_backend/oversubscription.py" "$test_dir/state/uib-libvirt-test/infrastructure/caches.json" \
	"$SLURM_CACHE_INVENTORY/inventory.json"

staged_image=$("$cli" load-image --profile libvirt-slurm \
	--run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--image uib-training:test --allow-mutable-tags)
[ "$staged_image" = /var/cache/uib/images/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sqsh ]
[ -f "$SLURM_IMAGE_CACHE/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sqsh" ]
[ -f "$SLURM_IMAGE_CACHE/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sqsh.sha256" ]
[ "$(cat "$test_dir/log/docker-removed")" = \
	bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb ]
python3 "$project_dir/tests/cases/libvirt_backend/owned_ledger.py" "$test_dir/state/uib-libvirt-test/infrastructure/images.json"
python3 "$project_dir/tests/cases/libvirt_backend/network_state.py" "$SLURM_IMAGE_CACHE/inventory.json"
rm "$LIBVIRT_REMOTE_ROOT/images/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sqsh"
rm "$test_dir/log/docker-removed"
cached_image_path=$("$cli" load-image --profile libvirt-slurm \
	--run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--image uib-training:test --allow-mutable-tags)
[ "$cached_image_path" = "$staged_image" ]
[ -f "$LIBVIRT_REMOTE_ROOT/images/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sqsh" ]
[ ! -e "$test_dir/log/docker-removed" ]

cache=$SLURM_IMAGE_CACHE/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sqsh
rm "$LIBVIRT_REMOTE_ROOT/images/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sqsh"
mv "$cache" "$cache.saved"
ln -s "$cache.saved" "$cache"
if "$cli" load-image --profile libvirt-slurm \
	--run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--image uib-training:test --allow-mutable-tags > /dev/null 2>&1; then
	echo "a symbolic-link Slurm image cache was accepted" >&2
	exit 1
fi
rm "$cache"
mv "$cache.saved" "$cache"
cached_image_path=$("$cli" load-image --profile libvirt-slurm \
	--run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--image uib-training:test --allow-mutable-tags)
[ "$cached_image_path" = "$staged_image" ]

if "$cli" run --profile libvirt-slurm \
	--request "$project_dir/simulation/requests/training-smoke.yaml" \
	--run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--overlay network/datacenter --allow-mutable-tags \
	> /dev/null 2>&1; then
	echo "a workload changed the provisioned libvirt network" >&2
	exit 1
fi
[ ! -e "$test_dir/state/uib-libvirt-test/ledger.json" ]

job_id=$("$cli" run --profile libvirt-slurm \
	--request "$project_dir/simulation/requests/training-smoke.yaml" \
	--run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--set "workload.image=$staged_image" --allow-mutable-tags)
[ "$job_id" = 7654 ]
grep -F '/opt/uib/bin/ip -o -4 addr show' \
	"$test_dir/log/submitted.sbatch" > /dev/null
grep -F -- "--container-image=$staged_image" \
	"$test_dir/log/submitted.sbatch" > /dev/null

remote_run=$LIBVIRT_REMOTE_ROOT/results/uib-libvirt-test
mkdir "$remote_run"
cat > "$remote_run/status.json" <<'EOF'
{
  "runId": "uib-libvirt-test",
  "scheduler": "slurm",
  "jobId": "7654",
  "exitCode": 0
}
EOF
printf '%s\n' worker-01 worker-02 > "$remote_run/nodes.txt"
printf '%s\n' '{"result":"passed"}' > "$remote_run/result.json"
printf '%s\n' 'UIB runtime preflight: PASS' \
	> "$LIBVIRT_REMOTE_ROOT/results/uib-libvirt-test-7654.out"
: > "$LIBVIRT_REMOTE_ROOT/results/uib-libvirt-test-7654.err"

"$cli" status --profile libvirt-slurm --run-id uib-libvirt-test \
	--state-dir "$test_dir/state" > "$test_dir/job-status.psv"
grep -Fx '7654|COMPLETED|0:0|00:00:19|worker-[01-02]' \
	"$test_dir/job-status.psv" > /dev/null

"$cli" collect --run-id uib-libvirt-test --state-dir "$test_dir/state" \
	--output-dir "$test_dir/collected" > /dev/null
if [ ! -f "$test_dir/collected/workload/result.json" ]; then
	find "$test_dir/collected" -type f -print >&2
	exit 1
fi
[ -f "$test_dir/collected/logs/batch.out" ]
[ -f "$test_dir/collected/staged-images.json" ]
[ -f "$test_dir/collected/staged-caches.json" ]
[ -f "$test_dir/collected/network-emulation.json" ]
python3 "$project_dir/tests/cases/libvirt_backend/status.py" "$test_dir/collected/summary.json" \
	"$test_dir/collected/images.json" \
	"$test_dir/collected/network-emulation.json"

if "$cli" up --profile libvirt-slurm --run-id uib-libvirt-test \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha" \
	> /dev/null 2>&1; then
	echo "an existing libvirt state was overwritten" >&2
	exit 1
fi

"$cli" down --profile libvirt-slurm --run-id uib-libvirt-test \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha"
grep -F -- ' destroy -auto-approve ' "$test_dir/log/tofu.args" > /dev/null
[ "$(cat "$test_dir/log/scancel.job")" = 7654 ]
python3 "$project_dir/tests/cases/libvirt_backend/cache_inventory.py" "$infra/ledger.json"

before=$(wc -l < "$test_dir/log/tofu.args")
"$cli" down --profile libvirt-slurm --run-id uib-libvirt-test \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha"
after=$(wc -l < "$test_dir/log/tofu.args")
[ "$before" -eq "$after" ]

"$cli" up --profile libvirt-slurm --run-id uib-libvirt-failed \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha" \
	--set nodes.cpu=2 --set nodes.memory=4Gi --set nodes.disk=16Gi \
	> /dev/null
if REMOTE_SBATCH_FAIL=1 "$cli" run --profile libvirt-slurm \
	--request "$project_dir/simulation/requests/training-smoke.yaml" \
	--run-id uib-libvirt-failed --state-dir "$test_dir/state" \
	--allow-mutable-tags > /dev/null 2>&1; then
	echo "a rejected remote submission was accepted" >&2
	exit 1
fi
"$cli" down --profile libvirt-slurm --run-id uib-libvirt-failed \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha"
python3 "$project_dir/tests/cases/libvirt_backend/image_state.py" "$test_dir/state/uib-libvirt-failed/infrastructure/ledger.json"

echo "libvirt backend test: PASS"
