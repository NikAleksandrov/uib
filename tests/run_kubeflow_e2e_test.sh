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
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kubeflow-e2e.XXXXXX")
kubeconfig=$test_dir/kubeconfig
manifest=$test_dir/workload.yaml
logs=$test_dir/training.log
created=0

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
		"$cli" down --profile kind-kubernetes --run-id "$name" \
			>/dev/null 2>&1 || true
	fi
	rm -rf -- "$test_dir"
}
trap cleanup EXIT INT TERM

require_command()
{
	case $1 in
	*/*) [ -x "$1" ] ;;
	*) command -v "$1" >/dev/null 2>&1 ;;
	esac
}

for command_name in "$kind_command" "$kubectl_command" "$helm_command"; do
	if ! require_command "$command_name"; then
		echo "required Kubeflow E2E command not found: $command_name" >&2
		exit 1
	fi
done

created=1
"$cli" up --profile kind-kubernetes --run-id "$name" \
	--kubeconfig "$kubeconfig" --wait 300s
"$cli" load-image --profile kind-kubernetes --run-id "$name" \
	--image "$training_image" --image "$runtime_image" --allow-mutable-tags

"$helm_command" --kubeconfig "$kubeconfig" upgrade --install \
	kubeflow-trainer oci://ghcr.io/kubeflow/charts/kubeflow-trainer \
	--version "$trainer_version" --namespace kubeflow-system \
	--create-namespace --set runtimes.defaultEnabled=true \
	--wait --timeout "$operator_timeout"

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id "$run_id" --allow-mutable-tags \
	--set "workload.image=$training_image" \
	--set "uib.bundle=$runtime_image" > "$manifest"
"$kubectl_command" --kubeconfig "$kubeconfig" create namespace "$run_id" \
	--save-config
"$kubectl_command" --kubeconfig "$kubeconfig" apply -f "$manifest"
"$kubectl_command" --kubeconfig "$kubeconfig" wait \
	--namespace "$run_id" --for=condition=Complete \
	"trainjob/$run_id" --timeout "$workload_timeout"

"$kubectl_command" --kubeconfig "$kubeconfig" get pods \
	--namespace "$run_id" --selector "uib.dev/run-id=$run_id" \
	-o json > "$test_dir/pods.json"
python3 - "$test_dir/pods.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    pods = json.load(source)["items"]
assert len(pods) == 2, f"expected two trainer pods, found {len(pods)}"
assert all(pod["status"]["phase"] == "Succeeded" for pod in pods)
nodes = {pod["spec"]["nodeName"] for pod in pods}
assert len(nodes) == 2, f"trainer pods did not use distinct nodes: {nodes}"
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
	--namespace "$run_id" -o yaml > "$test_dir/trainjob-result.yaml"
"$kubectl_command" --kubeconfig "$kubeconfig" delete namespace "$run_id" \
	--wait --timeout "$workload_timeout"
if "$kubectl_command" --kubeconfig "$kubeconfig" get namespace "$run_id" \
	>/dev/null 2>&1; then
	echo "run namespace still exists after cleanup" >&2
	exit 1
fi

"$helm_command" --kubeconfig "$kubeconfig" uninstall kubeflow-trainer \
	--namespace kubeflow-system --wait
"$cli" down --profile kind-kubernetes --run-id "$name"
created=0

echo "real Kubeflow training lifecycle test: PASS"
