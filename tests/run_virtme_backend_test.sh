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

"$cli" render --profile virtme-kubernetes --format json \
	--set infrastructure.kernel=/boot/vmlinuz-test \
	--set 'virtme.extraKernelArguments=["panic=1","iommu=pt"]' \
	> "$test_dir/kernel.json"
"$backend" render "$test_dir/kernel.json" --name uib-kernel-test \
	> "$test_dir/kernel-topology.json"
python3 "$project_dir/tests/cases/virtme_backend/slurm_topology.py" "$test_dir/kernel-topology.json"

if "$cli" render --profile virtme-kubernetes \
	--set virtme.sshPortBase=65534 > /dev/null 2>&1; then
	echo "virtme accepted an overflowing SSH port range" >&2
	exit 1
fi
if "$backend" render "$test_dir/config.json" --name INVALID \
	> /dev/null 2>&1; then
	echo "virtme accepted an unsafe run ID" >&2
	exit 1
fi

echo "virtme backend topology test: PASS"
