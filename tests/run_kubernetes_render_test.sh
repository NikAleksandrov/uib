#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
request=$project_dir/simulation/requests/training-smoke.yaml
inference_request=$project_dir/simulation/requests/inference-smoke.yaml
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kubernetes-render.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

python3 "$project_dir/tests/cases/kubernetes_render/device_anchor.py" "$project_dir/simulation/adapters/kubernetes/render"

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-smoke --allow-mutable-tags > "$test_dir/workload.yaml"

python3 "$project_dir/tests/cases/kubernetes_render/training.py" "$test_dir/workload.yaml"

if grep -Eq '[&*]id[0-9]+' "$test_dir/workload.yaml"; then
	echo "rendered workload contains YAML aliases" >&2
	exit 1
fi

if "$cli" render --profile kind-kubernetes \
	--overlay network/minimum-mtu > /dev/null 2>&1; then
	echo "Kind accepted an unimplemented custom link MTU" >&2
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
python3 "$project_dir/tests/cases/kubernetes_render/resources.py" "$test_dir/native.yaml"

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-uet --allow-mutable-tags --overlay protocol/uet-nscc \
	> "$test_dir/uet.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/native.py" "$test_dir/uet.yaml"

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-rocev2-nscc --allow-mutable-tags \
	--overlay protocol/rocev2-nscc-experimental \
	--set uib.congestionControl.parameters.maxWindowBytes=65536 \
	> "$test_dir/rocev2-nscc.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/uet.py" "$test_dir/rocev2-nscc.yaml"

"$cli" render-workload --profile kind-kubernetes --request "$request" \
	--run-id uib-storage --allow-mutable-tags \
	--set storage.modelCacheClaim=uib-model-cache \
	--set storage.resultsClaim=uib-results > "$test_dir/storage.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/rocev2_nscc.py" "$test_dir/storage.yaml"

"$cli" render-workload --profile kind-kubernetes \
	--request "$inference_request" --run-id uib-inference \
	--allow-mutable-tags > "$test_dir/inference.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/rocev2_dcqcn.py" "$test_dir/inference.yaml"

"$cli" render-workload --profile kind-kubernetes \
	--request "$inference_request" --run-id uib-inference-two \
	--allow-mutable-tags --set workload.parallelism.nodes=2 \
	--set workload.parallelism.data=2 \
	--set workload.parameters.config.backend=vllm \
	--set workload.parameters.config.server.dtype=auto \
	--set workload.parameters.config.server.dataParallel=2 \
	--set workload.parameters.config.server.distributedExecutor=ray \
	--set nodes.gpus.vendor=nvidia --set nodes.gpus.perWorker=1 \
	--set nodes.gpus.resourceName=nvidia.com/gpu \
	> "$test_dir/inference-two.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/uet_rccc.py" "$test_dir/inference-two.yaml"

if "$cli" render-workload --profile kind-kubernetes \
	--request "$inference_request" --run-id uib-inference-ranks \
	--allow-mutable-tags --set workload.parallelism.processesPerNode=2 \
	--set workload.parallelism.data=2 > /dev/null 2>&1; then
	echo "multiple inference ranks sharing one UIB bind port were accepted" >&2
	exit 1
fi

if "$cli" render-workload --profile kind-kubernetes \
	--request "$inference_request" --run-id uib-inference-idle \
	--allow-mutable-tags --set workload.parallelism.nodes=2 \
	--set workload.parallelism.data=2 > /dev/null 2>&1; then
	echo "a multi-node CPU request with an idle Ray worker was accepted" >&2
	exit 1
fi

"$cli" render-workload --profile kind-kubernetes \
	--request "$inference_request" --run-id uib-inference-cache \
	--allow-mutable-tags \
	--set storage.modelCacheImage=uib-model-cache:test \
	> "$test_dir/inference-cache.yaml"
python3 "$project_dir/tests/cases/kubernetes_render/storage.py" "$test_dir/inference-cache.yaml"

if "$cli" render-workload --profile kind-kubernetes \
	--request "$inference_request" --run-id uib-inference-cache-conflict \
	--allow-mutable-tags \
	--set storage.modelCacheImage=uib-model-cache:test \
	--set storage.modelCacheClaim=uib-model-cache > /dev/null 2>&1; then
	echo "a model cache image and PVC were accepted together" >&2
	exit 1
fi

echo "Kubernetes workload render test: PASS"
