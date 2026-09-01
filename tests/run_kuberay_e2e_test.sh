#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

preflight_only=0
if test "${1:-}" = --preflight-only; then
	preflight_only=1
	shift
fi
if test "$#" -ne 0; then
	echo "usage: $0 [--preflight-only]" >&2
	exit 2
fi

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/inference-smoke.yaml
name=${SIMULATION_KUBERAY_E2E_NAME:-uib-ray-$$}
run_id=${SIMULATION_KUBERAY_E2E_RUN_ID:-uib-infer-$$}
operator_version=${SIMULATION_KUBERAY_OPERATOR_VERSION:-1.6.2}
operator_timeout=${SIMULATION_KUBERAY_OPERATOR_TIMEOUT:-10m}
provisioning_timeout=${SIMULATION_KUBERAY_PROVISIONING_TIMEOUT:-1800}
workload_timeout=${SIMULATION_KUBERAY_WORKLOAD_TIMEOUT:-900}
inference_image=${SIMULATION_KUBERAY_INFERENCE_IMAGE:-uib-inference:test}
runtime_image=${SIMULATION_KUBERAY_RUNTIME_IMAGE:-uib-runtime:test}
cache_image=${SIMULATION_KUBERAY_MODEL_CACHE_IMAGE:-uib-model-cache:test}
kind_command=${KIND_COMMAND:-kind}
kubectl_command=${KUBECTL_COMMAND:-kubectl}
helm_command=${HELM_COMMAND:-helm}
operator_placement=
results_root=${SIMULATION_KUBERAY_RESULTS:-$project_dir/simulation/.artifacts/results}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kuberay-e2e.XXXXXX")
state_dir=${SIMULATION_STATE_DIR:-$project_dir/simulation/.artifacts/runs}
evidence=$results_root/$name
bundle=$evidence/bundle
logs=$evidence/inference.log
created=0
SIMULATION_KUBERNETES_WORKERS=${SIMULATION_KUBERNETES_WORKERS:-1}
SIMULATION_KUBERNETES_WORKLOAD_CPU=${SIMULATION_KUBERNETES_WORKLOAD_CPU:-1}
SIMULATION_KUBERNETES_WORKLOAD_MEMORY=${SIMULATION_KUBERNETES_WORKLOAD_MEMORY:-3Gi}
. "$project_dir/tests/kubernetes_e2e_cluster.sh"

memory_status=0
kubernetes_host_memory_preflight || memory_status=$?
if test "$preflight_only" = 1; then
	rm -rf -- "$test_dir"
	exit "$memory_status"
fi
if test "$memory_status" = 77; then
	exit 0
fi
if test "$memory_status" -ne 0; then
	exit "$memory_status"
fi

collect_diagnostics()
{
	[ -s "$kubeconfig" ] || return 0
	"$kubectl_command" --kubeconfig "$kubeconfig" get rayjob "$run_id" \
		--namespace "$run_id" -o yaml > "$evidence/rayjob.yaml" \
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
	"$kubectl_command" --kubeconfig "$kubeconfig" describe rayjob "$run_id" \
		--namespace "$run_id" > "$evidence/rayjob-describe.txt" \
		2>&1 || true
	"$kubectl_command" --kubeconfig "$kubeconfig" logs \
		--namespace kuberay-system deployment/kuberay-operator \
		--all-containers > "$evidence/operator.log" 2>&1 || true
	: > "$logs"
	for pod in $("$kubectl_command" --kubeconfig "$kubeconfig" get pods \
		--namespace "$run_id" \
		-o jsonpath='{range .items[*]}{.metadata.name}{"\n"}{end}' \
		2>/dev/null); do
		for container in $("$kubectl_command" --kubeconfig "$kubeconfig" \
			get pod --namespace "$run_id" "$pod" \
			-o jsonpath='{range .spec.initContainers[*]}{.name}{"\n"}{end}{range .spec.containers[*]}{.name}{"\n"}{end}' \
			2>/dev/null); do
			printf '\n[%s/%s]\n' "$pod" "$container" >> "$logs"
			"$kubectl_command" --kubeconfig "$kubeconfig" logs \
				--namespace "$run_id" "$pod" --container "$container" \
				>> "$logs" 2>&1 || true
		done
	done
}

finish()
{
	status=$?
	trap - EXIT INT TERM
	if [ "$status" -ne 0 ]; then
		collect_diagnostics
	fi
	if [ "$created" = 1 ]; then
		if [ -s "$kubeconfig" ]; then
			"$kubectl_command" --kubeconfig "$kubeconfig" delete namespace \
				"$run_id" --ignore-not-found --wait=false \
				>/dev/null 2>&1 || true
			"$helm_command" --kubeconfig "$kubeconfig" uninstall \
				kuberay-operator --namespace kuberay-system \
				--wait >/dev/null 2>&1 || true
		fi
		kubernetes_down >/dev/null 2>&1 || true
	fi
	rm -rf -- "$test_dir"
	if [ "$status" -ne 0 ]; then
		echo "KubeRay evidence retained at $evidence" >&2
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

case $provisioning_timeout:$workload_timeout in
*:*[!0-9]*|*[!0-9]*:*|0:*|*:0|:*|*:)
	echo "KubeRay timeouts must be positive integers" >&2
	exit 2
	;;
