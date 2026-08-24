# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    documents = list(yaml.safe_load_all(source))
runtime = documents[2]
job = documents[3]
pod = runtime["spec"]["template"]["spec"]["replicatedJobs"][0][
    "template"
]["spec"]["template"]["spec"]
assert "initContainers" not in pod
assert all(volume["name"] != "uib-runtime" for volume in pod["volumes"])
environment = {entry["name"]: entry for entry in job["spec"]["trainer"]["env"]}
assert environment["NCCL_IB_HCA"]["value"] == "=mlx5_0"
assert "NCCL_IBVERBS_LIB" not in environment
assert all(not name.startswith("UIB_") for name in environment)
