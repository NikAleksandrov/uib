# SPDX-License-Identifier: Apache-2.0

import json
import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    documents = list(yaml.safe_load_all(source))

assert [document["kind"] for document in documents] == [
    "Namespace", "ConfigMap", "TrainingRuntime", "TrainJob"
]
namespace, config_map, runtime, job = documents
assert namespace["metadata"]["name"] == "uib-smoke"
assert all(
    document["metadata"].get("namespace", "uib-smoke") == "uib-smoke"
    for document in documents
)
workload = json.loads(config_map["data"]["workload.json"])
assert workload["runId"] == "uib-smoke"
assert workload["training"]["device"] == "cpu"

assert runtime["metadata"]["name"] == "uib-smoke-torch"
assert runtime["spec"]["mlPolicy"] == {"numNodes": 2, "torch": {}}
pod = runtime["spec"]["template"]["spec"]["replicatedJobs"][0][
    "template"
]["spec"]["template"]["spec"]
assert [container["name"] for container in pod["initContainers"]] == [
    "uib-runtime", "uib-runtime-check"
]
assert pod["initContainers"][0]["args"] == [
    "cp -a /uib-runtime/. /opt/uib/"
]
assert pod["containers"][0]["image"] == "uib-training:test"
assert any(volume["name"] == "uib-runtime" for volume in pod["volumes"])
assert pod["affinity"]["podAntiAffinity"][
    "requiredDuringSchedulingIgnoredDuringExecution"
][0]["topologyKey"] == "kubernetes.io/hostname"

trainer = job["spec"]["trainer"]
assert job["spec"]["runtimeRef"] == {
    "apiGroup": "trainer.kubeflow.org",
    "kind": "TrainingRuntime",
    "name": "uib-smoke-torch",
}
assert trainer["image"] == "uib-training:test"
assert trainer["command"] == ["/bin/sh", "-c"]
assert trainer["args"][1:] == [
    "uib-kubeflow",
    "/opt/uib-training/bin/train",
    "--config",
    "/etc/uib-workload/workload.json",
]
assert 'exec torchrun --nnodes="$PET_NNODES"' in trainer["args"][0]
assert '--node-rank="$PET_NODE_RANK"' in trainer["args"][0]
assert '--no-python "$@"' in trainer["args"][0]
assert trainer["numNodes"] == 2
assert trainer["numProcPerNode"] == 1
environment = {entry["name"]: entry for entry in trainer["env"]}
assert environment["NCCL_NET"]["value"] == "IB"
assert environment["NCCL_IB_HCA"]["value"] == "=uib0"
assert environment["NCCL_IBVERBS_LIB"]["value"] == \
    "/opt/uib/lib/libuibv.so"
assert environment["UIB_PROTOCOL"]["value"] == "rocev2"
assert environment["UIB_BIND_PORT"]["value"] == "4791"
assert environment["UIB_BIND_IP"]["valueFrom"]["fieldRef"][
    "fieldPath"
] == "status.podIP"
