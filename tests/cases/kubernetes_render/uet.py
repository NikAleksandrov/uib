# SPDX-License-Identifier: Apache-2.0

import json
import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    documents = list(yaml.safe_load_all(source))
assert [document["kind"] for document in documents] == [
    "Namespace", "ConfigMap", "RayJob"
]
namespace, config_map, job = documents
assert namespace["metadata"]["name"] == "uib-inference"
config = json.loads(config_map["data"]["workload.json"])
assert config["runId"] == "uib-inference"
assert config["backend"] == "transformers"
assert config["model"]["revision"] == \
    "12fd25f77366fa6b3b4b768ec3050bf629380bac"
assert job["apiVersion"] == "ray.io/v1"
assert job["spec"]["shutdownAfterJobFinishes"] is True
assert job["spec"]["ttlSecondsAfterFinished"] == 60
assert job["spec"]["entrypoint"].startswith("/bin/sh -c ")
assert "inference serve" in job["spec"]["entrypoint"]
assert "inference probe" in job["spec"]["entrypoint"]
cluster = job["spec"]["rayClusterSpec"]
assert cluster["rayVersion"] == "2.58.0"
assert "workerGroupSpecs" not in cluster
pod = cluster["headGroupSpec"]["template"]["spec"]
assert pod["containers"][0]["name"] == "ray-head"
assert [container["name"] for container in pod["initContainers"]] == [
    "uib-runtime", "uib-runtime-check"
]
environment = {
    entry["name"]: entry for entry in pod["containers"][0]["env"]
}
assert environment["NCCL_IBVERBS_LIB"]["value"] == \
    "/opt/uib/lib/libuibv.so"
assert environment["UIB_BIND_IP"]["valueFrom"]["fieldRef"][
    "fieldPath"
] == "status.podIP"
