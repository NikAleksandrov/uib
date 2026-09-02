#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/training-smoke.yaml
name=${SIMULATION_KUBEFLOW_E2E_NAME:-uib-kf-$$}
run_id=${SIMULATION_KUBEFLOW_E2E_RUN_ID:-uib-smoke-$$}
trainer_version=${SIMULATION_KUBEFLOW_TRAINER_VERSION:-2.2.1}
operator_timeout=${SIMULATION_KUBEFLOW_OPERATOR_TIMEOUT:-10m}
workload_timeout=${SIMULATION_KUBEFLOW_WORKLOAD_TIMEOUT:-10m}
training_image=${SIMULATION_KUBEFLOW_TRAINING_IMAGE:-uib-training:test}
runtime_image=${SIMULATION_KUBEFLOW_RUNTIME_IMAGE:-uib-runtime:test}
kind_command=${KIND_COMMAND:-kind}
kubectl_command=${KUBECTL_COMMAND:-kubectl}
helm_command=${HELM_COMMAND:-helm}
results_root=${SIMULATION_KUBEFLOW_RESULTS:-$project_dir/simulation/.artifacts/results}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kubeflow-e2e.XXXXXX")
state_dir=${SIMULATION_STATE_DIR:-$test_dir/state}
evidence=$results_root/$name
bundle=$evidence/bundle
logs=$evidence/training.log
created=0
SIMULATION_KUBERNETES_WORKLOAD_CPU=${SIMULATION_KUBERNETES_WORKLOAD_CPU:-1}
SIMULATION_KUBERNETES_WORKLOAD_MEMORY=${SIMULATION_KUBERNETES_WORKLOAD_MEMORY:-1Gi}
. "$project_dir/tests/kubernetes_e2e_cluster.sh"

memory_status=0
kubernetes_host_memory_preflight || memory_status=$?
if test "$memory_status" = 77; then
	rm -rf -- "$test_dir"
	exit 0
fi
if test "$memory_status" -ne 0; then
	exit "$memory_status"
fi

collect_diagnostics()
{
	[ -s "$kubeconfig" ] || return 0
	"$kubectl_command" --kubeconfig "$kubeconfig" get trainjob "$run_id" \
		--namespace "$run_id" -o yaml > "$evidence/trainjob.yaml" \
		2>/dev/null || true
	"$kubectl_command" --kubeconfig "$kubeconfig" get all \
		--namespace "$run_id" -o wide > "$evidence/resources.txt" \
		2>&1 || true
	"$kubectl_command" --kubeconfig "$kubeconfig" get pods \
		--namespace "$run_id" -o yaml > "$evidence/pods.yaml" \
		2>&1 || true
	"$kubectl_command" --kubeconfig "$kubeconfig" describe pods \
		--namespace "$run_id" > "$evidence/pods-describe.txt" \
		2>&1 || true
	"$kubectl_command" --kubeconfig "$kubeconfig" logs \
		--namespace kubeflow-system deployment/kubeflow-trainer-controller-manager \
		--all-containers > "$evidence/operator.log" 2>&1 || true
}

cleanup()
{
	if [ "$created" = 1 ]; then
		if [ -s "$kubeconfig" ]; then
			"$kubectl_command" --kubeconfig "$kubeconfig" delete namespace \
				"$run_id" --ignore-not-found --wait=false \
				>/dev/null 2>&1 || true
			"$helm_command" --kubeconfig "$kubeconfig" uninstall \
				kubeflow-trainer --namespace kubeflow-system \
				--wait >/dev/null 2>&1 || true
		fi
		kubernetes_down >/dev/null 2>&1 || true
	fi
	rm -rf -- "$test_dir"
}

finish()
{
	status=$?
	trap - EXIT INT TERM
	if [ "$status" -ne 0 ]; then
		collect_diagnostics
	fi
	cleanup
	if [ "$status" -ne 0 ]; then
		echo "Kubeflow evidence retained at $evidence" >&2
	fi
	exit "$status"
}
trap finish EXIT
trap 'exit 130' INT TERM

