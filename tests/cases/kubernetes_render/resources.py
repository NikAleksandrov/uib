# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    job = list(yaml.safe_load_all(source))[-1]
resources = job["spec"]["trainer"]["resourcesPerNode"]
assert resources == {
    "limits": {"cpu": "1", "memory": "1Gi"},
    "requests": {"cpu": "1", "memory": "1Gi"},
}
