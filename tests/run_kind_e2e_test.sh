#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
name=${SIMULATION_KIND_E2E_NAME:-uib-e2e-$$}
workers=${SIMULATION_KIND_E2E_WORKERS:-1}
wait_time=${SIMULATION_KIND_E2E_WAIT:-180s}
test_timeout=${SIMULATION_KIND_E2E_TIMEOUT:-600}
images=${SIMULATION_KIND_E2E_IMAGES:-}
allow_tags=${SIMULATION_KIND_E2E_ALLOW_MUTABLE_TAGS:-0}
kind_command=${KIND_COMMAND:-kind}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kind-e2e.XXXXXX")
attempted=0

cleanup()
{
	if [ "$attempted" = 1 ]; then
		"$cli" down --profile kind-kubernetes --run-id "$name" \
			>/dev/null 2>&1 || true
	fi
	rm -rf -- "$test_dir"
}
trap cleanup EXIT INT TERM

case $workers in
''|*[!0-9]*|0)
	echo "SIMULATION_KIND_E2E_WORKERS must be a positive integer" >&2
	exit 2
	;;
esac
case $test_timeout in
''|*[!0-9]*|0)
	echo "SIMULATION_KIND_E2E_TIMEOUT must be a positive integer" >&2
	exit 2
	;;
esac
case $allow_tags in
0|1) ;;
*)
	echo "SIMULATION_KIND_E2E_ALLOW_MUTABLE_TAGS must be 0 or 1" >&2
	exit 2
	;;
esac

for command_name in "$kind_command" timeout; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required Kind E2E command not found: $command_name" >&2
		exit 1
	fi
done
if ! "$kind_command" get clusters >/dev/null 2>&1; then
	echo "Kind cannot access its container runtime; fix Docker/Podman permissions" >&2
	exit 1
fi

attempted=1
timeout -k 10 "$test_timeout" "$cli" up --profile kind-kubernetes \
	--run-id "$name" --wait "$wait_time" --set "nodes.workers=$workers"
"$cli" status --profile kind-kubernetes --run-id "$name" \
	--set "nodes.workers=$workers"
"$cli" kubeconfig --profile kind-kubernetes --run-id "$name" \
	--set "nodes.workers=$workers" > "$test_dir/exported-kubeconfig"

grep -q '^apiVersion:' "$test_dir/exported-kubeconfig"
node_count=$("$kind_command" get nodes --name "$name" | wc -l)
expected_nodes=$((workers + 1))
if [ "$node_count" -ne "$expected_nodes" ]; then
	echo "Kind returned $node_count nodes; expected $expected_nodes" >&2
	exit 1
fi

# Image references cannot contain whitespace; splitting here is intentional.
# shellcheck disable=SC2086
for image in $images; do
	if [ "$allow_tags" = 1 ]; then
		"$cli" load-image --profile kind-kubernetes --run-id "$name" \
			--image "$image" --allow-mutable-tags
	else
		"$cli" load-image --profile kind-kubernetes --run-id "$name" \
			--image "$image"
	fi
done

"$cli" down --profile kind-kubernetes --run-id "$name" \
	--set "nodes.workers=$workers"
attempted=0
if "$cli" status --profile kind-kubernetes --run-id "$name" \
	--set "nodes.workers=$workers" >/dev/null 2>&1; then
	echo "Kind cluster still exists after down" >&2
	exit 1
fi

echo "real Kind lifecycle test: PASS"
