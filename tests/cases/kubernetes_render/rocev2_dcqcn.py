# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    job = list(yaml.safe_load_all(source))[2]
pod = job["spec"]["rayClusterSpec"]["headGroupSpec"]["template"]["spec"]
initializers = {item["name"]: item for item in pod["initContainers"]}
assert initializers["model-cache"]["image"] == "uib-model-cache:test"
assert initializers["model-cache"]["args"] == [
    "mkdir -p /cache/huggingface && "
    "cp -a /model-cache/huggingface/. /cache/huggingface/"
]
environment = {
    item["name"]: item["value"] for item in pod["containers"][0]["env"]
    if "value" in item
}
assert environment["HF_HUB_OFFLINE"] == "1"
assert environment["TRANSFORMERS_OFFLINE"] == "1"