esac
for command_name in "$kubectl_command" "$helm_command"; do
	if ! require_command "$command_name"; then
		echo "required KubeRay E2E command not found: $command_name" >&2
		exit 1
	fi
done
if test "$kubernetes_profile" = kind-kubernetes && \
	! require_command "$kind_command"; then
	echo "required KubeRay E2E command not found: $kind_command" >&2
	exit 1
fi

if [ -e "$evidence" ]; then
	echo "KubeRay evidence path already exists: $evidence" >&2
	exit 1
fi
mkdir -p "$evidence"

created=1
kubernetes_up
kubernetes_load_image "$inference_image"
kubernetes_load_image "$runtime_image"
kubernetes_load_image "$cache_image"

if test "$kubernetes_profile" = virtme-kubernetes; then
	operator_placement="--set nodeSelector.kubernetes\\.io/hostname=$name-controller \
--set tolerations[0].key=node-role.kubernetes.io/control-plane \
--set tolerations[0].operator=Exists \
--set tolerations[0].effect=NoSchedule"
fi

"$helm_command" repo add kuberay \
	https://ray-project.github.io/kuberay-helm/ --force-update
"$helm_command" repo update kuberay
"$helm_command" --kubeconfig "$kubeconfig" upgrade --install \
	kuberay-operator kuberay/kuberay-operator \
	--version "$operator_version" --namespace kuberay-system \
	--set resources.requests.cpu=50m \
	--set resources.requests.memory=64Mi \
	--set resources.limits.cpu=100m \
	--set resources.limits.memory=256Mi \
	--create-namespace --wait --timeout "$operator_timeout" \
	$operator_placement

# shellcheck disable=SC2086
"$cli" run --profile "$kubernetes_profile" --request "$request" \
	--run-id "$run_id" --allow-mutable-tags \
	--state-dir "$state_dir" --kubeconfig "$kubeconfig" \
	--set "workload.image=$inference_image" \
	--set "uib.bundle=$runtime_image" \
	--set "storage.modelCacheImage=$cache_image" $kubernetes_workload_args

phase=provisioning
deadline=$(($(date +%s) + provisioning_timeout))
while :; do
	job_status=$("$kubectl_command" --kubeconfig "$kubeconfig" get rayjob \
		"$run_id" --namespace "$run_id" \
		-o jsonpath='{.status.jobStatus}' 2>/dev/null || true)
	deployment_status=$("$kubectl_command" --kubeconfig "$kubeconfig" \
		get rayjob "$run_id" --namespace "$run_id" \
		-o jsonpath='{.status.jobDeploymentStatus}' 2>/dev/null || true)
	case $job_status:$deployment_status in
	SUCCEEDED:Complete) break ;;
	FAILED:*|STOPPED:*)
		echo "RayJob failed: $job_status ($deployment_status)" >&2
		exit 1
		;;
	esac
	if [ "$phase" = provisioning ] && [ "$deployment_status" = Running ]; then
		phase=workload
		deadline=$(($(date +%s) + workload_timeout))
	fi
	if [ "$(date +%s)" -ge "$deadline" ]; then
		echo "RayJob $phase timed out: $job_status ($deployment_status)" >&2
		exit 1
	fi
	sleep 2
done

# shellcheck disable=SC2086
"$cli" status --profile "$kubernetes_profile" --run-id "$run_id" \
	--state-dir "$state_dir" --kubeconfig "$kubeconfig" \
	> "$evidence/status.json"
"$cli" collect --run-id "$run_id" --state-dir "$state_dir" \
	--kubeconfig "$kubeconfig" --output-dir "$bundle"
collect_diagnostics

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
    raise AssertionError("RayJob logs contain no passing inference result")
assert result["runId"] == sys.argv[2]
assert result["backend"] == "transformers"
assert result["requests"] == 4
assert result["outputTokens"] == 16
assert result["transport"]["uibEnabled"] is True
print("KubeRay result: " + json.dumps(result, sort_keys=True))
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
assert summary["uibTrafficEvidence"] == "not-exercised-single-rank"
assert summary["promotionEligible"] is False
assert preflight["expectedPods"] == preflight["passedPods"]
assert preflight["expectedPods"] > 0
PY

"$kubectl_command" --kubeconfig "$kubeconfig" delete namespace "$run_id" \
	--wait --timeout "${workload_timeout}s"
"$helm_command" --kubeconfig "$kubeconfig" uninstall kuberay-operator \
	--namespace kuberay-system --wait
kubernetes_down
created=0

echo "real KubeRay inference lifecycle test: PASS"
