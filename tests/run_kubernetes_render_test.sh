#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/training-smoke.yaml
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kubernetes-render.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-smoke --allow-mutable-tags > "$test_dir/workload.yaml"

python3 "$project_dir/tests/cases/kubernetes_render/device_anchor.py" "$test_dir/workload.yaml"

if grep -Eq '[&*]id[0-9]+' "$test_dir/workload.yaml"; then
	echo "rendered workload contains YAML aliases" >&2
	exit 1
fi

digest=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-digest \
	--set "workload.image=example.invalid/training@sha256:$digest" \
	--set "uib.bundle=example.invalid/runtime@sha256:$digest" \
	> "$test_dir/digest.yaml"

if "$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-tags > /dev/null 2>&1; then
	echo "mutable workload images were accepted without opt-in" >&2
	exit 1
fi

if "$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id 'uib-unsafe-' --allow-mutable-tags > /dev/null 2>&1; then
	echo "an invalid Kubernetes run ID was accepted" >&2
	exit 1
fi

if "$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-rccl --allow-mutable-tags --overlay communication/rccl \
	--set nodes.gpus.vendor=amd > /dev/null 2>&1; then
	echo "RCCL was accepted by the CUDA/NCCL training image" >&2
	exit 1
fi

if "$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-local-ranks --allow-mutable-tags \
	--set workload.parallelism.processesPerNode=2 \
	--set workload.parallelism.data=4 > /dev/null 2>&1; then
	echo "multiple ranks sharing one UIB bind port were accepted" >&2
	exit 1
fi

if "$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-reserved --allow-mutable-tags \
	--set communication.nccl.environment.NCCL_NET=Socket \
	> /dev/null 2>&1; then
	echo "a protected NCCL selection variable was accepted" >&2
	exit 1
fi

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-native --allow-mutable-tags \
	--set uib.enabled=false --set communication.nccl.hca==mlx5_0 \
	> "$test_dir/native.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/training.py" "$test_dir/native.yaml"

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-uet --allow-mutable-tags --overlay protocol/uet-nscc \
	> "$test_dir/uet.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/resources.py" "$test_dir/uet.yaml"

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-storage --allow-mutable-tags \
	--set storage.modelCacheClaim=uib-model-cache \
	--set storage.resultsClaim=uib-results > "$test_dir/storage.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/native.py" "$test_dir/storage.yaml"

echo "Kubernetes workload render test: PASS"
