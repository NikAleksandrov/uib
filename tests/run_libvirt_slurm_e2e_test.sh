#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
state_dir=${SIMULATION_LIBVIRT_STATE_DIR:-$project_dir/simulation/.artifacts/runs}
run_id=${LIBVIRT_E2E_RUN_ID:-uib-libvirt-e2e-$$}
uri=${LIBVIRT_URI:-qemu:///session}
network_mode=${LIBVIRT_NETWORK_MODE:-rootless}
pool=${LIBVIRT_POOL:-$run_id}
pool_dir=${LIBVIRT_POOL_DIR:-$project_dir/simulation/.artifacts/libvirt-pools/$pool}
ansible_tmp=${ANSIBLE_LOCAL_TEMP:-$project_dir/simulation/.artifacts/ansible-tmp}
created_pool=0

settings="--set libvirt.uri=$uri --set libvirt.pool=$pool"
settings="$settings --set libvirt.networkMode=$network_mode"
settings="$settings --set nodes.workers=${LIBVIRT_WORKERS:-2}"
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

echo "libvirt Slurm end-to-end test: PASS"