require_command()
{
	case $1 in
	*/*) [ -x "$1" ] ;;
	*) command -v "$1" >/dev/null 2>&1 ;;
	esac
}

for command_name in "$kubectl_command" "$helm_command"; do
	if ! require_command "$command_name"; then
		echo "required Kubeflow E2E command not found: $command_name" >&2
		exit 1
	fi
done
if test "$kubernetes_profile" = kind-kubernetes && \
	! require_command "$kind_command"; then
	echo "required Kubeflow E2E command not found: $kind_command" >&2
	exit 1
fi
if [ -e "$evidence" ]; then
	echo "Kubeflow evidence path already exists: $evidence" >&2
	exit 1
fi
mkdir -p "$evidence"

created=1
kubernetes_up
kubernetes_load_image "$training_image"
kubernetes_load_image "$runtime_image"

"$helm_command" --kubeconfig "$kubeconfig" upgrade --install \
	kubeflow-trainer oci://ghcr.io/kubeflow/charts/kubeflow-trainer \
	--version "$trainer_version" --namespace kubeflow-system \
	--create-namespace --set runtimes.defaultEnabled=true \
	--wait --timeout "$operator_timeout"

# shellcheck disable=SC2086
"$cli" run --profile "$kubernetes_profile" --request "$request" \
	--run-id "$run_id" --allow-mutable-tags \
	--state-dir "$state_dir" --kubeconfig "$kubeconfig" \
	--set "workload.image=$training_image" \
	--set "uib.bundle=$runtime_image" $kubernetes_workload_args
"$kubectl_command" --kubeconfig "$kubeconfig" wait \
	--namespace "$run_id" --for=condition=Complete \
	"trainjob/$run_id" --timeout "$workload_timeout"
# shellcheck disable=SC2086
"$cli" status --profile "$kubernetes_profile" --run-id "$run_id" \
	--state-dir "$state_dir" --kubeconfig "$kubeconfig" \
	> "$evidence/status.json"
"$cli" collect --run-id "$run_id" --state-dir "$state_dir" \
	--kubeconfig "$kubeconfig" --output-dir "$bundle"

"$kubectl_command" --kubeconfig "$kubeconfig" get pods \
	--namespace "$run_id" --selector "uib.dev/run-id=$run_id" \
	-o json > "$evidence/pods.json"
python3 - "$evidence/pods.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    pods = json.load(source)["items"]
assert len(pods) == 2, f"expected two trainer pods, found {len(pods)}"
assert all(pod["status"]["phase"] == "Succeeded" for pod in pods)
nodes = {pod["spec"]["nodeName"] for pod in pods}
assert len(nodes) == 2, f"trainer pods did not use distinct nodes: {nodes}"
PY

python3 - "$bundle/summary.json" "$bundle/preflight.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    summary = json.load(source)
with open(sys.argv[2], encoding="utf-8") as source:
    preflight = json.load(source)
assert summary["scheduler"] == "kubernetes"
assert summary["schedulerState"] == "COMPLETED"
assert summary["uibPreflightVerified"] is True
assert summary["uibTrafficEvidence"] == "not-exercised-non-rdma-backend"
assert summary["promotionEligible"] is False
assert preflight["expectedPods"] == 2
assert preflight["passedPods"] == 2
PY

: > "$logs"
for pod in $("$kubectl_command" --kubeconfig "$kubeconfig" get pods \
	--namespace "$run_id" --selector "uib.dev/run-id=$run_id" \
	-o jsonpath='{range .items[*]}{.metadata.name}{"\n"}{end}'); do
	"$kubectl_command" --kubeconfig "$kubeconfig" logs \
		--namespace "$run_id" "$pod" --container uib-runtime-check \
		| grep -F 'UIB runtime preflight: PASS' >/dev/null
	"$kubectl_command" --kubeconfig "$kubeconfig" logs \
		--namespace "$run_id" "$pod" --container node >> "$logs"
done

python3 - "$logs" "$run_id" <<'PY'
import json
import sys


result = None
with open(sys.argv[1], encoding="utf-8") as source:
    for line in source:
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if value.get("status") == "passed":
            result = value
if result is None:
    raise AssertionError("rank zero did not emit a passing result")
print("Kubeflow result: " + json.dumps(result, sort_keys=True))
assert result["runId"] == sys.argv[2]
assert result["worldSize"] == 2
assert result["device"] == "cpu"
assert result["completedSteps"] == 4
assert result["transport"]["distributedBackend"] == "gloo"
assert result["transport"]["uibEnabled"] is True
assert result["transport"]["ncclNetwork"] == "IB"
PY

"$kubectl_command" --kubeconfig "$kubeconfig" get trainjob "$run_id" \
	--namespace "$run_id" -o yaml > "$evidence/trainjob-result.yaml"
"$kubectl_command" --kubeconfig "$kubeconfig" delete namespace "$run_id" \
	--wait --timeout "$workload_timeout"
if "$kubectl_command" --kubeconfig "$kubeconfig" get namespace "$run_id" \
	>/dev/null 2>&1; then
	echo "run namespace still exists after cleanup" >&2
	exit 1
fi

"$helm_command" --kubeconfig "$kubeconfig" uninstall kubeflow-trainer \
	--namespace kubeflow-system --wait
kubernetes_down
created=0

echo "real Kubeflow training lifecycle test: PASS"
