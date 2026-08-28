# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    runtime = list(yaml.safe_load_all(source))[2]
pod = runtime["spec"]["template"]["spec"]["replicatedJobs"][0][
    "template"
]["spec"]["template"]["spec"]
volumes = {volume["name"]: volume for volume in pod["volumes"]}
assert volumes["model-cache"]["persistentVolumeClaim"]["claimName"] == \
    "uib-model-cache"
assert volumes["results"]["persistentVolumeClaim"]["claimName"] == \
    "uib-results"
