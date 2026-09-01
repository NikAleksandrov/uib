# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    job = list(yaml.safe_load_all(source))[2]
workers = job["spec"]["rayClusterSpec"]["workerGroupSpecs"]
assert len(workers) == 1
assert workers[0]["replicas"] == 1
assert workers[0]["minReplicas"] == 1
assert workers[0]["maxReplicas"] == 1
pod = workers[0]["template"]["spec"]
assert pod["containers"][0]["name"] == "ray-worker"
assert pod["affinity"]["podAntiAffinity"][
    "requiredDuringSchedulingIgnoredDuringExecution"
][0]["topologyKey"] == "kubernetes.io/hostname"
