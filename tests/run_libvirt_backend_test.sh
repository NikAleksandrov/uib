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
chmod +x "$test_dir/bin/ssh-keygen" "$test_dir/bin/tofu" \
	"$test_dir/bin/ansible-playbook"

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
if "$cli" up --profile libvirt-kubernetes --run-id uib-kubernetes-test \
	--state-dir "$test_dir/state" >/dev/null 2>&1; then
	echo "unimplemented libvirt Kubernetes provisioning was accepted" >&2
	exit 1
fi
[ ! -e "$test_dir/state/uib-kubernetes-test" ]

export TOFU_COMMAND="$test_dir/bin/tofu"
export ANSIBLE_PLAYBOOK_COMMAND="$test_dir/bin/ansible-playbook"
export SSH_KEYGEN_COMMAND="$test_dir/bin/ssh-keygen"
export LIBVIRT_DOWNLOAD_CACHE="$test_dir/cache"
export LIBVIRT_TEST_LOG="$test_dir/log"

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
python3 "$project_dir/tests/cases/libvirt_backend/rootless_topology.py" "$infra/ledger.json"
grep -F -- ' init -input=false' "$test_dir/log/tofu.args" > /dev/null
grep -F -- ' apply -auto-approve ' "$test_dir/log/tofu.args" > /dev/null

"$cli" status --profile libvirt-slurm --run-id uib-libvirt-test \
	--state-dir "$test_dir/state" > "$test_dir/status.json"
python3 "$project_dir/tests/cases/libvirt_backend/kubernetes_status.py" "$test_dir/status.json"

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
python3 "$project_dir/tests/cases/libvirt_backend/kubernetes_images.py" "$infra/ledger.json"

before=$(wc -l < "$test_dir/log/tofu.args")
"$cli" down --profile libvirt-slurm --run-id uib-libvirt-test \
	--state-dir "$test_dir/state" \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha"
after=$(wc -l < "$test_dir/log/tofu.args")
[ "$before" -eq "$after" ]

echo "libvirt backend test: PASS"
