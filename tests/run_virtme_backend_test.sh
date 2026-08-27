#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
backend=$project_dir/simulation/backends/virtme/cluster
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-virtme-test.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

"$cli" render --profile virtme-kubernetes --format json \
	--set nodes.cpu=2 --set nodes.memory=4Gi \
	> "$test_dir/config.json"
"$backend" render "$test_dir/config.json" --name uib-virtme-test \
	> "$test_dir/topology.json"

python3 "$project_dir/tests/cases/virtme_backend/topology.py" "$test_dir/topology.json"

"$cli" render --profile virtme-slurm --format json \
	> "$test_dir/slurm-config.json"
"$backend" render "$test_dir/slurm-config.json" --name uib-slurm-test \
	> "$test_dir/slurm-topology.json"
python3 "$project_dir/tests/cases/virtme_backend/slurm_topology.py" "$test_dir/slurm-config.json" \
	"$test_dir/slurm-topology.json"

"$cli" render --profile virtme-kubernetes --overlay virtme-bridge \
	--format json > "$test_dir/bridge-config.json"
"$backend" render "$test_dir/bridge-config.json" --name uib-bridge-test \
	> "$test_dir/bridge-topology.json"
python3 "$project_dir/tests/cases/virtme_backend/bridge_topology.py" "$test_dir/bridge-topology.json"

"$cli" render --profile virtme-kubernetes --format json \
	--set infrastructure.kernel=/boot/vmlinuz-test \
	--set 'virtme.extraKernelArguments=["panic=1","iommu=pt"]' \
	> "$test_dir/kernel.json"
"$backend" render "$test_dir/kernel.json" --name uib-kernel-test \
	> "$test_dir/kernel-topology.json"
python3 "$project_dir/tests/cases/virtme_backend/kernel_topology.py" "$test_dir/kernel-topology.json"

mkdir "$test_dir/fakes"
cp "$project_dir/tests/fakes/virtme/ssh-keygen" "$test_dir/fakes/ssh-keygen"
cp "$project_dir/tests/fakes/virtme/vng" "$test_dir/fakes/vng"
cp "$project_dir/tests/fakes/virtme/ssh" "$test_dir/fakes/ssh"
cp "$project_dir/tests/fakes/virtme/k3s" "$test_dir/fakes/k3s"
cp "$project_dir/tests/fakes/virtme/ip" "$test_dir/fakes/ip"
chmod +x "$test_dir/fakes/ssh-keygen" "$test_dir/fakes/vng" \
	"$test_dir/fakes/ssh" "$test_dir/fakes/ip" "$test_dir/fakes/k3s"

export SSH_KEYGEN_COMMAND="$test_dir/fakes/ssh-keygen"
export VNG_COMMAND="$test_dir/fakes/vng"
export SSH_COMMAND="$test_dir/fakes/ssh"
export IP_COMMAND="$test_dir/fakes/ip"
export K3S_BINARY="$test_dir/fakes/k3s"
export VIRTME_SSH_WAIT=2
export VIRTME_TEST_VNG_LOG="$test_dir/vng.log"
export VIRTME_TEST_IP_LOG="$test_dir/ip.log"
export VIRTME_TEST_SSH_LOG="$test_dir/ssh.log"
export VIRTME_API_FORWARD_CHECK_COMMAND=/bin/true
export VIRTME_TEST_SHAPING=active
export VIRTME_TEST_MTU=1280

"$cli" up --profile virtme-kubernetes --overlay network/datacenter \
	--run-id virtme-lifecycle \
	--state-dir "$test_dir/runs" \
	--set network.mtu=1280 \
	--set 'network.endpointShaping={worker-02/data: {enabled: true, latency: 5ms, queueLimit: 4096, seed: 12345}}' \
	> /dev/null
"$cli" status --profile virtme-kubernetes --overlay network/datacenter \
	--run-id virtme-lifecycle \
	--state-dir "$test_dir/runs" > "$test_dir/status.json"
python3 "$project_dir/tests/cases/virtme_backend/lifecycle_status.py" "$test_dir/runs/virtme-lifecycle/infrastructure" \
	"$test_dir/status.json"
"$cli" kubeconfig --profile virtme-kubernetes --overlay network/datacenter \
	--run-id virtme-lifecycle \
	--state-dir "$test_dir/runs" > "$test_dir/exported-kubeconfig"
grep -q 'server: https://127.0.0.1:23003' \
	"$test_dir/exported-kubeconfig"
[ "$(grep -c '/run/uib-network-schedule' "$test_dir/ssh.log")" -eq 2 ]
[ "$(grep -c '/run/uib-rank-schedule' "$test_dir/ssh.log")" -eq 2 ]
[ "$(grep -c '/run/uib-fault-target' "$test_dir/ssh.log")" -eq 2 ]
grep -F -- '--target worker-01/data' "$test_dir/ssh.log" > /dev/null
grep -F -- '--target worker-02/data' "$test_dir/ssh.log" > /dev/null
grep -F -- 'ip link set dev eth1 mtu 1280' "$test_dir/ssh.log" \
	> /dev/null
"$cli" down --profile virtme-kubernetes --overlay network/datacenter \
	--run-id virtme-lifecycle \
	--state-dir "$test_dir/runs"
test "$(grep -c '/run/uib-netem clear eth1' "$test_dir/ssh.log")" -eq 2
python3 "$project_dir/tests/cases/virtme_backend/network_evidence.py" "$test_dir/runs/virtme-lifecycle/infrastructure/ledger.json"
unset VIRTME_TEST_SHAPING
unset VIRTME_TEST_MTU

"$backend" up "$test_dir/bridge-config.json" --name virtme-bridge-life \
	--state-dir "$test_dir/bridge-state" > /dev/null
"$backend" down --name virtme-bridge-life \
	--state-dir "$test_dir/bridge-state"
test "$(wc -l < "$test_dir/ip.log")" -eq 4
grep -q '^link add name uibv-' "$test_dir/ip.log"
grep -q '^link delete dev uibv-' "$test_dir/ip.log"

if "$cli" render --profile virtme-kubernetes \
	--set virtme.sshPortBase=65534 > /dev/null 2>&1; then
	echo "virtme accepted an overflowing SSH port range" >&2
	exit 1
fi
if "$cli" render --profile virtme-kubernetes \
	--set network.dataInterface=eth0 > /dev/null 2>&1; then
	echo "virtme accepted a socket/interface mismatch" >&2
	exit 1
fi
if "$backend" render "$test_dir/config.json" --name INVALID \
	> /dev/null 2>&1; then
	echo "virtme accepted an unsafe run ID" >&2
	exit 1
fi
if "$cli" render --profile virtme-slurm --format json \
	--set cluster.version=24.11.6-1 > "$test_dir/bad-slurm.json" && \
	"$backend" render "$test_dir/bad-slurm.json" --name bad-slurm \
	> /dev/null 2>&1; then
	echo "virtme accepted an unpinned Slurm package version" >&2
	exit 1
fi

echo "virtme backend topology and lifecycle test: PASS"
